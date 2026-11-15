// Copyright 2026 Ian Karlsson
#include "js/bindings.h"

#include <algorithm>
#include <cstring>

#include "core/embedded.h"
#include "dom/observer.h"
#include "layout/layout.h"
#include "paint/canvas.h"

namespace qdom
{

namespace
{

JSValue trampoline(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValueConst *data)
{
	JSClassID id;
	Dom *dom = static_cast<Dom *>(JS_GetAnyOpaque(data[0], &id));
	if (!dom)
		return JS_ThrowInternalError(ctx, "quickdom: the document is gone");
	return dom->binding(magic)(*dom, ctx, this_val, argc, argv);
}

// ---------------------------------------------------------------- finalizers and marks

void node_finalizer(JSRuntime *rt, JSValueConst val)
{
	JSClassID id;
	Node *n = static_cast<Node *>(JS_GetAnyOpaque(val, &id));
	if (!n)
		return;
	n->release(rt);
	delete n;
}

void node_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
	JSClassID id;
	if (const Node *n = static_cast<const Node *>(JS_GetAnyOpaque(val, &id)))
		n->mark(rt, mark);
}

void event_finalizer(JSRuntime *rt, JSValueConst val)
{
	JSClassID id;
	Event *e = static_cast<Event *>(JS_GetAnyOpaque(val, &id));
	if (!e)
		return;
	e->release(rt);
	delete e;
}

void event_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
	JSClassID id;
	if (const Event *e = static_cast<const Event *>(JS_GetAnyOpaque(val, &id)))
		e->mark(rt, mark);
}


void holder_finalizer(JSRuntime *rt, JSValueConst val)
{
	JSClassID id;
	Holder *h = static_cast<Holder *>(JS_GetAnyOpaque(val, &id));
	if (!h)
		return;
	JS_FreeValueRT(rt, h->value);
	delete h;
}

void holder_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
	JSClassID id;
	if (const Holder *h = static_cast<const Holder *>(JS_GetAnyOpaque(val, &id)))
		JS_MarkValue(rt, h->value, mark);
}

void observer_finalizer(JSRuntime *rt, JSValueConst val)
{
	JSClassID id;
	ResizeObserver *o = static_cast<ResizeObserver *>(JS_GetAnyOpaque(val, &id));
	if (!o)
		return;
	if (o->dom)
	{
		auto &list = o->dom->observers;
		list.erase(std::remove(list.begin(), list.end(), o), list.end());
	}
	o->release(rt);
	delete o;
}

void observer_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
	JSClassID id;
	if (const ResizeObserver *o = static_cast<const ResizeObserver *>(JS_GetAnyOpaque(val, &id)))
		o->mark(rt, mark);
}

void context_finalizer(JSRuntime *rt, JSValueConst val)
{
	holder_finalizer(rt, val);
}

//! An array index from a property's atom ("0", "12"), or -1.
int64_t index_of(JSContext *ctx, JSAtom prop)
{
	size_t len;
	const char *s = JS_AtomToCStringLen(ctx, &len, prop);
	int64_t out = -1;
	if (s && len && len < 10 && (s[0] != '0' || len == 1))
	{
		out = 0;
		for (size_t i = 0; i < len && out >= 0; i++)
			out = s[i] >= '0' && s[i] <= '9' ? out * 10 + (s[i] - '0') : -1;
	}
	JS_FreeCString(ctx, s);
	return out;
}

//! A live list's members: a node's children, or its element children.
std::vector<Node *> members(const Holder &h)
{
	std::vector<Node *> out;
	for (const Link &l : h.node->links())
		if (h.kind == 0 || l.node->is_element())
			out.push_back(l.node);
	return out;
}

Node *member(const Holder &h, int64_t i)
{
	if (i < 0)
		return nullptr;
	if (h.kind == 0)
		return size_t(i) < h.node->child_count() ? h.node->child(size_t(i)) : nullptr;
	for (const Link &l : h.node->links())
		if (l.node->is_element() && i-- == 0)
			return l.node;
	return nullptr;
}

