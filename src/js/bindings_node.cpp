// Copyright 2026 Ian Karlsson
// The nodes' JavaScript classes: Node, Element, Text and Document, their child lists, class lists and styles.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "css/selectors.h"
#include "dom/observer.h"
#include "input/field.h"
#include "input/input.h"
#include "js/bindings.h"
#include "layout/layout.h"
#include "paint/canvas.h"
#include "paint/paint.h"

namespace qdom::js
{

namespace
{

#define FN [](Dom & dom, JSContext * ctx, JSValueConst self, int argc, JSValueConst *argv) -> JSValue
#define THIS_NODE \
	Node *n = dom.node_of(self); \
	if (!n) \
		return illegal(ctx)
#define THIS_ELEMENT \
	Element *el = dom.element_of(self); \
	if (!el) \
		return illegal(ctx)

const char *const HTML_NS = "http://www.w3.org/1999/xhtml";
const char *const SVG_NS = "http://www.w3.org/2000/svg";

std::string upper(std::string_view s)
{
	std::string out(s);
	for (char &c : out)
		if (c >= 'a' && c <= 'z')
			c = char(c - 'a' + 'A');
	return out;
}

//! An attribute's name as an element keeps it: lower case for an HTML element.
Name attribute_name(Dom &dom, const Element *el, JSContext *ctx, JSValueConst v)
{
	const std::string s = to_string(ctx, v);
	return dom.names.get(el->ns() == Namespace::Html ? lower(s) : s);
}

//! A parsed selector list, kept by its text; null (and an exception) when it is not one.
const std::vector<Selector> *selectors(Dom &dom, JSContext *ctx, JSValueConst v)
{
	auto &mine = dom.selector_cache;
	const std::string text = to_string(ctx, v);
	auto it = mine.find(text);
	if (it != mine.end())
		return &it->second;
	std::optional<std::vector<Selector>> list = parse_selectors(text, dom.names);
	if (!list)
	{
		JS_ThrowSyntaxError(ctx, "'%s' is not a valid selector", text.c_str());
		return nullptr;
	}
	if (mine.size() > 1000)
		mine.clear();
	return &mine.emplace(text, std::move(*list)).first->second;
}

void collect(const Node *root, const std::vector<Selector> &list, const Element *scope, bool first,
	std::vector<Element *> &out)
{
	for (const Link &l : root->links())
	{
		if (Element *e = l.node->as_element())
		{
			if (matches_any(list, *e, scope))
			{
				out.push_back(e);
				if (first)
					return;
			}
			collect(e, list, scope, first, out);
			if (first && !out.empty())
				return;
		}
	}
}

JSValue array_of(JSContext *ctx, const std::vector<Element *> &els)
{
	JSValue a = JS_NewArray(ctx);
	for (size_t i = 0; i < els.size(); i++)
		JS_SetPropertyUint32(ctx, a, uint32_t(i), JS_DupValue(ctx, els[i]->wrapper()));
	return a;
}

//! The nodes of append() and its kin: nodes as they are, strings as new texts (the caller frees the values).
bool nodes_of(Dom &dom, JSContext *ctx, int argc, JSValueConst *argv, std::vector<JSValue> &out)
{
	for (int i = 0; i < argc; i++)
	{
		if (dom.node_of(argv[i]))
			out.push_back(JS_DupValue(ctx, argv[i]));
		else
			out.push_back(dom.new_text(to_string(ctx, argv[i])));
	}
	return true;
}

JSValue insert_all(Dom &dom, JSContext *ctx, Node *parent, std::vector<JSValue> &nodes, Node *before)
{
	const char *error = nullptr;
	for (JSValue v : nodes)
		if (!error && !dom.insert(parent, dom.node_of(v), before, &error))
			break;
	for (JSValue v : nodes)
		JS_FreeValue(ctx, v);
	return error ? throw_dom(ctx, error) : JS_UNDEFINED;
}

JSValue holder(Dom &dom, JSContext *ctx, JSClassID cls, Node *node, int kind = 0)
{
	JSValue obj = JS_NewObjectClass(ctx, int(cls));
	JS_SetOpaque(obj, new Holder { node, JS_DupValue(ctx, node->wrapper()), kind });
	return obj;
}

//! A cached object of an element's, made on first use.
JSValue cached(Dom &dom, JSContext *ctx, JSValue &slot, JSClassID cls, Node *node, int kind = 0)
{
	if (JS_IsUndefined(slot))
		slot = holder(dom, ctx, cls, node, kind);
	return JS_DupValue(ctx, slot);
}

JSValue rect_object(JSContext *ctx, const Rect &r)
{
	JSValue o = JS_NewObject(ctx);
	const std::pair<const char *, double> fields[] = { { "x", r.x }, { "y", r.y }, { "width", r.w }, { "height", r.h },
		{ "top", r.y }, { "right", r.x + r.w }, { "bottom", r.y + r.h }, { "left", r.x } };
	for (const auto &[k, v] : fields)
		JS_SetPropertyStr(ctx, o, k, JS_NewFloat64(ctx, v));
	return o;
}

LayoutNode *box_of(Dom &dom, Element *el)
{
	dom.update();
	return dom.layout_of(el);
}

double whole(double v)
{
	return std::round(v);
}

//! Where an element's border box is from its offset parent's padding box, whatever scrolls between them.
std::pair<double, double> offset_of(Dom &dom, Element *el)
{
	if (!box_of(dom, el))
		return { 0, 0 };
	double x = 0, y = 0;
	for (Element *e = el; e; e = e->parent_element())
	{
		LayoutNode *b = dom.layout_of(e);
		if (!b)
			break;
		x += b->x;
		y += b->y;
		Element *p = e->parent_element();
		LayoutNode *pb = p ? dom.layout_of(p) : nullptr;
		if (!pb || pb->style->position != Position::Static || p->is(n::body))
		{
			if (pb)
			{
				x -= pb->bl;
				y -= pb->bt;
			}
			break;
		}
	}
	return { x, y };
}

bool text_field(const Element *el)
{
	if (el->is(n::textarea))
		return true;
	if (!el->is(n::input))
		return false;
	const std::string *t = el->attribute(n::type);
	const std::string type = t ? lower(*t) : "text";
	return type != "checkbox" && type != "radio" && type != "range" && type != "button" && type != "submit"
		&& type != "color" && type != "file";
}

std::string input_type(const Element *el)
{
	const std::string *t = el->attribute(n::type);
	if (el->is(n::input))
		return t ? lower(*t) : "text";
	if (el->is(n::button))
		return t ? lower(*t) : "submit";
	if (el->is(n::select))
		return el->has_attribute(n::multiple) ? "select-multiple" : "select-one";
	if (el->is(n::textarea))
		return "textarea";
	return t ? *t : "";
}

//! A field's selection set by script (anchor and focus as bytes): the caret shown, kept in view.
void set_selection(Dom &dom, Element *el, size_t anchor, size_t focus);

std::string value_of(Dom &dom, const Element *el)
{
	if (el->is(n::select))
	{
		const int i = dom.selected_index(el);
		const std::vector<Element *> os = dom.options(el);
		if (i < 0 || i >= int(os.size()))
			return "";
		const std::string *v = os[size_t(i)]->attribute(n::value);
		return v ? *v : os[size_t(i)]->text_content();
	}
	if (el->is(n::option))
	{
		const std::string *v = el->attribute(n::value);
		return v ? *v : el->text_content();
	}
	if (is_input_type(*el, "range"))
		return dom.range_value(el);
	if (el->form.dirty_value || el->is(n::textarea))
		return el->is(n::textarea) && !el->form.dirty_value ? el->text_content() : el->form.value;
	const std::string *v = el->attribute(n::value);
	return v ? *v : "";
}

void set_selection(Dom &dom, Element *el, size_t anchor, size_t focus)
{
	const std::string v = value_of(dom, el);
	el->form.sel_start = char_start(v, anchor);
	el->form.sel_end = char_start(v, focus);
	dom.invalidate_paint();
	dom.input().caret_reset();
	dom.input().reveal(el);
}

//! A string attribute reflected as a property.
template <Name N>
JSValue get_reflect(Dom &dom, JSContext *ctx, JSValueConst self, int, JSValueConst *)
{
	THIS_ELEMENT;
	const std::string *v = el->attribute(N);
	return str(ctx, v ? *v : std::string());
}

template <Name N>
JSValue set_reflect(Dom &dom, JSContext *ctx, JSValueConst self, int, JSValueConst *argv)
{
	THIS_ELEMENT;
	dom.set_attribute(el, N, to_string(ctx, argv[0]));
	return JS_UNDEFINED;
}

//! A boolean attribute reflected as a property.
template <Name N>
JSValue get_flag(Dom &dom, JSContext *ctx, JSValueConst self, int, JSValueConst *)
{
	THIS_ELEMENT;
	return JS_NewBool(ctx, el->has_attribute(N));
}

template <Name N>
JSValue set_flag(Dom &dom, JSContext *ctx, JSValueConst self, int, JSValueConst *argv)
{
	THIS_ELEMENT;
	if (JS_ToBool(ctx, argv[0]))
		dom.set_attribute(el, N, "");
	else
		dom.remove_attribute(el, N);
	return JS_UNDEFINED;
}

std::string serialize_node(Dom &dom, const Node *n, bool outer)
{
	if (const Text *t = n->as_text())
	{
		std::string out;
		for (char c : t->data())
			out += c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : std::string(1, c);
		return out;
	}
	std::string out;
	const Element *el = n->as_element();
	if (outer && el)
	{
		out += "<" + std::string(dom.names.str(el->tag()));
		for (const Attribute &a : el->attributes())
		{
			out += " " + std::string(dom.names.str(a.name)) + "=\"";
			for (char c : a.value)
				out += c == '"' ? "&quot;" : c == '&' ? "&amp;" : std::string(1, c);
			out += "\"";
		}
		out += ">";
	}
	for (const Link &l : n->links())
		out += serialize_node(dom, l.node, true);
	if (outer && el && !el->is(n::br) && !el->is(n::input) && !el->is(n::img))
		out += "</" + std::string(dom.names.str(el->tag())) + ">";
	return out;
}

JSValue clone(Dom &dom, JSContext *ctx, Node *n, bool deep)
{
	JSValue out;
	if (const Text *t = n->as_text())
		return dom.new_text(t->data());
	const Element *el = n->as_element();
	if (!el)
		return JS_ThrowTypeError(ctx, "the document cannot be cloned");
	out = dom.new_element(dom.names.str(el->tag()), el->ns());
	Element *c = dom.element_of(out);
	for (const Attribute &a : el->attributes())
		dom.set_attribute(c, a.name, a.value);
	if (el->attributes().empty() || !el->has_attribute(n::style))
		for (const auto &[k, v] : el->inline_style())
			dom.set_inline(c, k, v);
	c->form = el->form;
	if (deep)
		for (const Link &l : n->links())
		{
			JSValue k = clone(dom, ctx, l.node, true);
			dom.insert(c, dom.node_of(k), nullptr);
			JS_FreeValue(ctx, k);
		}
	return out;
}

//! Listener options: a boolean (capture) or { capture, once, passive }.
void listener_options(JSContext *ctx, JSValueConst o, bool &capture, bool &once)
{
	capture = once = false;
	if (JS_IsBool(o))
		capture = JS_ToBool(ctx, o);
	else if (JS_IsObject(o))
	{
		JSValue c = JS_GetPropertyStr(ctx, o, "capture"), k = JS_GetPropertyStr(ctx, o, "once");
		capture = JS_ToBool(ctx, c);
		once = JS_ToBool(ctx, k);
		JS_FreeValue(ctx, c);
		JS_FreeValue(ctx, k);
	}
}

} // namespace

JSValue add_listener(Dom &dom, JSContext *ctx, std::vector<Listener> &list, int argc, JSValueConst *argv)
{
	if (!JS_IsFunction(ctx, argv[1]) && !JS_IsObject(argv[1]))
		return JS_UNDEFINED;
	bool capture, once;
	listener_options(ctx, argv[2], capture, once);
	const Name type = dom.names.get(to_string(ctx, argv[0]));
	for (const Listener &l : list)
		if (l.type == type && l.capture == capture && JS_VALUE_GET_PTR(l.fn) == JS_VALUE_GET_PTR(argv[1]))
			return JS_UNDEFINED;
	list.push_back({ type, JS_DupValue(ctx, argv[1]), capture, once, false });
	return JS_UNDEFINED;
}

JSValue remove_listener(Dom &dom, JSContext *ctx, std::vector<Listener> &list, int argc, JSValueConst *argv)
{
	bool capture, once;
	listener_options(ctx, argv[2], capture, once);
	const Name type = dom.names.get(to_string(ctx, argv[0]));
	for (size_t i = 0; i < list.size(); i++)
		if (list[i].type == type && list[i].capture == capture && JS_VALUE_GET_PTR(list[i].fn) == JS_VALUE_GET_PTR(argv[1]))
		{
			JS_FreeValue(ctx, list[i].fn);
			list.erase(list.begin() + long(i));
			break;
		}
	return JS_UNDEFINED;
}

//! Scrolls every region about an element so that it is in view, the innermost first (block: start, center, end,
//! nearest; inline the same across).
void scroll_into_view(Dom &dom, Element *el, const std::string &block, const std::string &inl)
{
	Rect r = dom.client_rect(el);
	double top = r.y, bottom = r.y + r.h, left = r.x, right = r.x + r.w;
	for (Element *p = el->parent_element(); p; p = p->parent_element())
	{
		LayoutNode *b = dom.layout_of(p);
		if (!b || !b->scrolls)
			continue;
		const Rect q = dom.client_rect(p);
		auto axis = [&](double lo, double hi, double view, double size, double was, const std::string &how) {
			if (how == "center")
				return was + (lo - view) - (size - (hi - lo)) / 2;
			if (how == "end")
				return was + (hi - (view + size));
			if (how == "nearest")
			{
				if (lo < view)
					return was + (lo - view);
				if (hi > view + size)
					return was + std::min(lo - view, hi - (view + size));
				return was;
			}
			return was + (lo - view);
		};
		const double wy = p->scroll_y, wx = p->scroll_x;
		const double y = axis(top, bottom, q.y + b->bt, b->ch, wy, block);
		const double x = axis(left, right, q.x + b->bl, b->cw, wx, inl);
		dom.set_scroll(p, x, y);
		top -= p->scroll_y - wy;
		bottom -= p->scroll_y - wy;
		left -= p->scroll_x - wx;
		right -= p->scroll_x - wx;
	}
}

void install_node(Dom &dom)
{
	JSContext *ctx = dom.ctx();
	const Classes &c = dom.classes();
	JSValue node = JS_NewObject(ctx);

	define(dom, node, {
		{ "nodeType", FN { THIS_NODE; return JS_NewInt32(ctx, int(n->type())); }, nullptr },
		{ "nodeName", FN {
			THIS_NODE;
			if (const Element *e = n->as_element())
				return str(ctx, e->ns() == Namespace::Html ? upper(dom.names.str(e->tag())) : std::string(dom.names.str(e->tag())));
			return str(ctx, n->is_text() ? "#text" : "#document");
		}, nullptr },
		{ "ownerDocument", FN { THIS_NODE; return n->is_document() ? JS_NULL : node_value(ctx, n->document()); }, nullptr },
		{ "parentNode", FN { THIS_NODE; return node_value(ctx, n->parent()); }, nullptr },
		{ "parentElement", FN { THIS_NODE; return node_value(ctx, n->parent_element()); }, nullptr },
		{ "childNodes", FN {
			THIS_NODE;
			if (Element *e = n->as_element())
				return cached(dom, ctx, e->child_nodes, dom.classes().node_list, e, 0);
			return holder(dom, ctx, dom.classes().node_list, n, 0);
		}, nullptr },
		{ "firstChild", FN { THIS_NODE; return node_value(ctx, n->first_child()); }, nullptr },
		{ "lastChild", FN { THIS_NODE; return node_value(ctx, n->last_child()); }, nullptr },
		{ "nextSibling", FN { THIS_NODE; return node_value(ctx, n->next_sibling()); }, nullptr },
		{ "previousSibling", FN { THIS_NODE; return node_value(ctx, n->previous_sibling()); }, nullptr },
		{ "isConnected", FN { THIS_NODE; return JS_NewBool(ctx, n->connected()); }, nullptr },
		{ "textContent",
			FN { THIS_NODE; return n->is_document() ? JS_NULL : str(ctx, n->text_content()); },
			FN {
				THIS_NODE;
				if (!n->is_document())
					dom.set_text_content(n, JS_IsNull(argv[0]) ? std::string() : to_string(ctx, argv[0]));
				return JS_UNDEFINED;
			} },
		{ "nodeValue",
			FN { THIS_NODE; return n->is_text() ? str(ctx, n->as_text()->data()) : JS_NULL; },
			FN {
				THIS_NODE;
				if (Text *t = n->as_text())
					dom.set_data(t, JS_IsNull(argv[0]) ? std::string() : to_string(ctx, argv[0]));
				return JS_UNDEFINED;
			} },
	});
	define(dom, node, {
		{ "appendChild", 1, FN {
			THIS_NODE;
			Node *k = dom.node_of(argv[0]);
			if (!k)
				return JS_ThrowTypeError(ctx, "appendChild: the argument is not a node");
			const char *error = nullptr;
			if (!dom.insert(n, k, nullptr, &error))
				return throw_dom(ctx, error);
			return JS_DupValue(ctx, argv[0]);
		} },
		{ "insertBefore", 2, FN {
			THIS_NODE;
			Node *k = dom.node_of(argv[0]);
			if (!k)
				return JS_ThrowTypeError(ctx, "insertBefore: the argument is not a node");
			Node *before = dom.node_of(argv[1]);
			if (!before && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]))
				return JS_ThrowTypeError(ctx, "insertBefore: the reference is not a node");
			const char *error = nullptr;
			if (!dom.insert(n, k, before, &error))
				return throw_dom(ctx, error);
			return JS_DupValue(ctx, argv[0]);
		} },
		{ "removeChild", 1, FN {
			THIS_NODE;
			Node *k = dom.node_of(argv[0]);
			if (!k || k->parent() != n)
				return throw_dom(ctx, "NotFoundError: the node to be removed is not a child of this node");
			JSValue out = JS_DupValue(ctx, argv[0]);
			dom.remove(k);
			return out;
		} },
		{ "replaceChild", 2, FN {
			THIS_NODE;
			Node *k = dom.node_of(argv[0]), *old = dom.node_of(argv[1]);
			if (!k || !old || old->parent() != n)
				return throw_dom(ctx, "NotFoundError: the node to be replaced is not a child of this node");
			if (k == old)
				return JS_DupValue(ctx, argv[1]);
			const char *error = nullptr;
			if (!dom.insert(n, k, old, &error))
				return throw_dom(ctx, error);
			JSValue out = JS_DupValue(ctx, argv[1]);
			dom.remove(old);
			return out;
		} },
		{ "append", 0, FN {
			THIS_NODE;
			std::vector<JSValue> nodes;
			nodes_of(dom, ctx, argc, argv, nodes);
			return insert_all(dom, ctx, n, nodes, nullptr);
		} },
		{ "prepend", 0, FN {
			THIS_NODE;
			std::vector<JSValue> nodes;
			nodes_of(dom, ctx, argc, argv, nodes);
			return insert_all(dom, ctx, n, nodes, n->first_child());
		} },
		{ "remove", 0, FN {
			THIS_NODE;
			dom.remove(n);
			return JS_UNDEFINED;
		} },
		{ "replaceWith", 0, FN {
			THIS_NODE;
			Node *parent = n->parent();
			if (!parent)
				return JS_UNDEFINED;
			std::vector<JSValue> nodes;
			nodes_of(dom, ctx, argc, argv, nodes);
			auto among = [&](const Node *x) {
				for (JSValue v : nodes)
					if (dom.node_of(v) == x)
						return true;
				return false;
			};
			Node *next = n->next_sibling();
			while (next && among(next))
				next = next->next_sibling();
			JSValue hold = JS_DupValue(ctx, self);
			if (!among(n))
				dom.remove(n);
			JSValue r = insert_all(dom, ctx, parent, nodes, next);
			JS_FreeValue(ctx, hold);
			return r;
		} },
		{ "before", 0, FN {
			THIS_NODE;
			Node *parent = n->parent();
			if (!parent)
				return JS_UNDEFINED;
			std::vector<JSValue> nodes;
			nodes_of(dom, ctx, argc, argv, nodes);
			return insert_all(dom, ctx, parent, nodes, n);
		} },
		{ "after", 0, FN {
			THIS_NODE;
			Node *parent = n->parent();
			if (!parent)
				return JS_UNDEFINED;
			std::vector<JSValue> nodes;
			nodes_of(dom, ctx, argc, argv, nodes);
			return insert_all(dom, ctx, parent, nodes, n->next_sibling());
		} },
		{ "replaceChildren", 0, FN {
			THIS_NODE;
			std::vector<JSValue> nodes;
			nodes_of(dom, ctx, argc, argv, nodes);
			while (n->child_count())
				dom.remove(n->last_child());
			return insert_all(dom, ctx, n, nodes, nullptr);
		} },
		{ "contains", 1, FN {
			THIS_NODE;
			Node *k = dom.node_of(argv[0]);
			return JS_NewBool(ctx, k && n->contains(k));
		} },
		{ "hasChildNodes", 0, FN { THIS_NODE; return JS_NewBool(ctx, n->child_count() > 0); } },
		{ "getRootNode", 0, FN {
			THIS_NODE;
			Node *r = n;
			while (r->parent())
				r = r->parent();
			return node_value(ctx, r);
		} },
		{ "isSameNode", 1, FN { THIS_NODE; return JS_NewBool(ctx, dom.node_of(argv[0]) == n); } },
		{ "cloneNode", 1, FN { THIS_NODE; return clone(dom, ctx, n, JS_ToBool(ctx, argv[0])); } },
		{ "addEventListener", 3, FN { THIS_NODE; return add_listener(dom, ctx, n->listeners(), argc, argv); } },
		{ "removeEventListener", 3, FN { THIS_NODE; return remove_listener(dom, ctx, n->listeners(), argc, argv); } },
		{ "dispatchEvent", 1, FN {
			THIS_NODE;
			if (!dom.event_of(argv[0]))
				return JS_ThrowTypeError(ctx, "dispatchEvent: the argument is not an Event");
			return JS_NewBool(ctx, dom.dispatch(n, argv[0]));
		} },
	});

	// ---------------------------------------------------------------- Element

	JSValue element = JS_NewObjectProto(ctx, node);
	define(dom, element, {
		{ "tagName", FN {
			THIS_ELEMENT;
			const std::string_view t = dom.names.str(el->tag());
			return str(ctx, el->ns() == Namespace::Html ? upper(t) : std::string(t));
		}, nullptr },
		{ "localName", FN { THIS_ELEMENT; return str(ctx, dom.names.str(el->tag())); }, nullptr },
		{ "namespaceURI", FN { THIS_ELEMENT; return JS_NewString(ctx, el->ns() == Namespace::Svg ? SVG_NS : HTML_NS); }, nullptr },
		{ "id", get_reflect<n::id>, set_reflect<n::id> },
		{ "className", get_reflect<n::class_>, set_reflect<n::class_> },
		{ "title", get_reflect<n::title>, set_reflect<n::title> },
		{ "placeholder", get_reflect<n::placeholder>, set_reflect<n::placeholder> },
		{ "min", get_reflect<n::min>, set_reflect<n::min> },
		{ "max", get_reflect<n::max>, set_reflect<n::max> },
		{ "step", get_reflect<n::step>, set_reflect<n::step> },
		// an img's picture, once the host's bytes are decoded
		{ "naturalWidth", FN {
			THIS_ELEMENT;
			const Picture *p = el->is(n::img) ? dom.picture_of(el) : nullptr;
			return JS_NewInt32(ctx, p && p->state == Picture::State::Ready ? p->width : 0);
		}, nullptr },
		{ "naturalHeight", FN {
			THIS_ELEMENT;
			const Picture *p = el->is(n::img) ? dom.picture_of(el) : nullptr;
			return JS_NewInt32(ctx, p && p->state == Picture::State::Ready ? p->height : 0);
		}, nullptr },
		{ "complete", FN {
			THIS_ELEMENT;
			const Picture *p = el->is(n::img) ? dom.picture_of(el) : nullptr;
			return JS_NewBool(ctx, !p || p->settled());
		}, nullptr },
		{ "src", get_reflect<n::src>, set_reflect<n::src> },
		{ "name", get_reflect<n::name>, set_reflect<n::name> },
		{ "disabled", get_flag<n::disabled>, set_flag<n::disabled> },
		{ "hidden", get_flag<n::hidden>, set_flag<n::hidden> },
		{ "open", get_flag<n::open>, set_flag<n::open> },
		{ "multiple", get_flag<n::multiple>, set_flag<n::multiple> },
		{ "readOnly", get_flag<n::readonly>, set_flag<n::readonly> },
		{ "type",
			FN { THIS_ELEMENT; return str(ctx, input_type(el)); },
			FN { THIS_ELEMENT; dom.set_attribute(el, n::type, to_string(ctx, argv[0])); return JS_UNDEFINED; } },
		{ "maxLength",
			FN { THIS_ELEMENT; return JS_NewFloat64(ctx, el->number_attribute(n::maxlength, -1)); },
			FN { THIS_ELEMENT; dom.set_attribute(el, n::maxlength, format_number(to_number(ctx, argv[0]))); return JS_UNDEFINED; } },
		{ "size",
			FN { THIS_ELEMENT; return JS_NewFloat64(ctx, el->number_attribute(n::size, el->is(n::input) ? 20 : 0)); },
			FN { THIS_ELEMENT; dom.set_attribute(el, n::size, format_number(to_number(ctx, argv[0]))); return JS_UNDEFINED; } },
		{ "rows",
			FN { THIS_ELEMENT; return JS_NewFloat64(ctx, el->number_attribute(n::rows, 2)); },
			FN { THIS_ELEMENT; dom.set_attribute(el, n::rows, format_number(to_number(ctx, argv[0]))); return JS_UNDEFINED; } },
		{ "cols",
			FN { THIS_ELEMENT; return JS_NewFloat64(ctx, el->number_attribute(n::cols, 20)); },
			FN { THIS_ELEMENT; dom.set_attribute(el, n::cols, format_number(to_number(ctx, argv[0]))); return JS_UNDEFINED; } },
		{ "spellcheck",
			FN { THIS_ELEMENT; const std::string *v = el->attribute(n::spellcheck); return JS_NewBool(ctx, !v || *v != "false"); },
			FN { THIS_ELEMENT; dom.set_attribute(el, n::spellcheck, JS_ToBool(ctx, argv[0]) ? "true" : "false"); return JS_UNDEFINED; } },
		{ "tabIndex",
			FN {
				THIS_ELEMENT;
				const bool control = el->is(n::input) || el->is(n::select) || el->is(n::button) || el->is(n::textarea);
				return JS_NewFloat64(ctx, el->number_attribute(n::tabindex, control ? 0 : -1));
			},
			FN { THIS_ELEMENT; dom.set_attribute(el, n::tabindex, format_number(std::trunc(to_number(ctx, argv[0])))); return JS_UNDEFINED; } },
		{ "value",
			FN { THIS_ELEMENT; return str(ctx, value_of(dom, el)); },
			FN { THIS_ELEMENT; dom.set_value(el, JS_IsNull(argv[0]) ? std::string() : to_string(ctx, argv[0])); return JS_UNDEFINED; } },
		{ "defaultValue",
			FN { THIS_ELEMENT; const std::string *v = el->attribute(n::value); return str(ctx, v ? *v : std::string()); },
			FN { THIS_ELEMENT; dom.set_attribute(el, n::value, to_string(ctx, argv[0])); return JS_UNDEFINED; } },
		{ "checked",
			FN { THIS_ELEMENT; return JS_NewBool(ctx, el->form.dirty_checked ? el->form.checked : el->has_attribute(n::checked)); },
			FN { THIS_ELEMENT; dom.set_checked(el, JS_ToBool(ctx, argv[0])); return JS_UNDEFINED; } },
		{ "selectedIndex",
			FN { THIS_ELEMENT; return JS_NewInt32(ctx, el->is(n::select) ? dom.selected_index(el) : -1); },
			FN { THIS_ELEMENT; dom.set_selected_index(el, int(to_number(ctx, argv[0], -1))); return JS_UNDEFINED; } },
		{ "options", FN {
			THIS_ELEMENT;
			return array_of(ctx, el->is(n::select) ? dom.options(el) : std::vector<Element *>());
		}, nullptr },
		{ "selected",
			FN {
				THIS_ELEMENT;
				Element *s = el->parent_element();
				if (s && !s->is(n::select))
					s = s->parent_element();
				if (!s || !s->is(n::select))
					return JS_NewBool(ctx, el->has_attribute(n::selected));
				const std::vector<Element *> os = dom.options(s);
				const int i = dom.selected_index(s);
				return JS_NewBool(ctx, i >= 0 && i < int(os.size()) && os[size_t(i)] == el);
			},
			FN {
				THIS_ELEMENT;
				Element *s = el->parent_element();
				if (s && !s->is(n::select))
					s = s->parent_element();
				if (s && s->is(n::select) && JS_ToBool(ctx, argv[0]))
				{
					const std::vector<Element *> os = dom.options(s);
					const auto it = std::find(os.begin(), os.end(), el);
					dom.set_selected_index(s, int(it - os.begin()));
				}
				else if (!s)
				{
					if (JS_ToBool(ctx, argv[0]))
						dom.set_attribute(el, n::selected, "");
					else
						dom.remove_attribute(el, n::selected);
				}
				return JS_UNDEFINED;
			} },
		// offsets in UTF-16 units, as scripts count them
		{ "selectionStart",
			FN {
				THIS_ELEMENT;
				return JS_NewFloat64(ctx, double(utf16_index(value_of(dom, el), std::min(el->form.sel_start, el->form.sel_end))));
			},
			FN {
				THIS_ELEMENT;
				const std::string v = value_of(dom, el);
				const size_t a = byte_index(v, size_t(std::max(0.0, to_number(ctx, argv[0]))));
				set_selection(dom, el, a, std::max(a, std::max(el->form.sel_start, el->form.sel_end)));
				return JS_UNDEFINED;
			} },
		{ "selectionEnd",
			FN {
				THIS_ELEMENT;
				return JS_NewFloat64(ctx, double(utf16_index(value_of(dom, el), std::max(el->form.sel_start, el->form.sel_end))));
			},
			FN {
				THIS_ELEMENT;
				const std::string v = value_of(dom, el);
				const size_t z = byte_index(v, size_t(std::max(0.0, to_number(ctx, argv[0]))));
				set_selection(dom, el, std::min(z, std::min(el->form.sel_start, el->form.sel_end)), z);
				return JS_UNDEFINED;
			} },
		{ "selectionDirection",
			FN { THIS_ELEMENT; return str(ctx, el->form.sel_end < el->form.sel_start ? "backward" : "forward"); },
			nullptr },
		{ "width",
			FN {
				THIS_ELEMENT;
				return JS_NewFloat64(ctx, el->number_attribute(n::width, el->is(n::canvas) ? 300 : 0));
			},
			FN {
				THIS_ELEMENT;
				dom.set_attribute(el, n::width, format_number(std::trunc(to_number(ctx, argv[0]))));
				if (el->canvas)
					el->canvas->commands.clear();
				return JS_UNDEFINED;
			} },
		{ "height",
			FN {
				THIS_ELEMENT;
				return JS_NewFloat64(ctx, el->number_attribute(n::height, el->is(n::canvas) ? 150 : 0));
			},
			FN {
				THIS_ELEMENT;
				dom.set_attribute(el, n::height, format_number(std::trunc(to_number(ctx, argv[0]))));
				if (el->canvas)
					el->canvas->commands.clear();
				return JS_UNDEFINED;
			} },
		{ "classList",
			FN { THIS_ELEMENT; return cached(dom, ctx, el->class_list, dom.classes().token_list, el); },
			FN { THIS_ELEMENT; dom.set_attribute(el, n::class_, to_string(ctx, argv[0])); return JS_UNDEFINED; } },
		{ "style",
			FN { THIS_ELEMENT; return cached(dom, ctx, el->style_object, dom.classes().style, el); },
			FN { THIS_ELEMENT; dom.set_inline_text(el, to_string(ctx, argv[0])); return JS_UNDEFINED; } },
		{ "children", FN { THIS_ELEMENT; return cached(dom, ctx, el->children_list, dom.classes().node_list, el, 1); }, nullptr },
		{ "childElementCount", FN {
			THIS_ELEMENT;
			int k = 0;
			for (const Link &l : el->links())
				k += l.node->is_element();
			return JS_NewInt32(ctx, k);
		}, nullptr },
		{ "firstElementChild", FN { THIS_ELEMENT; return node_value(ctx, el->first_element_child()); }, nullptr },
		{ "lastElementChild", FN { THIS_ELEMENT; return node_value(ctx, el->last_element_child()); }, nullptr },
		{ "nextElementSibling", FN { THIS_ELEMENT; return node_value(ctx, el->next_element_sibling()); }, nullptr },
		{ "previousElementSibling", FN { THIS_ELEMENT; return node_value(ctx, el->previous_element_sibling()); }, nullptr },
		{ "innerHTML",
			FN { THIS_ELEMENT; return str(ctx, serialize_node(dom, el, false)); },
			FN {
				THIS_ELEMENT;
				const std::string s = to_string(ctx, argv[0]);
				if (s.find('<') != std::string::npos)
					return throw_dom(ctx, "NotSupportedError: quickdom does not parse HTML; build elements instead");
				dom.set_text_content(el, s);
				return JS_UNDEFINED;
			} },
		{ "outerHTML", FN { THIS_ELEMENT; return str(ctx, serialize_node(dom, el, true)); }, nullptr },
		{ "innerText",
			FN { THIS_ELEMENT; return str(ctx, el->text_content()); },
			FN { THIS_ELEMENT; dom.set_text_content(el, to_string(ctx, argv[0])); return JS_UNDEFINED; } },
		{ "isContentEditable", FN { return JS_FALSE; }, nullptr },
		{ "offsetWidth", FN { THIS_ELEMENT; LayoutNode *b = box_of(dom, el); return JS_NewFloat64(ctx, b ? whole(b->w) : 0); }, nullptr },
		{ "offsetHeight", FN { THIS_ELEMENT; LayoutNode *b = box_of(dom, el); return JS_NewFloat64(ctx, b ? whole(b->h) : 0); }, nullptr },
		{ "clientWidth", FN { THIS_ELEMENT; LayoutNode *b = box_of(dom, el); return JS_NewFloat64(ctx, b && !b->inline_box ? whole(b->cw) : 0); }, nullptr },
		{ "clientHeight", FN { THIS_ELEMENT; LayoutNode *b = box_of(dom, el); return JS_NewFloat64(ctx, b && !b->inline_box ? whole(b->ch) : 0); }, nullptr },
		{ "clientLeft", FN { THIS_ELEMENT; LayoutNode *b = box_of(dom, el); return JS_NewFloat64(ctx, b ? whole(b->bl) : 0); }, nullptr },
		{ "clientTop", FN { THIS_ELEMENT; LayoutNode *b = box_of(dom, el); return JS_NewFloat64(ctx, b ? whole(b->bt) : 0); }, nullptr },
		{ "scrollWidth", FN { THIS_ELEMENT; LayoutNode *b = box_of(dom, el); return JS_NewFloat64(ctx, b ? whole(std::max(b->cw, b->sw)) : 0); }, nullptr },
		{ "scrollHeight", FN { THIS_ELEMENT; LayoutNode *b = box_of(dom, el); return JS_NewFloat64(ctx, b ? whole(std::max(b->ch, b->sh)) : 0); }, nullptr },
		{ "scrollTop",
			FN { THIS_ELEMENT; return JS_NewFloat64(ctx, el->scroll_y); },
			FN { THIS_ELEMENT; dom.set_scroll(el, el->scroll_x, to_number(ctx, argv[0])); return JS_UNDEFINED; } },
		{ "scrollLeft",
			FN { THIS_ELEMENT; return JS_NewFloat64(ctx, el->scroll_x); },
			FN { THIS_ELEMENT; dom.set_scroll(el, to_number(ctx, argv[0]), el->scroll_y); return JS_UNDEFINED; } },
		{ "offsetParent", FN {
			THIS_ELEMENT;
			LayoutNode *b = box_of(dom, el);
			if (!b || b->style->position == Position::Fixed || el->is(n::body) || el->is(n::html))
				return JS_NULL;
			for (Element *p = el->parent_element(); p; p = p->parent_element())
			{
				LayoutNode *pb = dom.layout_of(p);
				if (!pb)
					return JS_NULL;
				if (pb->style->position != Position::Static || p->is(n::body) || p->is(n::td) || p->is(n::th)
					|| p->is(n::table))
					return node_value(ctx, p);
			}
			return JS_NULL;
		}, nullptr },
		{ "offsetLeft", FN { THIS_ELEMENT; return JS_NewFloat64(ctx, whole(offset_of(dom, el).first)); }, nullptr },
		{ "offsetTop", FN { THIS_ELEMENT; return JS_NewFloat64(ctx, whole(offset_of(dom, el).second)); }, nullptr },
	});
	define(dom, element, {
		{ "getAttribute", 1, FN {
			THIS_ELEMENT;
			const std::string *v = el->attribute(attribute_name(dom, el, ctx, argv[0]));
			if (!v && attribute_name(dom, el, ctx, argv[0]) == n::style && !el->inline_style().empty())
			{
				std::string css;
				for (const auto &[k, val] : el->inline_style())
					css += k + ": " + val + "; ";
				return str(ctx, std::string(trim(css)));
			}
			return v ? str(ctx, *v) : JS_NULL;
		} },
		{ "setAttribute", 2, FN {
			THIS_ELEMENT;
			dom.set_attribute(el, attribute_name(dom, el, ctx, argv[0]), to_string(ctx, argv[1]));
			return JS_UNDEFINED;
		} },
		{ "removeAttribute", 1, FN {
			THIS_ELEMENT;
			dom.remove_attribute(el, attribute_name(dom, el, ctx, argv[0]));
			return JS_UNDEFINED;
		} },
		{ "hasAttribute", 1, FN { THIS_ELEMENT; return JS_NewBool(ctx, el->has_attribute(attribute_name(dom, el, ctx, argv[0]))); } },
		{ "hasAttributes", 0, FN { THIS_ELEMENT; return JS_NewBool(ctx, !el->attributes().empty()); } },
		{ "toggleAttribute", 2, FN {
			THIS_ELEMENT;
			const Name name = attribute_name(dom, el, ctx, argv[0]);
			const bool on = argc > 1 && !JS_IsUndefined(argv[1]) ? JS_ToBool(ctx, argv[1]) : !el->has_attribute(name);
			if (on && !el->has_attribute(name))
				dom.set_attribute(el, name, "");
			else if (!on)
				dom.remove_attribute(el, name);
			return JS_NewBool(ctx, on);
		} },
		{ "getAttributeNames", 0, FN {
			THIS_ELEMENT;
			JSValue a = JS_NewArray(ctx);
			uint32_t i = 0;
			for (const Attribute &x : el->attributes())
				JS_SetPropertyUint32(ctx, a, i++, str(ctx, dom.names.str(x.name)));
			return a;
		} },
		{ "matches", 1, FN {
			THIS_ELEMENT;
			const std::vector<Selector> *list = selectors(dom, ctx, argv[0]);
			if (!list)
				return JS_EXCEPTION;
			return JS_NewBool(ctx, matches_any(*list, *el, el));
		} },
		{ "closest", 1, FN {
			THIS_ELEMENT;
			const std::vector<Selector> *list = selectors(dom, ctx, argv[0]);
			if (!list)
				return JS_EXCEPTION;
			for (Element *e = el; e; e = e->parent_element())
				if (matches_any(*list, *e, el))
					return node_value(ctx, e);
			return JS_NULL;
		} },
		{ "querySelector", 1, FN {
			THIS_ELEMENT;
			const std::vector<Selector> *list = selectors(dom, ctx, argv[0]);
			if (!list)
				return JS_EXCEPTION;
			std::vector<Element *> out;
			collect(el, *list, el, true, out);
			return out.empty() ? JS_NULL : node_value(ctx, out[0]);
		} },
		{ "querySelectorAll", 1, FN {
			THIS_ELEMENT;
			const std::vector<Selector> *list = selectors(dom, ctx, argv[0]);
			if (!list)
				return JS_EXCEPTION;
			std::vector<Element *> out;
			collect(el, *list, el, false, out);
			return array_of(ctx, out);
		} },
		{ "getElementsByTagName", 1, FN {
			THIS_ELEMENT;
			const std::vector<Selector> *list = selectors(dom, ctx, argv[0]);
			if (!list)
				return JS_EXCEPTION;
			std::vector<Element *> out;
			collect(el, *list, el, false, out);
			return array_of(ctx, out);
		} },
		{ "getElementsByClassName", 1, FN {
			THIS_ELEMENT;
			std::string sel;
			const std::string names = to_string(ctx, argv[0]);
			for (std::string_view w : split_words(names))
				sel += "." + std::string(w);
			JSValue s = str(ctx, sel.empty() ? "#__none__" : sel);
			const std::vector<Selector> *list = selectors(dom, ctx, s);
			JS_FreeValue(ctx, s);
			std::vector<Element *> out;
			if (list)
				collect(el, *list, el, false, out);
			return array_of(ctx, out);
		} },
		{ "getBoundingClientRect", 0, FN { THIS_ELEMENT; return rect_object(ctx, dom.client_rect(el)); } },
		{ "getClientRects", 0, FN {
			THIS_ELEMENT;
			JSValue a = JS_NewArray(ctx);
			const Rect r = dom.client_rect(el);
			if (dom.layout_of(el))
				JS_SetPropertyUint32(ctx, a, 0, rect_object(ctx, r));
			return a;
		} },
		{ "scrollIntoView", 1, FN {
			THIS_ELEMENT;
			std::string block = "start", inl = "nearest";
			if (JS_IsObject(argv[0]))
			{
				JSValue b = JS_GetPropertyStr(ctx, argv[0], "block"), i = JS_GetPropertyStr(ctx, argv[0], "inline");
				if (!JS_IsUndefined(b))
					block = to_string(ctx, b);
				if (!JS_IsUndefined(i))
					inl = to_string(ctx, i);
				JS_FreeValue(ctx, b);
				JS_FreeValue(ctx, i);
			}
			else if (JS_IsBool(argv[0]) && !JS_ToBool(ctx, argv[0]))
				block = "end";
			scroll_into_view(dom, el, block, inl);
			return JS_UNDEFINED;
		} },
		{ "scrollTo", 2, FN {
			THIS_ELEMENT;
			double x = el->scroll_x, y = el->scroll_y;
			if (JS_IsObject(argv[0]))
			{
				JSValue l = JS_GetPropertyStr(ctx, argv[0], "left"), t = JS_GetPropertyStr(ctx, argv[0], "top");
				x = to_number(ctx, l, x);
				y = to_number(ctx, t, y);
				JS_FreeValue(ctx, l);
				JS_FreeValue(ctx, t);
			}
			else
			{
				x = to_number(ctx, argv[0], x);
				y = to_number(ctx, argv[1], y);
			}
			dom.set_scroll(el, x, y);
			return JS_UNDEFINED;
		} },
		{ "scrollBy", 2, FN {
			THIS_ELEMENT;
			double dx = 0, dy = 0;
			if (JS_IsObject(argv[0]))
			{
				JSValue l = JS_GetPropertyStr(ctx, argv[0], "left"), t = JS_GetPropertyStr(ctx, argv[0], "top");
				dx = to_number(ctx, l);
				dy = to_number(ctx, t);
				JS_FreeValue(ctx, l);
				JS_FreeValue(ctx, t);
			}
			else
			{
				dx = to_number(ctx, argv[0]);
				dy = to_number(ctx, argv[1]);
			}
			dom.set_scroll(el, el->scroll_x + dx, el->scroll_y + dy);
			return JS_UNDEFINED;
		} },
		{ "focus", 1, FN {
			THIS_ELEMENT;
			if (el->focusable() && el->connected())
				dom.set_focus(el);
			return JS_UNDEFINED;
		} },
		{ "blur", 0, FN {
			THIS_ELEMENT;
			if (dom.document()->active() == el)
				dom.set_focus(nullptr);
			return JS_UNDEFINED;
		} },
		{ "click", 0, FN {
			THIS_ELEMENT;
			if (el->disabled() && (el->is(n::button) || el->is(n::input) || el->is(n::select) || el->is(n::textarea)))
				return JS_UNDEFINED;
			dom.input().click(el, 0, false);
			return JS_UNDEFINED;
		} },
		{ "select", 0, FN {
			THIS_ELEMENT;
			if (text_field(el))
				set_selection(dom, el, 0, value_of(dom, el).size());
			return JS_UNDEFINED;
		} },
		{ "setSelectionRange", 3, FN {
			THIS_ELEMENT;
			const std::string v = value_of(dom, el);
			size_t a = byte_index(v, size_t(std::max(0.0, to_number(ctx, argv[0]))));
			size_t z = byte_index(v, size_t(std::max(0.0, to_number(ctx, argv[1]))));
			a = std::min(a, z);
			const bool back = argc > 2 && to_string(ctx, argv[2]) == "backward";
			set_selection(dom, el, back ? z : a, back ? a : z);
			return JS_UNDEFINED;
		} },
		// the pointer is taken only while a button is held, as browsers take it
		{ "setPointerCapture", 1, FN {
			THIS_ELEMENT;
			if (dom.input().pressed() && el->connected())
				dom.set_capture(el);
			return JS_UNDEFINED;
		} },
		{ "releasePointerCapture", 1, FN {
			THIS_ELEMENT;
			if (dom.document()->capture() == el)
				dom.set_capture(nullptr);
			return JS_UNDEFINED;
		} },
		{ "hasPointerCapture", 1, FN { THIS_ELEMENT; return JS_NewBool(ctx, dom.document()->capture() == el); } },
		{ "getContext", 1, FN { THIS_ELEMENT; return get_context(dom, ctx, el, to_string(ctx, argv[0])); } },
		{ "showModal", 0, FN {
			THIS_ELEMENT;
			dom.set_attribute(el, n::open, "");
			dom.set_modal(el, true);
			return JS_UNDEFINED;
		} },
		{ "show", 0, FN { THIS_ELEMENT; dom.set_attribute(el, n::open, ""); return JS_UNDEFINED; } },
		{ "close", 1, FN { THIS_ELEMENT; dom.close_dialog(el); return JS_UNDEFINED; } },
	});
	JS_SetClassProto(ctx, c.element, JS_DupValue(ctx, element));

	// ---------------------------------------------------------------- Text

	JSValue text = JS_NewObjectProto(ctx, node);
	define(dom, text, {
		{ "data",
			FN { THIS_NODE; return n->is_text() ? str(ctx, n->as_text()->data()) : JS_UNDEFINED; },
			FN {
				THIS_NODE;
				if (Text *t = n->as_text())
					dom.set_data(t, to_string(ctx, argv[0]));
				return JS_UNDEFINED;
			} },
		{ "length", FN { THIS_NODE; return JS_NewInt32(ctx, n->is_text() ? int(n->as_text()->data().size()) : 0); }, nullptr },
		{ "wholeText", FN { THIS_NODE; return n->is_text() ? str(ctx, n->as_text()->data()) : JS_UNDEFINED; }, nullptr },
	});
	JS_SetClassProto(ctx, c.text, JS_DupValue(ctx, text));

	// ---------------------------------------------------------------- Document

	JSValue document = JS_NewObjectProto(ctx, node);
	define(dom, document, {
		{ "documentElement", FN { return node_value(ctx, dom.document()->document_element()); }, nullptr },
		{ "head", FN { return node_value(ctx, dom.document()->head()); }, nullptr },
		{ "body", FN { return node_value(ctx, dom.document()->body()); }, nullptr },
		{ "activeElement", FN { return node_value(ctx, dom.document()->active()); }, nullptr },
		{ "defaultView", FN { return JS_DupValue(ctx, dom.global()); }, nullptr },
		{ "readyState", FN { return JS_NewString(ctx, "complete"); }, nullptr },
		{ "visibilityState", FN { return JS_NewString(ctx, "visible"); }, nullptr },
		{ "hidden", FN { return JS_FALSE; }, nullptr },
		{ "children", FN {
			JSValue a = JS_NewArray(ctx);
			JS_SetPropertyUint32(ctx, a, 0, node_value(ctx, dom.document()->document_element()));
			return a;
		}, nullptr },
		{ "title",
			FN {
				for (const Link &l : dom.document()->head()->links())
					if (const Element *e = l.node->as_element(); e && e->is(n::title))
						return str(ctx, e->text_content());
				return JS_NewString(ctx, "");
			},
			FN {
				Element *head = dom.document()->head();
				for (const Link &l : head->links())
					if (Element *e = l.node->as_element(); e && e->is(n::title))
					{
						dom.set_text_content(e, to_string(ctx, argv[0]));
						return JS_UNDEFINED;
					}
				JSValue t = dom.new_element("title", Namespace::Html);
				dom.insert(head, dom.node_of(t), nullptr);
				dom.set_text_content(dom.node_of(t), to_string(ctx, argv[0]));
				JS_FreeValue(ctx, t);
				return JS_UNDEFINED;
			} },
	});
	define(dom, document, {
		{ "createElement", 1, FN { return dom.new_element(to_string(ctx, argv[0]), Namespace::Html); } },
		{ "createElementNS", 2, FN {
			const std::optional<std::string> ns = to_optional_string(ctx, argv[0]);
			return dom.new_element(to_string(ctx, argv[1]), ns && *ns == SVG_NS ? Namespace::Svg : Namespace::Html);
		} },
		{ "createTextNode", 1, FN { return dom.new_text(to_string(ctx, argv[0])); } },
		{ "getElementById", 1, FN {
			const Name id = dom.names.find(to_string(ctx, argv[0]));
			if (!id)
				return JS_NULL;
			auto walk = [&](const Node *p, auto &self) -> Element * {
				for (const Link &l : p->links())
					if (Element *e = l.node->as_element())
					{
						if (e->id() == id)
							return e;
						if (Element *f = self(e, self))
							return f;
					}
				return nullptr;
			};
			return node_value(ctx, walk(dom.document(), walk));
		} },
		{ "querySelector", 1, FN {
			const std::vector<Selector> *list = selectors(dom, ctx, argv[0]);
			if (!list)
				return JS_EXCEPTION;
			std::vector<Element *> out;
			collect(dom.document(), *list, nullptr, true, out);
			return out.empty() ? JS_NULL : node_value(ctx, out[0]);
		} },
		{ "querySelectorAll", 1, FN {
			const std::vector<Selector> *list = selectors(dom, ctx, argv[0]);
			if (!list)
				return JS_EXCEPTION;
			std::vector<Element *> out;
			collect(dom.document(), *list, nullptr, false, out);
			return array_of(ctx, out);
		} },
		{ "getElementsByTagName", 1, FN {
			const std::vector<Selector> *list = selectors(dom, ctx, argv[0]);
			if (!list)
				return JS_EXCEPTION;
			std::vector<Element *> out;
			collect(dom.document(), *list, nullptr, false, out);
			return array_of(ctx, out);
		} },
		{ "getElementsByClassName", 1, FN {
			std::string sel;
			const std::string names = to_string(ctx, argv[0]);
			for (std::string_view w : split_words(names))
				sel += "." + std::string(w);
			JSValue s = str(ctx, sel.empty() ? "#__none__" : sel);
			const std::vector<Selector> *list = selectors(dom, ctx, s);
			JS_FreeValue(ctx, s);
			std::vector<Element *> out;
			if (list)
				collect(dom.document(), *list, nullptr, false, out);
			return array_of(ctx, out);
		} },
		{ "hasFocus", 0, FN { return JS_NewBool(ctx, dom.input().window_focused()); } },
		{ "elementFromPoint", 2, FN { return node_value(ctx, hit(dom, to_number(ctx, argv[0]), to_number(ctx, argv[1]))); } },
	});
	JS_SetClassProto(ctx, c.document, JS_DupValue(ctx, document));

	// ---------------------------------------------------------------- the lists, the class list, the style

	JSValue array_proto = JS_GetPropertyStr(ctx, dom.global(), "Array");
	JSValue array_prototype = JS_GetPropertyStr(ctx, array_proto, "prototype");
	auto borrow = [&](JSValueConst to, const char *name, const char *as = nullptr) {
		JS_DefinePropertyValueStr(ctx, to, as ? as : name, JS_GetPropertyStr(ctx, array_prototype, name),
			JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
	};
	auto iterable = [&](JSValueConst proto) {
		for (const char *m : { "forEach", "entries", "keys", "values" })
			borrow(proto, m);
		JSValue iter = JS_GetPropertyStr(ctx, array_prototype, "values");
		JSValue symbol = JS_Eval(ctx, "Symbol.iterator", 15, "<quickdom>", JS_EVAL_TYPE_GLOBAL);
		JSAtom sym = JS_ValueToAtom(ctx, symbol);
		JS_DefinePropertyValue(ctx, proto, sym, iter, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
		JS_FreeAtom(ctx, sym);
		JS_FreeValue(ctx, symbol);
	};
	JSValue list = JS_NewObject(ctx);
	define(dom, list, {
		{ "length", FN {
			JSClassID id;
			const Holder *h = static_cast<const Holder *>(JS_GetAnyOpaque(self, &id));
			if (!h || id != dom.classes().node_list)
				return illegal(ctx);
			size_t k = 0;
			for (const Link &l : h->node->links())
				k += h->kind == 0 || l.node->is_element();
			return JS_NewInt32(ctx, int(k));
		}, nullptr },
	});
	define(dom, list, {
		{ "item", 1, FN {
			JSValue v = JS_GetPropertyUint32(ctx, self, uint32_t(to_number(ctx, argv[0])));
			if (JS_IsUndefined(v))
				return JS_NULL;
			return v;
		} },
	});
	iterable(list);
	JS_SetClassProto(ctx, c.node_list, list);

	JSValue tokens = JS_NewObject(ctx);
#define THIS_TOKENS \
	JSClassID id_; \
	const Holder *h_ = static_cast<const Holder *>(JS_GetAnyOpaque(self, &id_)); \
	if (!h_ || id_ != dom.classes().token_list) \
		return illegal(ctx); \
	Element *el = h_->node->as_element()
	define(dom, tokens, {
		{ "length", FN { THIS_TOKENS; return JS_NewInt32(ctx, int(el->classes().size())); }, nullptr },
		{ "value",
			FN { THIS_TOKENS; const std::string *v = el->attribute(n::class_); return str(ctx, v ? *v : std::string()); },
			FN { THIS_TOKENS; dom.set_attribute(el, n::class_, to_string(ctx, argv[0])); return JS_UNDEFINED; } },
	});
	define(dom, tokens, {
		{ "contains", 1, FN {
			THIS_TOKENS;
			const Name k = dom.names.find(to_string(ctx, argv[0]));
			return JS_NewBool(ctx, k && el->has_class(k));
		} },
		{ "add", 0, FN {
			THIS_TOKENS;
			std::vector<std::string> list;
			for (Name k : el->classes())
				list.emplace_back(dom.names.str(k));
			bool changed = false;
			for (int i = 0; i < argc; i++)
			{
				const std::string s = to_string(ctx, argv[i]);
				if (std::find(list.begin(), list.end(), s) == list.end())
				{
					list.push_back(s);
					changed = true;
				}
			}
			if (changed || !el->has_attribute(n::class_))
			{
				std::string v;
				for (const std::string &s : list)
					v += (v.empty() ? "" : " ") + s;
				dom.set_attribute(el, n::class_, v);
			}
			return JS_UNDEFINED;
		} },
		{ "remove", 0, FN {
			THIS_TOKENS;
			std::vector<std::string> list;
			for (Name k : el->classes())
				list.emplace_back(dom.names.str(k));
			for (int i = 0; i < argc; i++)
			{
				const std::string s = to_string(ctx, argv[i]);
				list.erase(std::remove(list.begin(), list.end(), s), list.end());
			}
			std::string v;
			for (const std::string &s : list)
				v += (v.empty() ? "" : " ") + s;
			if (el->has_attribute(n::class_))
				dom.set_attribute(el, n::class_, v);
			return JS_UNDEFINED;
		} },
		{ "toggle", 2, FN {
			THIS_TOKENS;
			const std::string s = to_string(ctx, argv[0]);
			const Name k = dom.names.get(s);
			const bool on = argc > 1 && !JS_IsUndefined(argv[1]) ? JS_ToBool(ctx, argv[1]) : !el->has_class(k);
			if (on == el->has_class(k))
				return JS_NewBool(ctx, on);
			std::string v;
			for (Name x : el->classes())
				if (x != k)
					v += (v.empty() ? "" : " ") + std::string(dom.names.str(x));
			if (on)
				v += (v.empty() ? "" : " ") + s;
			dom.set_attribute(el, n::class_, v);
			return JS_NewBool(ctx, on);
		} },
		{ "replace", 2, FN {
			THIS_TOKENS;
			const Name a = dom.names.get(to_string(ctx, argv[0]));
			if (!el->has_class(a))
				return JS_FALSE;
			const std::string b = to_string(ctx, argv[1]);
			std::string v;
			for (Name x : el->classes())
				v += (v.empty() ? "" : " ") + (x == a ? b : std::string(dom.names.str(x)));
			dom.set_attribute(el, n::class_, v);
			return JS_TRUE;
		} },
		{ "item", 1, FN {
			THIS_TOKENS;
			const double i = to_number(ctx, argv[0], -1);
			if (i < 0 || i >= double(el->classes().size()))
				return JS_NULL;
			return str(ctx, dom.names.str(el->classes()[size_t(i)]));
		} },
		{ "toString", 0, FN { THIS_TOKENS; const std::string *v = el->attribute(n::class_); return str(ctx, v ? *v : std::string()); } },
	});
#undef THIS_TOKENS
	iterable(tokens);
	JS_SetClassProto(ctx, c.token_list, tokens);

	// the style: every property this library knows, camel-cased and dashed, set and read as written
	JSValue style = JS_NewObject(ctx);
#define THIS_STYLE \
	JSClassID id_; \
	const Holder *h_ = static_cast<const Holder *>(JS_GetAnyOpaque(self, &id_)); \
	if (!h_ || id_ != dom.classes().style) \
		return illegal(ctx); \
	Element *el = h_->node->as_element()
	static const std::vector<std::string> prop_names = [] {
		std::vector<std::string> names;
		for (const std::string &p : style_property_names())
		{
			names.push_back(p);
			if (p.find('-') != std::string::npos)
				names.push_back(camel(p));
		}
		return names;
	}();
	for (size_t i = 0; i < prop_names.size(); i++)
	{
		// one getter and one setter per name: the name is the function's own, read from its data
		JSAtom atom = JS_NewAtom(ctx, prop_names[i].c_str());
		JSValue name = JS_NewString(ctx, dashed(prop_names[i]).c_str());
		JSValue data[2] = { JS_DupValue(ctx, dom.ref()), name };
		JSValue get = JS_NewCFunctionData(ctx, [](JSContext *ctx, JSValueConst self, int, JSValueConst *, int, JSValueConst *data) -> JSValue {
			JSClassID id;
			Dom *dom = static_cast<Dom *>(JS_GetAnyOpaque(data[0], &id));
			const Holder *h = static_cast<const Holder *>(JS_GetAnyOpaque(self, &id));
			if (!dom || !h || id != dom->classes().style)
				return illegal(ctx);
			const std::string name = to_string(ctx, data[1]);
			for (const auto &[k, v] : h->node->as_element()->inline_style())
				if (k == name)
					return str(ctx, v);
			return JS_NewString(ctx, "");
		}, 0, 0, 2, data);
		JSValue set = JS_NewCFunctionData(ctx, [](JSContext *ctx, JSValueConst self, int, JSValueConst *argv, int, JSValueConst *data) -> JSValue {
			JSClassID id;
			Dom *dom = static_cast<Dom *>(JS_GetAnyOpaque(data[0], &id));
			const Holder *h = static_cast<const Holder *>(JS_GetAnyOpaque(self, &id));
			if (!dom || !h || id != dom->classes().style)
				return illegal(ctx);
			const std::string value = JS_IsNull(argv[0]) || JS_IsUndefined(argv[0]) ? std::string() : to_string(ctx, argv[0]);
			dom->set_inline(h->node->as_element(), to_string(ctx, data[1]), std::string(trim(value)));
			return JS_UNDEFINED;
		}, 1, 0, 2, data);
		JS_FreeValue(ctx, data[0]);
		JS_FreeValue(ctx, data[1]);
		JS_DefinePropertyGetSet(ctx, style, atom, get, set, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
		JS_FreeAtom(ctx, atom);
	}
	define(dom, style, {
		{ "cssText",
			FN {
				THIS_STYLE;
				std::string css;
				for (const auto &[k, v] : el->inline_style())
					css += (css.empty() ? "" : " ") + k + ": " + v + ";";
				return str(ctx, css);
			},
			FN { THIS_STYLE; dom.set_inline_text(el, to_string(ctx, argv[0])); return JS_UNDEFINED; } },
		{ "length", FN { THIS_STYLE; return JS_NewInt32(ctx, int(el->inline_style().size())); }, nullptr },
	});
	define(dom, style, {
		{ "getPropertyValue", 1, FN {
			THIS_STYLE;
			const std::string name = dashed(to_string(ctx, argv[0]));
			for (const auto &[k, v] : el->inline_style())
				if (k == name)
					return str(ctx, v);
			return JS_NewString(ctx, "");
		} },
		{ "setProperty", 3, FN {
			THIS_STYLE;
			const std::string value = JS_IsNull(argv[1]) || JS_IsUndefined(argv[1]) ? std::string() : to_string(ctx, argv[1]);
			dom.set_inline(el, dashed(to_string(ctx, argv[0])), std::string(trim(value)));
			return JS_UNDEFINED;
		} },
		{ "removeProperty", 1, FN {
			THIS_STYLE;
			const std::string name = dashed(to_string(ctx, argv[0]));
			std::string was;
			for (const auto &[k, v] : el->inline_style())
				if (k == name)
					was = v;
			dom.set_inline(el, name, "");
			return str(ctx, was);
		} },
		{ "item", 1, FN {
			THIS_STYLE;
			const double i = to_number(ctx, argv[0], -1);
			if (i < 0 || i >= double(el->inline_style().size()))
				return JS_NewString(ctx, "");
			return str(ctx, el->inline_style()[size_t(i)].first);
		} },
	});
#undef THIS_STYLE
	JS_SetClassProto(ctx, c.style, style);
	JS_FreeValue(ctx, array_prototype);
	JS_FreeValue(ctx, array_proto);

	// ---------------------------------------------------------------- constructors

	auto illegal_ctor = FN { return JS_ThrowTypeError(ctx, "Illegal constructor"); };
	JSValue node_ctor = constructor(dom, "Node", 0, illegal_ctor, node);
	const std::pair<const char *, int> types[] = { { "ELEMENT_NODE", 1 }, { "TEXT_NODE", 3 }, { "DOCUMENT_NODE", 9 },
		{ "COMMENT_NODE", 8 }, { "DOCUMENT_FRAGMENT_NODE", 11 } };
	for (const auto &[k, v] : types)
	{
		JS_DefinePropertyValueStr(ctx, node_ctor, k, JS_NewInt32(ctx, v), 0);
		JS_DefinePropertyValueStr(ctx, node, k, JS_NewInt32(ctx, v), 0);
	}
	set_global(dom, "Node", node_ctor);
	set_global(dom, "EventTarget", JS_DupValue(ctx, node_ctor));
	JSValue element_ctor = constructor(dom, "Element", 0, illegal_ctor, element);
	set_global(dom, "Element", element_ctor);
	for (const char *alias : { "HTMLElement", "SVGElement", "HTMLDivElement", "HTMLInputElement", "HTMLCanvasElement",
		"HTMLSelectElement", "HTMLButtonElement", "HTMLTextAreaElement", "HTMLDialogElement" })
		set_global(dom, alias, JS_DupValue(ctx, element_ctor));
	set_global(dom, "Text", constructor(dom, "Text", 1, FN {
		return dom.new_text(argc > 0 && !JS_IsUndefined(argv[0]) ? to_string(ctx, argv[0]) : std::string());
	}, text));
	JSValue doc_ctor = constructor(dom, "Document", 0, illegal_ctor, document);
	set_global(dom, "Document", doc_ctor);
	set_global(dom, "HTMLDocument", JS_DupValue(ctx, doc_ctor));

	dom.proto_node = node;
	JS_FreeValue(ctx, element);
	JS_FreeValue(ctx, text);
	JS_FreeValue(ctx, document);
}

} // namespace qdom::js
