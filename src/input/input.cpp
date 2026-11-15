// Copyright 2026 Ian Karlsson
#include "input/input.h"

#include <algorithm>
#include <cmath>

#include "css/values.h"
#include "quickdom/quickdom.h"
#include "dom/dom.h"
#include "input/field.h"
#include "layout/layout.h"
#include "paint/commands.h"
#include "paint/paint.h"

namespace qdom
{

namespace
{

constexpr double CLICK_MS = 500;      // presses this close count on (detail, dblclick)
constexpr double CLICK_PX = 6;
constexpr double WHEEL_PX = 48;       // a wheel's notch
constexpr double TIP_MS = 700;        // the pointer at rest before a tooltip shows
constexpr double BLINK_MS = 530;      // the caret on, then off
constexpr double POPUP_GRACE_MS = 300;
constexpr size_t HISTORY = 100;       // a field's undo steps

// the colours the scrollbars and the selection take
const Color HIGHLIGHT { 43, 93, 134, 1 };  // sel
const Color PANE { 38, 41, 46, 1 };        // pane2
const Color LINE { 69, 73, 80, 1 };        // line2
const Color TEXT { 217, 219, 222, 1 };     // text

// the boundary events: out, leave, over, enter
const char *const BOUNDARY_POINTER[] = { "pointerout", "pointerleave", "pointerover", "pointerenter" };
const char *const BOUNDARY_MOUSE[] = { "mouseout", "mouseleave", "mouseover", "mouseenter" };

//! A node held while listeners run (they may take it out of the tree and drop every other reference).
class Hold
{
public:
	Hold(JSContext *ctx, Element *el) : m_ctx(ctx), m_value(el ? JS_DupValue(ctx, el->wrapper()) : JS_UNDEFINED) {}
	~Hold() { JS_FreeValue(m_ctx, m_value); }
	Hold(const Hold &) = delete;
	Hold &operator=(const Hold &) = delete;

private:
	JSContext *m_ctx;
	JSValue m_value;
};

int button_bit(int button)
{
	switch (button)
	{
	case 0: return 1;
	case 1: return 4;
	case 2: return 2;
	default: return button > 0 && button < 16 ? 1 << button : 0;
	}
}

bool form_control(const Element &e)
{
	return e.is(n::button) || e.is(n::input) || e.is(n::select) || e.is(n::textarea);
}

//! Whether an element is a disabled control or inside one.
bool disabled_in(const Element *el)
{
	for (const Element *e = el; e; e = e->parent_element())
		if (form_control(*e) && e->disabled())
			return true;
	return false;
}

Element *common_ancestor(Element *a, Element *b)
{
	for (Element *e = b; e; e = e->parent_element())
		if (e->contains(a))
			return e;
	return nullptr;
}

bool checked_of(const Element &el)
{
	return el.form.dirty_checked ? el.form.checked : el.has_attribute(n::checked);
}

std::string collapsed(const std::string &s)
{
	std::string out;
	bool space = false;
	for (char c : s)
	{
		if (c == ' ' || c == '\n' || c == '\t' || c == '\r')
		{
			space = !out.empty();
			continue;
		}
		if (space)
			out += ' ';
		space = false;
		out += c;
	}
	return out;
}

std::string lower_key(std::string_view key)
{
	return key.size() == 1 ? lower(key) : std::string(key);
}

template <typename F>
void each_element(Node *root, F &&f)
{
	for (const Link &l : root->links())
		if (Element *e = l.node->as_element())
		{
			f(e);
			each_element(e, f);
		}
}

} // namespace

Input::Input(Dom &dom) : m_dom(dom)
{
}

Input::~Input()
{
	release();
}

void Input::set(Ref &r, Element *el)
{
	if (r.el == el)
		return;
	JSValue old = r.value;
	r.el = el;
	r.value = el ? JS_DupValue(m_dom.ctx(), el->wrapper()) : JS_UNDEFINED;
	JS_FreeValue(m_dom.ctx(), old);
}

void Input::release()
{
	for (Ref *r : { &m_press, &m_drag_text, &m_drag_range, &m_bar.el, &m_popup.select, &m_tip_el, &m_history.el,
		&m_paste, &m_drop })
		set(*r, nullptr);
	m_popup_on = m_bar_on = m_tip_on = false;
}

void Input::each_ref(const std::function<void(const char *, const Element *)> &f) const
{
	const std::pair<const char *, const Ref *> refs[] = { { "input.press", &m_press }, { "input.drag_text", &m_drag_text },
		{ "input.drag_range", &m_drag_range }, { "input.bar", &m_bar.el }, { "input.popup", &m_popup.select },
		{ "input.tip", &m_tip_el }, { "input.history", &m_history.el }, { "input.paste", &m_paste }, { "input.drop", &m_drop } };
	for (const auto &[name, r] : refs)
		if (r->el)
			f(name, r->el);
}

void Input::gone(Node *n)
{
	// the press moves to what is left of its ancestors, as the click's common ancestor is found in the tree
	if (m_press.el && n->contains(m_press.el))
		set(m_press, n->parent_element());
	for (Ref *r : { &m_drag_text, &m_drag_range, &m_history.el, &m_paste, &m_drop })
		if (r->el && n->contains(r->el))
			set(*r, nullptr);
	if (m_bar.el.el && n->contains(m_bar.el.el))
	{
		set(m_bar.el, nullptr);
		m_bar_on = false;
	}
	if (m_popup.select.el && n->contains(m_popup.select.el))
		close_popup();
	if (m_tip_el.el && n->contains(m_tip_el.el))
	{
		tip_hide();
		set(m_tip_el, nullptr);
	}
}

// ---------------------------------------------------------------- the pointer

Element *Input::under(double x, double y)
{
	return hit(m_dom, x, y);
}

bool Input::fire_pointer(Element *target, const char *type, int button, int detail, bool cancelable, bool bubbles,
	Element *related)
{
	JSContext *ctx = m_dom.ctx();
	JSValue v = m_dom.new_event(type, bubbles, cancelable);
	Event *e = m_dom.event_of(v);
	e->trusted = true;
	e->composed = true;
	e->client_x = e->screen_x = m_x;
	e->client_y = e->screen_y = m_y;
	e->button = button;
	e->buttons = m_buttons;
	e->detail = detail;
	e->shift = m_mods & MOD_SHIFT;
	e->ctrl = m_mods & MOD_CTRL;
	e->alt = m_mods & MOD_ALT;
	e->meta = m_mods & MOD_META;
	if (related)
		e->related_target = JS_DupValue(ctx, related->wrapper());
	const bool ok = m_dom.dispatch(target, v);
	JS_FreeValue(ctx, v);
	return ok;
}

bool Input::fire_wheel(Element *target, double dx, double dy)
{
	JSValue v = m_dom.new_event("wheel", true, true);
	Event *e = m_dom.event_of(v);
	e->trusted = true;
	e->composed = true;
	e->client_x = e->screen_x = m_x;
	e->client_y = e->screen_y = m_y;
	e->buttons = m_buttons;
	e->delta_x = dx;
	e->delta_y = dy;
	e->delta_mode = 0;
	e->shift = m_mods & MOD_SHIFT;
	e->ctrl = m_mods & MOD_CTRL;
	e->alt = m_mods & MOD_ALT;
	e->meta = m_mods & MOD_META;
	const bool ok = m_dom.dispatch(target, v);
	JS_FreeValue(m_dom.ctx(), v);
	return ok;
}

bool Input::fire_drag(Element *target, const char *type, bool cancelable, const std::vector<std::string> &paths,
	Element *related)
{
	JSContext *ctx = m_dom.ctx();
	JSValue v = m_dom.new_event(type, true, cancelable);
	Event *e = m_dom.event_of(v);
	e->trusted = true;
	e->composed = true;
	e->client_x = e->screen_x = m_x;
	e->client_y = e->screen_y = m_y;
	e->shift = m_mods & MOD_SHIFT;
	e->ctrl = m_mods & MOD_CTRL;
	e->alt = m_mods & MOD_ALT;
	e->meta = m_mods & MOD_META;
	if (related)
		e->related_target = JS_DupValue(ctx, related->wrapper());
	JSValue q = JS_GetPropertyStr(ctx, m_dom.global(), "__qdom");
	JSValue make = JS_GetPropertyStr(ctx, q, "transfer");
	JSValue list = JS_NewArray(ctx);
	for (uint32_t i = 0; i < paths.size(); i++)
		JS_SetPropertyUint32(ctx, list, i, JS_NewStringLen(ctx, paths[i].data(), paths[i].size()));
	JSValue args[2] = { list, JS_TRUE };
	JSValue transfer = JS_Call(ctx, make, JS_UNDEFINED, 2, args);
	JS_FreeValue(ctx, list);
	JS_FreeValue(ctx, make);
	JS_FreeValue(ctx, q);
	if (JS_IsException(transfer))
		JS_FreeValue(ctx, JS_GetException(ctx));
	else
		e->data_transfer = transfer;
	const bool ok = m_dom.dispatch(target, v);
	JS_FreeValue(ctx, v);
	return ok;
}

//! The hover moved from one element to another: out and leave where it was, over and enter where it is.
void Input::boundary(Element *from, Element *to)
{
	Hold hf(m_dom.ctx(), from), ht(m_dom.ctx(), to);
	std::vector<Element *> was, now;
	for (Element *e = from; e; e = e->parent_element())
		was.push_back(e);
	for (Element *e = to; e; e = e->parent_element())
		now.push_back(e);
	std::vector<Element *> left, entered;
	for (Element *e : was)
		if (std::find(now.begin(), now.end(), e) == now.end())
			left.push_back(e);
	for (auto it = now.rbegin(); it != now.rend(); ++it)
		if (std::find(was.begin(), was.end(), *it) == was.end())
			entered.push_back(*it);
	std::vector<JSValue> held;
	for (Element *e : left)
		held.push_back(JS_DupValue(m_dom.ctx(), e->wrapper()));
	for (Element *e : entered)
		held.push_back(JS_DupValue(m_dom.ctx(), e->wrapper()));
	// the pointer's events, then the mouse's
	for (const char *const *k : { BOUNDARY_POINTER, BOUNDARY_MOUSE })
		if (from && from->connected())
		{
			fire_pointer(from, k[0], k == BOUNDARY_POINTER ? -1 : 0, 0, true, true, to);
			for (Element *e : left)
				if (e->connected())
					fire_pointer(e, k[1], k == BOUNDARY_POINTER ? -1 : 0, 0, false, false, to);
		}
	m_dom.set_hover(to && to->connected() ? to : nullptr);
	for (const char *const *k : { BOUNDARY_POINTER, BOUNDARY_MOUSE })
		if (to && to->connected())
		{
			fire_pointer(to, k[2], k == BOUNDARY_POINTER ? -1 : 0, 0, true, true, from);
			for (Element *e : entered)
				if (e->connected())
					fire_pointer(e, k[3], k == BOUNDARY_POINTER ? -1 : 0, 0, false, false, from);
		}
	for (JSValue v : held)
		JS_FreeValue(m_dom.ctx(), v);
}

void Input::set_hover(Element *el)
{
	Element *was = m_dom.document()->hover();
	if (was == el)
		return;
	boundary(was, el);
}

void Input::refresh_cursor()
{
	Element *el = m_dom.document()->capture();
	if (!el)
		el = m_dom.document()->hover();
	update_cursor(el);
}

void Input::update_cursor(Element *el)
{
	std::string c = "default";
	if (m_popup_on)
		el = nullptr;
	// the cursor is inherited: the element's own computed value (an svg's item may have none: its parent's)
	for (Element *e = el; e; e = e->parent_element())
	{
		if (!e->style)
			continue;
		const std::string &k = e->style->cursor;
		if (k != "auto")
		{
			// a list of images with a keyword last: the keyword
			const size_t comma = k.rfind(',');
			c = comma == std::string::npos ? k : std::string(trim(std::string_view(k).substr(comma + 1)));
		}
		else if (is_text_field(*e) && !e->disabled())
			c = "text";
		break;
	}
	if (c == m_cursor)
		return;
	m_cursor = c;
	m_dom.host_cursor(m_cursor);
}

static Element *title_of(Element *el)
{
	for (Element *e = el; e; e = e->parent_element())
		if (const std::string *t = e->attribute(n::title); t && !t->empty())
			return e;
	return nullptr;
}

void Input::tip_restart()
{
	if (m_tip_on || m_tip_suppressed || m_buttons || m_popup_on || !m_tip_el.el)
		return;
	m_tip_due = m_dom.now() + TIP_MS;
}

void Input::tip_hide()
{
	if (m_tip_on)
		m_overlay_dirty = true;
	m_tip_on = false;
	m_tip_due = -1;
}

void Input::move(double x, double y)
{
	// the same place again (or the frame looking again): the hover and the cursor only
	const bool same = x == m_x && y == m_y;
	m_x = x;
	m_y = y;
	if (m_popup_on)
	{
		const int i = popup_index(x, y);
		if (i >= 0 && i != m_popup.over)
		{
			m_popup.over = i;
			m_overlay_dirty = true;
		}
		return;
	}
	if (m_bar_on)
	{
		if (Element *e = m_bar.el.el)
		{
			const double v = m_bar.scroll + ((m_bar.horizontal ? x : y) - m_bar.from) * m_bar.per;
			m_dom.set_scroll(e, m_bar.horizontal ? v : e->scroll_x, m_bar.horizontal ? e->scroll_y : v);
		}
		return;
	}
	Element *capture = m_dom.document()->capture();
	Element *target = capture ? capture : under(x, y);
	Hold hold(m_dom.ctx(), target);
	if (Element *f = m_drag_text.el)
	{
		const size_t i = index_at(f, x, y);
		const std::string v = field_value(*f);
		if (m_drag_unit == 0)
			select(f, f->form.sel_start, i);
		else
		{
			size_t a = i, z = i;
			if (m_drag_unit == 1)
				word_at(v, i, a, z);
			else
			{
				a = 0;
				z = v.size();
			}
			if (i < m_drag_from)
				select(f, m_drag_to, a);
			else
				select(f, m_drag_from, std::max(z, m_drag_to));
		}
	}
	if (Element *r = m_drag_range.el)
		range_to(r, x);
	set_hover(target);
	update_cursor(target);
	if (target && target->connected() && !same)
	{
		fire_pointer(target, "pointermove", -1, 0);
		if (!m_mouse_blocked && target->connected())
			fire_pointer(target, "mousemove", 0, 0);
	}
	// a tooltip: the nearest title's, once the pointer rests
	Element *src = capture ? nullptr : title_of(m_dom.document()->hover());
	if (src != m_tip_el.el)
	{
		tip_hide();
		m_tip_suppressed = false;
		set(m_tip_el, src);
	}
	if (!same)
		tip_restart();
}

void Input::frame()
{
	// what is under a still pointer once the page changed
	if (m_x >= 0 && !m_buttons && !m_popup_on && !m_bar_on && !m_dom.document()->capture())
		move(m_x, m_y);
	refresh_cursor();
}

// A region's scrollbar under the pointer, as the paint draws it: along the bottom or the right edge where its content
// overflows, the thumb as long as the share in view (24 pixels at least). A press on the track outside the thumb
// brings the thumb's middle there first; either way the thumb then follows the pointer.
bool Input::bar_at(Element *target, double x, double y)
{
	for (Element *e = target; e; e = e->parent_element())
	{
		LayoutNode *n = m_dom.layout_of(e);
		if (!n || !n->scrolls)
			continue;
		const Rect at = m_dom.client_rect(e);
		struct Track
		{
			bool horizontal;
			double lo, track, all, pos;
			bool on;
		};
		std::vector<Track> tracks;
		if (n->bar_x > 0 && n->sw > n->cw + 0.5)
			tracks.push_back({ true, at.x + n->bl, n->cw, n->sw, x,
				y >= at.y + n->h - n->bb - n->bar_x && y < at.y + n->h - n->bb && x >= at.x + n->bl
					&& x < at.x + n->bl + n->cw });
		if (n->bar_y > 0 && n->sh > n->ch + 0.5)
			tracks.push_back({ false, at.y + n->bt, n->ch, n->sh, y,
				x >= at.x + n->w - n->br - n->bar_y && x < at.x + n->w - n->br && y >= at.y + n->bt
					&& y < at.y + n->bt + n->ch });
		for (const Track &t : tracks)
		{
			if (!t.on)
				continue;
			const double len = std::max(24.0, t.track * t.track / t.all), room = std::max(1.0, t.track - len);
			const double range = std::max(1.0, t.all - t.track), per = range / room;
			double scroll = t.horizontal ? e->scroll_x : e->scroll_y;
			const double thumb = t.lo + room * (scroll / range);
			if (t.pos < thumb || t.pos >= thumb + len)
			{
				const double v = (t.pos - t.lo - len / 2) * per;
				m_dom.set_scroll(e, t.horizontal ? v : e->scroll_x, t.horizontal ? e->scroll_y : v);
				scroll = t.horizontal ? e->scroll_x : e->scroll_y;
			}
			set(m_bar.el, e);
			m_bar.horizontal = t.horizontal;
			m_bar.from = t.pos;
			m_bar.scroll = scroll;
			m_bar.per = per;
			return true;
		}
	}
	return false;
}

void Input::down(double x, double y, int button)
{
	m_x = x;
	m_y = y;
	m_keyboard = false;
	tip_hide();
	m_tip_suppressed = true;
	if (m_popup_on)
	{
		// a press outside the popup closes it and goes no further; one inside waits for its release
		if (popup_index(x, y) == -2)
			close_popup();
		else
			m_buttons |= button_bit(button);
		return;
	}
	Element *capture = m_dom.document()->capture();
	Element *target = capture ? capture : under(x, y);
	if (!target || disabled_in(target))
		return;
	Hold hold(m_dom.ctx(), target);
	const bool first = m_buttons == 0;
	if (button == 0 && first && bar_at(target, x, y))
	{
		m_bar_on = true;
		return;
	}
	m_buttons |= button_bit(button);
	set_hover(target);
	if (!first)
	{
		// another button while one is held: a pointermove, and the mouse's own down
		fire_pointer(target, "pointermove", button, 0);
		if (!m_mouse_blocked && target->connected())
			fire_pointer(target, "mousedown", button, 1);
		return;
	}
	const double now = m_dom.now();
	if (m_count > 0 && button == m_last_button && now - m_last_at < CLICK_MS
		&& std::hypot(x - m_last_x, y - m_last_y) < CLICK_PX)
		m_count++;
	else
		m_count = 1;
	m_last_at = now;
	m_last_x = x;
	m_last_y = y;
	m_last_button = button;
	set(m_press, target);
	m_press_button = button;
	m_mouse_blocked = !fire_pointer(target, "pointerdown", button, 0);
	bool defaults = true;
	if (!m_mouse_blocked && target->connected())
		defaults = fire_pointer(target, "mousedown", button, m_count);
	if (defaults && target->connected())
		press_defaults(target, button);
	if (button == 2 && !m_mouse_blocked && target->connected())
		fire_pointer(target, "contextmenu", 2, 0);
}

void Input::press_defaults(Element *target, int button)
{
	// the focus: the nearest focusable element around the target, or none
	Element *f = nullptr;
	for (Element *e = target; e; e = e->parent_element())
		if (e->focusable())
		{
			f = e;
			break;
		}
	m_dom.set_focus(f);
	if (button != 0 || !f || m_dom.document()->active() != f)
		return;
	if (f->is(n::select))
	{
		if (!f->has_attribute(n::multiple))
			open_popup(f);
	}
	else if (is_input_type(*f, "range"))
	{
		set(m_drag_range, f);
		range_to(f, m_x);
	}
	else if (is_text_field(*f))
	{
		const std::string v = field_value(*f);
		const size_t i = index_at(f, m_x, m_y);
		m_drag_unit = m_count >= 3 ? 2 : m_count == 2 ? 1 : 0;
		if (m_drag_unit == 2)
		{
			// a line: an input's whole text, a textarea's line between its new lines
			size_t a = 0, z = v.size();
			if (f->is(n::textarea))
			{
				const size_t nl = i > 0 ? v.rfind('\n', i - 1) : std::string::npos;
				a = nl == std::string::npos ? 0 : nl + 1;
				z = std::min(v.find('\n', i), v.size());
			}
			m_drag_from = a;
			m_drag_to = z;
			select(f, a, z);
		}
		else if (m_drag_unit == 1)
		{
			word_at(v, i, m_drag_from, m_drag_to);
			select(f, m_drag_from, m_drag_to);
		}
		else if (m_mods & MOD_SHIFT)
			select(f, f->form.sel_start, i);
		else
			select(f, i, i);
		m_goal_x = -1;
		m_history.typing = false;
		set(m_drag_text, f);
	}
}

void Input::up(double x, double y, int button)
{
	m_x = x;
	m_y = y;
	if (m_bar_on && button == 0)
	{
		m_bar_on = false;
		set(m_bar.el, nullptr);
		return;
	}
	if (!(m_buttons & button_bit(button)))
		return;
	m_buttons &= ~button_bit(button);
	const bool last = m_buttons == 0;
	if (m_popup_on && popup_index(x, y) != -2)
	{
		// a release on the popup is its own; one beside it (the opening press's, on its list) is the page's
		const int i = popup_index(x, y);
		Element *sel = m_popup.select.el;
		// the release of the press that opened it does not choose the option already chosen
		const bool grace = m_dom.now() - m_popup.opened < POPUP_GRACE_MS && sel && i == m_dom.selected_index(sel);
		if (!grace && i >= 0 && sel)
		{
			const std::vector<Element *> os = m_dom.options(sel);
			if (!os[size_t(i)]->disabled())
			{
				Hold hold(m_dom.ctx(), sel);
				close_popup();
				choose(sel, i);
			}
		}
		if (last)
		{
			set(m_press, nullptr);
			m_press_button = -1;
			m_mouse_blocked = false;
			m_dom.set_capture(nullptr);
		}
		return;
	}
	Element *capture = m_dom.document()->capture();
	Element *u = under(x, y);
	Element *target = capture ? capture : u;
	Hold hold(m_dom.ctx(), target), hold_under(m_dom.ctx(), u);
	if (button == 0)
	{
		set(m_drag_text, nullptr);
		if (Element *r = m_drag_range.el)
		{
			Hold hr(m_dom.ctx(), r);
			set(m_drag_range, nullptr);
			change(r);
		}
	}
	if (target && target->connected())
	{
		fire_pointer(target, last ? "pointerup" : "pointermove", button, 0);
		if (!m_mouse_blocked && target->connected())
			fire_pointer(target, "mouseup", button, m_count);
	}
	if (last)
		m_dom.set_capture(nullptr);
	// the click, on the common ancestor of the press's target and the release's
	if (button == m_press_button && m_press.el && target && target->connected() && !disabled_in(target))
	{
		Element *t = common_ancestor(m_press.el, target);
		if (t && t->connected())
		{
			Hold ht(m_dom.ctx(), t);
			if (button == 0)
			{
				const int count = m_count;
				click(t, count, true);
				if (count == 2 && t->connected())
					fire_pointer(t, "dblclick", 0, 2);
			}
			else
				fire_pointer(t, "auxclick", button, m_count);
		}
	}
	if (last)
	{
		set(m_press, nullptr);
		m_press_button = -1;
		m_mouse_blocked = false;
	}
	Element *now = u && u->connected() ? u : nullptr;
	set_hover(now);
	update_cursor(now);
}

void Input::pointer(PointerKind kind, double x, double y, int button, int mods)
{
	m_dom.input_came();
	m_mods = mods;
	switch (kind)
	{
	case PointerKind::Move: move(x, y); break;
	case PointerKind::Down: down(x, y, button); break;
	case PointerKind::Up: up(x, y, button); break;
	}
	m_dom.run_jobs();
}

void Input::wheel(double x, double y, double dx, double dy, int mods)
{
	m_dom.input_came();
	m_mods = mods;
	m_x = x;
	m_y = y;
	tip_hide();
	m_tip_suppressed = true;
	// the wheel over the popup scrolls it; beside it, it closes it and goes on to the page
	if (m_popup_on && popup_index(x, y) == -2)
		close_popup();
	if (m_popup_on)
	{
		const double max = std::max(0.0, double(m_dom.options(m_popup.select.el).size()) * m_popup.row + 4 - m_popup.h);
		m_popup.scroll = std::clamp(m_popup.scroll + dy * WHEEL_PX, 0.0, max);
		const int i = popup_index(x, y);
		if (i >= 0)
			m_popup.over = i;
		m_overlay_dirty = true;
		return;
	}
	Element *target = under(x, y);
	if (!target)
		return;
	Hold hold(m_dom.ctx(), target);
	const bool ok = fire_wheel(target, dx * WHEEL_PX, dy * WHEEL_PX);
	m_dom.run_jobs();
	if (!ok || !target->connected())
		return;
	// the region under the pointer that can scroll that way; Shift turns the vertical wheel horizontal
	double sx = dx * WHEEL_PX, sy = dy * WHEEL_PX;
	if ((mods & MOD_SHIFT) && sx == 0)
		std::swap(sx, sy);
	m_dom.update();
	for (Element *e = target; e; e = e->parent_element())
	{
		if (e->is(n::textarea) && sy != 0 && field_wheel(e, sy))
			break;
		LayoutNode *n = m_dom.layout_of(e);
		if (!n || !n->scrolls)
			continue;
		const ComputedStyle &cs = n->cs();
		auto can = [](Overflow o) { return o == Overflow::Auto || o == Overflow::Scroll; };
		const double was_x = e->scroll_x, was_y = e->scroll_y;
		if (sy != 0 && n->sh > n->ch && can(cs.overflow_y))
			m_dom.set_scroll(e, e->scroll_x, e->scroll_y + sy);
		if (sx != 0 && n->sw > n->cw && can(cs.overflow_x))
			m_dom.set_scroll(e, e->scroll_x + sx, e->scroll_y);
		if (e->scroll_x != was_x || e->scroll_y != was_y)
			break;
	}
	// what is under the pointer now
	Element *capture = m_dom.document()->capture();
	Element *now = capture ? capture : under(x, y);
	if (!m_buttons)
		set_hover(now);
	update_cursor(now);
	m_dom.run_jobs();
}

void Input::leave()
{
	tip_hide();
	set(m_tip_el, nullptr);
	if (!m_buttons && !m_popup_on)
		set_hover(nullptr);
	m_x = m_y = -1;
	m_dom.run_jobs();
}

bool Input::drag(int kind, double x, double y, const std::vector<std::string> &paths)
{
	Element *was = m_drop.el;
	Hold hwas(m_dom.ctx(), was);
	bool taken = false;
	if (kind == QDOM_DRAG_LEAVE)
	{
		if (was)
			fire_drag(was, "dragleave", false, paths);
		set(m_drop, nullptr);
	}
	else
	{
		m_x = x;
		m_y = y;
		Element *now = under(x, y);
		Hold hnow(m_dom.ctx(), now);
		if (now != was)
		{
			if (now)
				fire_drag(now, "dragenter", true, paths, was);
			if (was)
				fire_drag(was, "dragleave", false, paths, now);
			set(m_drop, now);
		}
		if (now)
		{
			if (kind == QDOM_DRAG_OVER)
				taken = !fire_drag(now, "dragover", true, paths);
			else
			{
				taken = !fire_drag(now, "drop", true, paths);
				set(m_drop, nullptr);
			}
		}
	}
	m_dom.run_jobs();
	return taken;
}

void Input::window_blur()
{
	if (!m_window_focus)
		return;
	m_window_focus = false;
	close_popup();
	m_bar_on = false;
	set(m_bar.el, nullptr);
	tip_hide();
	Element *a = m_dom.document()->active();
	Hold hold(m_dom.ctx(), a);
	if (a && a != m_dom.document()->body())
	{
		m_dom.fire(a, "blur", false);
		m_dom.fire(a, "focusout", true);
	}
	m_dom.fire(nullptr, "blur", false);
	m_dom.invalidate_paint();
	m_dom.run_jobs();
}

void Input::window_focus()
{
	if (m_window_focus)
		return;
	m_window_focus = true;
	m_dom.fire(nullptr, "focus", false);
	Element *a = m_dom.document()->active();
	Hold hold(m_dom.ctx(), a);
	if (a && a != m_dom.document()->body())
	{
		m_dom.fire(a, "focus", false);
		m_dom.fire(a, "focusin", true);
	}
	caret_reset();
	m_dom.run_jobs();
}

// ---------------------------------------------------------------- clicks

bool Input::click(Element *target, int detail, bool trusted)
{
	Hold hold(m_dom.ctx(), target);
	// the element whose click does something: a checkbox, a radio, a label, or a button (which stops the search)
	Element *act = nullptr;
	for (Element *e = target; e; e = e->parent_element())
		if (is_input_type(*e, "checkbox") || is_input_type(*e, "radio") || e->is(n::label) || e->is(n::button)
			|| is_input_type(*e, "button") || is_input_type(*e, "submit") || is_input_type(*e, "reset"))
		{
			act = e;
			break;
		}
	Hold hact(m_dom.ctx(), act);
	if (act && form_control(*act) && act->disabled())
		act = nullptr;
	// a checkbox or a radio changes before the click, and back when it is canceled
	const bool box = act && is_input_type(*act, "checkbox"), radio = act && is_input_type(*act, "radio");
	const bool was = act ? checked_of(*act) : false;
	std::vector<Element *> group;
	std::vector<JSValue> held;
	if (radio)
	{
		const std::string *name = act->attribute(n::name);
		if (name && !name->empty())
			each_element(m_dom.document(), [&](Element *e) {
				if (e != act && is_input_type(*e, "radio") && checked_of(*e))
					if (const std::string *k = e->attribute(n::name); k && *k == *name)
					{
						group.push_back(e);
						held.push_back(JS_DupValue(m_dom.ctx(), e->wrapper()));
					}
			});
		for (Element *e : group)
			m_dom.set_checked(e, false);
	}
	if (box)
		m_dom.set_checked(act, !was);
	else if (radio)
		m_dom.set_checked(act, true);

	JSValue v = m_dom.new_event("click", true, true);
	Event *e = m_dom.event_of(v);
	e->trusted = trusted;
	e->composed = true;
	e->client_x = e->screen_x = detail > 0 ? m_x : 0;
	e->client_y = e->screen_y = detail > 0 ? m_y : 0;
	e->detail = detail;
	e->buttons = m_buttons;
	if (detail > 0)
	{
		e->shift = m_mods & MOD_SHIFT;
		e->ctrl = m_mods & MOD_CTRL;
		e->alt = m_mods & MOD_ALT;
		e->meta = m_mods & MOD_META;
	}
	const bool ok = m_dom.dispatch(target, v);
	JS_FreeValue(m_dom.ctx(), v);

	if (box || radio)
	{
		if (!ok)
		{
			m_dom.set_checked(act, was);
			for (Element *g : group)
				m_dom.set_checked(g, true);
		}
		else if (checked_of(*act) != was)
		{
			m_dom.fire(act, "input", true);
			m_dom.fire(act, "change", true);
		}
	}
	else if (ok && act && act->is(n::label))
	{
		// the label's control: its for's, or the first inside it
		Element *control = nullptr;
		if (const std::string *f = act->attribute(n::for_))
		{
			const Name id = m_dom.names.find(*f);
			if (id)
				each_element(m_dom.document(), [&](Element *x) {
					if (!control && x->id() == id)
						control = x;
				});
		}
		else
			each_element(act, [&](Element *x) {
				if (!control && form_control(*x) && !is_input_type(*x, "hidden"))
					control = x;
			});
		if (control && !control->contains(target) && form_control(*control) && !control->disabled())
		{
			Hold hc(m_dom.ctx(), control);
			if (is_text_field(*control) || control->is(n::select))
				m_dom.set_focus(control);
			else
				click(control, 0, trusted);
		}
	}
	for (JSValue h : held)
		JS_FreeValue(m_dom.ctx(), h);
	return ok;
}

void Input::change(Element *el)
{
	m_dom.fire(el, "change", true);
}

// ---------------------------------------------------------------- keys

bool Input::key(bool down, std::string_view key, std::string_view code, bool repeat, int mods)
{
	m_dom.input_came();
	m_mods = mods;
	auto fire_key = [&](Element *target, const char *type) {
		JSValue v = m_dom.new_event(type, true, true);
		Event *e = m_dom.event_of(v);
		e->trusted = true;
		e->composed = true;
		e->key = key;
		e->code = code;
		e->repeat = repeat;
		e->shift = mods & MOD_SHIFT;
		e->ctrl = mods & MOD_CTRL;
		e->alt = mods & MOD_ALT;
		e->meta = mods & MOD_META;
		const bool ok = m_dom.dispatch(target, v);
		JS_FreeValue(m_dom.ctx(), v);
		return ok;
	};
	Element *t = m_dom.document()->active();
	Hold hold(m_dom.ctx(), t);
	bool used = false;
	if (!down)
	{
		used = !fire_key(t, "keyup");
		if (m_space_click && key == " ")
		{
			m_space_click = false;
			if (!used && t->connected())
				click(t, 0, true);
			used = true;
		}
		m_dom.run_jobs();
		return used;
	}
	m_keyboard = true;
	m_key_blocked = false;
	m_space_click = false;
	tip_hide();
	m_tip_suppressed = true;
	if (m_popup_on)
	{
		popup_key(key);
		m_key_blocked = true;
		m_dom.run_jobs();
		return true;
	}
	if (!fire_key(t, "keydown"))
	{
		m_key_blocked = true;
		m_dom.run_jobs();
		return true;
	}
	t = m_dom.document()->active();
	Hold now(m_dom.ctx(), t);
	const bool plain = !(mods & (MOD_CTRL | MOD_ALT | MOD_META));
	if (Element *f = field(); f && f == t && edit_key(f, key, mods))
		used = true;
	else if (key == "Tab" && plain)
	{
		focus_next(mods & MOD_SHIFT);
		used = true;
	}
	else if (key == "Escape" && !m_dom.document()->top_layer().empty())
	{
		Element *d = m_dom.document()->top_layer().back().el;
		Hold hd(m_dom.ctx(), d);
		if (m_dom.fire(d, "cancel", false, true))
			m_dom.close_dialog(d);
		used = true;
	}
	else if (t->is(n::select) && !t->disabled())
		used = list_key(t, key, mods);
	else if (is_input_type(*t, "range") && !t->disabled())
		used = range_step(t, key);
	else if ((t->is(n::button) || is_input_type(*t, "button") || is_input_type(*t, "submit")
		|| is_input_type(*t, "reset") || is_input_type(*t, "checkbox") || is_input_type(*t, "radio")) && !t->disabled()
		&& plain)
	{
		if (key == "Enter" && !is_input_type(*t, "checkbox") && !is_input_type(*t, "radio"))
		{
			click(t, 0, true);
			used = true;
		}
		else if (key == " ")
		{
			m_space_click = true;
			used = true;
		}
	}
	m_dom.run_jobs();
	return used;
}

void Input::text(std::string_view s)
{
	m_dom.input_came();
	const bool blocked = m_key_blocked;
	m_key_blocked = false;
	if (blocked || m_popup_on || s.empty())
		return;
	Element *f = field();
	if (!f)
		return;
	Hold hold(m_dom.ctx(), f);
	const std::string v = field_value(*f);
	const size_t a = std::min(f->form.sel_start, v.size()), z = std::min(f->form.sel_end, v.size());
	replace(f, s, "insertText", std::min(a, z), std::max(a, z));
	m_dom.run_jobs();
}

bool Input::read_clipboard()
{
	if (m_reading)
		return true;
	// a host may answer at once, from within host_paste
	m_reading = true;
	if (m_dom.host_paste())
		return true;
	m_reading = false;
	return false;
}

void Input::paste(std::string_view s)
{
	if (m_reading)
	{
		// readText's promises, answered by the prelude's __qdom.pasted
		m_reading = false;
		JSContext *ctx = m_dom.ctx();
		JSValue own = JS_GetPropertyStr(ctx, m_dom.global(), "__qdom");
		JSValue fn = JS_GetPropertyStr(ctx, own, "pasted");
		JSValue text = JS_NewStringLen(ctx, s.data(), s.size());
		JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &text);
		if (JS_IsException(r))
			m_dom.report_exception();
		JS_FreeValue(ctx, r);
		JS_FreeValue(ctx, text);
		JS_FreeValue(ctx, fn);
		JS_FreeValue(ctx, own);
		m_dom.run_jobs();
	}
	Element *f = m_paste.el;
	Hold hold(m_dom.ctx(), f);
	set(m_paste, nullptr);
	if (!f || f != field())
		return;
	// new lines as \n; an input's lines joined by spaces, its last new lines left out
	std::string t;
	for (size_t i = 0; i < s.size(); i++)
		if (s[i] != '\r')
			t += s[i];
		else if (i + 1 >= s.size() || s[i + 1] != '\n')
			t += '\n';
	if (!f->is(n::textarea))
	{
		while (!t.empty() && t.back() == '\n')
			t.pop_back();
		std::replace(t.begin(), t.end(), '\n', ' ');
	}
	const std::string v = field_value(*f);
	const size_t a = std::min(f->form.sel_start, v.size()), z = std::min(f->form.sel_end, v.size());
	replace(f, t, "insertFromPaste", std::min(a, z), std::max(a, z));
	m_dom.run_jobs();
}

