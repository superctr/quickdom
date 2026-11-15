// Copyright 2026 Ian Karlsson
//! \file
//! The document's nodes: elements, texts and the document itself.
//!
//! Every node lives inside its JavaScript object (its wrapper): the wrapper is made with the node, owns it, and
//! deletes it in its finalizer, so properties a script puts on an element last as long as
//! the element. The tree's edges are counted references to the wrappers, parent to child and child to parent, which
//! the wrappers' gc_mark reports to QuickJS: a subtree that nothing reaches is collected as one cycle. A node's
//! destructor frees the references it holds and never touches another node, which the collector may have freed.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "quickjs.h"

#include "core/names.h"

namespace qdom
{

class Dom;
class Node;
class Document;
class Element;
class Text;
struct ComputedStyle;
struct LayoutNode;
class Canvas;

enum class NodeType : uint8_t
{
	Element = 1,
	Text = 3,
	Document = 9,
};

enum class Namespace : uint8_t
{
	Html,
	Svg,
};

//! A listener added with addEventListener: a function (or an object with handleEvent), counted.
struct Listener
{
	Name type = 0;
	JSValue fn = JS_UNDEFINED;
	bool capture = false;
	bool once = false;
	bool removed = false;
};

//! A child with the counted reference the parent holds on its wrapper.
struct Link
{
	Node *node;
	JSValue value;
};

//! An element held by a counted reference (it may leave the tree while held).
struct ElementRef
{
	Element *el = nullptr;
	JSValue value = JS_UNDEFINED;
};

class Node
{
public:
	Node(const Node &) = delete;
	Node &operator=(const Node &) = delete;
	virtual ~Node();

	//! The nodes alive in the process (for the tests: what the collector has freed).
	static long live() { return s_live; }
	//! Each node alive in the process, in turn (for the report of those kept off their page, Dom::retained_json).
	template <typename F> static void each_alive(F f)
	{
		std::lock_guard<std::mutex> hold(s_all_lock);
		for (Node *n = s_all; n; n = n->m_all_next)
			f(n);
	}

	NodeType type() const { return m_type; }
	bool is_element() const { return m_type == NodeType::Element; }
	bool is_text() const { return m_type == NodeType::Text; }
	bool is_document() const { return m_type == NodeType::Document; }
	inline Element *as_element();
	inline const Element *as_element() const;
	inline Text *as_text();
	inline const Text *as_text() const;

	Document *document() const { return m_doc; }
	Node *parent() const { return m_parent; }
	Element *parent_element() const;
	size_t child_count() const { return m_children.size(); }
	Node *child(size_t i) const { return m_children[i].node; }
	const std::vector<Link> &links() const { return m_children; }
	//! Its place among its parent's children.
	size_t index() const { return m_index; }
	Node *first_child() const { return m_children.empty() ? nullptr : m_children.front().node; }
	Node *last_child() const { return m_children.empty() ? nullptr : m_children.back().node; }
	Node *next_sibling() const;
	Node *previous_sibling() const;
	Element *first_element_child() const;
	Element *last_element_child() const;
	Element *next_element_sibling() const;
	Element *previous_element_sibling() const;
	//! Whether it is in its document's tree.
	bool connected() const;
	//! Whether n is this node or inside it.
	bool contains(const Node *n) const;
	//! The texts inside it, in order.
	std::string text_content() const;

	//! Its JavaScript object (not counted).
	JSValue wrapper() const { return m_wrapper; }
	std::vector<Listener> &listeners() { return m_listeners; }

	//! The references it holds, reported to the collector.
	virtual void mark(JSRuntime *rt, JS_MarkFunc *mark) const;
	//! The references it holds freed, from its wrapper's finalizer.
	virtual void release(JSRuntime *rt);

protected:
	Node(Document *doc, NodeType type);

private:
	friend class Dom;
	friend class Document;
	Document *m_doc;
	NodeType m_type;
	JSValue m_wrapper = JS_UNDEFINED;
	Node *m_parent = nullptr;
	JSValue m_parent_value = JS_UNDEFINED;
	size_t m_index = 0;
	std::vector<Link> m_children;
	std::vector<Listener> m_listeners;
	Node *m_all_prev = nullptr, *m_all_next = nullptr;
	static inline std::atomic<long> s_live { 0 };
	static inline std::mutex s_all_lock;
	static inline Node *s_all = nullptr;
};

class Text : public Node
{
public:
	Text(Document *doc, std::string data) : Node(doc, NodeType::Text), m_data(std::move(data)) {}
	~Text() override;

	const std::string &data() const { return m_data; }

	//! Its box among its parent's, kept across layouts (null while it has none).
	LayoutNode *layout = nullptr;

private:
	friend class Dom;
	std::string m_data;
};

//! An attribute as set.
struct Attribute
{
	Name name;
	std::string value;
};

//! What a text field or a list holds beyond its attributes.
struct FormState
{
	std::string value;
	std::string committed;     //!< the value when it took the focus: a change event when it leaves different
	size_t sel_start = 0;
	size_t sel_end = 0;
	int selected_index = -1;   //!< a list's choice; -1 until set
	bool dirty_value = false;  //!< set by script or typing, no longer the value attribute's
	bool checked = false;
	bool dirty_checked = false;
	//! How far a field's text is scrolled to keep its caret in view.
	double scroll_x = 0;
	double scroll_y = 0;
};

class Element : public Node
{
public:
	Element(Document *doc, Name tag, Namespace ns);
	~Element() override;

