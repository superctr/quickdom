// Copyright 2026 Ian Karlsson
#include "shots.h"

#include <algorithm>
#include <cstdlib>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

bool read_png(const std::string &path, Image &image)
{
	int w, h, n;
	stbi_uc *p = stbi_load(path.c_str(), &w, &h, &n, 4);
	if (!p)
		return false;
	image.width = w;
	image.height = h;
	image.rgba.assign(p, p + size_t(w) * size_t(h) * 4);
	stbi_image_free(p);
	return true;
}

size_t compare_images(const Image &a, const Image &b, int threshold, Image *diff)
{
	if (a.width != b.width || a.height != b.height)
		return size_t(std::max(a.width * a.height, b.width * b.height));
	size_t differ = 0;
	if (diff)
	{
		diff->width = a.width;
		diff->height = a.height;
		diff->rgba.assign(a.rgba.size(), 0);
	}
	for (size_t i = 0; i < a.rgba.size(); i += 4)
	{
		int d = 0;
		for (size_t k = 0; k < 3; k++)
			d = std::max(d, std::abs(int(a.rgba[i + k]) - int(b.rgba[i + k])));
		const bool off = d > threshold;
		differ += off;
		if (diff)
		{
			uint8_t *o = &diff->rgba[i];
			if (off)
			{
				o[0] = 255;
				o[1] = o[2] = 0;
			}
			else
				for (size_t k = 0; k < 3; k++)
					o[k] = uint8_t(b.rgba[i + k] / 4);
			o[3] = 255;
		}
	}
	return differ;
}

#ifndef QDOM_RUN_OFFSCREEN
bool draw_offscreen(qdom_t *, int, int, double, Image &, std::string &error)
{
	error = "qdom-run was built without quickdom_gl and EGL";
	return false;
}
#endif
