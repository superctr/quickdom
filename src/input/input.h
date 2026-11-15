// Copyright 2026 Ian Karlsson
//! \file
//! The window's input made into the DOM's events, with what a browser does by itself (design.md §3.7): hover and
//! the cursor, pointer and mouse events with capture, clicks and double clicks, the focus by press and by Tab, the
//! wheel scrolling regions, scrollbars' thumbs dragged, text fields edited, a list's popup, a range dragged, a
//! dialog's Escape, tooltips; all as Firefox on Linux behaves.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "quickjs.h"

namespace qdom
{

class Dom;
class Node;
class Element;
struct Commands;

//! The modifier keys held (QDOM_MOD_*).
enum : int
{
	MOD_SHIFT = 1,
	MOD_CTRL = 2,
	MOD_ALT = 4,
	MOD_META = 8,
};

enum class PointerKind
{
	Move,
	Down,
	Up,
};

class Input
{
public:
	explicit Input(Dom &dom);
	~Input();
	Input(const Input &) = delete;
	Input &operator=(const Input &) = delete;

	//! Every reference let go (before the context is freed).
	void release();

	void pointer(PointerKind kind, double x, double y, int button, int mods);
	//! The wheel turned by notches (fractions from a touchpad).
	void wheel(double x, double y, double dx, double dy, int mods);
	//! A key pressed or released; true when the page or the library used it.
	bool key(bool down, std::string_view key, std::string_view code, bool repeat, int mods);
	//! Text typed (after its key's keydown).
	void text(std::string_view s);
	//! The clipboard's text, answering the paste callback: to the field that asked, and to navigator.clipboard's
	//! readText.
	void paste(std::string_view s);
	//! navigator.clipboard's readText asks the host for the clipboard's text; false when it has none.
	bool read_clipboard();
	//! The pointer left the window.
	void leave();
	//! A drag from another program over the window (paths empty until it drops), out of it, or dropped: dragenter,
	//! dragover and dragleave as it moves between elements, drop where it lets go. True while a dragover or the drop
	//! was canceled: the page takes it.
	bool drag(int kind, double x, double y, const std::vector<std::string> &paths);
	void window_blur();
	void window_focus();
	bool window_focused() const { return m_window_focus; }

	//! The pointer's shape (a CSS cursor name).
	const std::string &cursor() const { return m_cursor; }
	//! Whether a button is held (setPointerCapture takes the pointer only then).
	bool pressed() const { return m_buttons != 0; }
	//! Whether the keys were the last input (for :focus-visible).
	bool keyboard() const { return m_keyboard; }

	//! When the library's own timer is due (the tooltip, the caret's blink), or a negative number.
	double next_due() const;
	//! The time moved on: a tooltip shown when it is due.
	void advance();
	//! Whether the caret shows now (it blinks while the window has the focus).
	bool caret_on() const;
	//! Whether the blink or the overlay changed since the last paint.
	bool needs_paint() const;
	void painted();
	//! The caret shown again from now (an edit, a move, a script setting a selection).
	void caret_reset();
	//! What the library draws over the page: a list's popup, a tooltip.
	void overlay(Commands &out);
	//! A node left the tree: what the input held inside it let go.
	void gone(Node *n);
	//! The elements it holds, each with what it holds it as (Dom::retained_json).
	void each_ref(const std::function<void(const char *, const Element *)> &f) const;

	//! A click at a target with its defaults (a checkbox toggled, a radio checked, a label's control clicked), as
	//! the pointer's (detail: the press count), the keys' (detail 0) or a script's click() (detail 0, not trusted)
	//! makes it; false when canceled.
	bool click(Element *target, int detail, bool trusted);
	//! A text field's caret kept in view.
	void reveal(Element *el);
	//! A script set a field's value: its history is forgotten.
	void value_set(Element *el);
	//! The cursor found again from the hover and the styles (after a frame's restyle), the host told of a change.
	void refresh_cursor();
	//! After a frame's layout: the hover found again under a still pointer, and the cursor.
	void frame();

private:
	struct Ref
	{
		Element *el = nullptr;
		JSValue value = JS_UNDEFINED;
	};
	struct Popup
	{
		Ref select;
		double x = 0, y = 0, w = 0, h = 0;
		double row = 0, scroll = 0;
		int over = -1;
		double opened = 0;
		int face = 0;
		double size = 0;
	};
	struct Bar
	{
		Ref el;
		bool horizontal = false;
		double from = 0, scroll = 0, per = 1;
	};
	struct Snapshot
	{
		std::string value;
		size_t start = 0, end = 0;
	};

