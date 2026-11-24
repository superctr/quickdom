// Copyright 2026 Ian Karlsson
// quickdom_pugl: a document in a PUGL view (see quickdom_pugl.h and docs/design.md §3.8).
#include "pugl/window.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "gl/api.h"

#include <pugl/gl.h>

#include "instance.h"
#include "pugl/keys.h"
#include "quickdom/quickdom_gl.h"

namespace
{

constexpr int DEFAULT_WIDTH = 1600, DEFAULT_HEIGHT = 1000;

int mods_of(uint32_t m)
{
	return (m & PUGL_MOD_SHIFT ? QDOM_MOD_SHIFT : 0) | (m & PUGL_MOD_CTRL ? QDOM_MOD_CTRL : 0)
		| (m & PUGL_MOD_ALT ? QDOM_MOD_ALT : 0) | (m & PUGL_MOD_SUPER ? QDOM_MOD_META : 0);
}

// PUGL's buttons are left, right, middle; the DOM's left, middle, right
int button_of(uint32_t b)
{
	return b == 1 ? 2 : b == 2 ? 1 : int(b);
}

} // namespace

struct qdom_child
{
	qdom_window_t *owner = nullptr;
	PuglView *view = nullptr;
	uint32_t width = 0, height = 0;
};

struct qdom_window
{
	PuglWorld *world = nullptr;
	PuglView *view = nullptr;
	qdom_t *dom = nullptr;
	qdom_gl_t *gl = nullptr;
	bool own = true;              // a window of its own, not in a host's
	bool realized = false, closed = false, asked = false;
	// the host decides on closing: a close only asks
	bool hold_close = false, close_asked = false;
	double scale = 1;
	uint32_t width = 0, height = 0;  // the view's pixels
	// the viewport last given to the document; negative: to be given again
	double vp_width = -1, vp_height = -1, vp_scale = -1;
	std::string title;            // the host's
	bool own_title = false;
	std::string shown_title;
	std::string cursor = "default";
	// the keys held by their key codes, with the names their presses had: a press of one again is its repeat
	struct Held
	{
		uint32_t keycode;
		std::string name;
	};
	std::vector<Held> held;
	// the last button press or release, to know it again: Wine under X11 sends each one twice
	PuglButtonEvent last_button = {};
	// the pointer's last move not handed over yet: the moves between two frames go as one
	bool moved = false;
	double move_x = 0, move_y = 0;
	int move_mods = 0;
	// a character key's press, named by the text it types, which PUGL sends after it
	bool key_waiting = false;
	uint32_t key = 0, keycode = 0;
	int key_mods = 0;
	bool key_repeat = false;
	bool paste_asked = false;
	// a drag of files from another program over the window: where it is, and whether the page takes it there
	bool dragging = false, drag_taken = false;
	double drag_x = 0, drag_y = 0;
	std::vector<qdom_child_t *> children;

	double css(double v) const { return v / (scale > 0 ? scale : 1); }
};

