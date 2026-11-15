// Copyright 2026 Ian Karlsson
//! \file
//! Interned strings: tag names, attribute names, ids and classes compare as integers.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qdom
{

//! An interned string; 0 is none.
using Name = uint32_t;

//! The names the library itself looks for, interned first so that their numbers are constants.
#define QDOM_NAMES(X) \
	X(html) X(head) X(body) X(div) X(span) X(style) X(script) X(title) X(input) X(select) X(option) X(button) \
	X(textarea) X(canvas) X(svg) X(img) X(dialog) X(br) X(label) X(a) X(table) X(tr) X(td) X(th) X(tbody) X(thead) \
	X(tfoot) X(id) X(class_) X(type) X(value) X(disabled) X(checked) X(selected) X(open) X(hidden) X(width) \
	X(height) X(size) X(maxlength) X(placeholder) X(min) X(max) X(tabindex) X(viewBox) X(src) X(spellcheck) X(text) \
	X(range) X(number) X(rows) X(cols) X(after) X(before) X(backdrop) \
	X(name) X(multiple) X(readonly) X(optgroup) X(fill) X(stroke) X(for_) X(step)

//! The predefined names.
namespace n
{
enum : Name
{
	none = 0,
#define QDOM_NAME_ENUM(x) x,
	QDOM_NAMES(QDOM_NAME_ENUM)
#undef QDOM_NAME_ENUM
	count_
};
}

//! A table of interned strings. Names are never forgotten.
class Names
{
public:
	Names();

	//! The name of a string, interned if it is new.
	Name get(std::string_view s);
	//! The name of a string, or 0 when it was never interned.
	Name find(std::string_view s) const;
	//! The string of a name.
	std::string_view str(Name n) const { return m_strings[n]; }

private:
	struct Hash
	{
		using is_transparent = void;
		size_t operator()(std::string_view s) const { return std::hash<std::string_view>()(s); }
	};
	std::deque<std::string> m_strings;
	std::unordered_map<std::string_view, Name, Hash, std::equal_to<>> m_map;
};

} // namespace qdom