	Dom &m_dom;
	double m_x = -1, m_y = -1;
	int m_mods = 0;
	int m_buttons = 0;           //!< DOM's buttons bits held
	bool m_mouse_blocked = false;  //!< a canceled pointerdown: no mouse events until the buttons are up
	Ref m_press;                 //!< the first button's press's target, for the click
	int m_press_button = -1;
	int m_last_button = -1;
	double m_last_at = -1e9, m_last_x = 0, m_last_y = 0;
	int m_count = 0;             //!< the presses counted for detail
	Ref m_drag_text;             //!< a text field selected by the pointer
	int m_drag_unit = 0;         //!< 0 characters, 1 words, 2 lines
	size_t m_drag_from = 0, m_drag_to = 0;  //!< the word or line first pressed
	Ref m_drag_range;
	Bar m_bar;
	bool m_bar_on = false;
	Popup m_popup;
	bool m_popup_on = false;
	std::string m_tip;
	double m_tip_x = 0, m_tip_y = 0;
	Ref m_tip_el;                //!< the element whose title shows (or waits to)
	double m_tip_due = -1;
	bool m_tip_on = false;
	bool m_tip_suppressed = false;  //!< after a press, a key or the wheel: no tooltip until the pointer leaves
	Ref m_paste;                 //!< the field that asked for the clipboard
	Ref m_drop;                  //!< the element a drag from outside is over
	bool m_reading = false;      //!< navigator.clipboard's readText asked for it
	double m_caret_at = 0;
	bool m_painted_caret = true;
	bool m_overlay_dirty = false;
	bool m_window_focus = true;
	bool m_keyboard = false;
	bool m_key_blocked = false;  //!< the last keydown canceled: its text is not typed
	bool m_space_click = false;  //!< a Space keydown on a button or checkbox, clicked at its keyup
	double m_goal_x = -1;        //!< the x Up and Down keep to in a textarea
	std::string m_cursor = "default";
	struct History
	{
		Ref el;
		std::vector<Snapshot> undo, redo;
		bool typing = false;
	};
	History m_history;

	void set(Ref &r, Element *el);

	// the pointer
	Element *under(double x, double y);
	void set_hover(Element *el);
	void update_cursor(Element *el);
	void boundary(Element *from, Element *to);
	bool fire_pointer(Element *target, const char *type, int button, int detail, bool cancelable = true,
		bool bubbles = true, Element *related = nullptr);
	bool fire_wheel(Element *target, double dx, double dy);
	bool fire_drag(Element *target, const char *type, bool cancelable, const std::vector<std::string> &paths,
		Element *related = nullptr);
	void down(double x, double y, int button);
	void up(double x, double y, int button);
	void move(double x, double y);
	bool bar_at(Element *target, double x, double y);
	void press_defaults(Element *target, int button);
	void tip_restart();
	void tip_hide();

	// text fields
	Element *field() const;
	bool edit_key(Element *f, std::string_view key, int mods);
	//! The selection replaced by text, with beforeinput and input; false when nothing changed.
	bool replace(Element *f, std::string_view s, const char *input_type, size_t from, size_t to);
	void select(Element *f, size_t anchor, size_t focus);
	size_t index_at(Element *f, double x, double y);
	void remember(Element *f, bool typing);
	void history(Element *f, bool redo);
	bool field_wheel(Element *f, double dy);

	// controls
	void range_to(Element *r, double x);
	bool range_step(Element *r, std::string_view key);
	void choose(Element *sel, int index);
	bool list_key(Element *sel, std::string_view key, int mods);
	void open_popup(Element *sel);
	void close_popup();
	int popup_index(double x, double y) const;
	void popup_scroll_to(int index);
	bool popup_key(std::string_view key);
	void focus_next(bool back);
	void change(Element *el);
};

} // namespace qdom