Element *Input::field() const
{
	Element *a = m_dom.document()->active();
	return a && a != m_dom.document()->body() && is_text_field(*a) && !a->disabled() ? a : nullptr;
}

void Input::focus_next(bool back)
{
	m_dom.update();
	const auto &top = m_dom.document()->top_layer();
	Node *root = top.empty() ? static_cast<Node *>(m_dom.document()) : top.back().el;
	std::vector<std::pair<double, Element *>> all;
	each_element(root, [&](Element *e) {
		if (!e->focusable() || !m_dom.layout_of(e) || !e->style || e->style->visibility != Visibility::Visible)
			return;
		const double ti = e->number_attribute(n::tabindex, 0);
		if (ti < 0)
			return;
		all.emplace_back(ti, e);
	});
	if (all.empty())
		return;
	// tabindex above 0 first, in order, then the rest in the document's order
	std::stable_sort(all.begin(), all.end(), [](const auto &a, const auto &b) {
		const double ka = a.first > 0 ? a.first : 1e18, kb = b.first > 0 ? b.first : 1e18;
		return ka < kb;
	});
	Element *cur = m_dom.document()->active();
	long i = -1;
	for (size_t k = 0; k < all.size(); k++)
		if (all[k].second == cur)
			i = long(k);
	const long count = long(all.size());
	const long next = i < 0 ? (back ? count - 1 : 0) : (i + (back ? -1 : 1) + count) % count;
	Element *el = all[size_t(next)].second;
	Hold hold(m_dom.ctx(), el);
	m_dom.set_focus(el);
	if (m_dom.document()->active() == el && is_text_field(*el))
	{
		const std::string v = field_value(*el);
		select(el, 0, v.size());
	}
}

