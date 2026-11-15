// Copyright 2026 Ian Karlsson
#include "dom/dom.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>

#include "quickdom/quickdom.h"

#include "dom/observer.h"
#include "input/field.h"
#include "input/input.h"
#include "layout/layout.h"
#include "paint/canvas.h"

namespace
{

// how long the input is still before a collection (Dom::advance)
constexpr double IDLE_GC_MS = 500;

double now_ms()
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void add_time(double &sum, double &max, double t0)
{
	const double t = now_ms() - t0;
	sum += t;
	max = std::max(max, t);
}

} // namespace

namespace qdom
{

Dom::Dom(JSContext *ctx, const qdom_options *options) : styles(names, fonts), m_ctx(ctx), m_rt(JS_GetRuntime(ctx))
{
	if (options)
	{
		m_log = options->log;
		m_log_user = options->user;
		m_flags = options->flags;
		m_cursor_fn = options->cursor;
		m_copy_fn = options->copy;
		m_paste_fn = options->paste;
		m_resource_fn = options->resource;
	}
	m_layout = std::make_unique<Layout>(*this);
	m_input = std::make_unique<Input>(*this);
	if (const char *c = getenv("QDOM_CHECK_LAYOUT"); c && *c && strcmp(c, "0"))
		m_check_layout = true;
	styles.set_viewport(m_width, m_height);
}

Dom::~Dom()
{
	m_input->release();
	for (ElementRef &r : m_picture_waiting)
		JS_FreeValue(m_ctx, r.value);
	m_picture_waiting.clear();
	for (Timer &t : m_timers)
	{
		JS_FreeValue(m_ctx, t.fn);
		for (JSValue a : t.args)
			JS_FreeValue(m_ctx, a);
	}
	m_timers.clear();
	for (FrameRequest &f : m_frames)
		JS_FreeValue(m_ctx, f.fn);
	m_frames.clear();
	for (Listener &l : m_window_listeners)
		JS_FreeValue(m_ctx, l.fn);
	m_window_listeners.clear();
	for (ResizeObserver *o : observers)
		o->dom = nullptr;
	observers.clear();
	if (m_doc)
		m_doc->m_dom = nullptr;
	// the functions made for the bindings throw from now on
	if (JS_IsObject(m_ref))
		JS_SetOpaque(m_ref, nullptr);
	JS_FreeValue(m_ctx, m_ref);
	JS_FreeValue(m_ctx, m_doc_value);
	JS_FreeValue(m_ctx, proto_node);
	JS_FreeValue(m_ctx, m_global);
}

void Dom::log(std::string_view text) const
{
	const std::string s(text);
	if (m_log)
		m_log(m_log_user, s.c_str());
	else
		fprintf(stderr, "[quickdom] %s\n", s.c_str());
}

void Dom::report_exception()
{
	JSValue e = JS_GetException(m_ctx);
	const char *text = JS_ToCString(m_ctx, e);
	JSValue stack = JS_IsError(e) ? JS_GetPropertyStr(m_ctx, e, "stack") : JS_UNDEFINED;
	const char *where = JS_IsUndefined(stack) ? nullptr : JS_ToCString(m_ctx, stack);
	std::string line = text ? text : "(exception)";
	if (where && *where)
	{
		line += '\n';
		line += where;
	}
	log(line);
	JS_FreeCString(m_ctx, where);
	JS_FreeCString(m_ctx, text);
	JS_FreeValue(m_ctx, stack);
	JS_FreeValue(m_ctx, e);
}

void Dom::run_jobs()
{
	JSContext *c;
	int r;
	while ((r = JS_ExecutePendingJob(m_rt, &c)) != 0)
		if (r < 0)
			report_exception();
}

// ---------------------------------------------------------------- nodes

void Dom::create_document()
{
	auto *doc = new Document();
	doc->m_dom = this;
	m_doc = doc;
	wrap(doc, m_cls.document);
	m_doc_value = doc->wrapper();
	auto add = [&](Node *parent, const char *tag) {
		JSValue v = new_element(tag, Namespace::Html);
		Element *el = element_of(v);
		insert(parent, el, nullptr);
		JS_FreeValue(m_ctx, v);
		return el;
	};
	doc->m_html = add(doc, "html");
	doc->m_head = add(doc->m_html, "head");
	doc->m_body = add(doc->m_html, "body");
}

void Dom::wrap(Node *node, JSClassID cls)
{
	JSValue obj = JS_NewObjectClass(m_ctx, int(cls));
	JS_SetOpaque(obj, node);
	node->m_wrapper = obj;
}

JSValue Dom::new_element(std::string_view tag, Namespace ns)
{
	const Name name = names.get(ns == Namespace::Html ? lower(tag) : std::string(tag));
	auto *el = new Element(m_doc, name, ns);
	wrap(el, m_cls.element);
	return el->wrapper();
}

JSValue Dom::new_text(std::string_view data)
{
	auto *t = new Text(m_doc, std::string(data));
	wrap(t, m_cls.text);
	return t->wrapper();
}

Node *Dom::node_of(JSValueConst v) const
{
	const JSClassID id = JS_GetClassID(v);
	if (id == m_cls.element || id == m_cls.text || id == m_cls.document)
		return static_cast<Node *>(JS_GetOpaque(v, id));
	return nullptr;
}

Element *Dom::element_of(JSValueConst v) const
{
	return JS_GetClassID(v) == m_cls.element ? static_cast<Element *>(JS_GetOpaque(v, m_cls.element)) : nullptr;
}

namespace
{

//! Whether a node is a style element or inside one: a change to it changes the stylesheets.
bool in_style(const Node *n)
{
	for (; n; n = n->parent())
		if (const Element *e = n->as_element(); e && e->is(n::style))
			return true;
	return false;
}

//! Whether an element's attributes reach its box itself (a replaced element's size, a list's options): any other
//! element's reach its box only through its style.
bool reads_attributes(const Element &el)
{
	return el.ns() == Namespace::Svg || el.is(n::img) || el.is(n::canvas) || el.is(n::input) || el.is(n::select)
		|| el.is(n::textarea) || el.is(n::option);
}

bool has_style(const Node *n)
{
	if (const Element *e = n->as_element(); e && e->is(n::style))
		return true;
	for (const Link &l : n->links())
		if (has_style(l.node))
			return true;
	return false;
}

} // namespace

void Dom::set_ref(ElementRef &ref, Element *el)
{
	if (ref.el == el)
		return;
	JSValue old = ref.value;
	ref.el = el;
	ref.value = el ? JS_DupValue(m_ctx, el->wrapper()) : JS_UNDEFINED;
	JS_FreeValue(m_ctx, old);
}

//! What leaves the tree lets go of the focus, the pointer's capture and the hover.
void Dom::gone(Node *n)
{
	m_input->gone(n);
	Document &d = *m_doc;
	if (d.m_capture.el && n->contains(d.m_capture.el))
		set_ref(d.m_capture, nullptr);
	if (d.m_active.el && n->contains(d.m_active.el))
	{
		d.m_active.el->focused = false;
		invalidate_style(d.m_active.el);
		set_ref(d.m_active, nullptr);
	}
	if (d.m_hover.el && n->contains(d.m_hover.el))
	{
		for (Element *e = d.m_hover.el; e; e = e->parent_element())
			e->hovered = false;
		set_ref(d.m_hover, n->parent_element());
		for (Element *e = d.m_hover.el; e; e = e->parent_element())
			e->hovered = true;
	}
	for (size_t i = 0; i < d.m_top.size();)
		if (n->contains(d.m_top[i].el))
		{
			d.m_top[i].el->modal = false;
			JS_FreeValue(m_ctx, d.m_top[i].value);
			d.m_top.erase(d.m_top.begin() + long(i));
		}
		else
			i++;
}

void Dom::detach(Node *child)
{
	Node *parent = child->m_parent;
	if (!parent)
		return;
	if (parent->connected())
		gone(child);
	if (in_style(parent) || has_style(child))
		invalidate_sheets();
	const size_t i = child->m_index;
	const JSValue link = parent->m_children[i].value;
	parent->m_children.erase(parent->m_children.begin() + long(i));
	for (size_t k = i; k < parent->m_children.size(); k++)
		parent->m_children[k].node->m_index = k;
	const JSValue up = child->m_parent_value;
	child->m_parent = nullptr;
	child->m_parent_value = JS_UNDEFINED;
	child->m_index = 0;
	if (Element *p = parent->as_element())
		invalidate_style(p, styles.children_reach());
	relayout(parent, true);
	// last: either reference may have been the last on its node
	JS_FreeValue(m_ctx, up);
	JS_FreeValue(m_ctx, link);
}

bool Dom::insert(Node *parent, Node *child, Node *before, const char **error)
{
	auto fail = [&](const char *message) {
		if (error)
			*error = message;
		return false;
	};
	if (child->is_document() || parent->is_text())
		return fail("HierarchyRequestError: the node cannot be put there");
	if (child->contains(parent))
		return fail("HierarchyRequestError: the new child contains the parent");
	if (before && before->m_parent != parent)
		return fail("NotFoundError: the node before which the new node is to be inserted is not a child of this node");
	if (before == child)
		before = child->next_sibling();
	if (parent->is_document() && child->is_text())
		return fail("HierarchyRequestError: a text cannot be a child of the document");
	// held while it moves, so that leaving its old parent does not free it
	JSValue hold = JS_DupValue(m_ctx, child->wrapper());
	detach(child);
	const size_t at = before ? before->m_index : parent->m_children.size();
	parent->m_children.insert(parent->m_children.begin() + long(at), Link { child, JS_DupValue(m_ctx, child->wrapper()) });
	for (size_t k = at; k < parent->m_children.size(); k++)
		parent->m_children[k].node->m_index = k;
	child->m_parent = parent;
	child->m_parent_value = JS_DupValue(m_ctx, parent->wrapper());
	if (Element *e = child->as_element())
		invalidate_style(e);
	if (Element *p = parent->as_element())
		invalidate_style(p, styles.children_reach());
	if (in_style(parent) || has_style(child))
		invalidate_sheets();
	relayout(parent, true);
	JS_FreeValue(m_ctx, hold);
	return true;
}

void Dom::remove(Node *child)
{
	detach(child);
}

void Dom::set_data(Text *t, std::string data)
{
	if (t->m_data == data)
		return;
	t->m_data = std::move(data);
	if (in_style(t))
		invalidate_sheets();
	relayout(t, false);
}

void Dom::set_text_content(Node *n, std::string_view text)
{
	if (Text *t = n->as_text())
	{
		set_data(t, std::string(text));
		return;
	}
	// the same text: nothing changes
	if (n->child_count() == 1 && n->child(0)->is_text() && n->child(0)->as_text()->data() == text)
		return;
	if (n->child_count() == 0 && text.empty())
		return;
	while (n->child_count())
		detach(n->last_child());
	if (!text.empty())
	{
		JSValue t = new_text(text);
		insert(n, node_of(t), nullptr);
		JS_FreeValue(m_ctx, t);
	}
}

void Dom::set_attribute(Element *el, Name name, std::string value)
{
	for (Attribute &a : el->m_attrs)
		if (a.name == name)
		{
			if (a.value == value)
				return;
			a.value = value;
			goto changed;
		}
	el->m_attrs.push_back({ name, value });
changed:
	using Reach = StyleEngine::Reach;
	// an svg's presentation attributes are part of its style
	Reach reach = el->ns() == Namespace::Svg ? Reach::Self : Reach::None;
	if (name == n::class_)
	{
		const std::vector<Name> was = el->m_classes;
		el->m_classes.clear();
		for (std::string_view w : split_words(value))
		{
			const Name c = names.get(w);
			if (!el->has_class(c))
				el->m_classes.push_back(c);
		}
		// the classes gone and come
		for (Name c : was)
			if (!el->has_class(c))
				reach = std::max(reach, styles.class_reach(c));
		for (Name c : el->m_classes)
			if (std::find(was.begin(), was.end(), c) == was.end())
				reach = std::max(reach, styles.class_reach(c));
	}
	else if (name == n::id)
	{
		const Name was = el->m_id;
		el->m_id = value.empty() ? 0 : names.get(value);
		if (was)
			reach = std::max(reach, styles.id_reach(was));
		if (el->m_id)
			reach = std::max(reach, styles.id_reach(el->m_id));
	}
	else if (name == n::style)
	{
		set_inline_text(el, value);
		return;
	}
	reach = std::max(reach, styles.attribute_reach(name));
	if (name == n::checked && !el->form.dirty_checked)
		el->form.checked = true;
	if (name == n::src && el->is(n::img))
		want_picture(el);
	invalidate_style(el, reach);
	// a replaced element's size reads its attributes; any other element's box only its style
	if (reads_attributes(*el))
		relayout(el, false);
}

void Dom::remove_attribute(Element *el, Name name)
{
	auto it = std::find_if(el->m_attrs.begin(), el->m_attrs.end(), [&](const Attribute &a) { return a.name == name; });
	if (it == el->m_attrs.end())
		return;
	el->m_attrs.erase(it);
	using Reach = StyleEngine::Reach;
	Reach reach = el->ns() == Namespace::Svg ? Reach::Self : Reach::None;
	if (name == n::class_)
	{
		for (Name c : el->m_classes)
			reach = std::max(reach, styles.class_reach(c));
		el->m_classes.clear();
	}
	else if (name == n::id)
	{
		if (el->m_id)
			reach = std::max(reach, styles.id_reach(el->m_id));
		el->m_id = 0;
	}
	else if (name == n::style)
	{
		el->m_inline.clear();
		reach = std::max(reach, Reach::Self);
	}
	reach = std::max(reach, styles.attribute_reach(name));
	if (name == n::checked && !el->form.dirty_checked)
		el->form.checked = false;
	invalidate_style(el, reach);
	if (reads_attributes(*el))
		relayout(el, false);
}

void Dom::set_inline(Element *el, std::string_view name, std::string_view value)
{
	auto &decls = el->m_inline;
	auto it = std::find_if(decls.begin(), decls.end(), [&](const auto &d) { return d.first == name; });
	if (value.empty())
	{
		if (it == decls.end())
			return;
		decls.erase(it);
	}
	else if (it != decls.end())
	{
		if (it->second == value)
			return;
		it->second = value;
	}
	else
		decls.emplace_back(std::string(name), std::string(value));
	invalidate_style(el, StyleEngine::Reach::Self);
}

void Dom::set_inline_text(Element *el, std::string_view text)
{
	el->m_inline.clear();
	for (Declaration &d : parse_declarations(text))
		el->m_inline.emplace_back(d.name, d.value);
	invalidate_style(el, StyleEngine::Reach::Self);
}

std::vector<Element *> Dom::options(const Element *select) const
{
	std::vector<Element *> out;
	for (const Link &l : select->links())
		if (Element *e = l.node->as_element())
		{
			if (e->is(n::option))
				out.push_back(e);
			else if (names.str(e->tag()) == "optgroup")
				for (const Link &k : e->links())
					if (Element *o = k.node->as_element(); o && o->is(n::option))
						out.push_back(o);
		}
	return out;
}

int Dom::selected_index(const Element *select) const
{
	const std::vector<Element *> os = options(select);
	const int chosen = select->form.selected_index;
	if (chosen >= 0 && chosen < int(os.size()))
		return chosen;
	for (size_t i = 0; i < os.size(); i++)
		if (os[i]->has_attribute(n::selected))
			return int(i);
	for (size_t i = 0; i < os.size(); i++)
		if (!os[i]->disabled())
			return int(i);
	return -1;
}

void Dom::set_selected_index(Element *el, int index)
{
	if (el->form.selected_index == index)
		return;
	el->form.selected_index = index;
	invalidate_style(el);
}

void Dom::set_value(Element *el, std::string value)
{
	if (el->is(n::select))
	{
		const std::vector<Element *> os = options(el);
		int index = -1;
		for (size_t i = 0; i < os.size(); i++)
		{
			const std::string *v = os[i]->attribute(n::value);
			if ((v ? *v : os[i]->text_content()) == value)
			{
				index = int(i);
				break;
			}
		}
		set_selected_index(el, index);
		return;
	}
	if (el->is(n::option))
	{
		set_attribute(el, n::value, std::move(value));
		return;
	}
	if (is_input_type(*el, "range"))
		value = range_value(el, value);
	m_input->value_set(el);
	el->form.dirty_value = true;
	if (el->form.value == value)
		return;
	el->form.value = std::move(value);
	el->form.sel_start = el->form.sel_end = el->form.value.size();
	invalidate_style(el);
	relayout(el, false);
}

std::string Dom::range_value(const Element *el, std::string_view value) const
{
	// clamped, and on a step from the minimum; the middle when it is no number
	const double lo = el->number_attribute(n::min, 0), hi = std::max(lo, el->number_attribute(n::max, 100));
	const std::string *s = el->attribute(n::step);
	const double step = s && lower(*s) == "any" ? 0 : std::max(0.0, el->number_attribute(n::step, 1));
	std::optional<double> v = parse_number(value);
	double x = std::clamp(v ? *v : lo + (hi - lo) / 2, lo, hi);
	if (step > 0)
	{
		x = lo + std::round((x - lo) / step) * step;
		if (x > hi)
			x -= step;
		// what the steps' sums leave of rounding errors
		x = std::round(x * 1e9) / 1e9;
	}
	return format_number(x);
}

std::string Dom::range_value(const Element *el) const
{
	const std::string *attr = el->attribute(n::value);
	return range_value(el, el->form.dirty_value ? el->form.value : attr ? *attr : std::string());
}

void Dom::set_field(Element *el, std::string value, size_t anchor, size_t focus)
{
	const bool was_empty = field_value(*el).empty();
	el->form.dirty_value = true;
	el->form.value = std::move(value);
	el->form.sel_start = std::min(anchor, el->form.value.size());
	el->form.sel_end = std::min(focus, el->form.value.size());
	// :placeholder-shown reads whether it is empty
	if (was_empty != el->form.value.empty())
		invalidate_style(el);
	invalidate_paint();
}

void Dom::set_checked(Element *el, bool on)
{
	el->form.dirty_checked = true;
	if (el->form.checked == on)
		return;
	el->form.checked = on;
	invalidate_style(el, styles.pseudo_reach(PseudoClass::Checked));
	invalidate_paint();
}

// ---------------------------------------------------------------- state

void Dom::set_focus(Element *el)
{
	Document &d = *m_doc;
	Element *was = d.m_active.el;
	if (el == d.m_body)
		el = nullptr;
	if (was == el)
		return;
	JSValue hold = was ? JS_DupValue(m_ctx, was->wrapper()) : JS_UNDEFINED;
	set_ref(d.m_active, el);
	if (was)
	{
		was->focused = false;
		focus_changed(was);
		fire(was, "blur", false);
		fire(was, "focusout", true);
		if ((was->is(n::input) || was->is(n::textarea)) && was->form.committed != was->form.value)
		{
			was->form.committed = was->form.value;
			fire(was, "change", true);
		}
	}
	if (el && d.m_active.el == el)
	{
		el->focused = true;
		el->focus_visible = is_text_field(*el) || m_input->keyboard();
		m_input->caret_reset();
		focus_changed(el);
		if (el->is(n::input) || el->is(n::textarea))
			el->form.committed = el->form.value;
		fire(el, "focus", false);
		fire(el, "focusin", true);
	}
	JS_FreeValue(m_ctx, hold);
	invalidate_paint();
}

void Dom::set_hover(Element *el)
{
	Document &d = *m_doc;
	if (d.m_hover.el == el)
		return;
	std::vector<Element *> was, now;
	for (Element *e = d.m_hover.el; e; e = e->parent_element())
		was.push_back(e);
	for (Element *e = el; e; e = e->parent_element())
		now.push_back(e);
	for (Element *e : was)
		if (std::find(now.begin(), now.end(), e) == now.end())
		{
			e->hovered = false;
			if (styles.reads_hover(*e))
				invalidate_style(e, styles.pseudo_reach(PseudoClass::Hover));
		}
	for (Element *e : now)
		if (std::find(was.begin(), was.end(), e) == was.end())
		{
			e->hovered = true;
			if (styles.reads_hover(*e))
				invalidate_style(e, styles.pseudo_reach(PseudoClass::Hover));
		}
	set_ref(d.m_hover, el);
	invalidate_paint();
}

void Dom::set_capture(Element *el)
{
	set_ref(m_doc->m_capture, el);
}

void Dom::set_modal(Element *dialog, bool on)
{
	Document &d = *m_doc;
	for (size_t i = 0; i < d.m_top.size(); i++)
		if (d.m_top[i].el == dialog)
		{
			JS_FreeValue(m_ctx, d.m_top[i].value);
			d.m_top.erase(d.m_top.begin() + long(i));
			break;
		}
	if (on)
		d.m_top.push_back({ dialog, JS_DupValue(m_ctx, dialog->wrapper()) });
	dialog->modal = on;
	invalidate_style(dialog, std::max(styles.pseudo_reach(PseudoClass::Modal), styles.pseudo_reach(PseudoClass::Open)));
	relayout_all();
}

void Dom::close_dialog(Element *dialog)
{
	if (!dialog->has_attribute(n::open))
		return;
	remove_attribute(dialog, n::open);
	set_modal(dialog, false);
	fire(dialog, "close", false);
}

void Dom::set_scroll(Element *el, double x, double y)
{
	x = std::round(x);
	y = std::round(y);
	// to the start needs no layout
	if (x > 0 || y > 0)
		update();
	double mx = 0, my = 0;
	if (LayoutNode *n = layout_of(el))
	{
		mx = std::max(0.0, n->sw - n->cw);
		my = std::max(0.0, n->sh - n->ch);
	}
	x = std::clamp(x, 0.0, mx);
	y = std::clamp(y, 0.0, my);
	if (x == el->scroll_x && y == el->scroll_y)
		return;
	el->scroll_x = x;
	el->scroll_y = y;
	invalidate_paint();
}

// ---------------------------------------------------------------- updating

void Dom::invalidate_style(Element *el, StyleEngine::Reach reach)
{
	using Reach = StyleEngine::Reach;
	if (reach == Reach::None)
		return;
	Element *t = el;
	if (reach == Reach::Parent)
	{
		if (Element *p = el->parent_element())
			t = p;
		reach = Reach::Subtree;
	}
	t->style_dirty = true;
	if (reach == Reach::Subtree)
		t->style_subtree = true;
	for (Element *a = t->parent_element(); a && !a->style_below; a = a->parent_element())
		a->style_below = true;
	m_style_dirty = true;
	m_paint = true;
}

void Dom::focus_changed(Element *el)
{
	invalidate_style(el, std::max({ styles.pseudo_reach(PseudoClass::Focus), styles.pseudo_reach(PseudoClass::FocusVisible),
		styles.pseudo_reach(PseudoClass::FocusWithin) }));
	if (const StyleEngine::Reach within = styles.pseudo_reach(PseudoClass::FocusWithin); within != StyleEngine::Reach::None)
		for (Element *a = el->parent_element(); a; a = a->parent_element())
			invalidate_style(a, within);
}

void Dom::invalidate_style(Element *el, bool siblings)
{
	invalidate_style(el, siblings && styles.reads_siblings() ? StyleEngine::Reach::Parent : StyleEngine::Reach::Subtree);
}

void Dom::relayout(Node *at, bool kids)
{
	m_paint = true;
	Element *e = at->as_element() ? at->as_element() : at->parent_element();
	if (!e)
	{
		// the document's own children
		relayout_all();
		return;
	}
	if (e->ns() == Namespace::Svg)
	{
		// an svg's items are its drawing; the outer svg's attributes are its size
		Element *outer = e;
		while (outer->parent_element() && outer->parent_element()->ns() == Namespace::Svg)
			outer = outer->parent_element();
		if (outer != e || kids)
			return;
	}
	else if (e->is(n::option) || e->is(n::select) || names.str(e->tag()) == "optgroup")
	{
		// a list's width is its longest option's
		Element *list = e;
		while (list && !list->is(n::select))
			list = list->parent_element();
		if (list)
		{
			e = list;
			kids = false;
		}
	}
	else if (e->is(n::textarea) || e->is(n::canvas) || e->is(n::img) || e->is(n::input))
		kids = false;
	e->layout_dirty = true;
	if (kids)
		e->kids_dirty = true;
	for (Element *a = e->parent_element(); a && !a->layout_below; a = a->parent_element())
		a->layout_below = true;
	m_layout_dirty = true;
}

void Dom::fonts_changed()
{
	styles.forget_shared();
	if (m_doc && m_doc->m_html)
	{
		m_doc->m_html->style_dirty = m_doc->m_html->style_subtree = true;
		m_style_dirty = true;
	}
	relayout_all();
}

void Dom::relayout_all()
{
	m_layout->relayout_all();
	m_layout_dirty = true;
	m_paint = true;
}

void Dom::add_sheet(std::string css)
{
	m_host_sheets.push_back(std::move(css));
	invalidate_sheets();
}

void Dom::read_sheets()
{
	m_sheets_dirty = false;
	std::vector<std::string> texts = m_host_sheets;
	auto walk = [&](const Node *n, auto &self) -> void {
		for (const Link &l : n->links())
			if (const Element *e = l.node->as_element())
			{
				if (e->is(n::style))
					texts.push_back(e->text_content());
				else
					self(e, self);
			}
	};
	walk(m_doc, walk);
	if (texts == m_sheet_texts)
		return;
	m_sheet_texts = texts;
	styles.set_sheets(texts);
	if (m_doc->m_html)
		m_doc->m_html->style_dirty = m_doc->m_html->style_subtree = true;
	m_style_dirty = true;
}

void Dom::restyle(Element *el, const ComputedStyle *parent, bool subtree, bool inherit)
{
	// computed again where it is marked, where what is above it marked what is under it, or where its parent's style
	// changed (what it inherits); walked through where something under it is marked
	const bool sub = subtree || el->style_subtree;
	const bool own = sub || inherit || el->style_dirty;
	if (!own && !el->style_below)
		return;
	bool changed = false;
	if (own)
	{
		stats.styles++;
		StylePtr was = el->style, was_before = el->before_style, was_after = el->after_style;
		el->style = styles.compute(*el, parent);
		changed = el->style != was;
		el->after_style = el->before_style = nullptr;
		if (el->style->display != Display::None && el->ns() == Namespace::Html)
		{
			el->after_style = styles.compute(*el, el->style.get(), n::after);
			el->before_style = styles.compute(*el, el->style.get(), n::before);
		}
		if (changed || el->before_style != was_before || el->after_style != was_after)
			styled(el, was, was_before, was_after);
	}
	el->style_dirty = el->style_subtree = el->style_below = false;
	if (el->style->display == Display::None)
	{
		// what is inside is styled when it is shown
		auto clear = [](Element *e, auto &self) -> void {
			for (const Link &l : e->links())
				if (Element *c = l.node->as_element())
				{
					c->style_dirty = true;
					self(c, self);
				}
		};
		clear(el, clear);
		return;
	}
	for (const Link &l : el->links())
		if (Element *c = l.node->as_element())
			restyle(c, el->style.get(), sub, changed);
}

void Dom::update()
{
	if (!m_doc || !m_doc->m_html)
		return;
	if (m_sheets_dirty)
		read_sheets();
	if (m_style_dirty)
	{
		const double t0 = now_ms();
		m_style_dirty = false;
		restyle(m_doc->m_html, nullptr, false, false);
		stats.restyles++;
		add_time(stats.style_ms, stats.style_max, t0);
		if (m_check_layout)
			check_styles();
	}
	if (m_layout_dirty)
	{
		// the scrollbars every element starts from, for the check's layout made anew
		std::unordered_map<const Element *, Layout::Bars> bars;
		if (m_check_layout)
		{
			auto walk = [&](const Node *n, auto &self) -> void {
				for (const Link &l : n->links())
					if (const Element *e = l.node->as_element())
					{
						bars[e] = { e->last_bar_x, e->last_bar_y };
						self(e, self);
					}
			};
			walk(m_doc, walk);
		}
		const double t0 = now_ms();
		m_layout_dirty = false;
		m_layout->run(m_width, m_height);
		m_paint = true;
		stats.layouts++;
		add_time(stats.layout_ms, stats.layout_max, t0);
		if (m_check_layout)
			check_layout(bars);
	}
}

LayoutNode *Dom::layout_of(const Element *el)
{
	return el->layout;
}

void Dom::styled(Element *el, const StylePtr &was, const StylePtr &was_before, const StylePtr &was_after)
{
	m_paint = true;
	const bool on = el->style->display != Display::None, was_on = was && was->display != Display::None;
	if (!was || on != was_on)
	{
		// shown or hidden: its parent's boxes change
		if (Node *p = el->parent())
			relayout(p, true);
		return;
	}
	if (!on)
		return;
	// the box drawn with its new style now (and its texts), whether or not it is laid out again
	if (LayoutNode *n = el->layout)
	{
		n->style = el->style;
		for (LayoutNode *k : n->kids)
			if (k->kind == LayoutNode::Kind::Text)
				k->style = el->style;
	}
	auto same = [](const StylePtr &a, const StylePtr &b) { return a == b || (a && b && same_layout(*a, *b)); };
	if (!same(was, el->style))
		relayout(el, true);
	else if (!same(was_before, el->before_style) || !same(was_after, el->after_style))
		relayout(el, true);
	else if (was_before != el->before_style || was_after != el->after_style)
	{
		// a pseudo-element's drawing alone
		if (LayoutNode *n = el->layout)
			for (LayoutNode *k : n->kids)
				if (k->kind == LayoutNode::Kind::Pseudo)
				{
					const bool before = k == n->kids.front() && el->before_style;
					k->style = before ? el->before_style : el->after_style;
					for (LayoutNode *t : k->kids)
						t->style = k->style;
				}
	}
}

void Dom::check_styles()
{
	// every shown element's style computed anew from its parent's, and compared with the one kept
	int found = 0;
	auto walk = [&](Element *el, const ComputedStyle *parent, const std::string &at, auto &self) -> void {
		if (found > 8)
			return;
		const std::string here = at + "/" + std::string(names.str(el->tag()));
		const StylePtr fresh = styles.compute(*el, parent);
		if (!el->style || !(*el->style == *fresh))
		{
			log("Error: a kept style differs from one computed anew: " + here);
			found++;
			return;
		}
		if (fresh->display == Display::None)
			return;
		if (el->ns() == Namespace::Html)
		{
			const StylePtr after = styles.compute(*el, fresh.get(), n::after), before = styles.compute(*el, fresh.get(), n::before);
			auto same = [](const StylePtr &a, const StylePtr &b) { return a == b || (a && b && *a == *b); };
			if (!same(after, el->after_style) || !same(before, el->before_style))
			{
				log("Error: a kept pseudo-element's style differs from one computed anew: " + here);
				found++;
			}
		}
		for (const Link &l : el->links())
			if (Element *c = l.node->as_element())
				self(c, fresh.get(), here, self);
	};
	if (m_doc->m_html)
		walk(m_doc->m_html, nullptr, "", walk);
}

void Dom::check_layout(const std::unordered_map<const Element *, Layout::Bars> &bars)
{
	if (!m_check)
		m_check = std::make_unique<Layout>(*this);
	m_check->run_shadow(m_width, m_height, bars);
	const std::vector<std::string> diffs = Layout::compare(*m_layout, *m_check);
	for (size_t i = 0; i < diffs.size() && i < 8; i++)
		log("Error: the kept layout differs from one made anew: " + diffs[i]);
	if (diffs.size() > 8)
		log("Error: the kept layout differs from one made anew in " + std::to_string(diffs.size()) + " places");
}

Rect Dom::client_rect(const Element *el)
{
	update();
	LayoutNode *n = layout_of(el);
	if (!n)
		return {};
	double x = 0, y = 0;
	for (const Element *e = el; e; e = e->parent_element())
	{
		LayoutNode *b = layout_of(e);
		if (!b)
			return {};
		// a fixed box and a dialog in the top layer are in the viewport, wherever their ancestors scroll
		if (b->fixed)
		{
			x += b->fx;
			y += b->fy;
			break;
		}
		x += b->x;
		y += b->y;
		if (const Element *p = e->parent_element())
		{
			x -= p->scroll_x;
			y -= p->scroll_y;
		}
		const ComputedStyle &cs = *b->style;
		if (cs.position == Position::Relative)
		{
			auto off = [&](int a, int b2, std::optional<double> basis) {
				if (std::optional<double> v = cs.inset[a].resolve(basis))
					return *v;
				if (std::optional<double> v = cs.inset[b2].resolve(basis))
					return -*v;
				return 0.0;
			};
			LayoutNode *pb = e->parent_element() ? layout_of(e->parent_element()) : nullptr;
			x += off(LEFT, RIGHT, pb ? std::optional<double>(pb->w) : std::nullopt);
			y += off(TOP, BOTTOM, std::nullopt);
		}
		else if (cs.position == Position::Sticky)
			y += m_layout->stuck(*b);
	}
	return { x, y, n->w, n->h };
}

std::string Dom::retained_json()
{
	JS_RunGC(m_rt);
	auto describe = [&](const Node *n) {
		std::string d;
		if (const Element *e = n->as_element())
		{
			d = std::string(names.str(e->tag()));
			if (const std::string *id = e->attribute(n::id); id && !id->empty())
				d += "#" + *id;
			for (size_t i = 0; i < e->classes().size() && i < 3; i++)
				d += "." + std::string(names.str(e->classes()[i]));
		}
		else
			d = "#text";
		std::string t = n->text_content();
		if (t.size() > 24)
			t = t.substr(0, 24) + "...";
		if (!t.empty())
			d += " \"" + t + "\"";
		return d;
	};
	auto quote = [](const std::string &v) {
		std::string o = "\"";
		for (unsigned char c : v)
			if (c == '"' || c == '\\')
				o += { '\\', char(c) };
			else if (c < 0x20)
				o += ' ';
			else
				o += char(c);
		return o + "\"";
	};
	auto root_of = [](const Node *n) {
		while (n->parent())
			n = n->parent();
		return n;
	};
	struct Cut
	{
		const Node *root;
		size_t nodes = 0;
		std::vector<std::pair<std::string, const Node *>> held;
	};
	std::vector<Cut> cuts;
	auto cut_of = [&](const Node *root) -> Cut & {
		for (Cut &c : cuts)
			if (c.root == root)
				return c;
		cuts.push_back({ root });
		return cuts.back();
	};
	Node::each_alive([&](Node *n) {
		if (n->document() == m_doc && !n->is_document() && root_of(n) != m_doc)
			cut_of(root_of(n)).nodes++;
	});
	auto held = [&](const char *by, const Node *el) {
		if (el && el->document() == m_doc && root_of(el) != m_doc)
			cut_of(root_of(el)).held.push_back({ by, el });
	};
	m_input->each_ref(held);
	held("document.capture", m_doc->m_capture.el);
	held("document.active", m_doc->m_active.el);
	held("document.hover", m_doc->m_hover.el);
	for (const ElementRef &r : m_doc->m_top)
		held("document.top", r.el);
	for (const ElementRef &r : m_picture_waiting)
		held("picture", r.el);
	std::sort(cuts.begin(), cuts.end(), [](const Cut &a, const Cut &b) { return a.nodes > b.nodes; });
	std::string out = "[";
	for (size_t i = 0; i < cuts.size(); i++)
	{
		const Cut &c = cuts[i];
		out += std::string(i ? "," : "") + "{\"nodes\":" + std::to_string(c.nodes) + ",\"root\":" + quote(describe(c.root)) + ",\"held\":[";
		for (size_t k = 0; k < c.held.size(); k++)
			out += std::string(k ? "," : "") + "{\"by\":" + quote(c.held[k].first) + ",\"el\":" + quote(describe(c.held[k].second)) + "}";
		out += "]}";
	}
	return out + "]";
}

std::string Dom::boxes_json()
{
	update();
	std::string out = "[";
	bool first = true;
	auto num = [](double v) {
		v = std::round(v * 10) / 10;
		if (v == 0)
			v = 0;  // no -0
		return format_number(v);
	};
	auto escape = [](const std::string &s) {
		std::string o;
		for (char c : s)
		{
			if (c == '"' || c == '\\')
				o += '\\';
			o += c;
		}
		return o;
	};
	auto walk = [&](const Element *el, const std::string &path, auto &self) -> void {
		for (const Link &l : el->links())
		{
			const Element *c = l.node->as_element();
			if (!c || c->is(n::style) || c->is(n::script))
				continue;
			const Rect r = client_rect(c);
			std::string name(names.str(c->tag()));
			if (c->id())
				name += "#" + std::string(names.str(c->id()));
			for (Name k : c->classes())
				name += "." + std::string(names.str(k));
			const std::string at = path + "/" + name;
			if (r.w != 0 || r.h != 0)
			{
				if (!first)
					out += ",\n";
				first = false;
				out += "[\"" + escape(at) + "\"," + num(r.x) + "," + num(r.y) + "," + num(r.w) + "," + num(r.h) + "]";
			}
			if (c->ns() != Namespace::Svg && !c->is(n::select))
				self(c, at, self);
		}
	};
	if (m_doc && m_doc->m_body)
		walk(m_doc->m_body, "", walk);
	out += "]";
	return out;
}

// ---------------------------------------------------------------- the window

void Dom::set_viewport(double width, double height, double scale)
{
	const bool resized = width != m_width || height != m_height;
	m_width = width;
	m_height = height;
	m_scale = scale;
	styles.set_viewport(width, height);
	if (!resized)
		return;
	if (m_doc && m_doc->m_html)
		m_doc->m_html->style_dirty = m_doc->m_html->style_subtree = true;
	m_style_dirty = true;
	relayout_all();
	fire(nullptr, "resize", false);
}

// ---------------------------------------------------------------- time

uint32_t Dom::set_timer(JSValueConst fn, double delay, bool repeat, int argc, JSValueConst *argv)
{
	Timer t;
	t.id = m_next_id++;
	t.interval = std::max(0.0, std::isfinite(delay) ? delay : 0.0);
	t.due = m_now + t.interval;
	t.repeat = repeat;
	t.order = m_timer_order++;
	t.fn = JS_DupValue(m_ctx, fn);
	for (int i = 0; i < argc; i++)
		t.args.push_back(JS_DupValue(m_ctx, argv[i]));
	m_timers.push_back(std::move(t));
	return m_timers.back().id;
}

void Dom::clear_timer(uint32_t id)
{
	if (id && id == m_running_timer)
		m_running_cleared = true;
	for (size_t i = 0; i < m_timers.size(); i++)
		if (m_timers[i].id == id)
		{
			JS_FreeValue(m_ctx, m_timers[i].fn);
			for (JSValue a : m_timers[i].args)
				JS_FreeValue(m_ctx, a);
			m_timers.erase(m_timers.begin() + long(i));
			return;
		}
}

double Dom::next_timer() const
{
	double next = m_input->next_due();
	for (const Timer &t : m_timers)
		if (next < 0 || t.due < next)
			next = t.due;
	return next;
}

bool Dom::needs_paint() const
{
	return m_paint || m_input->needs_paint();
}

void Dom::painted()
{
	m_paint = false;
	m_input->painted();
}

// ---------------------------------------------------------------- pictures

void Dom::renew_images()
{
	m_image_generation++;
	invalidate_paint();
}

Picture *Dom::picture_of(const Element *img)
{
	const std::string *src = img->attribute(n::src);
	if (!src || src->empty())
		return nullptr;
	auto it = m_pictures.find(*src);
	return it == m_pictures.end() ? nullptr : &it->second;
}

void Dom::want_picture(Element *img)
{
	const std::string *src = img->attribute(n::src);
	if (src && !src->empty() && !m_pictures.count(*src))
	{
		Picture &p = m_pictures[*src];
		p.id = -++m_pictures_made;
	}
	// its event, once (an src set again before it settles waits for the new one)
	for (const ElementRef &r : m_picture_waiting)
		if (r.el == img)
			return;
	m_picture_waiting.push_back({ img, JS_DupValue(m_ctx, img->wrapper()) });
	m_picture_work = true;
	relayout_all();
}

void Dom::ask_pictures()
{
	std::vector<std::string> wanted;
	for (auto &[src, p] : m_pictures)
		if (p.state == Picture::State::Wanted)
		{
			p.state = m_resource_fn ? Picture::State::Asked : Picture::State::Failed;
			if (m_resource_fn)
				wanted.push_back(src);
		}
	// the host may answer within its call
	for (const std::string &src : wanted)
		m_resource_fn(m_log_user, src.c_str());
}

void Dom::picture_bytes(const std::string &src, const void *data, size_t size)
{
	auto it = m_pictures.find(src);
	if (it == m_pictures.end() || it->second.state == Picture::State::Ready)
		return;
	Picture &p = it->second;
	p.state = decode_picture(data, size, p.width, p.height, p.rgba) ? Picture::State::Ready : Picture::State::Failed;
	p.uploaded = 0;
	m_picture_work = true;
	relayout_all();
}

void Dom::picture_events()
{
	// what is left waits for the host's answers, which ask for a frame again
	m_picture_work = false;
	if (m_picture_waiting.empty())
		return;
	std::vector<ElementRef> due, still;
	for (ElementRef &r : m_picture_waiting)
	{
		const Picture *p = picture_of(r.el);
		if (!p || p->settled())
			due.push_back(r);
		else
			still.push_back(r);
	}
	m_picture_waiting = std::move(still);
	for (ElementRef &r : due)
	{
		const Picture *p = picture_of(r.el);
		// an img without an src has nothing to load: no event, as a browser's
		if (p)
			fire(r.el, p->state == Picture::State::Ready ? "load" : "error", false);
		JS_FreeValue(m_ctx, r.value);
	}
	if (!due.empty())
		run_jobs();
}

void Dom::host_cursor(const std::string &name) const
{
	if (m_cursor_fn)
		m_cursor_fn(m_log_user, name.c_str());
}

void Dom::host_copy(const std::string &text) const
{
	if (m_copy_fn)
		m_copy_fn(m_log_user, text.c_str(), text.size());
	else if (m_window_copy)
		m_window_copy(m_window_user, text.c_str(), text.size());
}

bool Dom::host_paste() const
{
	if (m_paste_fn)
		m_paste_fn(m_log_user);
	else if (m_window_paste)
		m_window_paste(m_window_user);
	else
		return false;
	return true;
}

void Dom::set_window_clipboard(void (*copy)(void *, const char *, size_t), void (*paste)(void *), void *user)
{
	m_window_copy = copy;
	m_window_paste = paste;
	m_window_user = user;
}

std::string Dom::title() const
{
	for (const Link &l : m_doc->head()->links())
		if (const Element *e = l.node->as_element(); e && e->is(n::title))
			return e->text_content();
	return {};
}

void Dom::advance(double now)
{
	m_now = std::max(m_now, now);
	m_input->advance();
	for (;;)
	{
		// the earliest due, ties in the order they were set
		size_t best = m_timers.size();
		for (size_t i = 0; i < m_timers.size(); i++)
			if (m_timers[i].due <= m_now
				&& (best == m_timers.size() || m_timers[i].due < m_timers[best].due
					|| (m_timers[i].due == m_timers[best].due && m_timers[i].order < m_timers[best].order)))
				best = i;
		if (best == m_timers.size())
			break;
		Timer t = std::move(m_timers[best]);
		m_timers.erase(m_timers.begin() + long(best));
		std::vector<JSValueConst> args(t.args.begin(), t.args.end());
		m_running_timer = t.id;
		m_running_cleared = false;
		if (JS_IsFunction(m_ctx, t.fn))
		{
			JSValue r = JS_Call(m_ctx, t.fn, JS_UNDEFINED, int(args.size()), args.data());
			if (JS_IsException(r))
				report_exception();
			JS_FreeValue(m_ctx, r);
		}
		run_jobs();
		m_running_timer = 0;
		// an interval keeps its id; a clear from inside its own call ends it
		if (t.repeat && !m_running_cleared)
		{
			// due again an interval on; one far behind (the clock jumped) starts again from now
			t.due += std::max(t.interval, 1.0);
			if (t.due < m_now - 1000)
				t.due = m_now;
			t.order = m_timer_order++;
			m_timers.push_back(std::move(t));
		}
		else
		{
			JS_FreeValue(m_ctx, t.fn);
			for (JSValue a : t.args)
				JS_FreeValue(m_ctx, a);
		}
	}
	if (m_gc_due && m_now - m_input_at >= IDLE_GC_MS)
	{
		m_gc_due = false;
		JS_RunGC(m_rt);
	}
}

uint32_t Dom::request_frame(JSValueConst fn)
{
	m_frames.push_back({ m_next_id++, JS_DupValue(m_ctx, fn) });
	return m_frames.back().id;
}

void Dom::cancel_frame(uint32_t id)
{
	for (size_t i = 0; i < m_frames.size(); i++)
		if (m_frames[i].id == id)
		{
			JS_FreeValue(m_ctx, m_frames[i].fn);
			m_frames.erase(m_frames.begin() + long(i));
			return;
		}
}

bool Dom::frame(double now)
{
	const double t0 = now_ms();
	struct Timed
	{
		Stats &s;
		double t0;
		~Timed()
		{
			s.frames++;
			add_time(s.frame_ms, s.frame_max, t0);
		}
	} timed { stats, t0 };
	m_now = std::max(m_now, now);
	m_input->advance();
	ask_pictures();
	picture_events();
	std::vector<FrameRequest> frames;
	frames.swap(m_frames);
	for (FrameRequest &f : frames)
	{
		JSValue t = JS_NewFloat64(m_ctx, m_now);
		JSValue r = JS_Call(m_ctx, f.fn, JS_UNDEFINED, 1, &t);
		if (JS_IsException(r))
			report_exception();
		JS_FreeValue(m_ctx, r);
		JS_FreeValue(m_ctx, f.fn);
		run_jobs();
	}
	// the layout, and the resize observers told of new sizes (which may change the document again)
	for (int k = 0; k < 4; k++)
	{
		update();
		bool again = false;
		std::vector<ResizeObserver *> list = observers;
		for (ResizeObserver *o : list)
		{
			if (std::find(observers.begin(), observers.end(), o) == observers.end())
				continue;
			JSValue entries = JS_NewArray(m_ctx);
			uint32_t count = 0;
			for (ResizeObserver::Target &t : o->targets)
			{
				LayoutNode *n = t.el->connected() ? layout_of(t.el) : nullptr;
				const double w = n ? std::max(0.0, n->cw - n->pl - n->pr) : 0;
				const double h = n ? std::max(0.0, n->ch - n->pt - n->pb) : 0;
				if (w == t.w && h == t.h)
					continue;
				t.w = w;
				t.h = h;
				JSValue entry = JS_NewObject(m_ctx);
				JS_SetPropertyStr(m_ctx, entry, "target", JS_DupValue(m_ctx, t.value));
				JSValue rect = JS_NewObject(m_ctx);
				for (auto [key, v] : { std::pair<const char *, double> { "x", 0 }, { "y", 0 }, { "left", 0 },
					{ "top", 0 }, { "width", w }, { "height", h }, { "right", w }, { "bottom", h } })
					JS_SetPropertyStr(m_ctx, rect, key, JS_NewFloat64(m_ctx, v));
				JS_SetPropertyStr(m_ctx, entry, "contentRect", rect);
				auto box = [&](double iw, double bh) {
					JSValue size = JS_NewObject(m_ctx);
					JS_SetPropertyStr(m_ctx, size, "inlineSize", JS_NewFloat64(m_ctx, iw));
					JS_SetPropertyStr(m_ctx, size, "blockSize", JS_NewFloat64(m_ctx, bh));
					JSValue list = JS_NewArray(m_ctx);
					JS_SetPropertyUint32(m_ctx, list, 0, size);
					return list;
				};
				JS_SetPropertyStr(m_ctx, entry, "contentBoxSize", box(w, h));
				JS_SetPropertyStr(m_ctx, entry, "borderBoxSize", box(n ? n->w : 0, n ? n->h : 0));
				JS_SetPropertyUint32(m_ctx, entries, count++, entry);
			}
			if (count)
			{
				again = true;
				JSValue self = JS_DupValue(m_ctx, o->self);
				JSValue cb = JS_DupValue(m_ctx, o->callback);
				JSValueConst args[2] = { entries, self };
				JSValue r = JS_Call(m_ctx, cb, self, 2, args);
				if (JS_IsException(r))
					report_exception();
				JS_FreeValue(m_ctx, r);
				JS_FreeValue(m_ctx, cb);
				JS_FreeValue(m_ctx, self);
				run_jobs();
			}
			JS_FreeValue(m_ctx, entries);
		}
		if (!again)
			break;
	}
	update();
	m_input->frame();
	run_jobs();
	return needs_paint();
}

// ---------------------------------------------------------------- bindings

int Dom::bind(BindingFn fn)
{
	m_fns.push_back(fn);
	return int(m_fns.size() - 1);
}

} // namespace qdom
