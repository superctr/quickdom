// Copyright 2026 Ian Karlsson
// The window's one call into X11 itself, apart from quickdom's headers (X11's macros take common names).
#include "pugl/keys.h"

#if defined(__linux__) || defined(__FreeBSD__)
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#endif

namespace qdom::pugl
{

void ask_focus(void *display, uintptr_t window)
{
#if defined(__linux__) || defined(__FreeBSD__)
	auto *d = static_cast<Display *>(display);
	if (!d || !window)
		return;
	XEvent e {};
	e.xclient.type = ClientMessage;
	e.xclient.window = Window(window);
	e.xclient.message_type = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);
	e.xclient.format = 32;
	e.xclient.data.l[0] = 2;
	e.xclient.data.l[1] = CurrentTime;
	XSendEvent(d, DefaultRootWindow(d), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
	XFlush(d);
#endif
}

void give_focus(void *display, uintptr_t window)
{
#if defined(__linux__) || defined(__FreeBSD__)
	auto *d = static_cast<Display *>(display);
	if (!d || !window)
		return;
	XSetInputFocus(d, Window(window), RevertToParent, CurrentTime);
	XFlush(d);
#endif
}

void detectable_repeat(void *display)
{
#if defined(__linux__) || defined(__FreeBSD__)
	auto *d = static_cast<Display *>(display);
	Bool supported = False;
	if (d)
		XkbSetDetectableAutoRepeat(d, True, &supported);
#endif
}

bool focus_within(void *display, const uintptr_t *windows, size_t count)
{
#if defined(__linux__) || defined(__FreeBSD__)
	auto *d = static_cast<Display *>(display);
	if (!d || !count)
		return false;
	Window focus = 0;
	int revert = 0;
	XGetInputFocus(d, &focus, &revert);
	// up from the focused window to the root
	for (int depth = 0; focus && focus != PointerRoot && depth < 64; depth++)
	{
		for (size_t i = 0; i < count; i++)
			if (focus == Window(windows[i]))
				return true;
		Window root = 0, parent = 0, *children = nullptr;
		unsigned n = 0;
		if (!XQueryTree(d, focus, &root, &parent, &children, &n))
			return false;
		if (children)
			XFree(children);
		if (parent == root)
			return false;
		focus = parent;
	}
#endif
	return false;
}

} // namespace qdom::pugl