// ---------------------------------------------------------------- text fields

size_t Input::index_at(Element *f, double x, double y)
{
	const Rect r = m_dom.client_rect(f);
	LayoutNode *n = m_dom.layout_of(f);
	if (!n)
		return 0;
	const std::string v = field_value(*f);
	FieldText ft(m_dom, *f, *n, v);
	return ft.index_at(x - r.x - ft.cx + f->form.scroll_x, y - r.y - ft.cy + f->form.scroll_y);
}

void Input::select(Element *f, size_t anchor, size_t focus)
{
	const std::string v = field_value(*f);
	f->form.sel_start = char_start(v, anchor);
	f->form.sel_end = char_start(v, focus);
	m_dom.invalidate_paint();
	caret_reset();
	reveal(f);
}

void Input::reveal(Element *f)
{
	m_dom.update();
	LayoutNode *n = m_dom.layout_of(f);
	if (!n)
		return;
	const std::string v = field_value(*f);
	FieldText ft(m_dom, *f, *n, v);
	const size_t at = std::min(f->form.sel_end, v.size());
	double &sx = f->form.scroll_x, &sy = f->form.scroll_y;
	const double was_x = sx, was_y = sy;
	const double x = ft.x_of(at);
	// the caret, a pixel wide, inside the content box
	if (x - sx > ft.cw - 1)
		sx = x - ft.cw + 1;
	if (x < sx)
		sx = x;
	sx = std::clamp(sx, 0.0, std::max(0.0, ft.width() + 1 - ft.cw));
	if (ft.multiline)
	{
		const FieldLine &l = ft.lines[ft.line_of(at)];
		if (l.top < sy)
			sy = l.top;
		if (l.top + ft.line_h > sy + ft.ch)
			sy = l.top + ft.line_h - ft.ch;
		sy = std::clamp(sy, 0.0, std::max(0.0, ft.height() - ft.ch));
	}
	else
		sy = 0;
	if (sx != was_x || sy != was_y)
		m_dom.invalidate_paint();
}