	Name tag() const { return m_tag; }
	Namespace ns() const { return m_ns; }
	bool is(Name tag) const { return m_tag == tag && m_ns == Namespace::Html; }
	Name id() const { return m_id; }
	const std::vector<Name> &classes() const { return m_classes; }
	bool has_class(Name c) const;
	const std::vector<Attribute> &attributes() const { return m_attrs; }
	const std::string *attribute(Name n) const;
	bool has_attribute(Name n) const { return attribute(n) != nullptr; }
	//! An attribute as a number, or a default when it is missing or not a number.
	double number_attribute(Name n, double fallback) const;

	//! Its inline style as declared: (dashed name, value), in order.
	const std::vector<std::pair<std::string, std::string>> &inline_style() const { return m_inline; }

	bool disabled() const { return has_attribute(n::disabled); }
	//! Whether it can take the focus: a form control or an element with a tabindex, not disabled.
	bool focusable() const;

	// state the selectors and the window read
	bool hovered = false;
	bool focused = false;
	bool focus_visible = false;  //!< focused as :focus-visible matches (a text field, or by the keys)
	bool modal = false;
	bool open_dialog = false;

	FormState form;
	double scroll_x = 0;
	double scroll_y = 0;
	//! The scrollbars its last layout gave it room for: a region starts from them, as Firefox's does.
	double last_bar_x = 0;
	double last_bar_y = 0;

	//! Its computed style, and its ::after's where it has content; null while it is not shown.
	std::shared_ptr<const ComputedStyle> style;
	std::shared_ptr<const ComputedStyle> after_style;
	std::shared_ptr<const ComputedStyle> before_style;
	//! Its style computed again at the next update (style_dirty), with what is under it (style_subtree: a selector
	//! reads the change from an ancestor); style_below: something inside it is marked.
	bool style_dirty = true;
	bool style_subtree = false;
	bool style_below = false;
	//! Its box, kept across layouts until the box tree lets it go (null while it has none).
	LayoutNode *layout = nullptr;
	//! What the next layout does again here (docs/design.md §3.9): its box (layout_dirty), its box's children
	//! (kids_dirty); layout_below: something inside it is marked.
	bool layout_dirty = false;
	bool kids_dirty = false;
	bool layout_below = false;

	//! A canvas's drawing (its 2D context's commands and its pixels).
	std::unique_ptr<Canvas> canvas;

	void mark(JSRuntime *rt, JS_MarkFunc *mark) const override;
	void release(JSRuntime *rt) override;

	//! Objects made for it once and kept: its style, its class list, its child lists, its 2D context, the
	//! resize observers watching it.
	JSValue style_object = JS_UNDEFINED;
	JSValue class_list = JS_UNDEFINED;
	JSValue child_nodes = JS_UNDEFINED;
	JSValue children_list = JS_UNDEFINED;
	JSValue context = JS_UNDEFINED;
	std::vector<JSValue> observers;

private:
	friend class Dom;
	Name m_tag;
	Namespace m_ns;
	Name m_id = 0;
	std::vector<Name> m_classes;
	std::vector<Attribute> m_attrs;
	std::vector<std::pair<std::string, std::string>> m_inline;
};

class Document : public Node
{
public:
	explicit Document() : Node(this, NodeType::Document) {}

	Dom *dom() const { return m_dom; }
	Element *document_element() const { return m_html; }
	Element *head() const { return m_head; }
	Element *body() const { return m_body; }
	//! The element with the keys (the body when none has them).
	Element *active() const { return m_active.el ? m_active.el : m_body; }
	Element *hover() const { return m_hover.el; }
	Element *capture() const { return m_capture.el; }
	//! The modal dialogs, last on top.
	const std::vector<ElementRef> &top_layer() const { return m_top; }

	void mark(JSRuntime *rt, JS_MarkFunc *mark) const override;
	void release(JSRuntime *rt) override;

private:
	friend class Dom;
	Dom *m_dom = nullptr;
	Element *m_html = nullptr;
	Element *m_head = nullptr;
	Element *m_body = nullptr;
	ElementRef m_active;
	ElementRef m_hover;
	ElementRef m_capture;
	std::vector<ElementRef> m_top;
};

inline Element *Node::as_element()
{
	return is_element() ? static_cast<Element *>(this) : nullptr;
}

inline const Element *Node::as_element() const
{
	return is_element() ? static_cast<const Element *>(this) : nullptr;
}

inline Text *Node::as_text()
{
	return is_text() ? static_cast<Text *>(this) : nullptr;
}

inline const Text *Node::as_text() const
{
	return is_text() ? static_cast<const Text *>(this) : nullptr;
}

} // namespace qdom
