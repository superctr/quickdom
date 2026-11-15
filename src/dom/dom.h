// Copyright 2026 Ian Karlsson
//! \file
//! The library's instance in a JavaScript context: the document and the window, their JavaScript classes, the
//! fonts and the cascade, the layout, and the time (timers and animation frames the host moves on).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "quickjs.h"

#include "core/names.h"
#include "css/selectors.h"
#include "css/style.h"
#include "dom/events.h"
#include "dom/node.h"
#include "layout/layout.h"
#include "paint/picture.h"
#include "text/fonts.h"

struct qdom_options;

namespace qdom
{

class Input;
class Layout;
class ResizeObserver;

//! The JavaScript classes of one instance, numbered in its runtime.
struct Classes
{
	JSClassID ref = 0;          //!< the object binding functions keep to find the instance
	JSClassID element = 0;
	JSClassID text = 0;
	JSClassID document = 0;
	JSClassID event = 0;
	JSClassID style = 0;        //!< CSSStyleDeclaration
	JSClassID token_list = 0;   //!< DOMTokenList (classList)
	JSClassID node_list = 0;    //!< NodeList and HTMLCollection, live
	JSClassID computed = 0;     //!< getComputedStyle's answer
	JSClassID observer = 0;     //!< ResizeObserver
	JSClassID context2d = 0;    //!< CanvasRenderingContext2D
};

//! A border box in the viewport.
struct Rect
{
	double x = 0, y = 0, w = 0, h = 0;
};

//! A function the bindings call with the instance.
using BindingFn = JSValue (*)(class Dom &dom, JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

class Dom
{
public:
	Dom(JSContext *ctx, const qdom_options *options);
	~Dom();
	Dom(const Dom &) = delete;
	Dom &operator=(const Dom &) = delete;

	JSContext *ctx() const { return m_ctx; }
	JSRuntime *rt() const { return m_rt; }
	Document *document() const { return m_doc; }
	const Classes &classes() const { return m_cls; }
	//! The object every binding function keeps to find the instance.
	JSValue ref() const { return m_ref; }
	//! The options' flags (QDOM_NO_TIMERS).
	int flags() const { return m_flags; }

	Names names;
	FontSet fonts;
	StyleEngine styles;
	//! The selector lists the scripts have asked with, by their text.
	std::unordered_map<std::string, std::vector<Selector>> selector_cache;

	//! A line logged through the host's log function (stderr without one).
	void log(std::string_view text) const;
	//! The pending exception logged and cleared.
	void report_exception();
	//! The jobs QuickJS has pending (promises) run.
	void run_jobs();

	// ---------------------------------------------------------------- nodes

	//! The document and its html, head and body, made once the classes are registered.
	void create_document();

	//! A new element or text and its wrapper; the reference returned is the caller's.
	JSValue new_element(std::string_view tag, Namespace ns);
	JSValue new_text(std::string_view data);
	//! The node of a wrapper, or null when the value is not one of this instance's nodes.
	Node *node_of(JSValueConst v) const;
	Element *element_of(JSValueConst v) const;

	//! A child put into a parent before a sibling (null: last), taken from where it was. False with a message for
	//! an insertion the DOM forbids (a node into itself, a document anywhere).
	bool insert(Node *parent, Node *child, Node *before, const char **error = nullptr);
	//! A child taken from its parent.
	void remove(Node *child);
	void set_data(Text *t, std::string data);
	//! Its children replaced by one text (none for an empty one).
	void set_text_content(Node *n, std::string_view text);
	void set_attribute(Element *el, Name name, std::string value);
	void remove_attribute(Element *el, Name name);
	//! One property of its inline style (dashed name; an empty value removes it).
	void set_inline(Element *el, std::string_view name, std::string_view value);
	//! Its whole inline style from text ("a: b; c: d").
	void set_inline_text(Element *el, std::string_view text);
	//! A form control's value, as script sets it.
	void set_value(Element *el, std::string value);
	//! A range's value: a value clamped to its min and max and put on its step, or its own.
	std::string range_value(const Element *el, std::string_view value) const;
	std::string range_value(const Element *el) const;
	//! A text field's value and selection as the user edits them (its history kept).
	void set_field(Element *el, std::string value, size_t anchor, size_t focus);
	void set_checked(Element *el, bool on);
	void set_selected_index(Element *el, int index);
	//! A list's options, in order (an optgroup's too).
	std::vector<Element *> options(const Element *select) const;
	int selected_index(const Element *select) const;

	// ---------------------------------------------------------------- state

	//! The focus moved (null: to the body), with blur, focus and a text field's change.
	void set_focus(Element *el);
	void set_hover(Element *el);
	void set_capture(Element *el);
	//! A dialog put into the top layer, or taken out.
	void set_modal(Element *dialog, bool on);
	//! An open dialog closed, with its close event.
	void close_dialog(Element *dialog);
	void set_scroll(Element *el, double x, double y);

	// ---------------------------------------------------------------- updating

	//! Its style computed again at the next update, and as far as the change reaches (StyleEngine::Reach).
	void invalidate_style(Element *el, StyleEngine::Reach reach);
	//! Its style and what is under it; for a change a sibling's selector may read, its parent's and what is under it.
	void invalidate_style(Element *el, bool siblings = false);
	//! The layout of what a change reached again at the next update (docs/design.md §3.9): a node's element's box (its
	//! box's children too, kids), or the box of the replaced element it is in (an svg's items only redraw).
	void relayout(Node *at, bool kids);
	//! Everything laid out anew (the fonts, the top layer).
	void relayout_all();
	//! A font added or a generic family named: every style computed again (the faces they chose), everything laid out.
	void fonts_changed();
	void invalidate_paint() { m_paint = true; }
	//! The stylesheets read again (a style element changed).
	void invalidate_sheets() { m_sheets_dirty = true; m_paint = true; }
	//! Styles and layout brought up to date, as a read of a box does.
	void update();
	bool needs_paint() const;
	void painted();
	//! An element's box, kept across layouts (null while it has none).
	LayoutNode *layout_of(const Element *el);
	//! The last layout's boxes (null before the first).
	Layout *layout_tree() const { return m_layout.get(); }
	//! A number for an image the host is given (a canvas's pixels).
	int next_image_id() { return ++m_images; }
	//! Images are handed over again when this changes (a new GL context has none).
	uint32_t image_generation() const { return m_image_generation; }
	void renew_images();

	// ---------------------------------------------------------------- pictures

	//! The picture an <img> names by its src (null without one).
	Picture *picture_of(const Element *img);
	//! The host's answer for an src: its bytes (null: none), decoded now.
	void picture_bytes(const std::string &src, const void *data, size_t size);
	//! Its border box in the viewport, its ancestors' scrolling taken off (zero when it has no box).
	Rect client_rect(const Element *el);
	//! The host's stylesheets, before the document's style elements.
	void add_sheet(std::string css);
	//! Every element's box under the body as JSON: [[path, left, top, width, height], ...], a tenth of a pixel.
	std::string boxes_json();
	//! The nodes of this document alive though out of its tree, after a collection, as JSON: each subtree cut off
	//! ({ nodes, root, held }), the most nodes first, with the references this library holds into it ({ by, el });
	//! a subtree with none is held by a script (a variable, a closure, a property).
	std::string retained_json();

	// ---------------------------------------------------------------- the window

	void set_viewport(double width, double height, double scale);
	double width() const { return m_width; }
	double height() const { return m_height; }
	double scale() const { return m_scale; }
	//! The global object, the window.
	JSValue global() const { return m_global; }
	std::vector<Listener> &window_listeners() { return m_window_listeners; }

	// ---------------------------------------------------------------- what the work took

	//! Counts and times since the document was made or they were reset (qdom_stats): frames, restyles and the styles
	//! they computed, layouts and the boxes they laid out, paints; each one's time in all and its slowest.
	struct Stats
	{
		uint64_t frames = 0, restyles = 0, styles = 0, layouts = 0, boxes = 0, paints = 0;
		double frame_ms = 0, style_ms = 0, layout_ms = 0, paint_ms = 0;
		double frame_max = 0, style_max = 0, layout_max = 0, paint_max = 0;
	};
	Stats stats;

	// ---------------------------------------------------------------- time

	double now() const { return m_now; }
	uint32_t set_timer(JSValueConst fn, double delay, bool repeat, int argc, JSValueConst *argv);
	void clear_timer(uint32_t id);
	//! The time moved on: the timers due run, in order; once the input has been still a while after some, a
	//! collection (what a burst of input left is freed then, not in the middle of the next).
	void advance(double now);
	//! Input came: a collection is due once it has been still a while.
	void input_came() { m_gc_due = true; m_input_at = m_now; }
	//! When the next timer is due, or a negative number when none is set.
	double next_timer() const;
	uint32_t request_frame(JSValueConst fn);
	void cancel_frame(uint32_t id);
	bool frame_requested() const { return !m_frames.empty() || m_picture_work; }
	//! A frame: the animation frames asked for, the layout, the resize observers. True when it needs drawing.
	bool frame(double now);

	// ---------------------------------------------------------------- input

	//! The window's input made into events (input/input.h).
	Input &input() { return *m_input; }
	const Input &input() const { return *m_input; }
	//! The host told: the pointer's shape, text for the clipboard; the clipboard's text asked for (false when the
	//! host has no clipboard).
	void host_cursor(const std::string &name) const;
	void host_copy(const std::string &text) const;
	bool host_paste() const;
	//! The window's clipboard (quickdom_pugl's), for a host whose options have none; null functions: none.
	void set_window_clipboard(void (*copy)(void *, const char *, size_t), void (*paste)(void *), void *user);
	//! The document's title (its <title>'s text).
	std::string title() const;

	// ---------------------------------------------------------------- events

	//! An event dispatched at a target (a node, or the window when target is null); false when a listener
	//! prevented its default.
	bool dispatch(Node *target, JSValueConst event);
	//! A new event object of a type; its fields filled by the caller through event_of().
	JSValue new_event(std::string_view type, bool bubbles, bool cancelable);
	Event *event_of(JSValueConst v) const;
	//! A simple event fired at a target (focus, blur, change, input, close, resize...).
	bool fire(Node *target, std::string_view type, bool bubbles, bool cancelable = false);

	// ---------------------------------------------------------------- bindings

	//! A function of the bindings registered: its number, for a function object made with make_function.
	int bind(BindingFn fn);
	JSValue make_function(const char *name, int length, BindingFn fn);
	BindingFn binding(int n) const { return m_fns[size_t(n)]; }
	//! The prototypes of the node classes, and the others made at install.
	JSValue proto_node = JS_UNDEFINED;
	std::vector<ResizeObserver *> observers;

private:
	struct Timer
	{
		uint32_t id;
		double due;
		double interval;
		bool repeat;
		uint64_t order;
		JSValue fn;
		std::vector<JSValue> args;
	};
	struct FrameRequest
	{
		uint32_t id;
		JSValue fn;
	};

	void wrap(Node *node, JSClassID cls);
	void gone(Node *n);
	void detach(Node *child);
	void restyle(Element *el, const ComputedStyle *parent, bool subtree, bool inherit);
	//! A focus come or gone: the element's style, and its ancestors' where :focus-within is read.
	void focus_changed(Element *el);
	void read_sheets();
	void set_ref(ElementRef &ref, Element *el);
	void notify_observers();
	void want_picture(Element *img);
	//! At a frame: the pictures wanted asked for, the load and error events of those settled.
	void ask_pictures();
	void picture_events();

	JSContext *m_ctx;
	JSRuntime *m_rt;
	Classes m_cls;
	Document *m_doc = nullptr;
	JSValue m_doc_value = JS_UNDEFINED;
	JSValue m_ref = JS_UNDEFINED;
	JSValue m_global = JS_UNDEFINED;
	void (*m_log)(void *, const char *) = nullptr;
	void *m_log_user = nullptr;
	void (*m_cursor_fn)(void *, const char *) = nullptr;
	void (*m_copy_fn)(void *, const char *, size_t) = nullptr;
	void (*m_paste_fn)(void *) = nullptr;
	void (*m_window_copy)(void *, const char *, size_t) = nullptr;
	void (*m_window_paste)(void *) = nullptr;
	void *m_window_user = nullptr;
	void (*m_resource_fn)(void *, const char *) = nullptr;
	std::unordered_map<std::string, Picture> m_pictures;
	std::vector<ElementRef> m_picture_waiting;  //!< imgs whose load or error event is due once their picture settles
	bool m_picture_work = false;
	int m_pictures_made = 0;
	uint32_t m_image_generation = 1;
	std::unique_ptr<Input> m_input;
	bool m_gc_due = false;
	double m_input_at = 0;
	int m_flags = 0;
	std::vector<BindingFn> m_fns;

	double m_width = 1600, m_height = 1000, m_scale = 1;
	bool m_style_dirty = true;
	bool m_layout_dirty = true;
	bool m_sheets_dirty = true;
	bool m_paint = true;
	int m_images = 0;
	std::unique_ptr<Layout> m_layout;
	//! QDOM_CHECK_LAYOUT: each layout checked against one made anew (docs/design.md §3.9)
	bool m_check_layout = false;
	std::unique_ptr<Layout> m_check;
	//! A style changed: its box laid out again, or only drawn again when it differs only in what is drawn.
	void styled(Element *el, const StylePtr &was, const StylePtr &was_before, const StylePtr &was_after);
	void check_layout(const std::unordered_map<const Element *, Layout::Bars> &bars);
	void check_styles();
	std::vector<std::string> m_host_sheets;
	std::vector<std::string> m_sheet_texts;

	double m_now = 0;
	uint32_t m_next_id = 1;
	uint64_t m_timer_order = 0;
	uint32_t m_running_timer = 0;
	bool m_running_cleared = false;
	std::vector<Timer> m_timers;
	std::vector<FrameRequest> m_frames;
	std::vector<Listener> m_window_listeners;

	friend void install(Dom &dom);
	friend class Layout;
};

//! Every class, prototype and global of the library put into the instance's context.
void install(Dom &dom);

} // namespace qdom