bool Input::field_wheel(Element *f, double dy)
{
	LayoutNode *n = m_dom.layout_of(f);
	if (!n)
		return false;
	const std::string v = field_value(*f);
	FieldText ft(m_dom, *f, *n, v);
	const double was = f->form.scroll_y;
	f->form.scroll_y = std::clamp(was + dy, 0.0, std::max(0.0, ft.height() - ft.ch));
	if (f->form.scroll_y == was)
		return false;
	m_dom.invalidate_paint();
	return true;
}

void Input::remember(Element *f, bool typing)
{
	History &h = m_history;
	if (h.el.el != f)
	{
		set(h.el, f);
		h.undo.clear();
		h.redo.clear();
		h.typing = false;
	}
	if (!(typing && h.typing))
	{
		h.undo.push_back({ field_value(*f), f->form.sel_start, f->form.sel_end });
		if (h.undo.size() > HISTORY)
			h.undo.erase(h.undo.begin());
	}
	h.redo.clear();
	h.typing = typing;
}

void Input::value_set(Element *el)
{
	if (m_history.el.el == el)
	{
		set(m_history.el, nullptr);
		m_history.undo.clear();
		m_history.redo.clear();
	}
}

void Input::history(Element *f, bool redo)
{
	History &h = m_history;
	if (h.el.el != f)
		return;
	std::vector<Snapshot> &from = redo ? h.redo : h.undo, &to = redo ? h.undo : h.redo;
	if (from.empty())
		return;
	const char *type = redo ? "historyRedo" : "historyUndo";
	JSValue bv = m_dom.new_event("beforeinput", true, true);
	m_dom.event_of(bv)->trusted = true;
	m_dom.event_of(bv)->input_type = type;
	const bool ok = m_dom.dispatch(f, bv);
	JS_FreeValue(m_dom.ctx(), bv);
	if (!ok || h.el.el != f || from.empty())
		return;
	to.push_back({ field_value(*f), f->form.sel_start, f->form.sel_end });
	const Snapshot s = from.back();
	from.pop_back();
	h.typing = false;
	m_dom.set_field(f, s.value, s.start, s.end);
	caret_reset();
	reveal(f);
	JSValue iv = m_dom.new_event("input", true, false);
	m_dom.event_of(iv)->trusted = true;
	m_dom.event_of(iv)->input_type = type;
	m_dom.dispatch(f, iv);
	JS_FreeValue(m_dom.ctx(), iv);
}

