// Copyright 2026 Ian Karlsson
#include "dom/node.h"

#include <charconv>

#include "css/values.h"
#include "layout/layout.h"
#include "paint/canvas.h"

namespace qdom
{

Element *Node::parent_element() const
{
	return m_parent ? m_parent->as_element() : nullptr;
}

Node *Node::next_sibling() const
{
	if (!m_parent || m_index + 1 >= m_parent->m_children.size())
		return nullptr;
	return m_parent->m_children[m_index + 1].node;
}

Node *Node::previous_sibling() const
{
	if (!m_parent || m_index == 0)
		return nullptr;
	return m_parent->m_children[m_index - 1].node;
}

Element *Node::first_element_child() const
{
	for (const Link &l : m_children)
		if (l.node->is_element())
			return l.node->as_element();
	return nullptr;
}

Element *Node::last_element_child() const
{
	for (auto it = m_children.rbegin(); it != m_children.rend(); ++it)
		if (it->node->is_element())
			return it->node->as_element();
	return nullptr;
}

Element *Node::next_element_sibling() const
{
	if (!m_parent)
		return nullptr;
	const auto &c = m_parent->m_children;
	for (size_t i = m_index + 1; i < c.size(); i++)
		if (c[i].node->is_element())
			return c[i].node->as_element();
	return nullptr;
}

Element *Node::previous_element_sibling() const
{
	if (!m_parent)
		return nullptr;
	const auto &c = m_parent->m_children;
	for (size_t i = m_index; i-- > 0;)
		if (c[i].node->is_element())
			return c[i].node->as_element();
	return nullptr;
}

bool Node::connected() const
{
	const Node *n = this;
	while (n->m_parent)
		n = n->m_parent;
	return n->is_document();
}

bool Node::contains(const Node *n) const
{
	for (; n; n = n->m_parent)
		if (n == this)
			return true;
	return false;
}

std::string Node::text_content() const
{
	if (const Text *t = as_text())
		return t->data();
	std::string out;
	for (const Link &l : m_children)
		out += l.node->text_content();
	return out;
}

Node::Node(Document *doc, NodeType type) : m_doc(doc), m_type(type)
{
	s_live++;
	std::lock_guard<std::mutex> hold(s_all_lock);
	m_all_next = s_all;
	if (s_all)
		s_all->m_all_prev = this;
	s_all = this;
}

Node::~Node()
{
	s_live--;
	std::lock_guard<std::mutex> hold(s_all_lock);
	(m_all_prev ? m_all_prev->m_all_next : s_all) = m_all_next;
	if (m_all_next)
		m_all_next->m_all_prev = m_all_prev;
}

void Node::mark(JSRuntime *rt, JS_MarkFunc *mark) const
{
	JS_MarkValue(rt, m_parent_value, mark);
	for (const Link &l : m_children)
		JS_MarkValue(rt, l.value, mark);
	for (const Listener &l : m_listeners)
		JS_MarkValue(rt, l.fn, mark);
}

void Node::release(JSRuntime *rt)
{
	// the nodes these point to may be freed already (a collected cycle): only the references are let go
	JS_FreeValueRT(rt, m_parent_value);
	m_parent_value = JS_UNDEFINED;
	m_parent = nullptr;
	for (Link &l : m_children)
		JS_FreeValueRT(rt, l.value);
	m_children.clear();
	for (Listener &l : m_listeners)
		JS_FreeValueRT(rt, l.fn);
	m_listeners.clear();
}

Element::Element(Document *doc, Name tag, Namespace ns) : Node(doc, NodeType::Element), m_tag(tag), m_ns(ns)
{
}

// a box still in the tree forgets the node it was made for, which it lets go at the next layout
Element::~Element()
{
	if (layout)
		layout->el = nullptr;
}

Text::~Text()
{
	if (layout)
	{
		layout->text = nullptr;
		layout->data = nullptr;
	}
}

bool Element::has_class(Name c) const
{
	for (Name x : m_classes)
		if (x == c)
			return true;
	return false;
}

const std::string *Element::attribute(Name n) const
{
	for (const Attribute &a : m_attrs)
		if (a.name == n)
			return &a.value;
	return nullptr;
}

double Element::number_attribute(Name n, double fallback) const
{
	const std::string *v = attribute(n);
	if (!v)
		return fallback;
	std::optional<double> d = parse_number(*v);
	return d ? *d : fallback;
}

bool Element::focusable() const
{
	if (disabled() && m_ns == Namespace::Html && (is(n::input) || is(n::select) || is(n::button) || is(n::textarea)))
		return false;
	return (m_ns == Namespace::Html && (is(n::input) || is(n::select) || is(n::button) || is(n::textarea)))
		|| has_attribute(n::tabindex);
}

void Element::mark(JSRuntime *rt, JS_MarkFunc *mark) const
{
	Node::mark(rt, mark);
	JS_MarkValue(rt, style_object, mark);
	JS_MarkValue(rt, class_list, mark);
	JS_MarkValue(rt, child_nodes, mark);
	JS_MarkValue(rt, children_list, mark);
	JS_MarkValue(rt, context, mark);
	for (JSValue o : observers)
		JS_MarkValue(rt, o, mark);
}

void Element::release(JSRuntime *rt)
{
	Node::release(rt);
	for (JSValue *v : { &style_object, &class_list, &child_nodes, &children_list, &context })
	{
		JS_FreeValueRT(rt, *v);
		*v = JS_UNDEFINED;
	}
	for (JSValue o : observers)
		JS_FreeValueRT(rt, o);
	observers.clear();
}

void Document::mark(JSRuntime *rt, JS_MarkFunc *mark) const
{
	Node::mark(rt, mark);
	JS_MarkValue(rt, m_active.value, mark);
	JS_MarkValue(rt, m_hover.value, mark);
	JS_MarkValue(rt, m_capture.value, mark);
	for (const ElementRef &r : m_top)
		JS_MarkValue(rt, r.value, mark);
}

void Document::release(JSRuntime *rt)
{
	Node::release(rt);
	for (ElementRef *r : { &m_active, &m_hover, &m_capture })
	{
		JS_FreeValueRT(rt, r->value);
		*r = ElementRef();
	}
	for (ElementRef &r : m_top)
		JS_FreeValueRT(rt, r.value);
	m_top.clear();
	m_html = m_head = m_body = nullptr;
}

} // namespace qdom
