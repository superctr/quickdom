// Copyright 2026 Ian Karlsson
//! \file
//! PUGL's keys as the DOM names them: a key event's `key` from PUGL's key (or the text it typed), its `code` from the
//! key's place on the keyboard; and the window's own requests of the system.
#pragma once

#include <cstddef>

#include <cstdint>
#include <string>
#include <vector>

namespace qdom::pugl
{

//! Whether PUGL's key is a character's (its DOM name is the text the press types, which PUGL hands over after it).
bool is_character(uint32_t key);
//! The DOM's name of a key PUGL names: a special key's ("Enter", "ArrowLeft", "F5", "Shift"), or a character's own
//! (shift: a letter as a capital, for a press that typed no text). Empty for a key the DOM has no name for.
std::string key_name(uint32_t key, bool shift);
//! The DOM's code of a key by its place: X11's key code (evdev's plus 8), or Windows' scan code, on a US keyboard
//! ("KeyA", "Digit1", "ArrowLeft", "NumpadEnter"); null for one it does not know. \p key is PUGL's key of the press,
//! which tells Windows' extended keys from the keypad's that have their scan code.
const char *key_code(uint32_t keycode, uint32_t key = 0);
//! The same from Windows' scan code (set 1, without the extended prefix) and PUGL's key.
const char *scan_code(uint32_t scancode, uint32_t key);
//! A CSS cursor name as one of PUGL's shapes (PuglCursor).
int cursor_shape(const std::string &name);
//! A window of its own asks for the keyboard's focus as a pager does (X11's _NET_ACTIVE_WINDOW): the display and
//! the window as PUGL's native world and view.
void ask_focus(void *display, uintptr_t window);
//! The keyboard focus to a window of this client's own (a child view).
void give_focus(void *display, uintptr_t window);
//! A held key's repeats as presses alone (X11's detectable auto-repeat, for this client's connection): without it a
//! repeat is a release and a press, and a press that comes after its release is handled reads as a new key.
void detectable_repeat(void *display);
//! Whether the keyboard focus is in one of \p windows or a window inside one.
bool focus_within(void *display, const uintptr_t *windows, size_t count);
//! The files a drag from another program carries, from its text/uri-list: the file:// lines' paths, unescaped.
std::vector<std::string> uri_paths(const char *text, size_t length);

} // namespace qdom::pugl