bool Input::replace(Element *f, std::string_view text, const char *input_type, size_t from, size_t to)
{
	if (f->has_attribute(n::readonly))
		return false;
	const std::string v = field_value(*f);
	from = char_start(v, std::min(from, v.size()));
	to = char_start(v, std::min(std::max(from, to), v.size()));
	std::string s(text);
	// maxlength, in UTF-16 units: the new text cut to what fits
	const double max = f->number_attribute(n::maxlength, -1);
	if (max >= 0)
	{
		const double kept = double(utf16_index(v, v.size()) - (utf16_index(v, to) - utf16_index(v, from)));
		const double room = std::max(0.0, max - kept);
		if (double(utf16_index(s, s.size())) > room)
			s = s.substr(0, byte_index(s, size_t(room)));
	}
	if (s.empty() && from == to)
		return false;
	{
		JSValue bv = m_dom.new_event("beforeinput", true, true);
		Event *e = m_dom.event_of(bv);
		e->trusted = true;
		e->input_type = input_type;
		e->data = s;
		const bool ok = m_dom.dispatch(f, bv);
		JS_FreeValue(m_dom.ctx(), bv);
		if (!ok || field() != f)
			return false;
	}
	remember(f, std::string_view(input_type) == "insertText");
	const std::string now = field_value(*f);
	if (now != v)
		return false;
	std::string next = v.substr(0, from) + s + v.substr(to);
	const size_t caret = from + s.size();
	m_dom.set_field(f, std::move(next), caret, caret);
	caret_reset();
	m_goal_x = -1;
	reveal(f);
	JSValue iv = m_dom.new_event("input", true, false);
	Event *e = m_dom.event_of(iv);
	e->trusted = true;
	e->input_type = input_type;
	if (std::string_view(input_type) == "insertText" || std::string_view(input_type) == "insertFromPaste")
		e->data = s;
	m_dom.dispatch(f, iv);
	JS_FreeValue(m_dom.ctx(), iv);
	return true;
}