int list_get_own_property(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
	JSClassID id;
	const Holder *h = static_cast<const Holder *>(JS_GetAnyOpaque(obj, &id));
	const int64_t i = h ? index_of(ctx, prop) : -1;
	Node *n = h ? member(*h, i) : nullptr;
	if (!n)
		return 0;
	if (desc)
	{
		desc->flags = JS_PROP_ENUMERABLE;
		desc->value = JS_DupValue(ctx, n->wrapper());
		desc->getter = JS_UNDEFINED;
		desc->setter = JS_UNDEFINED;
	}
	return 1;
}

int list_get_own_property_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
	JSClassID id;
	const Holder *h = static_cast<const Holder *>(JS_GetAnyOpaque(obj, &id));
	const size_t n = h ? members(*h).size() : 0;
	auto *tab = static_cast<JSPropertyEnum *>(js_malloc(ctx, sizeof(JSPropertyEnum) * (n ? n : 1)));
	if (!tab)
		return -1;
	for (size_t i = 0; i < n; i++)
	{
		tab[i].is_enumerable = true;
		tab[i].atom = JS_NewAtomUInt32(ctx, uint32_t(i));
	}
	*ptab = tab;
	*plen = uint32_t(n);
	return 0;
}

int tokens_get_own_property(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
	JSClassID id;
	const Holder *h = static_cast<const Holder *>(JS_GetAnyOpaque(obj, &id));
	const int64_t i = h ? index_of(ctx, prop) : -1;
	if (!h || i < 0)
		return 0;
	const Element *el = h->node->as_element();
	if (size_t(i) >= el->classes().size())
		return 0;
	if (desc)
	{
		Dom *dom = el->document()->dom();
		desc->flags = JS_PROP_ENUMERABLE;
		desc->value = dom ? js::str(ctx, dom->names.str(el->classes()[size_t(i)])) : JS_UNDEFINED;
		desc->getter = JS_UNDEFINED;
		desc->setter = JS_UNDEFINED;
	}
	return 1;
}

JSClassExoticMethods LIST_EXOTIC = { list_get_own_property, list_get_own_property_names, nullptr, nullptr, nullptr,
	nullptr, nullptr };
JSClassExoticMethods TOKENS_EXOTIC = { tokens_get_own_property, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

} // namespace

