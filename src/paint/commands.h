// Copyright 2026 Ian Karlsson
//! \file
//! Drawing commands: numbers in one array with the texts beside them, the format quickdom_gl draws
//! (ops 1 to 17), and two more for canvases (a transform, a clear). A canvas's 2D context records into one;
//! the paint of the document (a later milestone) will write the whole frame into one for the GL backend.
#pragma once

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

#include "css/values.h"

namespace qdom
{

namespace op
{
//! The commands and their numbers after the op:
//!   SAVE, RESTORE; CLIP x y w h; ALPHA a; RECT x y w h r0 r1 r2 r3 rgba; STROKE_RECT x y w h r0 r1 r2 r3 width rgba;
//!   TEXT x y face size spacing rgba text; BEGIN; MOVE x y; LINE x y; BEZIER x1 y1 x2 y2 x y; CLOSE; FILL rgba;
//!   STROKE width cap join rgba; IMAGE id x y w h smooth; SHADOW x y w h r blur rgba;
//!   GRADIENT (gl.c's 25 numbers); TRANSFORM a b c d e f; CLEAR x y w h
enum : int
{
	SAVE = 1, RESTORE, CLIP, ALPHA, RECT, STROKE_RECT, TEXT, BEGIN, MOVE, LINE, BEZIER, CLOSE, FILL, STROKE, IMAGE,
	SHADOW, GRADIENT, TRANSFORM, CLEAR,
};
}

struct Commands
{
	std::vector<float> data;
	std::vector<std::string> texts;

	void push(std::initializer_list<double> xs)
	{
		for (double x : xs)
			data.push_back(float(x));
	}
	void color(const Color &c, double alpha = 1)
	{
		push({ c.r, c.g, c.b, c.a * alpha });
	}
	//! A text, without the NULs that end each text where the texts are handed on in one string
	float text(std::string s)
	{
		s.erase(std::remove(s.begin(), s.end(), '\0'), s.end());
		texts.push_back(std::move(s));
		return float(texts.size() - 1);
	}
	void clear()
	{
		data.clear();
		texts.clear();
	}
	bool empty() const { return data.empty(); }
};

} // namespace qdom
