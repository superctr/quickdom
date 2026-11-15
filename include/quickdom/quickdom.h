// Copyright 2026 Ian Karlsson
//! \file
//! quickdom: a DOM for QuickJS. The browser's document and window, their styles and layout, in a QuickJS-ng
//! context the host owns, so that a page's JavaScript runs on it as it runs in a browser.
//!
//! The host creates the runtime and the context, adds its own globals and module loader, and then the library:
//!
//!     qdom_t *dom = qdom_create(ctx, &(qdom_options_t){ .log = my_log });
//!     qdom_add_default_fonts(dom);
//!     qdom_add_stylesheet(dom, css, strlen(css));       // the page's stylesheet (a <link> in the browser)
//!     qdom_set_viewport(dom, 1600, 1000, 1.0);
//!     ... evaluate the page's modules ...
//!     for (;;) { ...qdom_input_*()...; qdom_advance(dom, now); if (qdom_frame(dom, now)) ...draw...; }
//!     qdom_destroy(dom);                                // before JS_FreeContext
//!
//! The library owns no clock and no thread: the host moves the time on (timers run in qdom_advance, animation
//! frames and resize observers in qdom_frame). It is used on one thread at a time. The window's input (the
//! pointer, the wheel, the keys, typed text) is handed over with the qdom_input_ functions, which fire the DOM's
//! events and do what a browser does by itself (hover, focus, clicks, scrolling, text fields' editing, a list's
//! popup, tooltips), at the time of the last qdom_advance or qdom_frame.
#ifndef QUICKDOM_H
#define QUICKDOM_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qdom_instance qdom_t;

//! The host leaves setTimeout, setInterval and their clears to itself.
#define QDOM_NO_TIMERS 1

typedef struct qdom_options
{
	//! A line the library logs: an exception thrown by a listener or a callback, a stylesheet it cannot read.
	//! NULL writes them to stderr.
	void (*log)(void *user, const char *text);
	//! A picture of pixels the commands draw by its number, RGBA rows, handed over when it changes (and again after a
	//! GL backend is made): a canvas's putImageData (a positive number), an <img>'s picture (a negative number: a
	//! still picture, drawn smooth from mipmaps). NULL when the host draws no pixels, or leaves them to quickdom_gl.
	void (*image)(void *user, int id, int width, int height, const unsigned char *rgba);
	//! The bytes of a file the page names (an <img>'s src) are asked for: answer with qdom_resource, now or later.
	void (*resource)(void *user, const char *src);
	//! The pointer's shape changed: a CSS cursor name (default, pointer, text, ns-resize, ...).
	void (*cursor)(void *user, const char *name);
	//! Text for the clipboard (a field's copy or cut), UTF-8.
	void (*copy)(void *user, const char *text, size_t length);
	//! The clipboard's text is asked for (a field's paste): answer with qdom_input_paste, now or later.
	void (*paste)(void *user);
	void *user;
	int flags;  //!< QDOM_NO_TIMERS
} qdom_options_t;

//! A frame's drawing: the commands as numbers (an op, then its numbers: the format of quickdom_gl, see
//! src/paint/commands.h) and their texts, NUL-separated, a TEXT op naming its text by number. Valid until the
//! next qdom_paint.
typedef struct qdom_commands
{
	const float *data;
	size_t count;
	const char *texts;
	size_t texts_size;
} qdom_commands_t;

//! The document and the window installed in a context: document, window (the global object), the node classes,
//! Event and its kinds, getComputedStyle, requestAnimationFrame, ResizeObserver, performance, and the timers.
qdom_t *qdom_create(JSContext *ctx, const qdom_options_t *options);
//! Its references freed and its functions made to throw; the context may be freed after.
void qdom_destroy(qdom_t *dom);

//! The window's size in CSS pixels and the device pixels per CSS pixel.
void qdom_set_viewport(qdom_t *dom, double width, double height, double scale);
//! A stylesheet of the host's, under the document's own <style> elements (as a <link> in the head).
void qdom_add_stylesheet(qdom_t *dom, const char *css, size_t length);

//! A TrueType font for a family's weight and style; the bytes are copied. ceil_metrics: the font's ascent and
//! descent are rounded up as Firefox does for a font with hinting (DejaVu), else to the nearest pixel (Noto). A
//! font's colour bitmaps (CBDT, as Noto Color Emoji's) are drawn as its glyphs' pictures; it needs outlines (glyf or
//! CFF, empty ones will do) for stb_truetype to take it.
bool qdom_add_font(qdom_t *dom, const char *family, int weight, bool italic, const void *data, size_t size,
	bool ceil_metrics);
//! The fonts built into the library (Noto Sans, DejaVu Sans, DejaVu Sans Mono, and the fallbacks after them: subsets
//! of Noto Sans Symbols 2 and of Noto Color Emoji, its colour pictures); false when it has none.
bool qdom_add_default_fonts(qdom_t *dom);
//! The family a generic name (sans-serif, monospace, system-ui, ...) stands for.
void qdom_set_generic_font(qdom_t *dom, const char *generic, const char *family);

