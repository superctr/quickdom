// Copyright 2026 Ian Karlsson
#include "paint/picture.h"

#include <climits>
#include <cstring>

// nanovg's copy of stb_image, private to the library (the GL backend's nanovg leaves its own out)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#ifndef __clang__
// a false alarm of GCC's optimizer in its zlib decoder
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace qdom
{

bool decode_picture(const void *data, size_t size, int &width, int &height, std::vector<uint8_t> &rgba)
{
	width = height = 0;
	rgba.clear();
	if (!data || !size || size > size_t(INT_MAX))
		return false;
	int w, h, n;
	stbi_uc *p = stbi_load_from_memory(static_cast<const stbi_uc *>(data), int(size), &w, &h, &n, 4);
	if (!p)
		return false;
	width = w;
	height = h;
	rgba.assign(p, p + size_t(w) * size_t(h) * 4);
	stbi_image_free(p);
	return true;
}

} // namespace qdom
