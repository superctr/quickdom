// Copyright 2026 Ian Karlsson
//! \file
//! quickdom_pugl: a document in a PUGL view with an OpenGL context, a window of its own or a child of a host's
//! native window (a plugin's). The window hands the system's input to the document, runs its frames, draws them
//! with quickdom_gl, follows its cursor and its title, and gives it the system's clipboard. The host keeps the
//! runtime, the document and the clock:
//!
//!     qdom_window_options_t o = { .title = "xvgui", .width = 1600, .height = 1000 };
//!     qdom_window_t *w = qdom_window_create(&o, err, sizeof(err));
//!     qdom_window_attach(w, dom);
//!     qdom_window_show(w);
//!     while (!qdom_window_closed(w))
//!     {
//!         qdom_advance(dom, now);                 // the timers; the frames run at this time
//!         qdom_window_update(w, 1.0 / 120);       // events, a frame, drawn if it changed
//!     }
//!     qdom_window_destroy(w);                     // before qdom_destroy
//!
//! A window with no document attached draws nothing of its own (black): another view may sit in it, as a plugin's
//! sits in a host's, sized in the view's device pixels (qdom_window_pixels, qdom_window_set_pixels).
//!
//! Linux (X11 and GLX) and Windows (WGL); the native view is a window id or an HWND.
#ifndef QUICKDOM_PUGL_H
#define QUICKDOM_PUGL_H

#include <stdint.h>

#include "quickdom/quickdom.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qdom_window qdom_window_t;

typedef struct qdom_window_options
{
	//! The window's title; NULL follows the document's (document.title).
	const char *title;
	//! The window's class (X11's WM_CLASS); NULL: "quickdom".
	const char *class_name;
	//! The size it opens at and the least it may take, in CSS pixels (the system's scale factor times them in the
	//! view's); zero: 1600 by 1000, no least.
	int width, height;
	int min_width, min_height;
	//! Whether the user may resize it.
	bool fixed_size;
	//! A native window to sit in (an X11 window id), or 0 for a window of its own.
	uintptr_t parent;
} qdom_window_options_t;

//! The window, made but not shown; NULL with why in err when there is no display.
qdom_window_t *qdom_window_create(const qdom_window_options_t *options, char *err, size_t err_size);
//! The window closed and freed, its renderer freed in its context: before the document it shows is destroyed.
void qdom_window_destroy(qdom_window_t *w);
//! The document it shows, NULL for none: its viewport is the window's from then on, its input the window's, and
//! it is drawn by a renderer made for it in the window's context (the one before freed). A document is shown by
//! one window at a time.
void qdom_window_attach(qdom_window_t *w, qdom_t *dom);
//! The window shown and raised; false when it could not be made.
bool qdom_window_show(qdom_window_t *w);
//! The system's events for up to timeout seconds (0: those waiting), handed to the document; then a frame at the
//! time of the document's last qdom_advance, drawn when something changed (a hidden window's frames run all the
//! same, undrawn).
void qdom_window_update(qdom_window_t *w, double timeout);
//! Whether the user closed it.
bool qdom_window_closed(const qdom_window_t *w);

//! The host decides whether the window closes: while \p hold is set, the window's close button (or the system's close)
//! only asks (qdom_window_close_requested), and qdom_window_close() closes it.
void qdom_window_hold_close(qdom_window_t *w, bool hold);
//! The user asked to close the window since the last call (with the close held).
bool qdom_window_close_requested(qdom_window_t *w);
//! Close the window: qdom_window_closed() is true from now on.
void qdom_window_close(qdom_window_t *w);

//! The window asked to take a size, in CSS pixels (a page's own request).
void qdom_window_set_size(qdom_window_t *w, int width, int height);
//! The view's size in device pixels, as the system last configured it (before it is shown: the size it opens at).
void qdom_window_pixels(const qdom_window_t *w, int *width, int *height);
//! The view asked to take a size in device pixels (before it is shown: the size it opens at).
void qdom_window_set_pixels(qdom_window_t *w, int width, int height);
//! The least and the most the user may resize the view to, in device pixels; zero: no limit. Equal limits fix its
//! size.
void qdom_window_set_pixel_limits(qdom_window_t *w, int min_width, int min_height, int max_width, int max_height);
//! Its title now (NULL: the document's again).
void qdom_window_set_title(qdom_window_t *w, const char *title);
//! The native view (an X11 window id): a parent for the host's own dialogs.
uintptr_t qdom_window_native(const qdom_window_t *w);
//! The system's device pixels per CSS pixel.
double qdom_window_scale(const qdom_window_t *w);
//! The keyboard focus to the window's own view (from a child's).
void qdom_window_focus(qdom_window_t *w);
//! Whether the window's own view has the keyboard focus.
bool qdom_window_has_focus(const qdom_window_t *w);

//! A native view inside a shown window, for another view to sit in (a plugin's beside a page): drawn black where
//! nothing covers it, and gone with the window.
typedef struct qdom_child qdom_child_t;
//! A child at x, y (device pixels from the window's top left), width by height; NULL before the window is shown.
qdom_child_t *qdom_window_add_child(qdom_window_t *w, int x, int y, int width, int height);
void qdom_child_destroy(qdom_child_t *c);
//! Moved and sized, in device pixels.
void qdom_child_place(qdom_child_t *c, int x, int y, int width, int height);
//! Its native view (an X11 window id): the parent of the view in it.
uintptr_t qdom_child_native(const qdom_child_t *c);
//! The keyboard focus to it.
void qdom_child_focus(qdom_child_t *c);
//! Whether the keyboard focus is in one of the window's children, or in a view inside one.
bool qdom_window_child_focused(const qdom_window_t *w);

//! The document laid out at width by height CSS pixels and drawn at scale device pixels to one into rgba (the
//! picture's width * scale by height * scale pixels, rounded, 4 bytes each, the top row first), in the window's
//! context without showing it; its own size comes back at the next update. False without a document or a context.
bool qdom_window_snapshot(qdom_window_t *w, unsigned char *rgba, int width, int height, double scale);

#ifdef __cplusplus
}
#endif

#endif