//! The time moved on to now (milliseconds): the timers due run, and the promises they settle.
void qdom_advance(qdom_t *dom, double now);
//! When the next timer is due, or a negative number when none is set.
double qdom_next_timer(qdom_t *dom);
//! Whether an animation frame was asked for.
bool qdom_frame_requested(qdom_t *dom);
//! A frame at now: the animation frames asked for, the styles and the layout, the resize observers. True when
//! something changed that needs drawing.
bool qdom_frame(qdom_t *dom, double now);
//! The styles and the layout brought up to date now.
void qdom_update(qdom_t *dom);

//! The document drawn: its commands in CSS pixels of the viewport, a list's popup and a tooltip over it. caret:
//! false leaves out a focused field's caret (a still picture); true draws it as it blinks.
qdom_commands_t qdom_paint(qdom_t *dom, bool caret);
//! Whether something changed since the last qdom_paint that needs drawing.
bool qdom_needs_paint(qdom_t *dom);

//! The modifier keys held, for the input calls.
#define QDOM_MOD_SHIFT 1
#define QDOM_MOD_CTRL 2
#define QDOM_MOD_ALT 4
#define QDOM_MOD_META 8

//! What the pointer did.
#define QDOM_POINTER_MOVE 0
#define QDOM_POINTER_DOWN 1
#define QDOM_POINTER_UP 2

//! The pointer moved, or a button was pressed or released, at x, y in CSS pixels of the viewport. button: as the
//! DOM numbers them (0 the left, 1 the middle, 2 the right); mods: QDOM_MOD_ bits.
void qdom_input_pointer(qdom_t *dom, int kind, double x, double y, int button, int mods);
//! The wheel turned at x, y by notches (dy positive: down; fractions from a touchpad), 48 pixels each.
void qdom_input_wheel(qdom_t *dom, double x, double y, double dx, double dy, int mods);
//! A key pressed (down) or released: key and code as the DOM names them ("a", "A", "Enter", "ArrowLeft", " ";
//! "KeyA", "Digit1", "Space"; code may be NULL), repeat for a held key's repeats. True when the page or the library
//! used it (a listener canceled it, or a field, the focus, a list or a dialog took it): the host may hand the
//! others on (a plugin's host).
bool qdom_input_key(qdom_t *dom, bool down, const char *key, const char *code, bool repeat, int mods);
//! Text typed, UTF-8, after its key's keydown: printable characters only (Enter, Tab and Backspace are keys).
void qdom_input_text(qdom_t *dom, const char *text, size_t length);
//! The clipboard's text, answering the paste callback.
void qdom_input_paste(qdom_t *dom, const char *text, size_t length);
//! The pointer left the window.
void qdom_input_leave(qdom_t *dom);
//! What a drag from another program did.
#define QDOM_DRAG_OVER 0
#define QDOM_DRAG_LEAVE 1
#define QDOM_DRAG_DROP 2
//! A drag of files from another program: over the window at x, y (dragenter and dragleave as it crosses elements,
//! dragover), out of it (dragleave), or dropped there (drop), its paths given with the drop (count of them; none
//! while it is dragged over, when the dataTransfer's types say only that it carries files). Each file in the
//! dataTransfer has its path (file.path, as Electron gives it). True when the page canceled the dragover (it would
//! take the drop) or the drop (it took it).
bool qdom_input_drag(qdom_t *dom, int kind, double x, double y, const char *const *paths, size_t count);
//! The window lost the keyboard's focus, or got it back.
void qdom_input_blur(qdom_t *dom);
void qdom_input_focus(qdom_t *dom);
//! The pointer's shape now: a CSS cursor name.
const char *qdom_cursor(qdom_t *dom);
//! The document's title (document.title, its <title>'s text): the window's. Valid until the next call.
const char *qdom_title(qdom_t *dom);

//! A file's bytes, answering the resource callback (NULL: there is none); decoded now (PNG, JPEG), the page told at
//! the next frame (load, or error).
void qdom_resource(qdom_t *dom, const char *src, const void *data, size_t size);

//! What the library's work took since the document was made or qdom_reset_stats: frames (qdom_frame), restyles and
//! the styles they computed, layouts and the boxes they laid out, paints (qdom_paint); each one's time in all and its
//! slowest, in milliseconds. For a host measuring it.
typedef struct qdom_stats
{
	unsigned long long frames, restyles, styles, layouts, boxes, paints;
	double frame_ms, style_ms, layout_ms, paint_ms;
	double frame_max_ms, style_max_ms, layout_max_ms, paint_max_ms;
} qdom_stats_t;
qdom_stats_t qdom_stats(qdom_t *dom);
void qdom_reset_stats(qdom_t *dom);

//! Every element's box under the body, laid out: JSON [[path, left, top, width, height], ...] with the path as
//! "/tag#id.class/..." and the measures to a tenth of a pixel (as the tests' boxes.js gives them in a browser).
//! Free with qdom_free.
char *qdom_boxes(qdom_t *dom);
void qdom_free(void *p);

#ifdef __cplusplus
}
#endif

#endif
