// Copyright 2026 Ian Karlsson
// quickdom_gl: the drawing commands drawn with nanovg on OpenGL 2.1. Boxes on whole
// device pixels as the browser keeps them; text glyph by glyph from an atlas, each glyph at its quarter of a device
// pixel and the baseline on a whole one, a colour glyph (an emoji's picture) from an atlas of its own, drawn smooth
// at its size from the pen's whole pixel as WebRender draws it; a canvas's recording under the transform the paint
// gives it.
#include "quickdom/quickdom_gl.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gl/api.h"
#include "nanovg.h"
#define NANOVG_GL2_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "nanovg_gl.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "instance.h"
#include "paint/commands.h"
#include "text/fonts.h"

namespace
{

constexpr int ATLAS = 2048;           // the glyphs' texture, square
constexpr int COLOR_ATLAS = 1024;     // the colour glyphs' texture, square
constexpr size_t GLYPHS = 6000;       // glyphs kept before an atlas starts again
constexpr int MAX_DEPTH = 64;

//! A glyph in the atlas.
struct GlyphSlot
{
	short x, y, w, h;   //!< in the atlas
	short left, top;    //!< the bitmap's corner from the pen, in device pixels
};

//! A texture of glyphs on shelves, drawn into as they are first asked for and uploaded before they are drawn: one
//! byte a pixel (coverage), or four (a colour glyph's premultiplied RGBA).
struct Atlas
{
	int size = 0, bytes = 1;
	int texture = 0;
	std::vector<uint8_t> pixels;
	int shelf_x = 1, shelf_y = 1, shelf_h = 0;
	int dirty_y0 = 0, dirty_y1 = 0;
	std::unordered_map<uint64_t, GlyphSlot> glyphs;
};

//! A picture of pixels: drawn to the nearest pixel, and smooth (from mipmaps for a still picture).
struct ImageSlot
{
	int nearest = 0, smooth = 0;
	int width = 0, height = 0;
};

//! Pixels handed over, uploaded at the next draw (the context may not be current when the library paints).
struct Pending
{
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;
};

struct State
{
	float x0 = 1, y0 = 1, x1 = 0, y1 = 0;  //!< the clip in the page's pixels, x0 > x1 for none
	float alpha = 1;
	float m[6] = { 1, 0, 0, 1, 0, 0 };     //!< the transform: page = m * local (a canvas's)
	bool identity = true;
};

} // namespace

struct qdom_gl
{
	qdom_t *dom = nullptr;
	NVGcontext *vg = nullptr;
	Atlas atlas, colors;
	std::unordered_map<int, ImageSlot> images;
	std::unordered_map<int, Pending> pending;
	std::vector<State> stack;
	float scale = 1;
	std::vector<NVGvertex> verts, color_verts;
	std::vector<qdom::PlacedGlyph> placed;
};

