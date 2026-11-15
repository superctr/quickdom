// Copyright 2026 Ian Karlsson
#include <algorithm>

#include "dom/dom.h"

namespace qdom
{

Event *Dom::event_of(JSValueConst v) const
{
	return static_cast<Event *>(JS_GetOpaque(v, m_cls.event));
}

JSValue Dom::new_event(std::string_view type, bool bubbles, bool cancelable)
{
	JSValue obj = JS_NewObjectClass(m_ctx, int(m_cls.event));
	auto *ev = new Event();
	ev->type = type;
	ev->type_name = names.get(type);
	ev->bubbles = bubbles;
	ev->cancelable = cancelable;
	ev->time_stamp = m_now;
	JS_SetOpaque(obj, ev);
	return obj;
}

bool Dom::fire(Node *target, std::string_view type, bool bubbles, bool cancelable)
{
	JSValue ev = new_event(type, bubbles, cancelable);
	event_of(ev)->trusted = true;
	const bool ok = dispatch(target, ev);
	JS_FreeValue(m_ctx, ev);
	return ok;
}

bool Dom::dispatch(Node *target, JSValueConst event)
{
	Event *ev = event_of(event);
	if (!ev || ev->dispatching)
		return true;
	ev->dispatching = true;
	ev->stop = ev->stop_now = false;
	JS_FreeValue(m_ctx, ev->target);
	ev->target = JS_DupValue(m_ctx, target ? target->wrapper() : m_global);

	// the path, held while listeners run (they may take nodes out): the target, its ancestors, the window
	struct Stop
	{
		Node *node;  //!< null for the window
		JSValue value;
	};
	std::vector<Stop> path;
	for (Node *n = target; n; n = n->parent())
		path.push_back({ n, JS_DupValue(m_ctx, n->wrapper()) });
	if (!target || (path.back().node->is_document() && path.back().node == m_doc))
		path.push_back({ nullptr, JS_DupValue(m_ctx, m_global) });

	auto invoke = [&](Stop &s, bool capture_phase, bool target_phase) {
		std::vector<Listener> &list = s.node ? s.node->listeners() : m_window_listeners;
		// the listeners as they are now: one added meanwhile waits for the next event, one removed is skipped
		std::vector<std::pair<JSValue, bool>> now;
		for (const Listener &l : list)
			if (l.type == ev->type_name && !l.removed && (target_phase || l.capture == capture_phase))
				now.emplace_back(JS_DupValue(m_ctx, l.fn), l.capture);
		if (now.empty())
			return;
		JS_FreeValue(m_ctx, ev->current_target);
		ev->current_target = JS_DupValue(m_ctx, s.value);
		for (auto &[fn, capture] : now)
		{
			auto it = std::find_if(list.begin(), list.end(), [&](const Listener &l) {
				return l.type == ev->type_name && l.capture == capture && JS_VALUE_GET_PTR(l.fn) == JS_VALUE_GET_PTR(fn)
					&& !l.removed;
			});
			if (it == list.end() || ev->stop_now)
				continue;
			if (it->once)
			{
				JS_FreeValue(m_ctx, it->fn);
				list.erase(it);
			}
			JSValue r;
			if (JS_IsFunction(m_ctx, fn))
				r = JS_Call(m_ctx, fn, s.value, 1, &event);
			else
			{
				JSValue handle = JS_GetPropertyStr(m_ctx, fn, "handleEvent");
				r = JS_IsFunction(m_ctx, handle) ? JS_Call(m_ctx, handle, fn, 1, &event) : JS_UNDEFINED;
				JS_FreeValue(m_ctx, handle);
			}
			if (JS_IsException(r))
				report_exception();
			JS_FreeValue(m_ctx, r);
		}
		for (auto &[fn, capture] : now)
			JS_FreeValue(m_ctx, fn);
	};

	// capturing from the window down, at the target, bubbling up
	ev->phase = 1;
	for (size_t i = path.size(); i-- > 1 && !ev->stop;)
		invoke(path[i], true, false);
	if (!ev->stop)
	{
		ev->phase = 2;
		invoke(path[0], false, true);
	}
	if (ev->bubbles)
	{
		ev->phase = 3;
		for (size_t i = 1; i < path.size() && !ev->stop; i++)
			invoke(path[i], false, false);
	}
	ev->phase = 0;
	JS_FreeValue(m_ctx, ev->current_target);
	ev->current_target = JS_NULL;
	ev->dispatching = false;
	for (Stop &s : path)
		JS_FreeValue(m_ctx, s.value);
	return !ev->default_prevented;
}

} // namespace qdom
