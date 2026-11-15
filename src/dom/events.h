// Copyright 2026 Ian Karlsson
//! \file
//! Events: what a listener gets (one native class with the fields of the event kinds supported), and
//! their dispatch through the tree with capture, target and bubble phases.
#pragma once

#include <string>

#include "quickjs.h"

#include "core/names.h"

namespace qdom
{

struct Event
{
	std::string type;
	Name type_name = 0;
	bool bubbles = false;
	bool cancelable = false;
	bool composed = false;
	bool trusted = false;
	bool default_prevented = false;
	bool stop = false;      //!< stopPropagation()
	bool stop_now = false;  //!< stopImmediatePropagation()
	bool passive = false;   //!< in a passive listener: preventDefault() does nothing
	bool dispatching = false;
	uint8_t phase = 0;      //!< 0 none, 1 capturing, 2 at target, 3 bubbling
	double time_stamp = 0;
	// counted
	JSValue target = JS_UNDEFINED;
	JSValue current_target = JS_NULL;
	JSValue related_target = JS_NULL;
	JSValue data_transfer = JS_NULL;   //!< a drag's: what it carries

	// keyboard
	std::string key;
	std::string code;
	bool repeat = false;
	bool is_composing = false;
	int location = 0;
	// modifiers
	bool ctrl = false, shift = false, alt = false, meta = false;
	// mouse, pointer, wheel
	int button = 0;
	int buttons = 0;
	double client_x = 0, client_y = 0;
	double screen_x = 0, screen_y = 0;
	int detail = 0;
	int pointer_id = 1;
	std::string pointer_type = "mouse";
	bool is_primary = true;
	double delta_x = 0, delta_y = 0, delta_z = 0;
	int delta_mode = 0;
	// input
	std::string input_type;
	std::string data;

	void release(JSRuntime *rt)
	{
		JS_FreeValueRT(rt, target);
		JS_FreeValueRT(rt, current_target);
		JS_FreeValueRT(rt, related_target);
		JS_FreeValueRT(rt, data_transfer);
		target = JS_UNDEFINED;
		current_target = related_target = data_transfer = JS_NULL;
	}
	void mark(JSRuntime *rt, JS_MarkFunc *mark) const
	{
		JS_MarkValue(rt, target, mark);
		JS_MarkValue(rt, current_target, mark);
		JS_MarkValue(rt, related_target, mark);
		JS_MarkValue(rt, data_transfer, mark);
	}
};

} // namespace qdom