namespace js
{

std::string to_string(JSContext *ctx, JSValueConst v)
{
	size_t len;
	const char *s = JS_ToCStringLen(ctx, &len, v);
	if (!s)
	{
		JS_FreeValue(ctx, JS_GetException(ctx));
		return {};
	}
	std::string out(s, len);
	JS_FreeCString(ctx, s);
	return out;
}

std::optional<std::string> to_optional_string(JSContext *ctx, JSValueConst v)
{
	if (JS_IsUndefined(v) || JS_IsNull(v))
		return std::nullopt;
	return to_string(ctx, v);
}

double to_number(JSContext *ctx, JSValueConst v, double fallback)
{
	if (JS_IsUndefined(v))
		return fallback;
	double d;
	if (JS_ToFloat64(ctx, &d, v))
	{
		JS_FreeValue(ctx, JS_GetException(ctx));
		return fallback;
	}
	return d;
}

JSValue str(JSContext *ctx, std::string_view s)
{
	return JS_NewStringLen(ctx, s.data(), s.size());
}

JSValue node_value(JSContext *ctx, const Node *n)
{
	return n ? JS_DupValue(ctx, n->wrapper()) : JS_NULL;
}

JSValue throw_dom(JSContext *ctx, const char *message)
{
	return JS_ThrowTypeError(ctx, "%s", message);
}

JSValue illegal(JSContext *ctx)
{
	return JS_ThrowTypeError(ctx, "Illegal invocation");
}

void define(Dom &dom, JSValueConst obj, std::initializer_list<Method> methods)
{
	JSContext *ctx = dom.ctx();
	for (const Method &m : methods)
		JS_DefinePropertyValueStr(ctx, obj, m.name, dom.make_function(m.name, m.length, m.fn),
			JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
}

void define(Dom &dom, JSValueConst obj, std::initializer_list<Accessor> accessors)
{
	JSContext *ctx = dom.ctx();
	for (const Accessor &a : accessors)
	{
		JSAtom atom = JS_NewAtom(ctx, a.name);
		JSValue get = dom.make_function(a.name, 0, a.get);
		JSValue set = a.set ? dom.make_function(a.name, 1, a.set) : JS_UNDEFINED;
		JS_DefinePropertyGetSet(ctx, obj, atom, get, set, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
		JS_FreeAtom(ctx, atom);
	}
}

JSValue constructor(Dom &dom, const char *name, int length, BindingFn fn, JSValueConst proto)
{
	JSContext *ctx = dom.ctx();
	JSValue f = dom.make_function(name, length, fn);
	JS_SetConstructorBit(ctx, f, true);
	JS_SetConstructor(ctx, f, proto);
	return f;
}

void set_global(Dom &dom, const char *name, JSValue value)
{
	JS_SetPropertyStr(dom.ctx(), dom.global(), name, value);
}

} // namespace js

JSValue Dom::make_function(const char *name, int length, BindingFn fn)
{
	const int k = bind(fn);
	JSValue f = JS_NewCFunctionData(m_ctx, trampoline, length, k, 1, &m_ref);
	JS_DefinePropertyValueStr(m_ctx, f, "name", JS_NewString(m_ctx, name), JS_PROP_CONFIGURABLE);
	return f;
}

void install(Dom &dom)
{
	JSRuntime *rt = dom.rt();
	JSContext *ctx = dom.ctx();
	Classes &c = dom.m_cls;
	auto make_class = [&](JSClassID &id, const char *name, JSClassFinalizer *fin, JSClassGCMark *mark,
		JSClassExoticMethods *exotic = nullptr) {
		JS_NewClassID(rt, &id);
		JSClassDef def {};
		def.class_name = name;
		def.finalizer = fin;
		def.gc_mark = mark;
		def.exotic = exotic;
		JS_NewClass(rt, id, &def);
	};
	make_class(c.ref, "QuickDOM", nullptr, nullptr);
	make_class(c.element, "Element", node_finalizer, node_mark);
	make_class(c.text, "Text", node_finalizer, node_mark);
	make_class(c.document, "Document", node_finalizer, node_mark);
	make_class(c.event, "Event", event_finalizer, event_mark);
	make_class(c.style, "CSSStyleDeclaration", holder_finalizer, holder_mark);
	make_class(c.token_list, "DOMTokenList", holder_finalizer, holder_mark, &TOKENS_EXOTIC);
	make_class(c.node_list, "NodeList", holder_finalizer, holder_mark, &LIST_EXOTIC);
	make_class(c.computed, "CSSStyleDeclaration", holder_finalizer, holder_mark);
	make_class(c.observer, "ResizeObserver", observer_finalizer, observer_mark);
	make_class(c.context2d, "CanvasRenderingContext2D", context_finalizer, holder_mark);

	dom.m_ref = JS_NewObjectClass(ctx, int(c.ref));
	JS_SetOpaque(dom.m_ref, &dom);
	dom.m_global = JS_GetGlobalObject(ctx);

	js::install_node(dom);
	js::install_window(dom);
	js::install_canvas(dom);

	dom.create_document();
	js::set_global(dom, "document", JS_DupValue(ctx, dom.document()->wrapper()));

	// the parts written in JavaScript
	for (size_t i = 0; i < qdom_prelude_count; i++)
	{
		JSValue r = JS_Eval(ctx, reinterpret_cast<const char *>(qdom_prelude[i].data), qdom_prelude[i].size,
			qdom_prelude[i].name, JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(r))
			dom.report_exception();
		JS_FreeValue(ctx, r);
	}
}

} // namespace qdom
