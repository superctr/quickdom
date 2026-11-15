// Copyright 2026 Ian Karlsson
#include "text/fonts.h"

#include <cmath>
#include <cstring>

#include "core/embedded.h"
#include "css/values.h"
#include "paint/picture.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace qdom
{

struct FontSet::Font
{
	std::vector<uint8_t> copy;
	stbtt_fontinfo info {};
	double ascent = 0, descent = 0, gap = 0, average = 0.5;  // in ems
	MetricsRounding rounding = MetricsRounding::Round;
	bool whole = false;  // advances in whole pixels (a bitmap font's, as Firefox lays them out)
	size_t size = 0;     // the file's bytes
	std::string family;
	uint32_t cblc = 0, cbdt = 0;  // its colour bitmaps' tables (Noto Color Emoji's), 0 for none

	//! A colour glyph's picture as the font has it, decoded, premultiplied: its strike's pixels an em, its size and
	//! its corner from the pen (bearing x, bearing y up from the baseline).
	struct Picture
	{
		int ppem = 0, width = 0, height = 0, left = 0, top = 0;
		std::vector<uint8_t> rgba;
	};
	//! A picture reduced by halves (steps of them), as WebRender reduces a bitmap glyph.
	struct Reduced
	{
		int width = 0, height = 0;
		float left = 0, top = 0;
		std::vector<uint8_t> rgba;
	};
	mutable std::unordered_map<int, Picture> pictures;      // by glyph; empty for a glyph without one
	std::unordered_map<uint64_t, double> advances;          // a bitmap's advance in ems, by glyph << 8 | ppem
	mutable std::unordered_map<uint64_t, Reduced> reduced;  // by glyph << 8 | steps
};

namespace
{

// a pair's kerning, as the browser's shaper applies it: every pair-positioning lookup's (an extension's too), in
// each the first subtable that covers the pair, its first value's advance
int value_size(stbtt_uint16 format)
{
	int n = 0;
	for (; format; format >>= 1)
		n += format & 1;
	return 2 * n;
}

int advance_of(stbtt_uint8 *record, stbtt_uint16 format)
{
	if (!(format & 4))
		return 0;
	return ttSHORT(record + value_size(format & 3));
}

//! One pair-positioning subtable: true and the advance when it covers the pair.
bool pair_subtable(stbtt_uint8 *t, int g1, int g2, int &out)
{
	const stbtt_uint16 format = ttUSHORT(t);
	const int ci = stbtt__GetCoverageIndex(t + ttUSHORT(t + 2), g1);
	if (ci < 0)
		return false;
	const stbtt_uint16 f1 = ttUSHORT(t + 4), f2 = ttUSHORT(t + 6);
	const int s1 = value_size(f1), s2 = value_size(f2);
	if (format == 1)
	{
		stbtt_uint8 *set = t + ttUSHORT(t + 10 + 2 * ci);
		const int n = ttUSHORT(set), size = 2 + s1 + s2;
		int l = 0, r = n - 1;
		while (l <= r)
		{
			const int m = (l + r) / 2;
			stbtt_uint8 *rec = set + 2 + size * m;
			const int g = ttUSHORT(rec);
			if (g2 < g)
				r = m - 1;
			else if (g2 > g)
				l = m + 1;
			else
			{
				out = advance_of(rec + 2, f1);
				return true;
			}
		}
		return false;
	}
	if (format == 2)
	{
		// a glyph the class table leaves out is in class 0
		int c1 = stbtt__GetGlyphClass(t + ttUSHORT(t + 8), g1);
		int c2 = stbtt__GetGlyphClass(t + ttUSHORT(t + 10), g2);
		const int n1 = ttUSHORT(t + 12), n2 = ttUSHORT(t + 14);
		c1 = c1 < 0 ? 0 : c1;
		c2 = c2 < 0 ? 0 : c2;
		if (c1 >= n1 || c2 >= n2)
			return false;
		out = advance_of(t + 16 + (c1 * n2 + c2) * (s1 + s2), f1);
		return true;
	}
	return false;
}

int kerning(const stbtt_fontinfo *f, int g1, int g2)
{
	if (!f->gpos)
		return stbtt_GetGlyphKernAdvance(f, g1, g2);
	stbtt_uint8 *gpos = f->data + f->gpos;
	if (ttUSHORT(gpos) != 1)
		return 0;
	stbtt_uint8 *list = gpos + ttUSHORT(gpos + 8);
	const int lookups = ttUSHORT(list);
	int sum = 0;
	for (int i = 0; i < lookups; i++)
	{
		stbtt_uint8 *lookup = list + ttUSHORT(list + 2 + 2 * i);
		const int type = ttUSHORT(lookup), subtables = ttUSHORT(lookup + 4);
		for (int k = 0; k < subtables; k++)
		{
			stbtt_uint8 *t = lookup + ttUSHORT(lookup + 6 + 2 * k);
			if (type == 9)
			{
				if (ttUSHORT(t + 2) != 2)
					continue;
				t += ttULONG(t + 4);
			}
			else if (type != 2)
				continue;
			int v;
			if (pair_subtable(t, g1, g2, v))
			{
				sum += v;
				break;
			}
		}
	}
	return sum;
}

uint32_t next_codepoint(const unsigned char *&p, const unsigned char *end)
{
	uint32_t c = *p++;
	int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
	if (more)
		c &= 0x3fu >> more;
	while (more-- && p < end && (*p & 0xc0) == 0x80)
		c = c << 6 | (*p++ & 0x3f);
	return c;
}

std::string family_key(std::string_view name)
{
	std::string_view n = trim(name);
	if (n.size() >= 2 && (n.front() == '"' || n.front() == '\'') && n.back() == n.front())
		n = n.substr(1, n.size() - 2);
	return lower(trim(n));
}

constexpr uint32_t CACHED = 0x3000;

// Unicode's Emoji_Presentation (emoji-data.txt, Unicode 15.1): the characters shown as emoji by default, which
// Firefox's fallback takes from the emoji font before any other
constexpr uint32_t EMOJI_PRESENTATION[][2] = {
	{ 0x231A, 0x231B }, { 0x23E9, 0x23EC }, { 0x23F0, 0x23F0 }, { 0x23F3, 0x23F3 }, { 0x25FD, 0x25FE },
	{ 0x2614, 0x2615 }, { 0x2648, 0x2653 }, { 0x267F, 0x267F }, { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 },
	{ 0x26AA, 0x26AB }, { 0x26BD, 0x26BE }, { 0x26C4, 0x26C5 }, { 0x26CE, 0x26CE }, { 0x26D4, 0x26D4 },
	{ 0x26EA, 0x26EA }, { 0x26F2, 0x26F3 }, { 0x26F5, 0x26F5 }, { 0x26FA, 0x26FA }, { 0x26FD, 0x26FD },
	{ 0x2705, 0x2705 }, { 0x270A, 0x270B }, { 0x2728, 0x2728 }, { 0x274C, 0x274C }, { 0x274E, 0x274E },
	{ 0x2753, 0x2755 }, { 0x2757, 0x2757 }, { 0x2795, 0x2797 }, { 0x27B0, 0x27B0 }, { 0x27BF, 0x27BF },
	{ 0x2B1B, 0x2B1C }, { 0x2B50, 0x2B50 }, { 0x2B55, 0x2B55 }, { 0x1F004, 0x1F004 }, { 0x1F0CF, 0x1F0CF },
	{ 0x1F18E, 0x1F18E }, { 0x1F191, 0x1F19A }, { 0x1F1E6, 0x1F1FF }, { 0x1F201, 0x1F201 }, { 0x1F21A, 0x1F21A },
	{ 0x1F22F, 0x1F22F }, { 0x1F232, 0x1F236 }, { 0x1F238, 0x1F23A }, { 0x1F250, 0x1F251 }, { 0x1F300, 0x1F320 },
	{ 0x1F32D, 0x1F335 }, { 0x1F337, 0x1F37C }, { 0x1F37E, 0x1F393 }, { 0x1F3A0, 0x1F3CA }, { 0x1F3CF, 0x1F3D3 },
	{ 0x1F3E0, 0x1F3F0 }, { 0x1F3F4, 0x1F3F4 }, { 0x1F3F8, 0x1F43E }, { 0x1F440, 0x1F440 }, { 0x1F442, 0x1F4FC },
	{ 0x1F4FF, 0x1F53D }, { 0x1F54B, 0x1F54E }, { 0x1F550, 0x1F567 }, { 0x1F57A, 0x1F57A }, { 0x1F595, 0x1F596 },
	{ 0x1F5A4, 0x1F5A4 }, { 0x1F5FB, 0x1F64F }, { 0x1F680, 0x1F6C5 }, { 0x1F6CC, 0x1F6CC }, { 0x1F6D0, 0x1F6D2 },
	{ 0x1F6D5, 0x1F6D9 }, { 0x1F6DC, 0x1F6DF }, { 0x1F6EB, 0x1F6EC }, { 0x1F6F4, 0x1F6FC }, { 0x1F7E0, 0x1F7EB },
	{ 0x1F7F0, 0x1F7F0 }, { 0x1F90C, 0x1F93A }, { 0x1F93C, 0x1F945 }, { 0x1F947, 0x1F9FF }, { 0x1FA70, 0x1FA7C },
	{ 0x1FA80, 0x1FAC6 }, { 0x1FAC8, 0x1FAC8 }, { 0x1FACC, 0x1FADD }, { 0x1FADF, 0x1FAEB }, { 0x1FAEF, 0x1FAFA },
};

bool emoji_presentation(uint32_t c)
{
	size_t l = 0, r = sizeof(EMOJI_PRESENTATION) / sizeof(EMOJI_PRESENTATION[0]);
	while (l < r)
	{
		const size_t m = (l + r) / 2;
		if (c < EMOJI_PRESENTATION[m][0])
			r = m;
		else if (c > EMOJI_PRESENTATION[m][1])
			l = m + 1;
		else
			return true;
	}
	return false;
}

//! The variation selectors, which choose a character's look and take no room: text (FE0E), emoji (FE0F).
bool variation_selector(uint32_t c)
{
	return c >= 0xfe00 && c <= 0xfe0f;
}

// ---------------------------------------------------------------- colour bitmaps (CBLC and CBDT)

//! Where a glyph's PNG is in a font's CBDT, and its metrics in its strike's pixels.
struct BitmapGlyph
{
	uint32_t offset = 0, length = 0;
	int ppem = 0, width = 0, height = 0, left = 0, top = 0, advance = 0;
};

//! The strike a size is drawn from, as FreeType chooses it: the smallest at least as large, else the largest.
//! The offset of its BitmapSize record, 0 for none.
uint32_t choose_strike(uint8_t *d, size_t size, uint32_t cblc, double pixels)
{
	if (size_t(cblc) + 8 > size)
		return 0;
	const uint32_t n = ttULONG(d + cblc + 4);
	uint32_t best = 0;
	int best_ppem = 0;
	for (uint32_t i = 0; i < n; i++)
	{
		const uint32_t rec = cblc + 8 + 48 * i;
		if (size_t(rec) + 48 > size)
			break;
		const int ppem = d[rec + 45];
		if (d[rec + 46] != 32 || !ppem)
			continue;
		const bool fits = ppem >= pixels, best_fits = best && best_ppem >= pixels;
		if (!best || (fits && (!best_fits || ppem < best_ppem)) || (!fits && !best_fits && ppem > best_ppem))
		{
			best = rec;
			best_ppem = ppem;
		}
	}
	return best;
}

bool find_bitmap(uint8_t *d, size_t size, uint32_t cblc, uint32_t cbdt, uint32_t strike, int glyph, BitmapGlyph &out)
{
	const uint32_t array = cblc + ttULONG(d + strike);
	const uint32_t subtables = ttULONG(d + strike + 8);
	out.ppem = d[strike + 45];
	auto fits = [&](uint64_t at, uint64_t n) { return at + n <= size; };
	for (uint32_t i = 0; i < subtables; i++)
	{
		const uint32_t e = array + 8 * i;
		if (!fits(e, 8))
			return false;
		const int first = ttUSHORT(d + e), last = ttUSHORT(d + e + 2);
		if (glyph < first || glyph > last)
			continue;
		const uint32_t sub = array + ttULONG(d + e + 4);
		if (!fits(sub, 8))
			return false;
		const int index_format = ttUSHORT(d + sub), image_format = ttUSHORT(d + sub + 2);
		const uint32_t data = cbdt + ttULONG(d + sub + 4);
		uint32_t at = 0, end = 0;
		uint8_t *big = nullptr;  // the subtable's metrics, for image format 19
		const int k = glyph - first;
		switch (index_format)
		{
		case 1:
			if (!fits(sub + 8 + 4 * uint64_t(k), 8))
				return false;
			at = ttULONG(d + sub + 8 + 4 * k);
			end = ttULONG(d + sub + 12 + 4 * k);
			break;
		case 2:
		{
			if (!fits(sub, 20))
				return false;
			const uint32_t n = ttULONG(d + sub + 8);
			at = n * uint32_t(k);
			end = at + n;
			big = d + sub + 12;
			break;
		}
		case 3:
			if (!fits(sub + 8 + 2 * uint64_t(k), 4))
				return false;
			at = ttUSHORT(d + sub + 8 + 2 * k);
			end = ttUSHORT(d + sub + 10 + 2 * k);
			break;
		case 4:
		{
			if (!fits(sub, 12))
				return false;
			const uint32_t n = ttULONG(d + sub + 8);
			if (!fits(sub + 12, 4 * (uint64_t(n) + 1)))
				return false;
			bool found = false;
			for (uint32_t j = 0; j < n && !found; j++)
				if (ttUSHORT(d + sub + 12 + 4 * j) == glyph)
				{
					at = ttUSHORT(d + sub + 14 + 4 * j);
					end = ttUSHORT(d + sub + 18 + 4 * j);
					found = true;
				}
			if (!found)
				return false;
			break;
		}
		case 5:
		{
			if (!fits(sub, 24))
				return false;
			const uint32_t n = ttULONG(d + sub + 8), count = ttULONG(d + sub + 20);
			if (!fits(sub + 24, 2 * uint64_t(count)))
				return false;
			bool found = false;
			for (uint32_t j = 0; j < count && !found; j++)
				if (ttUSHORT(d + sub + 24 + 2 * j) == glyph)
				{
					at = n * j;
					end = at + n;
					found = true;
				}
			if (!found)
				return false;
			big = d + sub + 12;
			break;
		}
		default:
			return false;
		}
		const uint64_t from = uint64_t(data) + at, to = uint64_t(data) + end;
		if (end <= at || to > size)
			return false;
		uint8_t *p = d + from;
		uint64_t header = 0;
		if (image_format == 17 && to - from >= 9)
		{
			out.height = p[0];
			out.width = p[1];
			out.left = int8_t(p[2]);
			out.top = int8_t(p[3]);
			out.advance = p[4];
			header = 5;
		}
		else if (image_format == 18 && to - from >= 12)
		{
			out.height = p[0];
			out.width = p[1];
			out.left = int8_t(p[2]);
			out.top = int8_t(p[3]);
			out.advance = p[4];
			header = 8;
		}
		else if (image_format == 19 && big && to - from >= 4)
		{
			out.height = big[0];
			out.width = big[1];
			out.left = int8_t(big[2]);
			out.top = int8_t(big[3]);
			out.advance = big[4];
		}
		else
			return false;
		const uint32_t length = ttULONG(p + header);
		if (from + header + 4 + length > to)
			return false;
		out.offset = uint32_t(from + header + 4);
		out.length = length;
		return true;
	}
	return false;
}

} // namespace

FontSet::FontSet()
{
	for (const char *g : { "sans-serif", "serif", "system-ui", "cursive", "fantasy", "ui-sans-serif" })
		m_generic[g] = "noto sans";
	m_generic["monospace"] = "dejavu sans mono";
	m_generic["ui-monospace"] = "dejavu sans mono";
	m_fallback = "dejavu sans";
	m_symbols = "noto sans symbols 2";
	m_emoji = "noto color emoji";
}

FontSet::~FontSet() = default;

bool FontSet::add(std::string_view family, int weight, bool italic, const uint8_t *data, size_t size, bool keep,
	MetricsRounding rounding, bool whole_advances)
{
	auto font = std::make_unique<Font>();
	const unsigned char *bytes = data;
	if (!keep)
	{
		font->copy.assign(data, data + size);
		bytes = font->copy.data();
	}
	const int offset = stbtt_GetFontOffsetForIndex(bytes, 0);
	if (offset < 0 || !stbtt_InitFont(&font->info, bytes, offset))
		return false;
	int a, d, g;
	stbtt_GetFontVMetrics(&font->info, &a, &d, &g);
	const double em = stbtt_ScaleForMappingEmToPixels(&font->info, 1);
	font->ascent = a * em;
	font->descent = -d * em;
	font->gap = g * em;
	// OS/2's xAvgCharWidth, which a browser sizes a text field by
	const stbtt_uint32 os2 = stbtt__find_table(font->info.data, font->info.fontstart, "OS/2");
	font->average = os2 ? ttSHORT(font->info.data + os2 + 2) * em : 0.5;
	font->rounding = rounding;
	font->whole = whole_advances;
	font->size = size;
	font->cblc = stbtt__find_table(font->info.data, font->info.fontstart, "CBLC");
	font->cbdt = stbtt__find_table(font->info.data, font->info.fontstart, "CBDT");
	if (!font->cblc || !font->cbdt)
		font->cblc = font->cbdt = 0;
	// a bitmap font's advances are whole pixels, as Firefox lays them out
	font->whole = font->whole || font->cblc;
	m_fonts.push_back(std::move(font));
	const int index = int(m_fonts.size() - 1);

	const std::string key = family_key(family);
	m_fonts.back()->family = key;
	Family *f = nullptr;
	for (Family &x : m_families)
		if (x.name == key)
			f = &x;
	if (!f)
	{
		m_families.push_back({ key, {} });
		f = &m_families.back();
	}
	f->fonts[(weight >= 600 ? 1 : 0) + (italic ? 2 : 0)] = index;
	m_face_keys.clear();
	m_faces.clear();
	m_widths.clear();
	m_metrics.clear();
	return true;
}

bool FontSet::add_defaults()
{
	static const struct
	{
		const char *file, *family;
		int weight;
		bool italic;
		MetricsRounding rounding;
		bool whole = false;
	} files[] = {
		{ "NotoSans-Regular.ttf", "Noto Sans", 400, false, MetricsRounding::Round },
		{ "NotoSans-Bold.ttf", "Noto Sans", 700, false, MetricsRounding::Round },
		{ "NotoSans-Italic.ttf", "Noto Sans", 400, true, MetricsRounding::Round },
		{ "NotoSans-BoldItalic.ttf", "Noto Sans", 700, true, MetricsRounding::Round },
		{ "DejaVuSans.ttf", "DejaVu Sans", 400, false, MetricsRounding::Ceil },
		{ "DejaVuSans-Bold.ttf", "DejaVu Sans", 700, false, MetricsRounding::Ceil },
		{ "DejaVuSansMono.ttf", "DejaVu Sans Mono", 400, false, MetricsRounding::Ceil },
		{ "DejaVuSansMono-Bold.ttf", "DejaVu Sans Mono", 700, false, MetricsRounding::Ceil },
		{ "NotoSansSymbols2-Regular.ttf", "Noto Sans Symbols 2", 400, false, MetricsRounding::Round },
		{ "NotoColorEmoji.ttf", "Noto Color Emoji", 400, false, MetricsRounding::Round, true },
	};
	bool all = qdom_fonts_count > 0;
	for (const auto &f : files)
	{
		const qdom_embedded_t *e = nullptr;
		for (size_t i = 0; i < qdom_fonts_count; i++)
			if (!strcmp(qdom_fonts[i].name, f.file))
				e = &qdom_fonts[i];
		if (!e || !add(f.family, f.weight, f.italic, e->data, e->size, true, f.rounding, f.whole))
			all = false;
	}
	return all;
}

void FontSet::set_generic(std::string_view generic, std::string_view family)
{
	m_generic[lower(generic)] = family_key(family);
	m_face_keys.clear();
	m_faces.clear();
	m_widths.clear();
	m_metrics.clear();
}

void FontSet::set_fallback(std::string_view family)
{
	m_fallback = family_key(family);
	m_face_keys.clear();
	m_faces.clear();
	m_widths.clear();
	m_metrics.clear();
}

const FontSet::Family *FontSet::find_family(std::string_view name) const
{
	for (const Family &f : m_families)
		if (f.name == name)
			return &f;
	return nullptr;
}

int FontSet::family_font(const Family &f, bool bold, bool italic) const
{
	const int want = (bold ? 1 : 0) + (italic ? 2 : 0);
	if (f.fonts[want] >= 0)
		return f.fonts[want];
	// an italic missing: upright (the browser slants it, the widths are the same); a bold missing: regular
	if (italic && f.fonts[want - 2] >= 0)
		return f.fonts[want - 2];
	if (bold && f.fonts[want - 1] >= 0)
		return f.fonts[want - 1];
	for (int k : f.fonts)
		if (k >= 0)
			return k;
	return -1;
}

FaceId FontSet::face(std::string_view families, int weight, FontStyle style)
{
	const bool bold = weight >= 600, italic = style != FontStyle::Normal;
	std::string key = std::string(families) + "|" + (bold ? "b" : "") + (italic ? "i" : "");
	auto it = m_face_keys.find(key);
	if (it != m_face_keys.end())
		return it->second;
	int font = -1;
	for (std::string_view name : split_commas(families))
	{
		std::string k = family_key(name);
		auto g = m_generic.find(k);
		if (g != m_generic.end())
			k = g->second;
		if (const Family *f = find_family(k))
		{
			font = family_font(*f, bold, italic);
			if (font >= 0)
				break;
		}
	}
	if (font < 0)
	{
		auto g = m_generic.find("sans-serif");
		if (const Family *f = g != m_generic.end() ? find_family(g->second) : nullptr)
			font = family_font(*f, bold, italic);
	}
	if (font < 0 && !m_families.empty())
		font = family_font(m_families.front(), bold, italic);
	int fallback = -1, symbols = -1, emoji = -1;
	if (const Family *f = find_family(m_fallback))
		fallback = family_font(*f, bold, false);
	if (const Family *f = find_family(m_symbols))
		symbols = family_font(*f, false, false);
	if (const Family *f = find_family(m_emoji))
		emoji = family_font(*f, false, false);
	// one face per set of fonts
	for (size_t i = 0; i < m_faces.size(); i++)
		if (m_faces[i].font == font && m_faces[i].fallback == fallback && m_faces[i].symbols == symbols
			&& m_faces[i].emoji == emoji)
			return m_face_keys[key] = FaceId(i);
	m_faces.push_back({ font, fallback, symbols, emoji, {} });
	return m_face_keys[key] = FaceId(m_faces.size() - 1);
}

void FontSet::glyph_of(Face &face, uint32_t c, bool emoji, int &font, int &glyph)
{
	const bool cached = c < CACHED && !emoji;
	if (cached && !face.glyphs.empty() && face.glyphs[c] >= 0)
	{
		font = face.glyphs[c] >> 24;
		glyph = face.glyphs[c] & 0xffffff;
		return;
	}
	font = face.font;
	glyph = font >= 0 ? stbtt_FindGlyphIndex(&m_fonts[font]->info, int(c)) : 0;
	// the face's own font first; then, as Firefox's fallback goes, the emoji font for a character shown as an emoji
	// (by default, or asked for by U+FE0F), DejaVu Sans, Noto Sans Symbols 2, and the emoji font for the rest
	const bool prefer = emoji || emoji_presentation(c);
	const int order[] = { prefer ? face.emoji : -1, face.fallback, face.symbols, face.emoji };
	for (int f : order)
	{
		if (glyph || f < 0 || f == face.font)
			continue;
		const int g = stbtt_FindGlyphIndex(&m_fonts[f]->info, int(c));
		if (g)
		{
			font = f;
			glyph = g;
		}
	}
	if (cached)
	{
		if (face.glyphs.empty())
			face.glyphs.assign(CACHED, -1);
		face.glyphs[c] = font << 24 | glyph;
	}
}

bool FontSet::find_glyph(FaceId id, uint32_t c, int &font, int &glyph)
{
	font = -1;
	glyph = 0;
	if (id >= m_faces.size() || m_faces[id].font < 0)
		return false;
	glyph_of(m_faces[id], c, false, font, glyph);
	return glyph != 0;
}

const std::string &FontSet::family_of(int font) const
{
	static const std::string none;
	return font >= 0 && size_t(font) < m_fonts.size() ? m_fonts[size_t(font)]->family : none;
}

double FontSet::layout(FaceId id, double size, double spacing, std::string_view utf8, std::vector<PlacedGlyph> &out)
{
	out.clear();
	if (id >= m_faces.size() || m_faces[id].font < 0)
		return 0;
	Face &face = m_faces[id];
	const unsigned char *p = reinterpret_cast<const unsigned char *>(utf8.data()), *end = p + utf8.size();
	double x = 0, pending = 0;
	int last_font = -1, last_glyph = 0;
	size_t n = 0;
	while (p < end)
	{
		const uint32_t c = next_codepoint(p, end);
		// a variation selector takes no room; U+FE0F after a character asks for its emoji
		if (variation_selector(c))
			continue;
		const unsigned char *q = p;
		const bool emoji = q < end && next_codepoint(q, end) == 0xfe0f;
		int font, glyph;
		glyph_of(face, c, emoji, font, glyph);
		const stbtt_fontinfo *info = &m_fonts[font]->info;
		const double scale = stbtt_ScaleForMappingEmToPixels(info, float(size));
		// a pair's kerning on the first glyph's advance, each advance then in 60ths of a pixel, as the
		// browser's shaper and layout have them
		if (font == last_font)
			pending += kerning(info, last_glyph, glyph) * scale;
		if (n)
			x += std::round(pending * 60) / 60 + spacing;
		out.push_back({ font, glyph, float(x), float(scale) });
		n++;
		int advance, bearing;
		stbtt_GetGlyphHMetrics(info, glyph, &advance, &bearing);
		pending = advance * scale;
		// a bitmap font's advance in whole pixels, its strike's (FreeType's) where it has a picture
		if (m_fonts[font]->whole)
			pending = std::round(m_fonts[font]->cblc ? bitmap_advance(font, glyph, size, pending) : pending);
		last_font = font;
		last_glyph = glyph;
	}
	if (n)
		x += std::round(pending * 60) / 60 + spacing;
	return x;
}

void FontSet::glyph_box(int font, int glyph, float scale, float shift, int &left, int &top, int &width,
	int &height) const
{
	left = top = width = height = 0;
	if (font < 0 || size_t(font) >= m_fonts.size())
		return;
	int x0, y0, x1, y1;
	stbtt_GetGlyphBitmapBoxSubpixel(&m_fonts[size_t(font)]->info, glyph, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
	left = x0;
	top = y0;
	width = x1 - x0;
	height = y1 - y0;
}

void FontSet::glyph_bitmap(int font, int glyph, float scale, float shift, uint8_t *out, int width, int height,
	int stride) const
{
	if (font < 0 || size_t(font) >= m_fonts.size() || width <= 0 || height <= 0)
		return;
	stbtt_MakeGlyphBitmapSubpixel(&m_fonts[size_t(font)]->info, out, width, height, stride, scale, scale, shift, 0,
		glyph);
}

double FontSet::bitmap_advance(int font, int glyph, double size, double otherwise)
{
	Font &f = *m_fonts[size_t(font)];
	const uint32_t strike = choose_strike(f.info.data, f.size, f.cblc, size);
	if (!strike)
		return otherwise;
	const uint64_t key = uint64_t(uint32_t(glyph)) << 8 | f.info.data[strike + 45];
	auto it = f.advances.find(key);
	if (it == f.advances.end())
	{
		BitmapGlyph b;
		const double em = find_bitmap(f.info.data, f.size, f.cblc, f.cbdt, strike, glyph, b) && b.ppem
			? double(b.advance) / b.ppem : -1;
		it = f.advances.emplace(key, em).first;
	}
	return it->second < 0 ? otherwise : it->second * size;
}

bool FontSet::has_color(int font) const
{
	return font >= 0 && size_t(font) < m_fonts.size() && m_fonts[size_t(font)]->cblc;
}

bool FontSet::color_glyph(int font, int glyph, float scale, ColorGlyph &out) const
{
	out = ColorGlyph();
	if (!has_color(font) || scale <= 0)
		return false;
	Font &f = *m_fonts[size_t(font)];
	const double pixels = double(scale) * ttUSHORT(f.info.data + f.info.head + 18);  // the size: units an em
	// the strike's picture, decoded once (a font has one strike in practice: Noto Color Emoji's 109 pixels)
	auto pit = f.pictures.find(glyph);
	if (pit == f.pictures.end())
	{
		Font::Picture pic;
		BitmapGlyph b;
		const uint32_t strike = choose_strike(f.info.data, f.size, f.cblc, pixels);
		int w, h;
		if (strike && find_bitmap(f.info.data, f.size, f.cblc, f.cbdt, strike, glyph, b)
			&& decode_picture(f.info.data + b.offset, b.length, w, h, pic.rgba))
		{
			pic.ppem = b.ppem;
			pic.width = w;
			pic.height = h;
			pic.left = b.left;
			pic.top = b.top;
			// premultiplied, as FreeType gives a colour bitmap
			for (size_t i = 0; i < pic.rgba.size(); i += 4)
			{
				const unsigned a = pic.rgba[i + 3];
				for (int k = 0; k < 3; k++)
					pic.rgba[i + k] = uint8_t((pic.rgba[i + k] * a + 127) / 255);
			}
		}
		else
			pic.rgba.clear();
		pit = f.pictures.emplace(glyph, std::move(pic)).first;
	}
	const Font::Picture &pic = pit->second;
	if (pic.rgba.empty() || !pic.ppem)
		return false;
	// halved until drawn at half its size or more, as WebRender's downscale_bitmap_if_required does
	float s = float(pixels / pic.ppem);
	int steps = 0;
	while (s < 0.5f && steps < 8)
	{
		s *= 2;
		steps++;
	}
	const uint64_t key = uint64_t(uint32_t(glyph)) << 8 | uint64_t(steps);
	auto rit = f.reduced.find(key);
	if (rit == f.reduced.end())
	{
		if (f.reduced.size() > 4096)
			f.reduced.clear();
		Font::Reduced r;
		const int n = 1 << steps;
		r.width = (pic.width + n - 1) >> steps;
		r.height = (pic.height + n - 1) >> steps;
		r.left = float(pic.left) / float(n);
		r.top = float(pic.top) / float(n);
		r.rgba.resize(size_t(r.width) * size_t(r.height) * 4);
		for (int y = 0; y < r.height; y++)
			for (int x = 0; x < r.width; x++)
			{
				const int sy = y << steps, sx = x << steps;
				const int ny = std::min(n, pic.height - sy), nx = std::min(n, pic.width - sx);
				const unsigned count = unsigned(nx * ny);
				unsigned sum[4] = { count / 2, count / 2, count / 2, count / 2 };
				for (int j = 0; j < ny; j++)
				{
					const uint8_t *src = pic.rgba.data() + (size_t(sy + j) * size_t(pic.width) + size_t(sx)) * 4;
					for (int i = 0; i < nx * 4; i++)
						sum[i & 3] += src[i];
				}
				uint8_t *dst = r.rgba.data() + (size_t(y) * size_t(r.width) + size_t(x)) * 4;
				for (int k = 0; k < 4; k++)
					dst[k] = uint8_t(sum[k] / count);
			}
		rit = f.reduced.emplace(key, std::move(r)).first;
	}
	const Font::Reduced &r = rit->second;
	out.width = r.width;
	out.height = r.height;
	out.left = r.left;
	out.top = r.top;
	out.scale = s;
	out.rgba = r.rgba.data();
	return true;
}

double FontSet::width(FaceId face, double size, double spacing, std::string_view utf8)
{
	if (utf8.empty())
		return 0;
	std::string key;
	key.reserve(utf8.size() + 32);
	key.append(reinterpret_cast<const char *>(&face), sizeof(face));
	key.append(reinterpret_cast<const char *>(&size), sizeof(size));
	key.append(reinterpret_cast<const char *>(&spacing), sizeof(spacing));
	key.append(utf8);
	auto it = m_widths.find(key);
	if (it != m_widths.end())
		return it->second;
	if (m_widths.size() > 50000)
		m_widths.clear();
	std::vector<PlacedGlyph> glyphs;
	const double w = layout(face, size, spacing, utf8, glyphs);
	m_widths.emplace(std::move(key), w);
	return w;
}

void FontSet::em_metrics(FaceId id, double &ascent, double &descent, double &gap, double &average) const
{
	if (id >= m_faces.size() || m_faces[id].font < 0)
	{
		ascent = 0.9;
		descent = 0.25;
		gap = 0;
		average = 0.5;
		return;
	}
	const Font &f = *m_fonts[m_faces[id].font];
	ascent = f.ascent;
	descent = f.descent;
	gap = f.gap;
	average = f.average;
}

FontSet::Decorations FontSet::decorations(FaceId id, double size) const
{
	Decorations d;
	if (id >= m_faces.size() || m_faces[id].font < 0)
		return d;
	const stbtt_fontinfo &info = m_fonts[m_faces[id].font]->info;
	const double em = stbtt_ScaleForMappingEmToPixels(&info, 1) * size;
	const stbtt_uint32 post = stbtt__find_table(info.data, info.fontstart, "post");
	const stbtt_uint32 os2 = stbtt__find_table(info.data, info.fontstart, "OS/2");
	auto thick = [](double t) { return std::max(1.0, std::floor(t + 0.5)); };
	if (post)
	{
		d.underline = ttSHORT(info.data + post + 8) * em;
		d.underline_size = thick(ttSHORT(info.data + post + 10) * em);
	}
	if (os2)
	{
		d.strikeout_size = thick(ttSHORT(info.data + os2 + 26) * em);
		d.strikeout = ttSHORT(info.data + os2 + 28) * em;
	}
	return d;
}

FontMetrics FontSet::font_metrics(int font, double size) const
{
	FontMetrics m;
	if (font < 0 || size_t(font) >= m_fonts.size())
		return m;
	const Font &f = *m_fonts[size_t(font)];
	auto round = [](double x) { return std::floor(x + 0.5); };
	if (f.rounding == MetricsRounding::Ceil)
	{
		m.ascent = std::ceil(f.ascent * size - 0.05);
		m.descent = std::ceil(f.descent * size - 0.05);
		m.normal = m.ascent + m.descent + round(f.gap * size);
	}
	else
	{
		m.ascent = round(f.ascent * size);
		m.descent = round(f.descent * size);
		m.normal = round((f.ascent + f.descent + f.gap) * size);
	}
	m.average = round(f.average * size);
	return m;
}

FontMetrics FontSet::metrics(FaceId id, double size)
{
	uint64_t bits;
	static_assert(sizeof(bits) == sizeof(size));
	memcpy(&bits, &size, sizeof(bits));
	const uint64_t key = bits * 31 + id;
	auto it = m_metrics.find(key);
	if (it != m_metrics.end())
		return it->second;
	FontMetrics m;
	if (id < m_faces.size() && m_faces[id].font >= 0)
		m = font_metrics(m_faces[id].font, size);
	else
	{
		auto round = [](double x) { return std::floor(x + 0.5); };
		m.ascent = round(0.9 * size);
		m.descent = round(0.25 * size);
		m.normal = round(1.15 * size);
		m.average = round(0.5 * size);
	}
	m_metrics.emplace(key, m);
	return m;
}

void FontSet::extent(FaceId id, double size, std::string_view utf8, double &ascent, double &descent)
{
	const FontMetrics m = metrics(id, size);
	ascent = m.ascent;
	descent = m.descent;
	// ASCII is the face's own font's
	bool plain = true;
	for (char ch : utf8)
		plain = plain && static_cast<unsigned char>(ch) < 0x80;
	if (plain || id >= m_faces.size() || m_faces[id].font < 0)
		return;
	std::vector<PlacedGlyph> glyphs;
	layout(id, size, 0, utf8, glyphs);
	int last = m_faces[id].font;
	for (const PlacedGlyph &g : glyphs)
		if (g.font != last)
		{
			last = g.font;
			const FontMetrics f = font_metrics(g.font, size);
			ascent = std::max(ascent, f.ascent);
			descent = std::max(descent, f.descent);
		}
}

} // namespace qdom
