// Copyright 2026 Ian Karlsson
// qdom-run's pictures of a page: PNGs read and written, two compared pixel by pixel, and (with an EGL context, built
// with QDOM_RUN_OFFSCREEN) the page drawn by quickdom_gl.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "quickdom/quickdom.h"

struct Image
{
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;  //!< rows from the top
};

bool write_png(const std::string &path, const Image &image);
bool read_png(const std::string &path, Image &image);

//! The pixels where a colour channel is off by more than threshold (of 255), and where they are (diff: the second
//! image dimmed, the pixels that differ red), when asked; images of two sizes differ everywhere.
size_t compare_images(const Image &a, const Image &b, int threshold, Image *diff);

//! The document's frame drawn by quickdom_gl offscreen, width by height CSS pixels at scale; false with why when no
//! GL context can be made.
bool draw_offscreen(qdom_t *dom, int width, int height, double scale, Image &out, std::string &error);