namespace
{

void flush_move(qdom_window_t *w)
{
	if (!w->moved || !w->dom)
		return;
	w->moved = false;
	qdom_input_pointer(w->dom, QDOM_POINTER_MOVE, w->move_x, w->move_y, 0, w->move_mods);
}

// the key's press handed over, named by its text when it typed one
void flush_key(qdom_window_t *w, const char *text)
{
	if (!w->key_waiting || !w->dom)
		return;
	w->key_waiting = false;
	const bool printable = text && static_cast<unsigned char>(text[0]) >= 0x20 && text[0] != 0x7f;
	const std::string name = printable ? std::string(text) : qdom::pugl::key_name(w->key, w->key_mods & QDOM_MOD_SHIFT);
	if (name.empty())
		return;
	for (auto &h : w->held)
		if (h.keycode == w->keycode)
			h.name = name;
	qdom_input_key(w->dom, true, name.c_str(), qdom::pugl::key_code(w->keycode, w->key), w->key_repeat, w->key_mods);
}

void give_viewport(qdom_window_t *w)
{
	if (!w->dom || !w->width || !w->height)
		return;
	const double cw = w->css(w->width), ch = w->css(w->height);
	if (cw == w->vp_width && ch == w->vp_height && w->scale == w->vp_scale)
		return;
	w->vp_width = cw;
	w->vp_height = ch;
	w->vp_scale = w->scale;
	qdom_set_viewport(w->dom, cw, ch, w->scale);
}

void follow(qdom_window_t *w)
{
	if (!w->dom)
		return;
	const std::string c = qdom_cursor(w->dom);
	if (c != w->cursor && w->realized)
	{
		w->cursor = c;
		puglSetCursor(w->view, PuglCursor(qdom::pugl::cursor_shape(c)));
	}
	const std::string t = w->own_title ? w->title : qdom_title(w->dom);
	if (t != w->shown_title)
	{
		w->shown_title = t;
		puglSetViewString(w->view, PUGL_WINDOW_TITLE, t.empty() ? "quickdom" : t.c_str());
	}
}

// the renderer freed in the window's context, which it draws in
void drop_renderer(qdom_window_t *w)
{
	if (!w->gl)
		return;
	const bool entered = w->realized && puglEnterContext(w->view) == PUGL_SUCCESS;
	qdom_gl_destroy(w->gl);
	w->gl = nullptr;
	if (entered)
		puglLeaveContext(w->view);
}

// the clipboard: the system's, through PUGL
void copy_text(void *user, const char *text, size_t length)
{
	auto *w = static_cast<qdom_window_t *>(user);
	if (w->realized)
		puglSetClipboard(w->view, PUGL_CLIPBOARD_GENERAL, "text/plain", text, length);
}

void paste_text(void *user)
{
	auto *w = static_cast<qdom_window_t *>(user);
	if (w->realized && puglPaste(w->view) == PUGL_SUCCESS)
		w->paste_asked = true;
	else if (w->dom)
		qdom_input_paste(w->dom, "", 0);
}

void document_gone(void *ctx)
{
	qdom_window_attach(static_cast<qdom_window_t *>(ctx), nullptr);
}

// the focus for a window of its own once it is shown: X11's window managers keep it from a window that names no
// user's action, so it is asked for as a pager does
void activate(qdom_window_t *w)
{
	qdom::pugl::ask_focus(puglGetNativeWorld(w->world), uintptr_t(puglGetNativeView(w->view)));
	puglGrabFocus(w->view);
}

void expose(qdom_window_t *w)
{
	if (!w->dom)
	{
		glViewport(0, 0, GLsizei(w->width), GLsizei(w->height));
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}
	if (!w->gl)
		w->gl = qdom_gl_create(w->dom);
	if (!w->gl)
		return;
	const qdom_commands_t c = qdom_paint(w->dom, true);
	qdom_gl_draw(w->gl, &c, w->css(w->width), w->css(w->height), w->scale);
}

// the system's refusal of an offer; PUGL's Windows view has none (a drop there comes alone, taken or not)
void reject(qdom_window_t *w, const PuglDataOfferEvent &offer)
{
#if !defined(_WIN32)
	puglRejectOffer(w->view, &offer, 0, 0, w->width, w->height);
#endif
}

// a drag from another program moving over the window: taken where the page cancels its dragover, and then only as
// files (text/uri-list)
void drag_offered(qdom_window_t *w, const PuglDataOfferEvent &offer)
{
	const uint32_t n = puglGetNumClipboardTypes(w->view, offer.clipboard);
	int files = -1;
	for (uint32_t i = 0; i < n; i++)
		if (const char *type = puglGetClipboardType(w->view, offer.clipboard, i); type && !strcmp(type, "text/uri-list"))
			files = int(i);
	w->dragging = true;
	w->drag_x = w->css(offer.x);
	w->drag_y = w->css(offer.y);
	w->drag_taken = files >= 0 && w->dom && qdom_input_drag(w->dom, QDOM_DRAG_OVER, w->drag_x, w->drag_y, nullptr, 0);
	if (w->drag_taken)
		puglAcceptOffer(w->view, &offer, uint32_t(files), PUGL_DATA_ACTION_COPY, 0, 0, w->width, w->height);
	else
		reject(w, offer);
}

// the drop's files, where it was taken
void dropped(qdom_window_t *w, const PuglDataEvent &data)
{
	// Windows hands over a drop alone, with no drag over the window before it
	if (!w->dragging && w->dom)
	{
		w->drag_x = w->css(data.x);
		w->drag_y = w->css(data.y);
		w->drag_taken = qdom_input_drag(w->dom, QDOM_DRAG_OVER, w->drag_x, w->drag_y, nullptr, 0);
	}
	w->dragging = false;
	if (!w->drag_taken || !w->dom)
		return;
	size_t length = 0;
	const auto *text = static_cast<const char *>(puglGetClipboard(w->view, data.clipboard, data.typeIndex, &length));
	const std::vector<std::string> paths = text ? qdom::pugl::uri_paths(text, length) : std::vector<std::string>();
	std::vector<const char *> list;
	for (const std::string &p : paths)
		list.push_back(p.c_str());
	qdom_input_drag(w->dom, QDOM_DRAG_DROP, w->drag_x, w->drag_y, list.data(), list.size());
}

// the clipboard's offer taken as text, where it has text
void offered(qdom_window_t *w, const PuglDataOfferEvent &offer)
{
	if (offer.clipboard == PUGL_CLIPBOARD_DRAG)
		return drag_offered(w, offer);
	const uint32_t n = puglGetNumClipboardTypes(w->view, offer.clipboard);
	int best = -1;
	for (uint32_t i = 0; i < n; i++)
	{
		const char *type = puglGetClipboardType(w->view, offer.clipboard, i);
		if (!type)
			continue;
		if (!strcmp(type, "text/plain;charset=utf-8") || !strcmp(type, "UTF8_STRING"))
		{
			best = int(i);
			break;
		}
		if (best < 0 && (!strcmp(type, "text/plain") || !strcmp(type, "STRING") || !strcmp(type, "TEXT")))
			best = int(i);
	}
	if (best >= 0)
		puglAcceptOffer(w->view, &offer, uint32_t(best), PUGL_DATA_ACTION_COPY, 0, 0, w->width, w->height);
	else
	{
		reject(w, offer);
		if (w->paste_asked && w->dom)
		{
			w->paste_asked = false;
			qdom_input_paste(w->dom, "", 0);
		}
	}
}

void received(qdom_window_t *w, const PuglDataEvent &data)
{
	if (data.clipboard == PUGL_CLIPBOARD_DRAG)
		return dropped(w, data);
	if (!w->paste_asked)
		return;
	w->paste_asked = false;
	size_t length = 0;
	const auto *text = static_cast<const char *>(puglGetClipboard(w->view, data.clipboard, data.typeIndex, &length));
	while (text && length && !text[length - 1])
		length--;
	if (w->dom)
		qdom_input_paste(w->dom, text ? text : "", text ? length : 0);
}

// a frame at the time of the document's last qdom_advance, the view obscured when it needs drawing
void frame(qdom_window_t *w)
{
	if (!w->dom)
		return;
	flush_move(w);
	flush_key(w, nullptr);
	give_viewport(w);
	if (qdom_frame(w->dom, w->dom->dom->now()) || qdom_needs_paint(w->dom))
		puglObscureView(w->view);
	follow(w);
}

// a child draws black where its view does not cover it
PuglStatus on_child_event(PuglView *view, const PuglEvent *e)
{
	auto *c = static_cast<qdom_child_t *>(puglGetHandle(view));
	if (e->type == PUGL_CONFIGURE)
	{
		c->width = e->configure.width;
		c->height = e->configure.height;
	}
	else if (e->type == PUGL_EXPOSE)
	{
		glViewport(0, 0, GLsizei(c->width), GLsizei(c->height));
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	return PUGL_SUCCESS;
}

PuglStatus on_event(PuglView *view, const PuglEvent *e)
{
	return qdom::pugl::handle_event(static_cast<qdom_window_t *>(puglGetHandle(view)), *e);
}

} // namespace

namespace qdom::pugl
{

PuglView *view_of(qdom_window_t *w)
{
	return w->view;
}

PuglStatus handle_event(qdom_window_t *w, const PuglEvent &e)
{
	// the pointer where it last was before anything else happens, a key's press before anything but its text
	if (e.type != PUGL_MOTION)
		flush_move(w);
	if (e.type != PUGL_TEXT)
		flush_key(w, nullptr);
	qdom_t *dom = w->dom;
	switch (e.type)
	{
	case PUGL_REALIZE:
		w->realized = true;
		w->scale = puglGetScaleFactor(w->view);
		break;
	case PUGL_UNREALIZE:
		// the context is current
		qdom_gl_destroy(w->gl);
		w->gl = nullptr;
		w->realized = false;
		break;
	case PUGL_CONFIGURE:
		w->width = e.configure.width;
		w->height = e.configure.height;
		w->scale = puglGetScaleFactor(w->view);
		break;
	case PUGL_UPDATE:
		frame(w);
		break;
	case PUGL_EXPOSE:
		expose(w);
		if (w->own && !w->asked)
		{
			w->asked = true;
			activate(w);
		}
		break;
	case PUGL_BUTTON_PRESS:
	case PUGL_BUTTON_RELEASE:
		if (e.button.type == w->last_button.type && e.button.button == w->last_button.button
			&& e.button.time == w->last_button.time && e.button.x == w->last_button.x && e.button.y == w->last_button.y)
			break;
		w->last_button = e.button;
		if (dom)
			qdom_input_pointer(dom, e.type == PUGL_BUTTON_PRESS ? QDOM_POINTER_DOWN : QDOM_POINTER_UP, w->css(e.button.x),
				w->css(e.button.y), button_of(e.button.button), mods_of(e.button.state));
		break;
	case PUGL_MOTION:
		w->moved = true;
		w->move_x = w->css(e.motion.x);
		w->move_y = w->css(e.motion.y);
		w->move_mods = mods_of(e.motion.state);
		break;
	case PUGL_SCROLL:
		// PUGL counts up as positive, the DOM down
		if (dom)
			qdom_input_wheel(dom, w->css(e.scroll.x), w->css(e.scroll.y), e.scroll.dx, -e.scroll.dy, mods_of(e.scroll.state));
		break;
	case PUGL_POINTER_OUT:
		if (dom)
			qdom_input_leave(dom);
		break;
	case PUGL_FOCUS_IN:
		if (dom)
			qdom_input_focus(dom);
		break;
	case PUGL_FOCUS_OUT:
		w->held.clear();
		if (dom)
			qdom_input_blur(dom);
		break;
	case PUGL_KEY_PRESS:
	case PUGL_KEY_RELEASE:
	{
		const bool press = e.type == PUGL_KEY_PRESS;
		const auto at = std::find_if(w->held.begin(), w->held.end(), [&](const auto &h) { return h.keycode == e.key.keycode; });
		const bool repeat = press && at != w->held.end();
		std::string name = key_name(e.key.key, e.key.state & PUGL_MOD_SHIFT);
		if (!press && at != w->held.end())
		{
			// released by the name it was pressed by
			if (!at->name.empty())
				name = at->name;
			w->held.erase(at);
		}
		else if (press && !repeat)
			w->held.push_back({ e.key.keycode, name });
		if (!dom)
			break;
		if (press && is_character(e.key.key))
		{
			// named when its text comes
			w->key_waiting = true;
			w->key = e.key.key;
			w->keycode = e.key.keycode;
			w->key_mods = mods_of(e.key.state);
			w->key_repeat = repeat;
			break;
		}
		if (!name.empty())
			qdom_input_key(dom, press, name.c_str(), key_code(e.key.keycode, e.key.key), repeat, mods_of(e.key.state));
		break;
	}
	case PUGL_TEXT:
		flush_key(w, e.text.string);
		if (dom && !(e.text.state & (PUGL_MOD_CTRL | PUGL_MOD_ALT | PUGL_MOD_SUPER)) && e.text.character >= 0x20
			&& e.text.character != 0x7f)
			qdom_input_text(dom, e.text.string, strlen(e.text.string));
		break;
	case PUGL_DATA_OFFER:
		offered(w, e.offer);
		break;
	case PUGL_DATA:
		received(w, e.data);
		break;
	case PUGL_CLOSE:
		if (w->hold_close)
			w->close_asked = true;
		else
			w->closed = true;
		break;
	default:
		break;
	}
	return PUGL_SUCCESS;
}

} // namespace qdom::pugl

qdom_window_t *qdom_window_create(const qdom_window_options_t *options, char *err, size_t err_size)
{
	const qdom_window_options_t o = options ? *options : qdom_window_options_t {};
	auto fail = [&](const char *why) {
		if (err && err_size)
			snprintf(err, err_size, "%s", why);
	};
	auto *w = new qdom_window();
	w->own = !o.parent;
	w->world = puglNewWorld(o.parent ? PUGL_MODULE : PUGL_PROGRAM, PuglWorldFlags(0));
	if (!w->world)
	{
		fail("no display");
		delete w;
		return nullptr;
	}
	qdom::pugl::detectable_repeat(puglGetNativeWorld(w->world));
	puglSetWorldString(w->world, PUGL_CLASS_NAME, o.class_name ? o.class_name : "quickdom");
	w->view = puglNewView(w->world);
	puglSetHandle(w->view, w);
	puglSetEventFunc(w->view, on_event);
	puglSetBackend(w->view, puglGlBackend());
	puglSetViewHint(w->view, PUGL_CONTEXT_VERSION_MAJOR, 2);
	puglSetViewHint(w->view, PUGL_CONTEXT_VERSION_MINOR, 0);
	puglSetViewHint(w->view, PUGL_STENCIL_BITS, 8);
#if defined(_WIN32)
	// the window's pixel formats under Wine have no alpha, and nothing here reads the window's alpha
	puglSetViewHint(w->view, PUGL_ALPHA_BITS, 0);
#endif
	puglSetViewHint(w->view, PUGL_DOUBLE_BUFFER, PUGL_TRUE);
	puglSetViewHint(w->view, PUGL_SWAP_INTERVAL, 1);
	puglSetViewHint(w->view, PUGL_RESIZABLE, o.fixed_size ? PUGL_FALSE : PUGL_TRUE);
	// a held key's repeats come as presses alone, without the releases between them
	puglSetViewHint(w->view, PUGL_IGNORE_KEY_REPEAT, PUGL_TRUE);
	puglSetViewHint(w->view, PUGL_ACCEPT_DROP, PUGL_TRUE);
	w->own_title = o.title != nullptr;
	w->title = o.title ? o.title : "";
	w->shown_title = w->own_title ? w->title : "quickdom";
	puglSetViewString(w->view, PUGL_WINDOW_TITLE, w->shown_title.c_str());
	w->scale = puglGetScaleFactor(w->view);
	if (!(w->scale > 0))
		w->scale = 1;
	const int width = o.width > 0 ? o.width : DEFAULT_WIDTH, height = o.height > 0 ? o.height : DEFAULT_HEIGHT;
	w->width = uint32_t(std::lrint(width * w->scale));
	w->height = uint32_t(std::lrint(height * w->scale));
	puglSetSizeHint(w->view, PUGL_DEFAULT_SIZE, PuglSpan(w->width), PuglSpan(w->height));
	if (o.min_width > 0 && o.min_height > 0)
		puglSetSizeHint(w->view, PUGL_MIN_SIZE, PuglSpan(std::lrint(o.min_width * w->scale)),
			PuglSpan(std::lrint(o.min_height * w->scale)));
	if (o.parent)
		puglSetParent(w->view, PuglNativeView(o.parent));
	return w;
}

void qdom_window_destroy(qdom_window_t *w)
{
	if (!w)
		return;
	qdom_window_attach(w, nullptr);
	while (!w->children.empty())
		qdom_child_destroy(w->children.back());
	if (w->view)
	{
		if (w->realized)
			puglUnrealize(w->view);
		puglFreeView(w->view);
	}
	if (w->world)
		puglFreeWorld(w->world);
	delete w;
}

void qdom_window_attach(qdom_window_t *w, qdom_t *dom)
{
	if (w->dom == dom)
		return;
	if (dom && dom->gone)
		dom->gone(dom->gone_ctx);
	drop_renderer(w);
	if (w->dom)
	{
		w->dom->dom->set_window_clipboard(nullptr, nullptr, nullptr);
		w->dom->gone = nullptr;
		w->dom->gone_ctx = nullptr;
	}
	w->dom = dom;
	w->moved = w->key_waiting = w->paste_asked = false;
	w->held.clear();
	w->vp_width = w->vp_height = w->vp_scale = -1;
	w->cursor.clear();
	if (!dom)
		return;
	dom->gone = document_gone;
	dom->gone_ctx = w;
	dom->dom->set_window_clipboard(copy_text, paste_text, w);
	give_viewport(w);
	if (w->realized)
		puglObscureView(w->view);
}

bool qdom_window_show(qdom_window_t *w)
{
	if (!w->realized && puglRealize(w->view) != PUGL_SUCCESS)
		return false;
	return puglShow(w->view, PUGL_SHOW_RAISE) == PUGL_SUCCESS;
}

void qdom_window_update(qdom_window_t *w, double timeout)
{
	puglUpdate(w->world, timeout);
	// a drag that left the window, or was given up: PUGL empties its drag's types and says nothing else
	if (w->dragging && w->realized && !puglGetNumClipboardTypes(w->view, PUGL_CLIPBOARD_DRAG))
	{
		w->dragging = false;
		if (w->dom)
			qdom_input_drag(w->dom, QDOM_DRAG_LEAVE, 0, 0, nullptr, 0);
	}
	// PUGL updates a view only while it is shown: a hidden one's document has its frames all the same
	if (!puglGetVisible(w->view))
		frame(w);
}

bool qdom_window_closed(const qdom_window_t *w)
{
	return w->closed;
}

void qdom_window_hold_close(qdom_window_t *w, bool hold)
{
	w->hold_close = hold;
}

bool qdom_window_close_requested(qdom_window_t *w)
{
	const bool asked = w->close_asked;
	w->close_asked = false;
	return asked;
}

void qdom_window_close(qdom_window_t *w)
{
	w->closed = true;
}

void qdom_window_set_size(qdom_window_t *w, int width, int height)
{
	if (width <= 0 || height <= 0)
		return;
	puglSetSizeHint(w->view, PUGL_CURRENT_SIZE, PuglSpan(std::lrint(width * w->scale)),
		PuglSpan(std::lrint(height * w->scale)));
}

void qdom_window_set_title(qdom_window_t *w, const char *title)
{
	w->own_title = title != nullptr;
	w->title = title ? title : "";
	if (w->own_title)
	{
		w->shown_title = w->title;
		puglSetViewString(w->view, PUGL_WINDOW_TITLE, w->title.c_str());
	}
	else
		w->shown_title.clear();
	if (w->realized)
		puglObscureView(w->view);
}

void qdom_window_pixels(const qdom_window_t *w, int *width, int *height)
{
	if (width)
		*width = int(w->width);
	if (height)
		*height = int(w->height);
}

void qdom_window_set_pixels(qdom_window_t *w, int width, int height)
{
	if (width <= 0 || height <= 0)
		return;
	if (w->realized && puglGetNativeView(w->view))
		puglSetSizeHint(w->view, PUGL_CURRENT_SIZE, PuglSpan(width), PuglSpan(height));
	else
	{
		puglSetSizeHint(w->view, PUGL_DEFAULT_SIZE, PuglSpan(width), PuglSpan(height));
		w->width = uint32_t(width);
		w->height = uint32_t(height);
	}
}

void qdom_window_set_pixel_limits(qdom_window_t *w, int min_width, int min_height, int max_width, int max_height)
{
	if (min_width > 0 && min_height > 0)
		puglSetSizeHint(w->view, PUGL_MIN_SIZE, PuglSpan(min_width), PuglSpan(min_height));
	if (max_width > 0 && max_height > 0)
		puglSetSizeHint(w->view, PUGL_MAX_SIZE, PuglSpan(max_width), PuglSpan(max_height));
}

void qdom_window_focus(qdom_window_t *w)
{
	if (w->realized)
		puglGrabFocus(w->view);
}

bool qdom_window_has_focus(const qdom_window_t *w)
{
	return w->realized && puglHasFocus(w->view);
}

qdom_child_t *qdom_window_add_child(qdom_window_t *w, int x, int y, int width, int height)
{
	if (!w->realized || !puglGetNativeView(w->view) || width <= 0 || height <= 0)
		return nullptr;
	auto *c = new qdom_child();
	c->owner = w;
	c->width = uint32_t(width);
	c->height = uint32_t(height);
	c->view = puglNewView(w->world);
	puglSetHandle(c->view, c);
	puglSetEventFunc(c->view, on_child_event);
	puglSetBackend(c->view, puglGlBackend());
	puglSetViewHint(c->view, PUGL_CONTEXT_VERSION_MAJOR, 2);
	puglSetViewHint(c->view, PUGL_CONTEXT_VERSION_MINOR, 0);
	puglSetParent(c->view, puglGetNativeView(w->view));
	puglSetPositionHint(c->view, PUGL_DEFAULT_POSITION, x, y);
	puglSetSizeHint(c->view, PUGL_DEFAULT_SIZE, PuglSpan(width), PuglSpan(height));
	if (puglRealize(c->view) != PUGL_SUCCESS)
	{
		puglFreeView(c->view);
		delete c;
		return nullptr;
	}
	puglShow(c->view, PUGL_SHOW_PASSIVE);
	w->children.push_back(c);
	return c;
}

void qdom_child_destroy(qdom_child_t *c)
{
	if (!c)
		return;
	auto &list = c->owner->children;
	list.erase(std::remove(list.begin(), list.end(), c), list.end());
	puglUnrealize(c->view);
	puglFreeView(c->view);
	delete c;
}

void qdom_child_place(qdom_child_t *c, int x, int y, int width, int height)
{
	puglSetPositionHint(c->view, PUGL_CURRENT_POSITION, x, y);
	if (width > 0 && height > 0)
		puglSetSizeHint(c->view, PUGL_CURRENT_SIZE, PuglSpan(width), PuglSpan(height));
}

uintptr_t qdom_child_native(const qdom_child_t *c)
{
	return uintptr_t(puglGetNativeView(c->view));
}

void qdom_child_focus(qdom_child_t *c)
{
	qdom::pugl::give_focus(puglGetNativeWorld(c->owner->world), qdom_child_native(c));
}

bool qdom_window_child_focused(const qdom_window_t *w)
{
	std::vector<uintptr_t> natives;
	for (auto *c : w->children)
		natives.push_back(qdom_child_native(c));
	return qdom::pugl::focus_within(puglGetNativeWorld(w->world), natives.data(), natives.size());
}

uintptr_t qdom_window_native(const qdom_window_t *w)
{
	return uintptr_t(puglGetNativeView(w->view));
}

double qdom_window_scale(const qdom_window_t *w)
{
	return w->scale;
}

bool qdom_window_snapshot(qdom_window_t *w, unsigned char *rgba, int width, int height, double scale)
{
	qdom_t *dom = w->dom;
	if (!dom || width <= 0 || height <= 0 || !(scale > 0))
		return false;
	const int fw = int(std::lrint(width * scale)), fh = int(std::lrint(height * scale));
	if (!w->realized && puglRealize(w->view) != PUGL_SUCCESS)
		return false;
	flush_move(w);
	flush_key(w, nullptr);
	// the document at the picture's size, its frames until it settles
	qdom_set_viewport(dom, width, height, scale);
	w->vp_width = w->vp_height = w->vp_scale = -1;
	const double now = dom->dom->now();
	qdom_frame(dom, now);
	for (int k = 0; k < 8 && qdom_frame_requested(dom); k++)
		qdom_frame(dom, now);
	if (puglEnterContext(w->view) != PUGL_SUCCESS)
		return false;
	if (!qdom_gl_load())
	{
		puglLeaveContext(w->view);
		return false;
	}
	bool ok = false;
	if (!w->gl)
		w->gl = qdom_gl_create(dom);
	GLuint fbo = 0, rb[2] = { 0, 0 };
	if (w->gl)
	{
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glGenRenderbuffers(2, rb);
		glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, fw, fh);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
		glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fw, fh);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
		{
			// a still picture: the caret left out
			const qdom_commands_t c = qdom_paint(dom, false);
			qdom_gl_draw(w->gl, &c, width, height, scale);
			glFinish();
			const size_t stride = size_t(fw) * 4;
			std::vector<unsigned char> rows(stride * size_t(fh));
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
			// GL's rows from the bottom
			for (int y = 0; y < fh; y++)
				std::memcpy(rgba + size_t(y) * stride, rows.data() + size_t(fh - 1 - y) * stride, stride);
			for (size_t i = 3; i < stride * size_t(fh); i += 4)
				rgba[i] = 255;
			ok = true;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDeleteRenderbuffers(2, rb);
		glDeleteFramebuffers(1, &fbo);
	}
	puglLeaveContext(w->view);
	puglObscureView(w->view);
	return ok;
}
