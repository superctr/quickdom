// Copyright 2026 Ian Karlsson
// The page drawn by quickdom_gl in an EGL surfaceless context (the GPU's, or Mesa's llvmpipe), into a framebuffer of
// colours and a stencil, read back: qdom-run --shot.
#include "shots.h"

#include <algorithm>
#include <cmath>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include "quickdom/quickdom_gl.h"

namespace
{

EGLDisplay open_display()
{
	auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
	if (platform)
	{
		EGLDisplay d = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
		if (d != EGL_NO_DISPLAY)
			return d;
	}
	return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

} // namespace

bool draw_offscreen(qdom_t *dom, int width, int height, double scale, Image &out, std::string &error)
{
	const int fw = int(std::lrint(width * scale)), fh = int(std::lrint(height * scale));
	EGLDisplay dpy = open_display();
	EGLint major = 0, minor = 0;
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor))
	{
		error = "no EGL display";
		return false;
	}
	struct Display
	{
		EGLDisplay d;
		~Display() { eglTerminate(d); }
	} display { dpy };
	if (!eglBindAPI(EGL_OPENGL_API))
	{
		error = "EGL offers no OpenGL";
		return false;
	}
	// no surface: a pbuffer's configuration, or any (a window's is the default)
	EGLConfig config;
	EGLint count = 0;
	for (EGLint surface : { EGL_PBUFFER_BIT, 0 })
	{
		const EGLint want[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, surface, EGL_NONE };
		if (eglChooseConfig(dpy, want, &config, 1, &count) && count > 0)
			break;
	}
	if (count < 1)
	{
		error = "no EGL configuration for OpenGL";
		return false;
	}
	EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, nullptr);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
	{
		if (ctx != EGL_NO_CONTEXT)
			eglDestroyContext(dpy, ctx);
		error = "no surfaceless OpenGL context";
		return false;
	}
	GLuint fbo = 0, rb[2] = { 0, 0 };
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenRenderbuffers(2, rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, fw, fh);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
	glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fw, fh);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
	bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	qdom_gl_t *gl = ok ? qdom_gl_create(dom) : nullptr;
	if (!gl)
	{
		error = ok ? "quickdom_gl could not start in the context (no OpenGL 2.1?)" : "no framebuffer";
		ok = false;
	}
	else
	{
		// the frame as it is, the caret left out: a still picture
		const qdom_commands_t c = qdom_paint(dom, false);
		qdom_gl_draw(gl, &c, width, height, scale);
		glFinish();
		out.width = fw;
		out.height = fh;
		std::vector<uint8_t> rows(size_t(fw) * size_t(fh) * 4);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
		// GL's rows from the bottom
		out.rgba.resize(rows.size());
		const size_t stride = size_t(fw) * 4;
		for (int y = 0; y < fh; y++)
			std::copy_n(rows.data() + size_t(fh - 1 - y) * stride, stride, out.rgba.data() + size_t(y) * stride);
		for (size_t i = 3; i < out.rgba.size(); i += 4)
			out.rgba[i] = 255;
		qdom_gl_destroy(gl);
	}
	glDeleteRenderbuffers(2, rb);
	glDeleteFramebuffers(1, &fbo);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, ctx);
	return ok;
}