namespace
{

// ---------------------------------------------------------------- the glyphs

void forget_glyphs(Atlas &a)
{
	a.glyphs.clear();
	std::fill(a.pixels.begin(), a.pixels.end(), 0);
	a.shelf_x = a.shelf_y = 1;
	a.shelf_h = 0;
	a.dirty_y0 = 0;
	a.dirty_y1 = a.size;
}

//! Room for a glyph of a size in an atlas (the atlas started again when full): its corner, or false when it is
//! larger than the atlas.
bool place(Atlas &a, int w, int h, int &x, int &y)
{
	if (w >= a.size - 2 || h >= a.size - 2)
		return false;
	if (a.glyphs.size() >= GLYPHS)
		forget_glyphs(a);
	if (a.shelf_x + w + 1 >= a.size)
	{
		a.shelf_x = 1;
		a.shelf_y += a.shelf_h + 1;
		a.shelf_h = 0;
	}
	if (a.shelf_y + h + 1 >= a.size)
		forget_glyphs(a);
	x = a.shelf_x;
	y = a.shelf_y;
	a.shelf_x += w + 1;
	a.shelf_h = std::max(a.shelf_h, h);
	a.dirty_y0 = std::min(a.dirty_y0, y);
	a.dirty_y1 = std::max(a.dirty_y1, y + h);
	return true;
}

//! The atlas's copy of a glyph at a scale (device pixels a font unit) and a quarter-pixel offset, drawn in if new.
const GlyphSlot &glyph_at(qdom_gl *g, int font, int glyph, float scale, int sub)
{
	Atlas &a = g->atlas;
	const uint64_t size = uint64_t(std::lrint(double(scale) * 65536.0)) & 0xffffffffu;
	const uint64_t key = uint64_t(font & 0xff) << 50 | uint64_t(glyph & 0xffff) << 34 | size << 2 | uint64_t(sub);
	if (auto it = a.glyphs.find(key); it != a.glyphs.end())
		return it->second;
	const qdom::FontSet &fonts = g->dom->dom->fonts;
	const float shift = float(sub) * 0.25f;
	int left, top, w, h, x = 0, y = 0;
	fonts.glyph_box(font, glyph, scale, shift, left, top, w, h);
	if (!place(a, w, h, x, y))
		w = h = 0;
	GlyphSlot s { short(x), short(y), short(w), short(h), short(left), short(top) };
	if (w > 0 && h > 0)
		fonts.glyph_bitmap(font, glyph, scale, shift, a.pixels.data() + size_t(y) * size_t(a.size) + size_t(x), w, h,
			a.size);
	return a.glyphs.emplace(key, s).first->second;
}

//! A colour glyph's picture in the colour atlas, with a border of its edge's pixels around it (as WebRender clamps
//! its samples to the picture), drawn in if new; its size and corner are the picture's (FontSet::ColorGlyph).
const GlyphSlot *color_at(qdom_gl *g, int font, int glyph, float scale, qdom::FontSet::ColorGlyph &cg)
{
	Atlas &a = g->colors;
	const qdom::FontSet &fonts = g->dom->dom->fonts;
	if (!fonts.color_glyph(font, glyph, scale, cg))
		return nullptr;
	// the picture depends on the scale only through its halvings, which its size tells
	const uint64_t key = uint64_t(font & 0xff) << 50 | uint64_t(glyph & 0xffff) << 34 | uint64_t(cg.width & 0xffff) << 17
		| uint64_t(cg.height & 0xffff);
	if (auto it = a.glyphs.find(key); it != a.glyphs.end())
		return &it->second;
	int x, y;
	if (!place(a, cg.width + 2, cg.height + 2, x, y))
		return nullptr;
	// the picture and its border: each row's ends repeated, the first and last rows repeated
	const size_t stride = size_t(a.size) * 4, row = size_t(cg.width) * 4;
	for (int j = -1; j <= cg.height; j++)
	{
		const int sj = std::clamp(j, 0, cg.height - 1);
		const uint8_t *src = cg.rgba + size_t(sj) * row;
		uint8_t *dst = a.pixels.data() + size_t(y + 1 + j) * stride + size_t(x) * 4;
		std::memcpy(dst, src, 4);
		std::memcpy(dst + 4, src, row);
		std::memcpy(dst + 4 + row, src + row - 4, 4);
	}
	GlyphSlot s { short(x + 1), short(y + 1), short(cg.width), short(cg.height), 0, 0 };
	return &a.glyphs.emplace(key, s).first->second;
}

void upload(qdom_gl *g, Atlas &a)
{
	if (a.dirty_y1 <= a.dirty_y0)
		return;
	NVGparams *p = nvgInternalParams(g->vg);
	// nanovg's GL backend updates whole rows from the start of the image
	p->renderUpdateTexture(p->userPtr, a.texture, 0, a.dirty_y0, a.size, a.dirty_y1 - a.dirty_y0, a.pixels.data());
	a.dirty_y0 = a.size;
	a.dirty_y1 = 0;
}

NVGscissor scissor_of(const qdom_gl *g)
{
	const State &c = g->stack.back();
	NVGscissor s;
	std::memset(&s, 0, sizeof(s));
	if (c.x0 > c.x1)
	{
		s.extent[0] = s.extent[1] = -1;
		return s;
	}
	s.xform[0] = s.xform[3] = 1;
	s.xform[4] = (c.x0 + c.x1) / 2;
	s.xform[5] = (c.y0 + c.y1) / 2;
	s.extent[0] = (c.x1 - c.x0) / 2;
	s.extent[1] = (c.y1 - c.y0) / 2;
	return s;
}

//! Triangles of an atlas's texture, in the paint's colour (premultiplied by nanovg).
void draw_triangles(qdom_gl *g, const Atlas &a, NVGcolor color, const std::vector<NVGvertex> &verts, size_t n)
{
	NVGpaint paint;
	std::memset(&paint, 0, sizeof(paint));
	nvgTransformIdentity(paint.xform);
	paint.feather = 1;
	paint.innerColor = paint.outerColor = color;
	paint.image = a.texture;
	NVGcompositeOperationState op = { NVG_ONE, NVG_ONE_MINUS_SRC_ALPHA, NVG_ONE, NVG_ONE_MINUS_SRC_ALPHA };
	NVGscissor sc = scissor_of(g);
	NVGparams *p = nvgInternalParams(g->vg);
	p->renderTriangles(p->userPtr, &paint, op, &sc, verts.data(), int(n), 1.0f / g->scale);
}

void quad(NVGvertex *v, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
	v[0] = { x0, y0, u0, v0 };
	v[1] = { x1, y1, u1, v1 };
	v[2] = { x1, y0, u1, v0 };
	v[3] = { x0, y0, u0, v0 };
	v[4] = { x0, y1, u0, v1 };
	v[5] = { x1, y1, u1, v1 };
}

//! A line of text at its baseline, through the transform: its origin mapped, its size scaled. A colour glyph is
//! drawn in its own colours, with the text's alpha (as WebRender draws one), after the others.
void draw_text(qdom_gl *g, float x, float y, int face, float size, float spacing, NVGcolor color, std::string_view s)
{
	const State &st = g->stack.back();
	const float k = std::sqrt(std::fabs(st.m[0] * st.m[3] - st.m[1] * st.m[2]));
	const float ox = st.m[0] * x + st.m[2] * y + st.m[4], oy = st.m[1] * x + st.m[3] * y + st.m[5];
	const float device = g->scale * (k > 0 ? k : 1);
	const qdom::FontSet &fonts = g->dom->dom->fonts;
	g->dom->dom->fonts.layout(qdom::FaceId(face), size * device, spacing * device, s, g->placed);
	if (g->placed.empty())
		return;
	g->verts.resize(g->placed.size() * 6);
	g->color_verts.resize(g->placed.size() * 6);
	// under a turn (an SVG text's rotate) the line is laid out from its origin and each glyph's quad turned about it
	const bool turned = k > 0 && (st.m[1] != 0 || st.m[2] != 0);
	const float co = turned ? st.m[0] / k : 1, si = turned ? st.m[1] / k : 0;
	const float dx = turned ? 0 : ox * g->scale, by = turned ? 0 : std::round(oy * g->scale), inv = 1.0f / g->scale;
	size_t n = 0, nc = 0;
	for (const qdom::PlacedGlyph &pg : g->placed)
	{
		const float pen = dx + pg.x;
		if (fonts.has_color(pg.font))
		{
			// on the pen's whole pixel (no subpixel positions for a bitmap font), at its fractional size
			qdom::FontSet::ColorGlyph cg;
			const GlyphSlot *gs = color_at(g, pg.font, pg.glyph, pg.scale, cg);
			if (!gs)
				continue;
			const float x0 = (std::round(pen) + cg.left * cg.scale) * inv, y0 = (by - cg.top * cg.scale) * inv;
			const float x1 = x0 + float(cg.width) * cg.scale * inv, y1 = y0 + float(cg.height) * cg.scale * inv;
			const float a = float(COLOR_ATLAS);
			quad(&g->color_verts[nc], x0, y0, x1, y1, float(gs->x) / a, float(gs->y) / a, float(gs->x + gs->w) / a,
				float(gs->y + gs->h) / a);
			nc += 6;
			continue;
		}
		const int q = int(std::floor(pen * 4 + 0.5f));
		const int ix = q >> 2, sub = q & 3;
		const GlyphSlot &gs = glyph_at(g, pg.font, pg.glyph, pg.scale, sub);
		if (!gs.w || !gs.h)
			continue;
		const float x0 = float(ix + gs.left) * inv, y0 = (by + float(gs.top)) * inv;
		const float x1 = x0 + float(gs.w) * inv, y1 = y0 + float(gs.h) * inv;
		quad(&g->verts[n], x0, y0, x1, y1, float(gs.x) / ATLAS, float(gs.y) / ATLAS, float(gs.x + gs.w) / ATLAS,
			float(gs.y + gs.h) / ATLAS);
		if (turned)
			for (size_t v = n; v < n + 6; v++)
			{
				const float lx = g->verts[v].x, ly = g->verts[v].y;
				g->verts[v].x = ox + co * lx - si * ly;
				g->verts[v].y = oy + si * lx + co * ly;
			}
		n += 6;
	}
	color.a *= st.alpha;
	if (n)
	{
		upload(g, g->atlas);
		draw_triangles(g, g->atlas, color, g->verts, n);
	}
	if (nc)
	{
		upload(g, g->colors);
		draw_triangles(g, g->colors, nvgRGBAf(1, 1, 1, color.a), g->color_verts, nc);
	}
}

// ---------------------------------------------------------------- the pictures of pixels

void take_image(void *ctx, int id, int width, int height, const unsigned char *rgba)
{
	qdom_gl *g = static_cast<qdom_gl *>(ctx);
	Pending &p = g->pending[id];
	p.width = width;
	p.height = height;
	if (width > 0 && height > 0 && rgba)
		p.rgba.assign(rgba, rgba + size_t(width) * size_t(height) * 4);
	else
		p.rgba.clear();
}

void upload_images(qdom_gl *g)
{
	for (auto &[id, p] : g->pending)
	{
		auto it = g->images.find(id);
		if (p.rgba.empty())
		{
			if (it != g->images.end())
			{
				nvgDeleteImage(g->vg, it->second.nearest);
				nvgDeleteImage(g->vg, it->second.smooth);
				g->images.erase(it);
			}
			continue;
		}
		if (it != g->images.end() && it->second.width == p.width && it->second.height == p.height && id >= 0)
		{
			nvgUpdateImage(g->vg, it->second.nearest, p.rgba.data());
			nvgUpdateImage(g->vg, it->second.smooth, p.rgba.data());
			continue;
		}
		if (it != g->images.end())
		{
			nvgDeleteImage(g->vg, it->second.nearest);
			nvgDeleteImage(g->vg, it->second.smooth);
		}
		// a still picture (a negative number) drawn smooth from its mipmaps at any scale
		ImageSlot s;
		s.width = p.width;
		s.height = p.height;
		s.nearest = nvgCreateImageRGBA(g->vg, p.width, p.height, NVG_IMAGE_NEAREST, p.rgba.data());
		s.smooth = nvgCreateImageRGBA(g->vg, p.width, p.height, id < 0 ? NVG_IMAGE_GENERATE_MIPMAPS : 0, p.rgba.data());
		g->images[id] = s;
	}
	g->pending.clear();
}

// ---------------------------------------------------------------- a frame

NVGcolor color_at(const float *c)
{
	return nvgRGBAf(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3]);
}

