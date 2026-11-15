// Copyright 2026 Ian Karlsson
#include "input/field.h"

#include <algorithm>
#include <cmath>

#include "css/values.h"
#include "dom/dom.h"
#include "layout/layout.h"

namespace qdom
{

bool is_input_type(const Element &el, std::string_view type)
{
	if (!el.is(n::input))
		return false;
	const std::string *t = el.attribute(n::type);
	return (t ? lower(*t) : std::string("text")) == type;
}

bool is_text_field(const Element &el)
{
	if (el.is(n::textarea))
		return true;
	if (!el.is(n::input))
		return false;
	const std::string *t = el.attribute(n::type);
	const std::string type = t ? lower(*t) : "text";
	return type != "checkbox" && type != "radio" && type != "range" && type != "button" && type != "submit"
		&& type != "reset" && type != "color" && type != "file" && type != "hidden" && type != "image";
}

std::string field_value(const Element &el)
{
	if (el.form.dirty_value)
		return el.form.value;
	if (el.is(n::textarea))
		return el.text_content();
	const std::string *v = el.attribute(n::value);
	return v ? *v : std::string();
}

// ---------------------------------------------------------------- characters

namespace
{

bool continuation(char c)
{
	return (static_cast<unsigned char>(c) & 0xc0) == 0x80;
}

//! The code point at an offset (a character's start).
uint32_t code_at(std::string_view s, size_t i)
{
	const auto b = [&](size_t k) { return k < s.size() ? uint32_t(static_cast<unsigned char>(s[k])) : 0u; };
	const uint32_t c = b(i);
	if (c < 0x80)
		return c;
	if (c < 0xe0)
		return (c & 0x1f) << 6 | (b(i + 1) & 0x3f);
	if (c < 0xf0)
		return (c & 0x0f) << 12 | (b(i + 1) & 0x3f) << 6 | (b(i + 2) & 0x3f);
	return (c & 0x07) << 18 | (b(i + 1) & 0x3f) << 12 | (b(i + 2) & 0x3f) << 6 | (b(i + 3) & 0x3f);
}

//! What a character is to word moves: 0 space, 1 punctuation, 2 a word's.
int char_class(uint32_t c)
{
	if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xa0)
		return 0;
	if (c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
		return 2;
	return 1;
}

} // namespace

size_t char_start(std::string_view s, size_t i)
{
	i = std::min(i, s.size());
	while (i > 0 && i < s.size() && continuation(s[i]))
		i--;
	return i;
}

size_t prev_char(std::string_view s, size_t i)
{
	i = std::min(i, s.size());
	if (i == 0)
		return 0;
	i--;
	while (i > 0 && continuation(s[i]))
		i--;
	return i;
}

size_t next_char(std::string_view s, size_t i)
{
	if (i >= s.size())
		return s.size();
	i++;
	while (i < s.size() && continuation(s[i]))
		i++;
	return i;
}

size_t prev_word(std::string_view s, size_t i)
{
	i = char_start(s, i);
	// the spaces before, then the run of one class
	while (i > 0 && char_class(code_at(s, prev_char(s, i))) == 0)
		i = prev_char(s, i);
	if (i == 0)
		return 0;
	const int k = char_class(code_at(s, prev_char(s, i)));
	while (i > 0 && char_class(code_at(s, prev_char(s, i))) == k)
		i = prev_char(s, i);
	return i;
}

size_t next_word(std::string_view s, size_t i)
{
	i = char_start(s, i);
	while (i < s.size() && char_class(code_at(s, i)) == 0)
		i = next_char(s, i);
	if (i >= s.size())
		return s.size();
	const int k = char_class(code_at(s, i));
	while (i < s.size() && char_class(code_at(s, i)) == k)
		i = next_char(s, i);
	return i;
}

void word_at(std::string_view s, size_t i, size_t &from, size_t &to)
{
	i = char_start(s, i);
	if (s.empty())
	{
		from = to = 0;
		return;
	}
	// the character after the offset, or the one before at the end of the text
	size_t at = i < s.size() ? i : prev_char(s, i);
	if (s[at] == '\n' && at > 0)
		at = prev_char(s, at);
	const int k = char_class(code_at(s, at));
	from = at;
	while (from > 0 && s[prev_char(s, from)] != '\n' && char_class(code_at(s, prev_char(s, from))) == k)
		from = prev_char(s, from);
	to = at;
	while (to < s.size() && s[to] != '\n' && char_class(code_at(s, to)) == k)
		to = next_char(s, to);
}

size_t utf16_index(std::string_view s, size_t byte)
{
	size_t n = 0;
	for (size_t i = 0; i < s.size() && i < byte; i = next_char(s, i))
		n += code_at(s, i) >= 0x10000 ? 2 : 1;
	return n;
}

size_t byte_index(std::string_view s, size_t utf16)
{
	size_t i = 0, n = 0;
	while (i < s.size() && n < utf16)
	{
		n += code_at(s, i) >= 0x10000 ? 2 : 1;
		i = next_char(s, i);
	}
	return i;
}

// ---------------------------------------------------------------- the lines

FieldText::FieldText(Dom &dom, const Element &el, const LayoutNode &n, const std::string &text)
	: m_dom(dom), m_text(text)
{
	const ComputedStyle &cs = n.cs();
	m_face = cs.face;
	m_size = cs.font_size;
	m_spacing = cs.letter_spacing;
	cx = n.bl + n.pl;
	cy = n.bt + n.pt;
	cw = std::max(0.0, n.w - n.bl - n.br - n.pl - n.pr);
	ch = std::max(0.0, n.h - n.bt - n.bb - n.pt - n.pb);
	const FontMetrics fm = dom.fonts.metrics(m_face, m_size);
	ascent = fm.ascent;
	descent = fm.descent;
	multiline = el.is(n::textarea);
	if (!multiline)
	{
		// one line, centred in the content box as the paint of a control draws it
		line_h = ch;
		lines.push_back({ 0, text.size(), 0, (ch - ascent - descent) / 2 + ascent });
		align(cs.text_align);
		return;
	}
	switch (cs.line_height.kind)
	{
	case LineHeight::Kind::Normal: line_h = fm.normal; break;
	case LineHeight::Kind::Number: line_h = cs.line_height.value * m_size; break;
	default: line_h = cs.line_height.value; break;
	}
	if (line_h <= 0)
		line_h = fm.normal;
	const bool wraps = cs.white_space != WhiteSpace::Pre && cs.white_space != WhiteSpace::Nowrap;
	size_t from = 0;
	for (;;)
	{
		const size_t nl = text.find('\n', from);
		const size_t to = nl == std::string::npos ? text.size() : nl;
		wrap(from, to, cw, wraps);
		if (nl == std::string::npos)
			break;
		from = nl + 1;
	}
	for (size_t i = 0; i < lines.size(); i++)
	{
		lines[i].top = double(i) * line_h;
		lines[i].base = lines[i].top + (line_h - ascent - descent) / 2 + ascent;
	}
	align(cs.text_align);
}

void FieldText::align(TextAlign a)
{
	if (a != TextAlign::Center && a != TextAlign::Right && a != TextAlign::End)
		return;
	const std::string_view s = m_text;
	for (FieldLine &l : lines)
	{
		// a wrapped line's spaces at its end hang
		size_t to = l.to;
		while (multiline && to > l.from && s[to - 1] == ' ')
			to--;
		const double free = std::max(0.0, cw - measure(l.from, to));
		l.x = a == TextAlign::Center ? free / 2 : free;
	}
}

double FieldText::measure(size_t from, size_t to) const
{
	if (to <= from)
		return 0;
	return m_dom.fonts.width(m_face, m_size, m_spacing, std::string_view(m_text).substr(from, to - from));
}

void FieldText::wrap(size_t from, size_t to, double width, bool wraps)
{
	const std::string_view s = m_text;
	if (!wraps || from == to || measure(from, to) <= width)
	{
		lines.push_back({ from, to, 0, 0 });
		return;
	}
	size_t start = from;
	while (start < to)
	{
		// the last break (after a run of spaces) at which the line, its spaces hanging, still fits
		size_t brk = 0;
		size_t i = start;
		while (i < to)
		{
			size_t word = i;
			while (word < to && s[word] != ' ')
				word = next_char(s, word);
			if (word > start && measure(start, word) > width)
				break;
			size_t after = word;
			while (after < to && s[after] == ' ')
				after = next_char(s, after);
			brk = after;
			i = after;
		}
		if (i >= to && brk >= to)
		{
			lines.push_back({ start, to, 0, 0 });
			return;
		}
		if (brk <= start)
		{
			// a word wider than the line: as many characters as fit, one at least
			size_t end = next_char(s, start);
			while (end < to && measure(start, next_char(s, end)) <= width)
				end = next_char(s, end);
			brk = end;
		}
		lines.push_back({ start, brk, 0, 0 });
		start = brk;
	}
}

size_t FieldText::line_of(size_t i) const
{
	// the last line starting at or before it: a wrapped line's end is the next one's start
	for (size_t k = lines.size(); k-- > 0;)
		if (lines[k].from <= i)
			return k;
	return 0;
}

double FieldText::x_of(size_t i) const
{
	if (lines.empty())
		return 0;
	const FieldLine &l = lines[line_of(i)];
	return l.x + measure(l.from, std::min(i, l.to));
}

size_t FieldText::index_on(size_t line, double x) const
{
	if (lines.empty())
		return 0;
	const FieldLine &l = lines[std::min(line, lines.size() - 1)];
	const std::string_view s = m_text;
	x -= l.x;
	// a wrapped line's last place is before the next line's first character
	const bool wrapped = line + 1 < lines.size() && lines[line + 1].from == l.to;
	const size_t last = wrapped && l.to > l.from ? prev_char(s, l.to) : l.to;
	size_t best = l.from;
	double prev = 0;
	for (size_t i = l.from; i < last;)
	{
		const size_t nx = next_char(s, i);
		const double w = measure(l.from, nx);
		// past the middle of this character: after it
		if (x < (prev + w) / 2)
			return i;
		best = nx;
		prev = w;
		i = nx;
	}
	return best;
}

size_t FieldText::index_at(double x, double y) const
{
	if (lines.empty())
		return 0;
	const double k = multiline ? std::floor(y / line_h) : 0;
	const size_t line = size_t(std::clamp(k, 0.0, double(lines.size() - 1)));
	return index_on(line, x);
}

double FieldText::width() const
{
	double w = 0;
	for (const FieldLine &l : lines)
		w = std::max(w, l.x + measure(l.from, l.to));
	return w;
}

} // namespace qdom
