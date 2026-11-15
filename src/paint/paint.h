// Copyright 2026 Ian Karlsson
//! \file
//! The laid-out document drawn as a list of commands (commands.h), and the element under a point found in the
//! same order. Boxes are drawn as the browser draws them: shadow, background, gradients, border, the content
//! (text, a control's look, an svg's items, a canvas's picture), the children in flow, then the placed ones, the
//! outline; a box that clips clips its children and shows its scrollbars; the top layer's dialogs last, each over
//! its backdrop. Stacking is document order (no z-index).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "paint/commands.h"

namespace qdom
{

class Dom;
class Element;

//! A canvas's pixels the host is to upload: its image's number, its size, RGBA rows.
struct ImageUpload
{
	int id;
	int width, height;
	const uint8_t *rgba;
};

struct PaintOptions
{
	//! The caret's blink: whether the focused field's caret is drawn now.
	bool caret = true;
	//! A canvas's pixels that changed since they were last handed over.
	std::function<void(const ImageUpload &)> image;
};

//! The document's picture in the viewport, in CSS pixels.
void paint(Dom &dom, Commands &out, const PaintOptions &options);

//! The element under a point of the viewport (an svg's item where one is hit), the last drawn there; null when none.
Element *hit(Dom &dom, double x, double y);

//! The number of numbers an op takes after it (the text index counted), or -1 for one not known.
int op_length(int op);

} // namespace qdom