bool Input::edit_key(Element *f, std::string_view key, int mods)
{
	const std::string v = field_value(*f);
	const size_t a = char_start(v, f->form.sel_start), z = char_start(v, f->form.sel_end);
	const size_t lo = std::min(a, z), hi = std::max(a, z);
	const bool shift = mods & MOD_SHIFT, ctrl = mods & MOD_CTRL;
	if (mods & (MOD_ALT | MOD_META))
		return false;
	auto to = [&](size_t i, bool keep_goal = false) {
		if (shift)
			select(f, a, i);
		else
			select(f, i, i);
		if (!keep_goal)
			m_goal_x = -1;
		m_history.typing = false;
		return true;
	};
	m_dom.update();
	LayoutNode *n = m_dom.layout_of(f);
	const bool multi = f->is(n::textarea);
	auto lines = [&](long by) {
		if (!n)
			return true;
		FieldText ft(m_dom, *f, *n, v);
		const size_t line = ft.line_of(z);
		const double goal = m_goal_x >= 0 ? m_goal_x : ft.x_of(z);
		const long target = long(line) + by;
		size_t i;
		if (target < 0)
			i = 0;
		else if (target >= long(ft.lines.size()))
			i = v.size();
		else
			i = ft.index_on(size_t(target), goal);
		to(i, true);
		m_goal_x = goal;
		return true;
	};
	auto page = [&]() {
		if (!n)
			return 1L;
		FieldText ft(m_dom, *f, *n, v);
		return std::max(1L, long(ft.ch / std::max(1.0, ft.line_h)));
	};
	if (key == "ArrowLeft")
		return to(!shift && lo != hi ? lo : ctrl ? prev_word(v, z) : prev_char(v, z));
	if (key == "ArrowRight")
		return to(!shift && lo != hi ? hi : ctrl ? next_word(v, z) : next_char(v, z));
	if (key == "ArrowUp" || key == "ArrowDown" || key == "PageUp" || key == "PageDown")
	{
		const bool upward = key == "ArrowUp" || key == "PageUp";
		if (!multi)
			return to(upward ? 0 : v.size());
		const long by = key.starts_with("Page") ? page() : 1;
		return lines(upward ? -by : by);
	}
	if (key == "Home" || key == "End")
	{
		if (ctrl || !multi || !n)
			return to(key == "Home" ? 0 : v.size());
		FieldText ft(m_dom, *f, *n, v);
		const size_t k = ft.line_of(z);
		const FieldLine &l = ft.lines[k];
		if (key == "Home")
			return to(l.from);
		// a wrapped line ends before its hanging space
		const bool wrapped = k + 1 < ft.lines.size() && ft.lines[k + 1].from == l.to;
		return to(wrapped && l.to > l.from ? prev_char(v, l.to) : l.to);
	}
	if (key == "Backspace" || key == "Delete")
	{
		size_t from = lo, until = hi;
		const bool back = key == "Backspace";
		if (lo == hi)
		{
			if (back)
				from = ctrl ? prev_word(v, lo) : prev_char(v, lo);
			else
				until = ctrl ? next_word(v, hi) : next_char(v, hi);
		}
		const char *type = back ? (ctrl ? "deleteWordBackward" : "deleteContentBackward")
			: (ctrl ? "deleteWordForward" : "deleteContentForward");
		if (lo != hi)
			type = back ? "deleteContentBackward" : "deleteContentForward";
		replace(f, "", type, from, until);
		m_history.typing = false;
		return true;
	}
	if (key == "Enter")
	{
		if (multi)
			replace(f, "\n", "insertLineBreak", lo, hi);
		else if (f->form.committed != field_value(*f))
		{
			f->form.committed = field_value(*f);
			change(f);
		}
		return true;
	}
	if (ctrl)
	{
		const std::string k = lower_key(key);
		if (k == "a")
		{
			select(f, 0, v.size());
			return true;
		}
		if (k == "c" || k == "x")
		{
			if (lo != hi)
			{
				m_dom.host_copy(v.substr(lo, hi - lo));
				if (k == "x")
					replace(f, "", "deleteByCut", lo, hi);
			}
			return true;
		}
		if (k == "v")
		{
			set(m_paste, f);
			if (!m_dom.host_paste())
				set(m_paste, nullptr);
			return true;
		}
		if (k == "z" || k == "y")
		{
			history(f, k == "y" || shift);
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------- controls

void Input::range_to(Element *r, double x)
{
	const Rect rc = m_dom.client_rect(r);
	const double lo = r->number_attribute(n::min, 0), hi = r->number_attribute(n::max, 100);
	const double f = std::clamp((x - rc.x - 8) / std::max(1.0, rc.w - 16), 0.0, 1.0);
	const std::string was = m_dom.range_value(r);
	m_dom.set_value(r, format_number(lo + f * (hi - lo)));
	if (m_dom.range_value(r) != was)
		m_dom.fire(r, "input", true);
}

bool Input::range_step(Element *r, std::string_view key)
{
	const double lo = r->number_attribute(n::min, 0), hi = r->number_attribute(n::max, 100);
	const double step = std::max(1e-9, r->number_attribute(n::step, 1));
	const double v = parse_number(m_dom.range_value(r)).value_or(lo);
	double next;
	if (key == "ArrowUp" || key == "ArrowRight")
		next = v + step;
	else if (key == "ArrowDown" || key == "ArrowLeft")
		next = v - step;
	else if (key == "PageUp")
		next = v + std::max(step, (hi - lo) / 10);
	else if (key == "PageDown")
		next = v - std::max(step, (hi - lo) / 10);
	else if (key == "Home")
		next = lo;
	else if (key == "End")
		next = hi;
	else
		return false;
	const std::string was = m_dom.range_value(r);
	m_dom.set_value(r, format_number(next));
	if (m_dom.range_value(r) != was)
	{
		m_dom.fire(r, "input", true);
		change(r);
	}
	return true;
}

void Input::choose(Element *sel, int index)
{
	const std::vector<Element *> os = m_dom.options(sel);
	if (index < 0 || index >= int(os.size()) || os[size_t(index)]->disabled() || index == m_dom.selected_index(sel))
		return;
	m_dom.set_selected_index(sel, index);
	m_dom.fire(sel, "input", true);
	m_dom.fire(sel, "change", true);
}

bool Input::list_key(Element *sel, std::string_view key, int mods)
{
	if (key == "F4" || key == " " || ((mods & MOD_ALT) && (key == "ArrowDown" || key == "ArrowUp")))
	{
		open_popup(sel);
		return true;
	}
	const std::vector<Element *> os = m_dom.options(sel);
	const int count = int(os.size());
	int i = m_dom.selected_index(sel);
	int dir = 0, start = i;
	if (key == "ArrowDown" || key == "ArrowRight" || key == "PageDown")
		dir = 1;
	else if (key == "ArrowUp" || key == "ArrowLeft" || key == "PageUp")
		dir = -1;
	else if (key == "Home")
		dir = 1, start = -1;
	else if (key == "End")
		dir = -1, start = count;
	else
		return false;
	int steps = key.starts_with("Page") ? 10 : 1;
	int found = -1;
	for (int k = start + dir; k >= 0 && k < count; k += dir)
		if (!os[size_t(k)]->disabled())
		{
			found = k;
			if (--steps == 0)
				break;
		}
	if (found >= 0)
		choose(sel, found);
	return true;
}

void Input::open_popup(Element *sel)
{
	m_dom.update();
	LayoutNode *n = m_dom.layout_of(sel);
	if (!n)
		return;
	const Rect rc = m_dom.client_rect(sel);
	const ComputedStyle &cs = n->cs();
	const std::vector<Element *> os = m_dom.options(sel);
	double w = rc.w;
	for (Element *o : os)
		w = std::max(w, m_dom.fonts.width(cs.face, cs.font_size, 0, collapsed(o->text_content())) + 24);
	Popup &p = m_popup;
	p.row = std::ceil(cs.font_size * 1.362) + 4;
	const double want = double(os.size()) * p.row + 4;
	const double below = m_dom.height() - (rc.y + rc.h) - 8, above = rc.y - 8;
	p.h = std::min(want, std::max({ below, above, p.row * 4 }));
	p.y = want <= below || below >= above ? rc.y + rc.h : rc.y - p.h;
	p.x = std::max(0.0, std::min(rc.x, m_dom.width() - w - 4));
	p.w = w;
	p.face = cs.face;
	p.size = cs.font_size;
	p.over = m_dom.selected_index(sel);
	p.opened = m_dom.now();
	// the chosen option in view
	p.scroll = std::clamp((p.over + 0.5) * p.row - p.h / 2, 0.0, std::max(0.0, want - p.h));
	set(p.select, sel);
	m_popup_on = true;
	m_overlay_dirty = true;
	update_cursor(nullptr);
}

void Input::close_popup()
{
	if (!m_popup_on)
		return;
	m_popup_on = false;
	set(m_popup.select, nullptr);
	m_overlay_dirty = true;
}

int Input::popup_index(double x, double y) const
{
	const Popup &p = m_popup;
	if (x < p.x || x >= p.x + p.w || y < p.y || y >= p.y + p.h)
		return -2;
	const int i = int(std::floor((y - p.y - 2 + p.scroll) / p.row));
	return i >= 0 && p.select.el && i < int(m_dom.options(p.select.el).size()) ? i : -1;
}

void Input::popup_scroll_to(int index)
{
	Popup &p = m_popup;
	const double top = index * p.row, bottom = top + p.row + 4;
	if (top < p.scroll)
		p.scroll = top;
	else if (bottom > p.scroll + p.h)
		p.scroll = bottom - p.h;
}

bool Input::popup_key(std::string_view key)
{
	Popup &p = m_popup;
	Element *sel = p.select.el;
	if (!sel)
		return false;
	Hold hold(m_dom.ctx(), sel);
	const std::vector<Element *> os = m_dom.options(sel);
	const int count = int(os.size());
	if (key == "Escape" || key == "Tab")
		close_popup();
	else if (key == "Enter" || key == " ")
	{
		const int i = p.over;
		close_popup();
		choose(sel, i);
	}
	else if (key == "ArrowDown" || key == "ArrowUp" || key == "Home" || key == "End" || key == "PageUp"
		|| key == "PageDown")
	{
		int i = p.over;
		const int dir = key == "ArrowDown" || key == "End" || key == "PageDown" ? 1 : -1;
		if (key == "Home")
			i = -1;
		if (key == "End")
			i = count;
		int steps = key == "Home" || key == "End" ? 1 : key.starts_with("Page") ? std::max(1, int(p.h / p.row) - 1) : 1;
		for (int k = i + dir; k >= 0 && k < count; k += dir)
			if (!os[size_t(k)]->disabled())
			{
				p.over = k;
				if (--steps == 0)
					break;
			}
		popup_scroll_to(p.over);
		m_overlay_dirty = true;
	}
	return true;
}

// ---------------------------------------------------------------- the time and the paint

void Input::caret_reset()
{
	m_caret_at = m_dom.now();
	if (field())
		m_overlay_dirty = true;
}

bool Input::caret_on() const
{
	if (!m_window_focus)
		return false;
	const double t = m_dom.now() - m_caret_at;
	return t < 0 || std::fmod(t, 2 * BLINK_MS) < BLINK_MS;
}

double Input::next_due() const
{
	double next = m_tip_due;
	if (field() && m_window_focus)
	{
		const double t = std::max(0.0, m_dom.now() - m_caret_at);
		const double flip = m_caret_at + (std::floor(t / BLINK_MS) + 1) * BLINK_MS;
		if (next < 0 || flip < next)
			next = flip;
	}
	return next;
}

void Input::advance()
{
	if (m_tip_due >= 0 && m_dom.now() >= m_tip_due)
	{
		m_tip_due = -1;
		Element *src = m_tip_el.el;
		const std::string *t = src && src->connected() ? src->attribute(n::title) : nullptr;
		if (t && !t->empty() && !m_buttons && !m_popup_on)
		{
			m_tip = *t;
			m_tip_x = m_x;
			m_tip_y = m_y;
			m_tip_on = true;
			m_overlay_dirty = true;
		}
	}
}

bool Input::needs_paint() const
{
	return m_overlay_dirty || (field() && caret_on() != m_painted_caret);
}

void Input::painted()
{
	m_painted_caret = caret_on();
	m_overlay_dirty = false;
}

void Input::overlay(Commands &out)
{
	if (m_popup_on && m_popup.select.el && m_popup.select.el->style)
	{
		const Popup &p = m_popup;
		Element *sel = p.select.el;
		const ComputedStyle &cs = *sel->style;
		const Color bg = cs.background_color.a >= 1 ? cs.background_color : PANE;
		const Color line = cs.border_color[0].visible() ? cs.border_color[0] : LINE;
		out.push({ op::SHADOW, p.x, p.y + 2, p.w, p.h, 4, 10 });
		out.color({ 0, 0, 0, 0.6f });
		out.push({ op::RECT, p.x, p.y, p.w, p.h, 4, 4, 4, 4 });
		out.color(bg);
		out.push({ op::STROKE_RECT, p.x + 0.5, p.y + 0.5, p.w - 1, p.h - 1, 4, 4, 4, 4, 1 });
		out.color(line);
		out.push({ op::SAVE, op::CLIP, p.x + 1, p.y + 1, p.w - 2, p.h - 2 });
		const std::vector<Element *> os = m_dom.options(sel);
		const int chosen = m_dom.selected_index(sel);
		const int first = std::max(0, int(std::floor(p.scroll / p.row)) - 1);
		const int last = std::min(int(os.size()), int(std::ceil((p.scroll + p.h) / p.row)) + 1);
		for (int i = first; i < last; i++)
		{
			const double y = p.y + 2 + i * p.row - p.scroll;
			const Element *o = os[size_t(i)];
			if (i == p.over && !o->disabled())
			{
				out.push({ op::RECT, p.x + 2, y, p.w - 4, p.row, 3, 3, 3, 3 });
				out.color(HIGHLIGHT);
			}
			else if (i == chosen)
			{
				out.push({ op::RECT, p.x + 2, y, p.w - 4, p.row, 3, 3, 3, 3 });
				out.color(HIGHLIGHT, 0.5);
			}
			out.push({ op::TEXT, p.x + 10, y + p.row / 2 + p.size * 0.36, double(p.face), p.size, 0 });
			out.color(cs.color, o->disabled() ? 0.54 : 1);
			out.data.push_back(out.text(collapsed(o->text_content())));
		}
		out.push({ op::RESTORE });
	}
	if (m_tip_on)
	{
		const double size = 12, pad = 6, wrap = 420;
		const FaceId face = m_dom.fonts.face("sans-serif", 400, FontStyle::Normal);
		std::vector<std::string> lines;
		size_t from = 0;
		for (;;)
		{
			const size_t nl = m_tip.find('\n', from);
			const std::string para = m_tip.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
			std::string cur;
			for (std::string_view w : split_words(para))
			{
				std::string next = cur.empty() ? std::string(w) : cur + " " + std::string(w);
				if (!cur.empty() && m_dom.fonts.width(face, size, 0, next) > wrap)
				{
					lines.push_back(cur);
					cur = std::string(w);
				}
				else
					cur = std::move(next);
			}
			lines.push_back(cur);
			if (nl == std::string::npos)
				break;
			from = nl + 1;
		}
		double w = 0;
		for (const std::string &l : lines)
			w = std::max(w, m_dom.fonts.width(face, size, 0, l));
		w += 2 * pad;
		const double h = double(lines.size()) * 16 + 2 * pad - 2;
		double x = m_tip_x + 12, y = m_tip_y + 20;
		if (x + w > m_dom.width() - 4)
			x = std::max(4.0, m_dom.width() - 4 - w);
		if (y + h > m_dom.height() - 4)
			y = std::max(4.0, m_tip_y - h - 8);
		out.push({ op::RECT, x, y, w, h, 4, 4, 4, 4 });
		out.color(PANE);
		out.push({ op::STROKE_RECT, x + 0.5, y + 0.5, w - 1, h - 1, 4, 4, 4, 4, 1 });
		out.color(LINE);
		for (size_t i = 0; i < lines.size(); i++)
		{
			out.push({ op::TEXT, x + pad, y + pad + 12 + double(i) * 16, double(face), size, 0 });
			out.color(TEXT);
			out.data.push_back(out.text(lines[i]));
		}
	}
}

} // namespace qdom
