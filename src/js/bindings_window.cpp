// Copyright 2026 Ian Karlsson
// The window: the global object's sizes and listeners, Event and its kinds, the timers, animation frames,
// getComputedStyle, ResizeObserver and performance.
#include <algorithm>
#include <cmath>

#include "quickdom/quickdom.h"

#include "dom/observer.h"
#include "input/input.h"
#include "js/bindings.h"
#include "layout/layout.h"
#include "paint/paint.h"

namespace qdom::js
{

JSValue add_listener(Dom &dom, JSContext *ctx, std::vector<Listener> &list, int argc, JSValueConst *argv);
JSValue remove_listener(Dom &dom, JSContext *ctx, std::vector<Listener> &list, int argc, JSValueConst *argv);

namespace
{

#define FN [](Dom & dom, JSContext * ctx, JSValueConst self, int argc, JSValueConst *argv) -> JSValue
#define THIS_EVENT \
	Event *ev = dom.event_of(self); \
	if (!ev) \
		return illegal(ctx)

bool get_bool(JSContext *ctx, JSValueConst o, const char *k, bool fallback)
{
	JSValue v = JS_GetPropertyStr(ctx, o, k);
	const bool out = JS_IsUndefined(v) ? fallback : JS_ToBool(ctx, v);
	JS_FreeValue(ctx, v);
	return out;
}

double get_number(JSContext *ctx, JSValueConst o, const char *k, double fallback)
{
	JSValue v = JS_GetPropertyStr(ctx, o, k);
	const double out = to_number(ctx, v, fallback);
	JS_FreeValue(ctx, v);
	return out;
}

std::string get_string(JSContext *ctx, JSValueConst o, const char *k, const std::string &fallback)
{
	JSValue v = JS_GetPropertyStr(ctx, o, k);
	std::string out = JS_IsUndefined(v) ? fallback : to_string(ctx, v);
	JS_FreeValue(ctx, v);
	return out;
}

//! An event from a script: new Event(type, init), and its kinds.
JSValue construct_event(Dom &dom, JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "Event: a type is needed");
	JSValueConst init = argc > 1 && JS_IsObject(argv[1]) ? argv[1] : JS_UNDEFINED;
	const bool has = JS_IsObject(init);
	JSValue obj = dom.new_event(to_string(ctx, argv[0]), has && get_bool(ctx, init, "bubbles", false),
		has && get_bool(ctx, init, "cancelable", false));
	if (!has)
		return obj;
	Event *e = dom.event_of(obj);
	e->composed = get_bool(ctx, init, "composed", false);
	e->key = get_string(ctx, init, "key", "");
	e->code = get_string(ctx, init, "code", "");
	e->repeat = get_bool(ctx, init, "repeat", false);
	e->ctrl = get_bool(ctx, init, "ctrlKey", false);
	e->shift = get_bool(ctx, init, "shiftKey", false);
	e->alt = get_bool(ctx, init, "altKey", false);
	e->meta = get_bool(ctx, init, "metaKey", false);
	e->button = int(get_number(ctx, init, "button", 0));
	e->buttons = int(get_number(ctx, init, "buttons", 0));
	e->client_x = get_number(ctx, init, "clientX", 0);
	e->client_y = get_number(ctx, init, "clientY", 0);
	e->screen_x = get_number(ctx, init, "screenX", 0);
	e->screen_y = get_number(ctx, init, "screenY", 0);
	e->detail = int(get_number(ctx, init, "detail", 0));
	e->pointer_id = int(get_number(ctx, init, "pointerId", 1));
	e->pointer_type = get_string(ctx, init, "pointerType", "mouse");
	e->is_primary = get_bool(ctx, init, "isPrimary", true);
	e->delta_x = get_number(ctx, init, "deltaX", 0);
	e->delta_y = get_number(ctx, init, "deltaY", 0);
	e->delta_z = get_number(ctx, init, "deltaZ", 0);
	e->delta_mode = int(get_number(ctx, init, "deltaMode", 0));
	e->input_type = get_string(ctx, init, "inputType", "");
	e->data = get_string(ctx, init, "data", "");
	JSValue rel = JS_GetPropertyStr(ctx, init, "relatedTarget");
	if (dom.node_of(rel))
		e->related_target = rel;
	else
		JS_FreeValue(ctx, rel);
	JSValue transfer = JS_GetPropertyStr(ctx, init, "dataTransfer");
	if (JS_IsObject(transfer))
		e->data_transfer = transfer;
	else
		JS_FreeValue(ctx, transfer);
	return obj;
}

//! A property of an element's computed style as getComputedStyle gives it: the used value of the box's measures
//! when it is laid out, else the computed one.
std::string computed_value(Dom &dom, Element *el, const ComputedStyle &cs, Prop p)
{
	LayoutNode *b = dom.layout_of(el);
	if (b && !b->inline_box)
	{
		auto px = [](double v) { return format_number(v) + "px"; };
		switch (p)
		{
		case Prop::Width:
			return px(cs.box_sizing == BoxSizing::BorderBox ? b->w : b->w - b->bl - b->br - b->pl - b->pr);
		case Prop::Height:
			return px(cs.box_sizing == BoxSizing::BorderBox ? b->h : b->h - b->bt - b->bb - b->pt - b->pb);
		case Prop::PaddingTop: return px(b->pt);
		case Prop::PaddingRight: return px(b->pr);
		case Prop::PaddingBottom: return px(b->pb);
		case Prop::PaddingLeft: return px(b->pl);
		case Prop::MarginTop: return px(b->mt.value_or(0));
		case Prop::MarginRight: return px(b->mr.value_or(0));
		case Prop::MarginBottom: return px(b->mb.value_or(0));
		case Prop::MarginLeft: return px(b->ml.value_or(0));
		default: break;
		}
	}
	return cs.value(p);
}

void observer_targets(Dom &dom, JSContext *ctx, ResizeObserver *o, Element *el, bool add)
{
	auto it = std::find_if(o->targets.begin(), o->targets.end(), [&](const auto &t) { return t.el == el; });
	if (add)
	{
		if (it != o->targets.end())
			return;
		o->targets.push_back({ el, JS_DupValue(ctx, el->wrapper()), 0, 0 });
		el->observers.push_back(JS_DupValue(ctx, o->self));
		return;
	}
	if (it == o->targets.end())
		return;
	JSValue v = it->value;
	o->targets.erase(it);
	for (size_t i = 0; i < el->observers.size(); i++)
		if (JS_VALUE_GET_PTR(el->observers[i]) == JS_VALUE_GET_PTR(o->self))
		{
			JSValue w = el->observers[i];
			el->observers.erase(el->observers.begin() + long(i));
			JS_FreeValue(ctx, w);
			break;
		}
	JS_FreeValue(ctx, v);
}

ResizeObserver *observer_of(Dom &dom, JSValueConst v)
{
	return static_cast<ResizeObserver *>(JS_GetOpaque(v, dom.classes().observer));
}

} // namespace