//! A box's edge on a whole device pixel, as the browser keeps it (the page's own boxes; a canvas's drawing is not).
float snap(const qdom_gl *g, float v)
{
	return g->stack.back().identity ? std::round(v * g->scale) / g->scale : v;
}

//! nanovg's scissor set to the clip, in the page's pixels whatever the transform.
void apply_clip(qdom_gl *g)
{
	const State &s = g->stack.back();
	float t[6];
	nvgCurrentTransform(g->vg, t);
	nvgResetTransform(g->vg);
	if (s.x0 > s.x1)
		nvgResetScissor(g->vg);
	else
		nvgScissor(g->vg, s.x0, s.y0, s.x1 - s.x0, s.y1 - s.y0);
	nvgTransform(g->vg, t[0], t[1], t[2], t[3], t[4], t[5]);
}

} // namespace

qdom_gl_t *qdom_gl_create(qdom_t *dom)
{
	if (!dom)
		return nullptr;
	if (!qdom_gl_load())
		return nullptr;
	auto *g = new qdom_gl();
	g->dom = dom;
	g->vg = nvgCreateGL2(NVG_ANTIALIAS);
	if (!g->vg)
	{
		delete g;
		return nullptr;
	}
	NVGparams *p = nvgInternalParams(g->vg);
	g->atlas.size = ATLAS;
	g->atlas.pixels.assign(size_t(ATLAS) * ATLAS, 0);
	g->atlas.texture = p->renderCreateTexture(p->userPtr, NVG_TEXTURE_ALPHA, ATLAS, ATLAS, 0, g->atlas.pixels.data());
	// premultiplied, sampled smooth: a colour glyph is drawn at a fractional size
	g->colors.size = COLOR_ATLAS;
	g->colors.bytes = 4;
	g->colors.pixels.assign(size_t(COLOR_ATLAS) * COLOR_ATLAS * 4, 0);
	g->colors.texture = p->renderCreateTexture(p->userPtr, NVG_TEXTURE_RGBA, COLOR_ATLAS, COLOR_ATLAS,
		NVG_IMAGE_PREMULTIPLIED, g->colors.pixels.data());
	g->stack.reserve(MAX_DEPTH);
	dom->sink = take_image;
	dom->sink_ctx = g;
	// a new context has none of the pictures handed over before
	dom->dom->renew_images();
	return g;
}

