// Copyright 2026 Ian Karlsson
#include "pugl/keys.h"

#include <cctype>
#include <string_view>

#include <cstring>

#include <pugl/pugl.h>

namespace qdom::pugl
{

namespace
{

struct Named
{
	uint32_t key;
	const char *name;
};

constexpr Named NAMES[] = {
	{ PUGL_KEY_BACKSPACE, "Backspace" }, { PUGL_KEY_TAB, "Tab" }, { PUGL_KEY_ENTER, "Enter" },
	{ PUGL_KEY_ESCAPE, "Escape" }, { PUGL_KEY_DELETE, "Delete" }, { PUGL_KEY_PAGE_UP, "PageUp" },
	{ PUGL_KEY_PAGE_DOWN, "PageDown" }, { PUGL_KEY_END, "End" }, { PUGL_KEY_HOME, "Home" },
	{ PUGL_KEY_LEFT, "ArrowLeft" }, { PUGL_KEY_UP, "ArrowUp" }, { PUGL_KEY_RIGHT, "ArrowRight" },
	{ PUGL_KEY_DOWN, "ArrowDown" }, { PUGL_KEY_PRINT_SCREEN, "PrintScreen" }, { PUGL_KEY_INSERT, "Insert" },
	{ PUGL_KEY_PAUSE, "Pause" }, { PUGL_KEY_MENU, "ContextMenu" }, { PUGL_KEY_NUM_LOCK, "NumLock" },
	{ PUGL_KEY_SCROLL_LOCK, "ScrollLock" }, { PUGL_KEY_CAPS_LOCK, "CapsLock" }, { PUGL_KEY_SHIFT_L, "Shift" },
	{ PUGL_KEY_SHIFT_R, "Shift" }, { PUGL_KEY_CTRL_L, "Control" }, { PUGL_KEY_CTRL_R, "Control" },
	{ PUGL_KEY_ALT_L, "Alt" }, { PUGL_KEY_ALT_R, "Alt" }, { PUGL_KEY_SUPER_L, "Meta" }, { PUGL_KEY_SUPER_R, "Meta" },
	// the keypad: its digits and signs with Num Lock on, its moves with it off
	{ PUGL_KEY_PAD_0, "0" }, { PUGL_KEY_PAD_1, "1" }, { PUGL_KEY_PAD_2, "2" }, { PUGL_KEY_PAD_3, "3" },
	{ PUGL_KEY_PAD_4, "4" }, { PUGL_KEY_PAD_5, "5" }, { PUGL_KEY_PAD_6, "6" }, { PUGL_KEY_PAD_7, "7" },
	{ PUGL_KEY_PAD_8, "8" }, { PUGL_KEY_PAD_9, "9" }, { PUGL_KEY_PAD_ENTER, "Enter" },
	{ PUGL_KEY_PAD_PAGE_UP, "PageUp" }, { PUGL_KEY_PAD_PAGE_DOWN, "PageDown" }, { PUGL_KEY_PAD_END, "End" },
	{ PUGL_KEY_PAD_HOME, "Home" }, { PUGL_KEY_PAD_LEFT, "ArrowLeft" }, { PUGL_KEY_PAD_UP, "ArrowUp" },
	{ PUGL_KEY_PAD_RIGHT, "ArrowRight" }, { PUGL_KEY_PAD_DOWN, "ArrowDown" }, { PUGL_KEY_PAD_CLEAR, "Clear" },
	{ PUGL_KEY_PAD_INSERT, "Insert" }, { PUGL_KEY_PAD_DELETE, "Delete" }, { PUGL_KEY_PAD_EQUAL, "=" },
	{ PUGL_KEY_PAD_MULTIPLY, "*" }, { PUGL_KEY_PAD_ADD, "+" }, { PUGL_KEY_PAD_SEPARATOR, "," },
	{ PUGL_KEY_PAD_SUBTRACT, "-" }, { PUGL_KEY_PAD_DECIMAL, "." }, { PUGL_KEY_PAD_DIVIDE, "/" },
};

// the codes by evdev's numbers (X11's key code less 8), the US keyboard's places
constexpr const char *CODES[128] = {
	nullptr, "Escape", "Digit1", "Digit2", "Digit3", "Digit4", "Digit5", "Digit6", "Digit7", "Digit8", "Digit9",
	"Digit0", "Minus", "Equal", "Backspace", "Tab", "KeyQ", "KeyW", "KeyE", "KeyR", "KeyT", "KeyY", "KeyU", "KeyI",
	"KeyO", "KeyP", "BracketLeft", "BracketRight", "Enter", "ControlLeft", "KeyA", "KeyS", "KeyD", "KeyF", "KeyG",
	"KeyH", "KeyJ", "KeyK", "KeyL", "Semicolon", "Quote", "Backquote", "ShiftLeft", "Backslash", "KeyZ", "KeyX",
	"KeyC", "KeyV", "KeyB", "KeyN", "KeyM", "Comma", "Period", "Slash", "ShiftRight", "NumpadMultiply", "AltLeft",
	"Space", "CapsLock", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "NumLock", "ScrollLock",
	"Numpad7", "Numpad8", "Numpad9", "NumpadSubtract", "Numpad4", "Numpad5", "Numpad6", "NumpadAdd", "Numpad1",
	"Numpad2", "Numpad3", "Numpad0", "NumpadDecimal", nullptr, nullptr, "IntlBackslash", "F11", "F12", nullptr,
	nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "NumpadEnter", "ControlRight", "NumpadDivide",
	"PrintScreen", "AltRight", nullptr, "Home", "ArrowUp", "PageUp", "ArrowLeft", "ArrowRight", "End", "ArrowDown",
	"PageDown", "Insert", "Delete", nullptr, nullptr, nullptr, nullptr, nullptr, "NumpadEqual", nullptr, "Pause",
	nullptr, "NumpadComma", nullptr, nullptr, nullptr, "MetaLeft", "MetaRight", "ContextMenu",
};

void utf8(uint32_t c, std::string &out)
{
	if (c < 0x80)
		out += char(c);
	else if (c < 0x800)
	{
		out += char(0xc0 | c >> 6);
		out += char(0x80 | (c & 0x3f));
	}
	else if (c < 0x10000)
	{
		out += char(0xe0 | c >> 12);
		out += char(0x80 | (c >> 6 & 0x3f));
		out += char(0x80 | (c & 0x3f));
	}
	else
	{
		out += char(0xf0 | c >> 18);
		out += char(0x80 | (c >> 12 & 0x3f));
		out += char(0x80 | (c >> 6 & 0x3f));
		out += char(0x80 | (c & 0x3f));
	}
}

} // namespace

bool is_character(uint32_t key)
{
	return key >= 0x20 && key != PUGL_KEY_DELETE && (key < 0xe000 || key >= 0xf900) && key < 0x110000;
}

std::string key_name(uint32_t key, bool shift)
{
	for (const Named &n : NAMES)
		if (n.key == key)
			return n.name;
	if (key == PUGL_KEY_SPACE)
		return " ";
	if (key >= PUGL_KEY_F1 && key <= PUGL_KEY_F12)
		return "F" + std::to_string(key - PUGL_KEY_F1 + 1);
	if (!is_character(key))
		return {};
	if (shift && key >= 'a' && key <= 'z')
		key -= 'a' - 'A';
	std::string out;
	utf8(key, out);
	return out;
}

const char *scan_code(uint32_t scancode, uint32_t key)
{
	// the keys that share a scan code with a keypad key and differ in an extended prefix PUGL does not pass on
	switch (key)
	{
	case PUGL_KEY_PAD_ENTER: return "NumpadEnter";
	case PUGL_KEY_PAD_DIVIDE: return "NumpadDivide";
	case PUGL_KEY_CTRL_R: return "ControlRight";
	case PUGL_KEY_ALT_R: return "AltRight";
	case PUGL_KEY_SUPER_L: return "MetaLeft";
	case PUGL_KEY_SUPER_R: return "MetaRight";
	case PUGL_KEY_MENU: return "ContextMenu";
	case PUGL_KEY_PRINT_SCREEN: return "PrintScreen";
	case PUGL_KEY_PAUSE: return "Pause";
	case PUGL_KEY_HOME: return "Home";
	case PUGL_KEY_END: return "End";
	case PUGL_KEY_PAGE_UP: return "PageUp";
	case PUGL_KEY_PAGE_DOWN: return "PageDown";
	case PUGL_KEY_UP: return "ArrowUp";
	case PUGL_KEY_DOWN: return "ArrowDown";
	case PUGL_KEY_LEFT: return "ArrowLeft";
	case PUGL_KEY_RIGHT: return "ArrowRight";
	case PUGL_KEY_INSERT: return "Insert";
	case PUGL_KEY_DELETE: return "Delete";
	default: break;
	}
	return scancode < 128 ? CODES[scancode] : nullptr;
}

const char *key_code(uint32_t keycode, uint32_t key)
{
#if defined(_WIN32)
	return scan_code(keycode, key);
#else
	return keycode >= 8 && keycode - 8 < 128 ? CODES[keycode - 8] : nullptr;
#endif
}

int cursor_shape(const std::string &name)
{
	static constexpr struct
	{
		const char *name;
		PuglCursor shape;
	} SHAPES[] = {
		{ "pointer", PUGL_CURSOR_HAND }, { "grab", PUGL_CURSOR_HAND }, { "grabbing", PUGL_CURSOR_HAND },
		{ "text", PUGL_CURSOR_CARET }, { "vertical-text", PUGL_CURSOR_CARET }, { "crosshair", PUGL_CURSOR_CROSSHAIR },
		{ "cell", PUGL_CURSOR_CROSSHAIR }, { "not-allowed", PUGL_CURSOR_NO }, { "no-drop", PUGL_CURSOR_NO },
		{ "ew-resize", PUGL_CURSOR_LEFT_RIGHT }, { "col-resize", PUGL_CURSOR_LEFT_RIGHT },
		{ "e-resize", PUGL_CURSOR_LEFT_RIGHT }, { "w-resize", PUGL_CURSOR_LEFT_RIGHT },
		{ "ns-resize", PUGL_CURSOR_UP_DOWN }, { "row-resize", PUGL_CURSOR_UP_DOWN },
		{ "n-resize", PUGL_CURSOR_UP_DOWN }, { "s-resize", PUGL_CURSOR_UP_DOWN },
		{ "nwse-resize", PUGL_CURSOR_UP_LEFT_DOWN_RIGHT }, { "nw-resize", PUGL_CURSOR_UP_LEFT_DOWN_RIGHT },
		{ "se-resize", PUGL_CURSOR_UP_LEFT_DOWN_RIGHT }, { "nesw-resize", PUGL_CURSOR_UP_RIGHT_DOWN_LEFT },
		{ "ne-resize", PUGL_CURSOR_UP_RIGHT_DOWN_LEFT }, { "sw-resize", PUGL_CURSOR_UP_RIGHT_DOWN_LEFT },
		{ "move", PUGL_CURSOR_ALL_SCROLL }, { "all-scroll", PUGL_CURSOR_ALL_SCROLL },
	};
	for (const auto &s : SHAPES)
		if (name == s.name)
			return s.shape;
	return PUGL_CURSOR_ARROW;
}

std::vector<std::string> uri_paths(const char *text, size_t length)
{
	std::vector<std::string> out;
	std::string_view rest(text, length);
	while (!rest.empty())
	{
		const size_t end = rest.find_first_of("\r\n");
		std::string_view line = rest.substr(0, end);
		rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
		if (line.empty() || line[0] == '#' || line.substr(0, 7) != "file://")
			continue;
		line.remove_prefix(7);
		// file://host/path: the path from its first slash
		const size_t slash = line.find('/');
		if (slash == std::string_view::npos)
			continue;
		line.remove_prefix(slash);
		std::string path;
		for (size_t i = 0; i < line.size(); i++)
			if (line[i] == '%' && i + 2 < line.size() && isxdigit((unsigned char)line[i + 1]) && isxdigit((unsigned char)line[i + 2]))
			{
				path += char(std::stoi(std::string(line.substr(i + 1, 2)), nullptr, 16));
				i += 2;
			}
			else
				path += line[i];
		// file:///C:/dir: Windows' drive letter without the slash before it
		if (path.size() > 2 && path[0] == '/' && isalpha((unsigned char)path[1]) && path[2] == ':')
			path.erase(0, 1);
		out.push_back(path);
	}
	return out;
}

} // namespace qdom::pugl