void install_window(Dom &dom)
{
	JSContext *ctx = dom.ctx();
	const Classes &c = dom.classes();
	JSValue global = dom.global();

	// ---------------------------------------------------------------- Event

	JSValue event = JS_NewObject(ctx);
	define(dom, event, {
		{ "type", FN { THIS_EVENT; return str(ctx, ev->type); }, nullptr },
		{ "bubbles", FN { THIS_EVENT; return JS_NewBool(ctx, ev->bubbles); }, nullptr },
		{ "cancelable", FN { THIS_EVENT; return JS_NewBool(ctx, ev->cancelable); }, nullptr },
		{ "composed", FN { THIS_EVENT; return JS_NewBool(ctx, ev->composed); }, nullptr },
		{ "defaultPrevented", FN { THIS_EVENT; return JS_NewBool(ctx, ev->default_prevented); }, nullptr },
		{ "target", FN { THIS_EVENT; return JS_DupValue(ctx, JS_IsUndefined(ev->target) ? JS_NULL : ev->target); }, nullptr },
		{ "srcElement", FN { THIS_EVENT; return JS_DupValue(ctx, JS_IsUndefined(ev->target) ? JS_NULL : ev->target); }, nullptr },
		{ "currentTarget", FN { THIS_EVENT; return JS_DupValue(ctx, ev->current_target); }, nullptr },
		{ "relatedTarget", FN { THIS_EVENT; return JS_DupValue(ctx, ev->related_target); }, nullptr },
		{ "dataTransfer", FN { THIS_EVENT; return JS_DupValue(ctx, ev->data_transfer); }, nullptr },
		{ "eventPhase", FN { THIS_EVENT; return JS_NewInt32(ctx, ev->phase); }, nullptr },
		{ "timeStamp", FN { THIS_EVENT; return JS_NewFloat64(ctx, ev->time_stamp); }, nullptr },
		{ "isTrusted", FN { THIS_EVENT; return JS_NewBool(ctx, ev->trusted); }, nullptr },
		{ "key", FN { THIS_EVENT; return str(ctx, ev->key); }, nullptr },
		{ "code", FN { THIS_EVENT; return str(ctx, ev->code); }, nullptr },
		{ "repeat", FN { THIS_EVENT; return JS_NewBool(ctx, ev->repeat); }, nullptr },
		{ "isComposing", FN { THIS_EVENT; return JS_NewBool(ctx, ev->is_composing); }, nullptr },
		{ "location", FN { THIS_EVENT; return JS_NewInt32(ctx, ev->location); }, nullptr },
		{ "ctrlKey", FN { THIS_EVENT; return JS_NewBool(ctx, ev->ctrl); }, nullptr },
		{ "shiftKey", FN { THIS_EVENT; return JS_NewBool(ctx, ev->shift); }, nullptr },
		{ "altKey", FN { THIS_EVENT; return JS_NewBool(ctx, ev->alt); }, nullptr },
		{ "metaKey", FN { THIS_EVENT; return JS_NewBool(ctx, ev->meta); }, nullptr },
		{ "button", FN { THIS_EVENT; return JS_NewInt32(ctx, ev->button); }, nullptr },
		{ "buttons", FN { THIS_EVENT; return JS_NewInt32(ctx, ev->buttons); }, nullptr },
		{ "clientX", FN { THIS_EVENT; return JS_NewFloat64(ctx, ev->client_x); }, nullptr },
		{ "clientY", FN { THIS_EVENT; return JS_NewFloat64(ctx, ev->client_y); }, nullptr },
		{ "x", FN { THIS_EVENT; return JS_NewFloat64(ctx, ev->client_x); }, nullptr },
		{ "y", FN { THIS_EVENT; return JS_NewFloat64(ctx, ev->client_y); }, nullptr },
		{ "pageX", FN { THIS_EVENT; return JS_NewFloat64(ctx, ev->client_x); }, nullptr },
		{ "pageY", FN { THIS_EVENT; return JS_NewFloat64(ctx, ev->client_y); }, nullptr },
		{ "screenX", FN { THIS_EVENT; return JS_NewFloat64(ctx, ev->screen_x); }, nullptr },
		{ "screenY", FN { THIS_EVENT; return JS_NewFloat64(ctx, ev->screen_y); }, nullptr },
		{ "offsetX", FN {
			THIS_EVENT;
			Element *t = dom.element_of(ev->target);
			return JS_NewFloat64(ctx, ev->client_x - (t ? dom.client_rect(t).x : 0));
		}, nullptr },
		{ "offsetY", FN {
			THIS_EVENT;
			Element *t = dom.element_of(ev->target);
			return JS_NewFloat64(ctx, ev->client_y - (t ? dom.client_rect(t).y : 0));
		}, nullptr },
		{ "detail", FN { THIS_EVENT; return JS_NewInt32(ctx, ev->detail); }, nullptr },
		{ "pointerId", FN { THIS_EVENT; return JS_NewInt32(ctx, ev->pointer_id); }, nullptr },
		{ "pointerType", FN { THIS_EVENT; return str(ctx, ev->pointer_type); }, nullptr },
		{ "isPrimary", FN { THIS_EVENT; return JS_NewBool(ctx, ev->is_primary); }, nullptr },
		{ "deltaX", FN { THIS_EVENT; return JS_NewFloat64(ctx, ev->delta_x); }, nullptr },
		{ "deltaY", FN { THIS_EVENT; return JS_NewFloat64(ctx, ev->delta_y); }, nullptr },
		{ "deltaZ", FN { THIS_EVENT; return JS_NewFloat64(ctx, ev->delta_z); }, nullptr },
		{ "deltaMode", FN { THIS_EVENT; return JS_NewInt32(ctx, ev->delta_mode); }, nullptr },
		{ "inputType", FN { THIS_EVENT; return str(ctx, ev->input_type); }, nullptr },
		{ "data", FN { THIS_EVENT; return ev->data.empty() ? JS_NULL : str(ctx, ev->data); }, nullptr },
		{ "returnValue",
			FN { THIS_EVENT; return JS_NewBool(ctx, !ev->default_prevented); },
			FN {
				THIS_EVENT;
				if (!JS_ToBool(ctx, argv[0]) && ev->cancelable)
					ev->default_prevented = true;
				return JS_UNDEFINED;
			} },
		{ "cancelBubble",
			FN { THIS_EVENT; return JS_NewBool(ctx, ev->stop); },
			FN {
				THIS_EVENT;
				if (JS_ToBool(ctx, argv[0]))
					ev->stop = true;
				return JS_UNDEFINED;
			} },
	});
	define(dom, event, {
		{ "preventDefault", 0, FN {
			THIS_EVENT;
			if (ev->cancelable && !ev->passive)
				ev->default_prevented = true;
			return JS_UNDEFINED;
		} },
		{ "stopPropagation", 0, FN { THIS_EVENT; ev->stop = true; return JS_UNDEFINED; } },
		{ "stopImmediatePropagation", 0, FN {
			THIS_EVENT;
			ev->stop = ev->stop_now = true;
			return JS_UNDEFINED;
		} },
		{ "getModifierState", 1, FN {
			THIS_EVENT;
			const std::string k = to_string(ctx, argv[0]);
			return JS_NewBool(ctx, (k == "Control" && ev->ctrl) || (k == "Shift" && ev->shift) || (k == "Alt" && ev->alt)
				|| (k == "Meta" && ev->meta));
		} },
		{ "composedPath", 0, FN {
			THIS_EVENT;
			JSValue a = JS_NewArray(ctx);
			uint32_t i = 0;
			for (Node *n = dom.node_of(ev->target); n; n = n->parent())
				JS_SetPropertyUint32(ctx, a, i++, JS_DupValue(ctx, n->wrapper()));
			if (i)
				JS_SetPropertyUint32(ctx, a, i, JS_DupValue(ctx, dom.global()));
			return a;
		} },
	});
	const std::pair<const char *, int> phases[] = { { "NONE", 0 }, { "CAPTURING_PHASE", 1 }, { "AT_TARGET", 2 },
		{ "BUBBLING_PHASE", 3 } };
	JSValue event_ctor = constructor(dom, "Event", 2, construct_event, event);
	for (const auto &[k, v] : phases)
	{
		JS_DefinePropertyValueStr(ctx, event_ctor, k, JS_NewInt32(ctx, v), 0);
		JS_DefinePropertyValueStr(ctx, event, k, JS_NewInt32(ctx, v), 0);
	}
	JS_SetClassProto(ctx, c.event, event);
	set_global(dom, "Event", event_ctor);
	for (const char *kind : { "UIEvent", "MouseEvent", "PointerEvent", "WheelEvent", "KeyboardEvent", "FocusEvent",
		"InputEvent", "CustomEvent", "DragEvent" })
		set_global(dom, kind, JS_DupValue(ctx, event_ctor));

	// ---------------------------------------------------------------- the window's measures and listeners

	define(dom, global, {
		{ "innerWidth", FN { return JS_NewFloat64(ctx, dom.width()); }, nullptr },
		{ "innerHeight", FN { return JS_NewFloat64(ctx, dom.height()); }, nullptr },
		{ "outerWidth", FN { return JS_NewFloat64(ctx, dom.width()); }, nullptr },
		{ "outerHeight", FN { return JS_NewFloat64(ctx, dom.height()); }, nullptr },
		{ "devicePixelRatio", FN { return JS_NewFloat64(ctx, dom.scale()); }, nullptr },
		{ "scrollX", FN { return JS_NewInt32(ctx, 0); }, nullptr },
		{ "scrollY", FN { return JS_NewInt32(ctx, 0); }, nullptr },
		{ "pageXOffset", FN { return JS_NewInt32(ctx, 0); }, nullptr },
		{ "pageYOffset", FN { return JS_NewInt32(ctx, 0); }, nullptr },
	});
	for (const char *self_name : { "window", "self", "top", "parent", "frames" })
		JS_SetPropertyStr(ctx, global, self_name, JS_DupValue(ctx, global));
	define(dom, global, {
		{ "addEventListener", 3, FN { return add_listener(dom, ctx, dom.window_listeners(), argc, argv); } },
		{ "removeEventListener", 3, FN { return remove_listener(dom, ctx, dom.window_listeners(), argc, argv); } },
		{ "dispatchEvent", 1, FN {
			if (!dom.event_of(argv[0]))
				return JS_ThrowTypeError(ctx, "dispatchEvent: the argument is not an Event");
			return JS_NewBool(ctx, dom.dispatch(nullptr, argv[0]));
		} },
		{ "requestAnimationFrame", 1, FN {
			if (!JS_IsFunction(ctx, argv[0]))
				return JS_ThrowTypeError(ctx, "requestAnimationFrame: the argument is not a function");
			return JS_NewInt64(ctx, dom.request_frame(argv[0]));
		} },
		{ "cancelAnimationFrame", 1, FN {
			dom.cancel_frame(uint32_t(to_number(ctx, argv[0])));
			return JS_UNDEFINED;
		} },
		{ "getComputedStyle", 2, FN {
			Element *el = dom.element_of(argv[0]);
			if (!el)
				return JS_ThrowTypeError(ctx, "getComputedStyle: the argument is not an element");
			dom.update();
			// a snapshot: every property, dashed and camel-cased, and getPropertyValue over them
			JSValue o = JS_NewObject(ctx);
			const ComputedStyle *cs = el->style.get();
			if (argc > 1 && JS_IsString(argv[1]))
			{
				const std::string pseudo = to_string(ctx, argv[1]);
				if (pseudo == "::after" || pseudo == ":after")
					cs = el->after_style.get();
				else if (pseudo == "::before" || pseudo == ":before")
					cs = el->before_style.get();
			}
			for (size_t i = 0; i < PROP_COUNT; i++)
			{
				const Prop p = Prop(i);
				const std::string v = cs ? computed_value(dom, el, *cs, p) : std::string();
				JS_SetPropertyStr(ctx, o, prop_name(p), str(ctx, v));
				const std::string cam = camel(prop_name(p));
				if (cam != prop_name(p))
					JS_SetPropertyStr(ctx, o, cam.c_str(), str(ctx, v));
			}
			JSValue get = JS_Eval(ctx, "(function (k) { return this[k] ?? ''; })", 40, "<quickdom>", JS_EVAL_TYPE_GLOBAL);
			JS_SetPropertyStr(ctx, o, "getPropertyValue", get);
			return o;
		} },
	});

	// ---------------------------------------------------------------- the timers

	if (!(dom.flags() & QDOM_NO_TIMERS))
		define(dom, global, {
		{ "setTimeout", 2, FN {
			if (!JS_IsFunction(ctx, argv[0]))
				return JS_NewInt32(ctx, 0);
			return JS_NewInt64(ctx, dom.set_timer(argv[0], to_number(ctx, argv[1]), false, std::max(0, argc - 2), argv + 2));
		} },
		{ "setInterval", 2, FN {
			if (!JS_IsFunction(ctx, argv[0]))
				return JS_NewInt32(ctx, 0);
			return JS_NewInt64(ctx, dom.set_timer(argv[0], to_number(ctx, argv[1]), true, std::max(0, argc - 2), argv + 2));
		} },
		{ "clearTimeout", 1, FN { dom.clear_timer(uint32_t(to_number(ctx, argv[0]))); return JS_UNDEFINED; } },
		{ "clearInterval", 1, FN { dom.clear_timer(uint32_t(to_number(ctx, argv[0]))); return JS_UNDEFINED; } },
	});

	// ---------------------------------------------------------------- performance

	JSValue performance = JS_NewObject(ctx);
	define(dom, performance, { Method { "now", 0, FN { return JS_NewFloat64(ctx, dom.now()); } } });
	set_global(dom, "performance", performance);

	// ---------------------------------------------------------------- ResizeObserver

	JSValue observer = JS_NewObject(ctx);
	define(dom, observer, {
		{ "observe", 2, FN {
			ResizeObserver *o = observer_of(dom, self);
			Element *el = dom.element_of(argv[0]);
			if (!o || !el)
				return JS_ThrowTypeError(ctx, "ResizeObserver.observe: not an element");
			observer_targets(dom, ctx, o, el, true);
			return JS_UNDEFINED;
		} },
		{ "unobserve", 1, FN {
			ResizeObserver *o = observer_of(dom, self);
			Element *el = dom.element_of(argv[0]);
			if (o && el)
				observer_targets(dom, ctx, o, el, false);
			return JS_UNDEFINED;
		} },
		{ "disconnect", 0, FN {
			ResizeObserver *o = observer_of(dom, self);
			if (o)
				while (!o->targets.empty())
					observer_targets(dom, ctx, o, o->targets.back().el, false);
			return JS_UNDEFINED;
		} },
	});
	JSValue observer_ctor = constructor(dom, "ResizeObserver", 1, FN {
		if (!JS_IsFunction(ctx, argv[0]))
			return JS_ThrowTypeError(ctx, "ResizeObserver: the argument is not a function");
		JSValue obj = JS_NewObjectClass(ctx, int(dom.classes().observer));
		auto *o = new ResizeObserver();
		o->dom = &dom;
		o->self = obj;
		o->callback = JS_DupValue(ctx, argv[0]);
		JS_SetOpaque(obj, o);
		dom.observers.push_back(o);
		return obj;
	}, observer);
	JS_SetClassProto(ctx, c.observer, observer);
	set_global(dom, "ResizeObserver", observer_ctor);

	// ---------------------------------------------------------------- the library's own

	JSValue own = JS_NewObject(ctx);
	define(dom, own, {
		{ "boxes", 0, FN { return str(ctx, dom.boxes_json()); } },
		{ "update", 0, FN { dom.update(); return JS_UNDEFINED; } },
		// what the work took (qdom_stats), for the benchmarks
		{ "stats", 0, FN {
			const Dom::Stats &st = dom.stats;
			JSValue o = JS_NewObject(ctx);
			for (auto [k, v] : { std::pair<const char *, double> { "frames", double(st.frames) },
					{ "restyles", double(st.restyles) }, { "styles", double(st.styles) }, { "layouts", double(st.layouts) },
					{ "boxes", double(st.boxes) }, { "paints", double(st.paints) }, { "frameMs", st.frame_ms },
					{ "styleMs", st.style_ms }, { "layoutMs", st.layout_ms }, { "paintMs", st.paint_ms },
					{ "frameMax", st.frame_max }, { "styleMax", st.style_max }, { "layoutMax", st.layout_max },
					{ "paintMax", st.paint_max } })
				JS_SetPropertyStr(ctx, o, k, JS_NewFloat64(ctx, v));
			return o;
		} },
		{ "resetStats", 0, FN { dom.stats = {}; return JS_UNDEFINED; } },
		// the boxes the layout keeps (the pool's in use), for the tests
		{ "layoutBoxes", 0, FN { return JS_NewInt64(ctx, int64_t(dom.layout_tree()->boxes_in_use())); } },
		{ "log", 1, FN { dom.log(to_string(ctx, argv[0])); return JS_UNDEFINED; } },
		{ "stylesheet", 1, FN { dom.add_sheet(to_string(ctx, argv[0])); return JS_UNDEFINED; } },
		{ "liveNodes", 0, FN { return JS_NewInt64(ctx, Node::live()); } },
		{ "retained", 0, FN { const std::string j = dom.retained_json(); return JS_ParseJSON(ctx, j.c_str(), j.size(), "<retained>"); } },
		{ "paint", 0, FN {
			// the frame's commands as [name, numbers..., text] for the tests
			static const char *const names[] = { "", "SAVE", "RESTORE", "CLIP", "ALPHA", "RECT", "STROKE_RECT", "TEXT",
				"BEGIN", "MOVE", "LINE", "BEZIER", "CLOSE", "FILL", "STROKE", "IMAGE", "SHADOW", "GRADIENT", "TRANSFORM",
				"CLEAR" };
			Commands out;
			paint(dom, out, PaintOptions());
			JSValue list = JS_NewArray(ctx);
			uint32_t k = 0;
			for (size_t i = 0; i < out.data.size();)
			{
				const int o = int(out.data[i]);
				const int len = op_length(o);
				if (len < 0)
					break;
				JSValue item = JS_NewArray(ctx);
				JS_SetPropertyUint32(ctx, item, 0, JS_NewString(ctx, names[o]));
				for (int j = 0; j < len; j++)
				{
					const double v = out.data[i + 1 + size_t(j)];
					if (o == op::TEXT && j == 9)
						JS_SetPropertyUint32(ctx, item, uint32_t(j + 1), str(ctx, out.texts[size_t(v)]));
					else
						JS_SetPropertyUint32(ctx, item, uint32_t(j + 1), JS_NewFloat64(ctx, std::round(v * 100) / 100));
				}
				JS_SetPropertyUint32(ctx, list, k++, item);
				i += 1 + size_t(len);
			}
			return list;
		} },
		{ "gc", 0, FN { JS_RunGC(dom.rt()); return JS_UNDEFINED; } },
		// the window's input, as a host hands it over (the tests' scripted sequences)
		{ "pointer", 5, FN {
			const std::string k = to_string(ctx, argv[0]);
			const PointerKind kind = k == "down" ? PointerKind::Down : k == "up" ? PointerKind::Up : PointerKind::Move;
			dom.input().pointer(kind, to_number(ctx, argv[1]), to_number(ctx, argv[2]), int(to_number(ctx, argv[3])),
				int(to_number(ctx, argv[4])));
			return JS_UNDEFINED;
		} },
		{ "wheel", 5, FN {
			dom.input().wheel(to_number(ctx, argv[0]), to_number(ctx, argv[1]), to_number(ctx, argv[2]),
				to_number(ctx, argv[3]), int(to_number(ctx, argv[4])));
			return JS_UNDEFINED;
		} },
		{ "key", 5, FN {
			return JS_NewBool(ctx, dom.input().key(JS_ToBool(ctx, argv[0]), to_string(ctx, argv[1]),
				JS_IsUndefined(argv[2]) ? std::string() : to_string(ctx, argv[2]), JS_ToBool(ctx, argv[3]),
				int(to_number(ctx, argv[4]))));
		} },
		{ "type", 1, FN { dom.input().text(to_string(ctx, argv[0])); return JS_UNDEFINED; } },
		{ "paste", 1, FN { dom.input().paste(to_string(ctx, argv[0])); return JS_UNDEFINED; } },
		// navigator.clipboard (the prelude): the host's clipboard
		{ "clipboardWrite", 1, FN { dom.host_copy(to_string(ctx, argv[0])); return JS_UNDEFINED; } },
		{ "clipboardRead", 0, FN { return JS_NewBool(ctx, dom.input().read_clipboard()); } },
		{ "leave", 0, FN { dom.input().leave(); return JS_UNDEFINED; } },
		{ "windowBlur", 0, FN { dom.input().window_blur(); return JS_UNDEFINED; } },
		{ "windowFocus", 0, FN { dom.input().window_focus(); return JS_UNDEFINED; } },
		{ "cursor", 0, FN {
			dom.update();
			dom.input().refresh_cursor();
			return str(ctx, dom.input().cursor());
		} },
	});
	set_global(dom, "__qdom", own);
}

} // namespace qdom::js
