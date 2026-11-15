// Copyright 2026 Ian Karlsson
//! \file
//! Fonts and text measurement: TrueType fonts read with stb_truetype, laid out glyph by glyph with their kerning
//! (GPOS pair adjustments, as the browser's shaper applies them, without other shaping), each advance in 60ths of
//! a pixel as Firefox keeps them. A glyph a face lacks comes from the fallback fonts as Firefox's fallback finds
//! them: the emoji family first for a character shown as an emoji (Noto Color Emoji), else the fallback family
//! (DejaVu Sans), the symbols family (Noto Sans Symbols 2), the emoji family. A colour glyph (a CBDT bitmap, Noto
//! Color Emoji's) is a picture, reduced as Firefox's WebRender reduces it. The same measure serves the layout and
//! the drawing (headless, no GPU needed).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "css/style.h"

namespace qdom
{

//! How Firefox rounds a font's extent to pixels: each of ascent and descent rounded and the normal line their sum
//! with the gap rounded (Noto Sans), or each rounded up and the gap rounded on its own (DejaVu Sans Mono, a font
//! with hinting instructions).
enum class MetricsRounding : uint8_t
{
	Round,
	Ceil,
};

//! A face's extent at a size, in whole pixels as the browser rounds it.
struct FontMetrics
{
	double ascent = 0;
	double descent = 0;
	double normal = 0;   //!< the line's height for line-height: normal
	double average = 0;  //!< the average character's width (OS/2 xAvgCharWidth), which sizes text fields
};

//! A glyph placed: the font it is drawn from, its index there, its pen position and the font's scale.
struct PlacedGlyph
{
	int font;
	int glyph;
	float x;
	float scale;
};

class FontSet
{
public:
	FontSet();
	~FontSet();
	FontSet(const FontSet &) = delete;
	FontSet &operator=(const FontSet &) = delete;

	//! A font file for a family's weight and style. The bytes are copied unless keep is set, in which case they
	//! must outlive the set. False when the file is not a font.
	bool add(std::string_view family, int weight, bool italic, const uint8_t *data, size_t size, bool keep,
		MetricsRounding rounding, bool whole_advances = false);
	//! The fonts built into the library (Noto Sans, DejaVu Sans, DejaVu Sans Mono), when it was built with them;
	//! monospace is DejaVu Sans Mono, every other generic family Noto Sans, and DejaVu Sans the fallback; then a
	//! subset of Noto Sans Symbols 2 (pictographs shown as text) and one of Noto Color Emoji (its colour bitmaps,
	//! its advances in whole pixels as Firefox lays out a bitmap font's).
	bool add_defaults();
	//! The family a generic name (sans-serif, serif, monospace, system-ui, cursive, fantasy) stands for.
	void set_generic(std::string_view generic, std::string_view family);
	//! The family a glyph comes from when a face lacks it.
	void set_fallback(std::string_view family);

	//! The face for a font-family list, a weight and a style.
	FaceId face(std::string_view families, int weight, FontStyle style);
	//! Whether a face is monospaced (its family's name says so, as the browser's defaults go).
	bool empty() const { return m_fonts.empty(); }

	//! The font and glyph a face draws a character with, from its own font or a fallback as a text's are found;
	//! false when no font has it.
	bool find_glyph(FaceId face, uint32_t c, int &font, int &glyph);
	//! The family a font was added for, as the face lists name it (lower case).
	const std::string &family_of(int font) const;

	//! The width of a text at a size, spacing added after every character.
	double width(FaceId face, double size, double spacing, std::string_view utf8);
	//! The glyphs of a text and its width.
	double layout(FaceId face, double size, double spacing, std::string_view utf8, std::vector<PlacedGlyph> &out);
	//! A face's extent at a size, rounded as Firefox rounds it.
	FontMetrics metrics(FaceId face, double size);
	//! The extent of a text's glyphs at a size: the largest ascent and descent, each rounded as Firefox rounds it,
	//! of the fonts its glyphs come from (the face's own and the fallbacks), which a line of `line-height: normal`
	//! makes room for as Firefox's does.
	void extent(FaceId face, double size, std::string_view utf8, double &ascent, double &descent);
	//! A face's extent in ems: ascent, descent (positive), line gap, average character width.
	void em_metrics(FaceId face, double &ascent, double &descent, double &gap, double &average) const;
	//! Where a face's text decorations go at a size, in pixels: the top of each line from the baseline (up is
	//! positive) and its thickness (the font's post and OS/2 tables', each line a pixel thick at least).
	struct Decorations
	{
		double underline = -1, underline_size = 1;
		double strikeout = 4, strikeout_size = 1;
	};
	Decorations decorations(FaceId face, double size) const;

	//! A glyph's coverage at a scale (pixels a font unit, PlacedGlyph's) shifted right by part of a pixel: its box
	//! from the pen (left and top in whole pixels, its size), then its bitmap into rows stride bytes apart.
	void glyph_box(int font, int glyph, float scale, float shift, int &left, int &top, int &width, int &height) const;
	void glyph_bitmap(int font, int glyph, float scale, float shift, uint8_t *out, int width, int height,
		int stride) const;

	//! A colour glyph's picture at a scale (pixels a font unit), as WebRender makes it: the strike's PNG decoded
	//! (premultiplied RGBA) and halved by a box filter until it is drawn at half its size or more, then drawn
	//! smooth at `scale` pixels to one of its own, its corner `left` right of the pen and `top` above the baseline
	//! (in its own pixels). False for a glyph that has no picture (an outline's).
	struct ColorGlyph
	{
		int width = 0, height = 0;
		float left = 0, top = 0, scale = 1;
		const uint8_t *rgba = nullptr;  //!< valid until the next call
	};
	bool color_glyph(int font, int glyph, float scale, ColorGlyph &out) const;
	//! Whether a font has colour glyphs (CBDT bitmaps).
	bool has_color(int font) const;

private:
	struct Font;
	struct Family
	{
		std::string name;
		int fonts[4] = { -1, -1, -1, -1 };  //!< regular, bold, italic, bold italic
	};
	struct Face
	{
		int font;
		int fallback;
		int symbols;
		int emoji;
		std::vector<int32_t> glyphs;  //!< font << 24 | glyph for the codepoints below 0x3000, -1 until looked up
	};

	int family_font(const Family &f, bool bold, bool italic) const;
	FontMetrics font_metrics(int font, double size) const;
	//! A colour glyph's advance at a size in pixels: its strike's, scaled (otherwise when it has no picture).
	double bitmap_advance(int font, int glyph, double size, double otherwise);
	const Family *find_family(std::string_view name) const;
	void glyph_of(Face &face, uint32_t c, bool emoji, int &font, int &glyph);

	std::vector<std::unique_ptr<Font>> m_fonts;
	std::vector<Family> m_families;
	std::unordered_map<std::string, std::string> m_generic;
	std::string m_fallback;
	std::string m_symbols;
	std::string m_emoji;
	std::vector<Face> m_faces;
	std::unordered_map<std::string, FaceId> m_face_keys;
	std::unordered_map<std::string, double> m_widths;
	std::unordered_map<uint64_t, FontMetrics> m_metrics;
};

} // namespace qdom
