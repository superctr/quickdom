// Copyright 2026 Ian Karlsson
//! \file
//! CSS values as the cascade computes them: lengths (pixels plus a share of a basis, or a keyword), colours, and the
//! tokens of a declared value.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qdom
{

//! A colour: red, green and blue from 0 to 255, alpha from 0 to 1.
struct Color
{
	float r = 0, g = 0, b = 0, a = 0;

	bool visible() const { return a > 0; }
	bool operator==(const Color &) const = default;
};

//! What lengths in a declared value are relative to: the font's size, the root's, the viewport.
struct LengthEnv
{
	double em = 16, rem = 16, vw = 0, vh = 0;
};

//! A length as computed: px plus pct times a basis (a percentage, a calc() with one), or a keyword.
struct Length
{
	enum class Kind : uint8_t
	{
		Value, Auto, None, Normal, FitContent, MinContent, MaxContent, Content
	};
	Kind kind = Kind::Value;
	double px = 0;
	double pct = 0;  //!< a fraction of the basis
	//! A percentage was given, 0% too: the length needs a basis (0% of an indefinite size is not 0px).
	bool of_basis = false;

	static Length of(double px) { return { Kind::Value, px, 0 }; }
	static Length percent(double p) { return { Kind::Value, 0, p / 100, true }; }
	static Length keyword(Kind k) { return { k, 0, 0 }; }

	bool is_value() const { return kind == Kind::Value; }
	bool is_auto() const { return kind == Kind::Auto; }
	bool has_percent() const { return kind == Kind::Value && (pct != 0 || of_basis); }
	//! Pixels against a basis; none for a keyword, or for a share of a basis not known.
	std::optional<double> resolve(std::optional<double> basis) const
	{
		if (kind != Kind::Value)
			return std::nullopt;
		if (pct == 0 && !of_basis)
			return px;
		if (!basis)
			return std::nullopt;
		return px + pct * *basis;
	}
	bool operator==(const Length &) const = default;
};

//! A token of a declared value.
struct Token
{
	enum class Type : uint8_t
	{
		Number, Ident, Function, Hash, String, Comma, Slash, Op
	};
	Type type;
	double number = 0;
	std::string text;  //!< the unit (lower case), the identifier, the function's name, the hash, the string, the op
	std::string args;  //!< a function's arguments as written
};

//! A value's tokens: numbers with their units, words, functions with their arguments, commas, slashes, strings.
std::vector<Token> tokenize(std::string_view s);
//! A value's parts at its top-level commas, trimmed.
std::vector<std::string_view> split_commas(std::string_view s);
//! A value's parts at its top-level spaces (quotes and brackets kept whole).
std::vector<std::string_view> split_words(std::string_view s);
std::string_view trim(std::string_view s);
std::string lower(std::string_view s);

//! A length or a length keyword ("auto", "none", "normal", "fit-content", "min-content", "max-content",
//! "content"); none when the value is not one.
std::optional<Length> parse_length(std::string_view s, const LengthEnv &env);
//! A colour, with "currentcolor" as none and *current set; none when the value is not one.
std::optional<Color> parse_color(std::string_view s, bool *current = nullptr);
//! A plain number ("1.5"); none otherwise.
std::optional<double> parse_number(std::string_view s);

//! A length as CSS writes it computed ("12px", "50%", "auto").
std::string serialize(const Length &l);
std::string serialize(const Color &c);
std::string format_number(double v);

} // namespace qdom
