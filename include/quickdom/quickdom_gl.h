// Copyright 2026 Ian Karlsson
//! \file
//! quickdom_gl: the document's drawing commands (qdom_paint's) drawn with nanovg in the host's OpenGL 2.1 context.
//! The host makes it once its context is current and draws each frame with it:
//!
//!     qdom_gl_t *gl = qdom_gl_create(dom);              // the context current
//!     ...
//!     if (qdom_frame(dom, now) || qdom_needs_paint(dom))
//!     {
//!         qdom_commands_t c = qdom_paint(dom, true);
//!         qdom_gl_draw(gl, &c, width, height, scale);    // into the current framebuffer, then swap
//!     }
//!     qdom_gl_destroy(gl);                              // the context current, before qdom_destroy
//!
//! The pictures of pixels (canvases' putImageData, <img>'s) come to it from the library without the host; the text
//! is drawn from the library's fonts. Linux (the system's GL) and Windows (opengl32, its newer functions loaded).
#ifndef QUICKDOM_GL_H
#define QUICKDOM_GL_H

#include "quickdom/quickdom.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qdom_gl qdom_gl_t;

//! The renderer, in the GL context current now; the library hands it every picture again. NULL when the context
//! cannot take it (no GL 2.1).
qdom_gl_t *qdom_gl_create(qdom_t *dom);
//! Its textures and buffers freed: the same context current.
void qdom_gl_destroy(qdom_gl_t *gl);
//! A frame's commands drawn into the current framebuffer: the viewport width by height CSS pixels, scale device
//! pixels to one (the framebuffer width * scale by height * scale), cleared first.
void qdom_gl_draw(qdom_gl_t *gl, const qdom_commands_t *commands, double width, double height, double scale);

#ifdef __cplusplus
}
#endif

#endif
