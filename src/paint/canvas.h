// Copyright 2026 Ian Karlsson
//! \file
//! A canvas element's drawing: what its 2D context recorded since its last whole clear (immediate-mode pictures
//! redraw everything each frame), in the canvas's own pixels, and the pixels put with putImageData.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "css/style.h"
#include "paint/commands.h"

namespace qdom
{

struct Canvas2DState
{
	Color fill { 0, 0, 0, 1 };
	Color stroke { 0, 0, 0, 1 };
	double line_width = 1;
	int cap = 0;   //!< butt, round, square
	int join = 0;  //!< miter, round, bevel
	double miter_limit = 10;
	std::vector<double> dash;
	double dash_offset = 0;
	double alpha = 1;
	std::string font = "10px sans-serif";
	FaceId face = 0;
	double font_size = 10;
	int font_weight = 400;
	FontStyle font_style = FontStyle::Normal;
	std::string family = "sans-serif";
	std::string text_align = "start";
	std::string text_baseline = "alphabetic";
	double m[6] = { 1, 0, 0, 1, 0, 0 };  //!< the transform: x' = a x + c y + e, y' = b x + d y + f
	int clips = 0;                       //!< the clips this state added (each a SAVE in the commands)
};

class Canvas
{
public:
	Commands commands;
	Canvas2DState state;
	std::vector<Canvas2DState> stack;
	//! The path being built, in the canvas's pixels: MOVE x y, LINE x y, BEZIER ..., CLOSE as in the commands.
	std::vector<float> path;
	double path_x = 0, path_y = 0;     //!< the current point, in user space
	bool has_point = false;
	//! putImageData's pixels (RGBA, the canvas's width by height), and a number changed with each put.
	std::vector<uint8_t> pixels;
	int pixels_w = 0, pixels_h = 0;
	uint32_t version = 0;
	bool face_valid = false;
	//! The image its pixels are drawn as, and the version the host was last given and in which hand-over.
	int image_id = 0;
	uint32_t uploaded = 0;
	uint32_t uploaded_generation = 0;
};

} // namespace qdom
