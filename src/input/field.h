// Copyright 2026 Ian Karlsson
//! \file
//! A text field's text laid out in its content box: one line for an input, a textarea's lines broken at its new
//! lines and wrapped at its width (pre-wrap, a word too long broken where it must be). The paint draws it and the
//! input places the caret with it; offsets are bytes of the UTF-8 value, always on a character's start.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "css/style.h"

namespace qdom
{

class Dom;
class Element;
struct LayoutNode;

//! Whether an element is a text field: a textarea, or an input of a text kind.
bool is_text_field(const Element &el);
//! Whether an element is an input of a type (lower case).
bool is_input_type(const Element &el, std::string_view type);
//! A field's value as script reads it (a textarea's text until it is edited, an input's value attribute until then).
std::string field_value(const Element &el);

//! The start of the character before an offset, and after it.
size_t prev_char(std::string_view s, size_t i);
size_t next_char(std::string_view s, size_t i);
//! An offset moved back onto a character's start.
size_t char_start(std::string_view s, size_t i);
//! Where Ctrl+Left and Ctrl+Right go: the start of the word before, the end of the word after.
size_t prev_word(std::string_view s, size_t i);
size_t next_word(std::string_view s, size_t i);
//! The word around an offset (its characters, or the run of spaces or of punctuation there).
void word_at(std::string_view s, size_t i, size_t &from, size_t &to);
//! UTF-16 units before a byte offset, and the byte offset after a number of them, as scripts count.
size_t utf16_index(std::string_view s, size_t byte);
size_t byte_index(std::string_view s, size_t utf16);

struct FieldLine
{
	size_t from = 0;  //!< its first byte
	size_t to = 0;    //!< after its last (a new line not counted)
	double top = 0;   //!< relative to the content box
	double base = 0;  //!< its baseline, relative to the content box
	double x = 0;     //!< where it starts in the content box: its text-align's place while it fits
};

class FieldText
{
public:
	//! The value (or what is shown: the placeholder) laid out in the field's box.
	FieldText(Dom &dom, const Element &el, const LayoutNode &n, const std::string &text);

	//! The content box relative to the border box, and its size.
	double cx = 0, cy = 0, cw = 0, ch = 0;
	double line_h = 0;
	double ascent = 0, descent = 0;
	bool multiline = false;
	std::vector<FieldLine> lines;

	//! The line an offset is on (a wrapped line's end is the next one's start).
	size_t line_of(size_t i) const;
	//! The x of a caret before an offset, relative to the content box, not scrolled (its line's x included).
	double x_of(size_t i) const;
	//! The offset nearest a point of the content box, not scrolled.
	size_t index_at(double x, double y) const;
	//! The offset on a line nearest an x.
	size_t index_on(size_t line, double x) const;
	//! The widest line and the lines' height.
	double width() const;
	double height() const { return double(lines.size()) * line_h; }
	//! The width of a part of a line.
	double measure(size_t from, size_t to) const;

private:
	Dom &m_dom;
	const std::string &m_text;
	FaceId m_face = 0;
	double m_size = 0, m_spacing = 0;

	void wrap(size_t from, size_t to, double width, bool wraps);
	//! Each line placed by the field's text-align (centred or at the end while it is narrower than the box).
	void align(TextAlign a);
};

} // namespace qdom
