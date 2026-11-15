// Copyright 2026 Ian Karlsson
// The entry points of OpenGL beyond 1.1, fetched from the current context where the system's library has only 1.1.
#include "gl/api.h"

#if defined(_WIN32)

#include <mutex>

namespace qdom_glfn
{
#define QDOM_GL_DEFINE(type, name) type name = nullptr;
QDOM_GL_FUNCTIONS(QDOM_GL_DEFINE)
#undef QDOM_GL_DEFINE
} // namespace qdom_glfn

namespace
{

void *entry(HMODULE library, const char *name)
{
	void *p = reinterpret_cast<void *>(wglGetProcAddress(name));
	// some drivers answer 1, 2, 3 or -1 for a function they lack
	if (!p || p == reinterpret_cast<void *>(1) || p == reinterpret_cast<void *>(2) || p == reinterpret_cast<void *>(3)
		|| p == reinterpret_cast<void *>(-1))
		p = library ? reinterpret_cast<void *>(GetProcAddress(library, name)) : nullptr;
	return p;
}

} // namespace

bool qdom_gl_load()
{
	static std::mutex lock;
	static bool loaded = false;
	std::lock_guard<std::mutex> hold(lock);
	if (loaded)
		return true;
	if (!wglGetCurrentContext())
		return false;
	HMODULE library = GetModuleHandleA("opengl32.dll");
	bool all = true;
#define QDOM_GL_LOAD(type, name) \
	{ \
		auto *p = reinterpret_cast<type>(entry(library, #name)); \
		all = all && p; \
		qdom_glfn::name = p; \
	}
	QDOM_GL_FUNCTIONS(QDOM_GL_LOAD)
#undef QDOM_GL_LOAD
	loaded = all;
	return all;
}

#else

bool qdom_gl_load()
{
	return true;
}

#endif