void qdom_gl_destroy(qdom_gl_t *g)
{
	if (!g)
		return;
	if (g->dom && g->dom->sink_ctx == g)
	{
		g->dom->sink = nullptr;
		g->dom->sink_ctx = nullptr;
	}
	for (auto &[id, s] : g->images)
	{
		nvgDeleteImage(g->vg, s.nearest);
		nvgDeleteImage(g->vg, s.smooth);
	}
	nvgDeleteGL2(g->vg);
	delete g;
}

void qdom_gl_draw(qdom_gl_t *g, const qdom_commands_t *commands, double width, double height, double scale)
{
	if (!g || !commands)
		return;
	// the texts by their numbers
	std::vector<std::string_view> texts;
	for (size_t i = 0, from = 0; i <= commands->texts_size; i++)
		if (i == commands->texts_size || commands->texts[i] == 0)
		{
			if (commands->texts_size)
				texts.emplace_back(commands->texts + from, i - from);
			from = i + 1;
		}
	upload_images(g);

	g->scale = float(scale);
	glViewport(0, 0, GLsizei(std::lrint(width * scale)), GLsizei(std::lrint(height * scale)));
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	NVGcontext *vg = g->vg;
	nvgBeginFrame(vg, float(width), float(height), float(scale));
	g->stack.assign(1, State());
	const float *c = commands->data;
	const size_t count = commands->count;
	size_t i = 0;
	auto need = [&](size_t n) { return i + n <= count; };
	while (i < count)
	{
		const int op = int(c[i++]);
		switch (op)
		{
		case qdom::op::SAVE:
			if (g->stack.size() < MAX_DEPTH)
				g->stack.push_back(g->stack.back());
			nvgSave(vg);
			break;
		case qdom::op::RESTORE:
			if (g->stack.size() > 1)
				g->stack.pop_back();
			nvgRestore(vg);
			break;
		case qdom::op::CLIP:
		{
			if (!need(4))
				goto done;
			State &s = g->stack.back();
			// the clip's corners through the transform: the box they cover in the page
			float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
			for (int k = 0; k < 4; k++)
			{
				const float lx = c[i] + (k & 1 ? c[i + 2] : 0), ly = c[i + 1] + (k & 2 ? c[i + 3] : 0);
				const float px = s.m[0] * lx + s.m[2] * ly + s.m[4], py = s.m[1] * lx + s.m[3] * ly + s.m[5];
				x0 = std::min(x0, px);
				y0 = std::min(y0, py);
				x1 = std::max(x1, px);
				y1 = std::max(y1, py);
			}
			if (s.x0 > s.x1)
			{
				s.x0 = x0;
				s.y0 = y0;
				s.x1 = x1;
				s.y1 = y1;
			}
			else
			{
				s.x0 = std::max(s.x0, x0);
				s.y0 = std::max(s.y0, y0);
				s.x1 = std::max(s.x0, std::min(s.x1, x1));
				s.y1 = std::max(s.y0, std::min(s.y1, y1));
			}
			apply_clip(g);
			i += 4;
			break;
		}
		case qdom::op::ALPHA:
			if (!need(1))
				goto done;
			g->stack.back().alpha *= c[i++];
			nvgGlobalAlpha(vg, g->stack.back().alpha);
			break;
		case qdom::op::RECT:
		{
			if (!need(12))
				goto done;
			const float x0 = snap(g, c[i]), y0 = snap(g, c[i + 1]);
			const float x1 = snap(g, c[i] + c[i + 2]), y1 = snap(g, c[i + 1] + c[i + 3]);
			if (x1 > x0 && y1 > y0)
			{
				nvgBeginPath(vg);
				nvgRoundedRectVarying(vg, x0, y0, x1 - x0, y1 - y0, c[i + 4], c[i + 5], c[i + 6], c[i + 7]);
				nvgFillColor(vg, color_at(c + i + 8));
				nvgFill(vg);
			}
			i += 12;
			break;
		}
		case qdom::op::STROKE_RECT:
		{
			if (!need(13))
				goto done;
			// the border's outer edges on whole pixels, the line along its middle
			const float w = c[i + 8], h2 = w / 2;
			const float x0 = snap(g, c[i] - h2) + h2, y0 = snap(g, c[i + 1] - h2) + h2;
			const float x1 = snap(g, c[i] + c[i + 2] + h2) - h2, y1 = snap(g, c[i + 1] + c[i + 3] + h2) - h2;
			nvgBeginPath(vg);
			nvgRoundedRectVarying(vg, x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0), c[i + 4], c[i + 5],
				c[i + 6], c[i + 7]);
			nvgStrokeWidth(vg, w);
			nvgStrokeColor(vg, color_at(c + i + 9));
			nvgStroke(vg);
			i += 13;
			break;
		}
		case qdom::op::TEXT:
		{
			if (!need(10))
				goto done;
			const size_t t = size_t(c[i + 9]);
			if (t < texts.size())
				draw_text(g, c[i], c[i + 1], int(c[i + 2]), c[i + 3], c[i + 4], color_at(c + i + 5), texts[t]);
			i += 10;
			break;
		}
		case qdom::op::BEGIN:
			nvgBeginPath(vg);
			break;
		case qdom::op::MOVE:
			if (!need(2))
				goto done;
			nvgMoveTo(vg, c[i], c[i + 1]);
			i += 2;
			break;
		case qdom::op::LINE:
			if (!need(2))
				goto done;
			nvgLineTo(vg, c[i], c[i + 1]);
			i += 2;
			break;
		case qdom::op::BEZIER:
			if (!need(6))
				goto done;
			nvgBezierTo(vg, c[i], c[i + 1], c[i + 2], c[i + 3], c[i + 4], c[i + 5]);
			i += 6;
			break;
		case qdom::op::CLOSE:
			nvgClosePath(vg);
			break;
		case qdom::op::FILL:
			if (!need(4))
				goto done;
			nvgPathWinding(vg, NVG_SOLID);
			nvgFillColor(vg, color_at(c + i));
			nvgFill(vg);
			i += 4;
			break;
		case qdom::op::STROKE:
		{
			if (!need(7))
				goto done;
			static const int caps[] = { NVG_BUTT, NVG_ROUND, NVG_SQUARE }, joins[] = { NVG_MITER, NVG_ROUND, NVG_BEVEL };
			nvgStrokeWidth(vg, c[i]);
			nvgLineCap(vg, caps[int(c[i + 1]) % 3]);
			nvgLineJoin(vg, joins[int(c[i + 2]) % 3]);
			nvgStrokeColor(vg, color_at(c + i + 3));
			nvgStroke(vg);
			i += 7;
			break;
		}
		case qdom::op::IMAGE:
		{
			if (!need(6))
				goto done;
			auto it = g->images.find(int(c[i]));
			if (it != g->images.end())
			{
				const int im = c[i + 5] != 0 ? it->second.smooth : it->second.nearest;
				NVGpaint p = nvgImagePattern(vg, c[i + 1], c[i + 2], c[i + 3], c[i + 4], 0, im, 1);
				nvgBeginPath(vg);
				nvgRect(vg, c[i + 1], c[i + 2], c[i + 3], c[i + 4]);
				nvgFillPaint(vg, p);
				nvgFill(vg);
			}
			i += 6;
			break;
		}
		case qdom::op::SHADOW:
		{
			if (!need(10))
				goto done;
			const float x = c[i], y = c[i + 1], w = c[i + 2], h = c[i + 3], r = c[i + 4], blur = c[i + 5];
			NVGcolor in = color_at(c + i + 6), out = in;
			out.a = 0;
			NVGpaint p = nvgBoxGradient(vg, x, y, w, h, r, std::max(1.0f, blur), in, out);
			nvgBeginPath(vg);
			nvgRect(vg, x - blur, y - blur, w + 2 * blur, h + 2 * blur);
			nvgFillPaint(vg, p);
			nvgFill(vg);
			i += 10;
			break;
		}
		case qdom::op::GRADIENT:
		{
			// a box's rounded shape filled with a gradient within a band: linear from one point to another, or
			// radial from a centre between two radii
			if (!need(25))
				goto done;
			const float *b = c + i + 8;
			const NVGcolor c0 = color_at(c + i + 17), c1 = color_at(c + i + 21);
			const NVGpaint p = b[4] != 0 ? nvgRadialGradient(vg, b[5], b[6], b[7], std::max(b[8], b[7] + 0.5f), c0, c1)
			                             : nvgLinearGradient(vg, b[5], b[6], b[7], b[8], c0, c1);
			nvgSave(vg);
			nvgIntersectScissor(vg, b[0], b[1], b[2], b[3]);
			nvgBeginPath(vg);
			nvgRoundedRectVarying(vg, c[i], c[i + 1], c[i + 2], c[i + 3], c[i + 4], c[i + 5], c[i + 6], c[i + 7]);
			nvgFillPaint(vg, p);
			nvgFill(vg);
			nvgRestore(vg);
			i += 25;
			break;
		}
		case qdom::op::TRANSFORM:
		{
			// a canvas's pixels to the page: after what is there
			if (!need(6))
				goto done;
			State &s = g->stack.back();
			const float a = c[i], b = c[i + 1], cc = c[i + 2], d = c[i + 3], e = c[i + 4], f = c[i + 5];
			const float m[6] = { s.m[0] * a + s.m[2] * b, s.m[1] * a + s.m[3] * b, s.m[0] * cc + s.m[2] * d,
				s.m[1] * cc + s.m[3] * d, s.m[0] * e + s.m[2] * f + s.m[4], s.m[1] * e + s.m[3] * f + s.m[5] };
			std::memcpy(s.m, m, sizeof(m));
			s.identity = m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1 && m[4] == 0 && m[5] == 0;
			nvgTransform(vg, a, b, cc, d, e, f);
			i += 6;
			break;
		}
		case qdom::op::CLEAR:
			// a canvas's partial clear needs a framebuffer of its own: not drawn (design.md §9)
			if (!need(4))
				goto done;
			i += 4;
			break;
		default:
			goto done;
		}
	}
done:
	nvgEndFrame(vg);
}
