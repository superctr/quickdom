// Copyright 2026 Ian Karlsson
//! \file
//! ResizeObserver: elements watched for a change of their content box's size, told after each frame's layout.
//! An observer lives while an element it watches does (the element holds it) or while script holds it.
#pragma once

#include <vector>

#include "quickjs.h"

namespace qdom
{

class Dom;
class Element;

class ResizeObserver
{
public:
	struct Target
	{
		Element *el;
		JSValue value;  //!< counted
		double w = 0, h = 0;
	};

	Dom *dom = nullptr;
	JSValue self = JS_UNDEFINED;      //!< its wrapper, not counted
	JSValue callback = JS_UNDEFINED;  //!< counted
	std::vector<Target> targets;

	void mark(JSRuntime *rt, JS_MarkFunc *mark) const
	{
		JS_MarkValue(rt, callback, mark);
		for (const Target &t : targets)
			JS_MarkValue(rt, t.value, mark);
	}
	void release(JSRuntime *rt)
	{
		JS_FreeValueRT(rt, callback);
		callback = JS_UNDEFINED;
		for (Target &t : targets)
			JS_FreeValueRT(rt, t.value);
		targets.clear();
	}
};

} // namespace qdom
