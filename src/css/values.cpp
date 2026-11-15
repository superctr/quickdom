// Copyright 2026 Ian Karlsson
#include "css/values.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace qdom
{

namespace
{

bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

bool is_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_ident(char c)
{
	return is_alpha(c) || is_digit(c) || c == '_' || c == '-' || (unsigned char)c >= 0x80;
}

//! The length of a number at the start of s ([+-] digits [. digits] [e[+-]digits]), 0 if none.
size_t number_length(std::string_view s)
{
	size_t i = 0;
	if (i < s.size() && (s[i] == '+' || s[i] == '-'))
		i++;
	const size_t start = i;
	while (i < s.size() && is_digit(s[i]))
		i++;
	bool digits = i > start;
	if (i < s.size() && s[i] == '.' && (digits || (i + 1 < s.size() && is_digit(s[i + 1]))))
	{
		i++;
		while (i < s.size() && is_digit(s[i]))
		{
			i++;
			digits = true;
		}
	}
	if (!digits)
		return 0;
	if (i + 1 < s.size() && (s[i] == 'e' || s[i] == 'E'))
	{
		size_t j = i + 1;
		if (j < s.size() && (s[j] == '+' || s[j] == '-'))
			j++;
		if (j < s.size() && is_digit(s[j]))
		{
			while (j < s.size() && is_digit(s[j]))
				j++;
			i = j;
		}
	}
	return i;
}

double to_double(std::string_view s)
{
	if (!s.empty() && s[0] == '+')
		s.remove_prefix(1);
	double v = 0;
	std::from_chars(s.data(), s.data() + s.size(), v);
	return v;
}

//! A length's term: a number with a unit, a function, a keyword.
std::optional<Length> term(const Token &t, const LengthEnv &env);

//! Terms joined by + - * /, as a linear length.
std::optional<Length> sum(const std::vector<Token> &ts, const LengthEnv &env)
{
	std::optional<Length> acc;
	char op = '+';
	for (const Token &t : ts)
	{
		if (t.type == Token::Type::Op)
		{
			op = t.text[0];
			continue;
		}
		if (t.type == Token::Type::Slash)
		{
			op = '/';
			continue;
		}
		std::optional<Length> x = term(t, env);
		if (!x || !x->is_value())
			return std::nullopt;
		if (!acc)
		{
			acc = op == '-' ? Length { Length::Kind::Value, -x->px, -x->pct, x->of_basis } : *x;
			continue;
		}
		switch (op)
		{
		case '+':
			acc->px += x->px;
			acc->pct += x->pct;
			acc->of_basis = acc->of_basis || x->of_basis;
			break;
		case '-':
			acc->px -= x->px;
			acc->pct -= x->pct;
			acc->of_basis = acc->of_basis || x->of_basis;
			break;
		case '*':
		{
			// one side is a plain number
			const double k = x->pct != 0 ? acc->px : x->px;
			const Length base = x->pct != 0 ? *x : *acc;
			acc = Length { Length::Kind::Value, base.px * k, base.pct * k, base.of_basis };
			break;
		}
		case '/':
			if (x->px == 0)
				return std::nullopt;
			acc->px /= x->px;
			acc->pct /= x->px;
			break;
		default:
			return std::nullopt;
		}
	}
	return acc;
}

std::optional<Length> term(const Token &t, const LengthEnv &env)
{
	switch (t.type)
	{
	case Token::Type::Number:
	{
		const double v = t.number;
		const std::string &u = t.text;
		if (u.empty() || u == "px")
			return Length::of(v);
		if (u == "%")
			return Length::percent(v);
		if (u == "em")
			return Length::of(v * env.em);
		if (u == "rem")
			return Length::of(v * env.rem);
		if (u == "vw")
			return Length::of(v * env.vw / 100);
		if (u == "vh")
			return Length::of(v * env.vh / 100);
		if (u == "vmin")
			return Length::of(v * std::min(env.vw, env.vh) / 100);
		if (u == "vmax")
			return Length::of(v * std::max(env.vw, env.vh) / 100);
		if (u == "pt")
			return Length::of(v * 4 / 3);
		if (u == "pc")
			return Length::of(v * 16);
		if (u == "in")
			return Length::of(v * 96);
		if (u == "cm")
			return Length::of(v * 96 / 2.54);
		if (u == "mm")
			return Length::of(v * 96 / 25.4);
		if (u == "ch" || u == "ex")
			return Length::of(v * env.em / 2);
		return std::nullopt;
	}
	case Token::Type::Function:
	{
		if (t.text == "calc")
			return parse_length(t.args, env);
		if (t.text == "min" || t.text == "max" || t.text == "clamp")
		{
			std::vector<double> xs;
			for (std::string_view a : split_commas(t.args))
			{
				std::optional<Length> l = parse_length(a, env);
				// a share of a basis inside min() or max() is not kept (none of the stylesheets need it)
				if (!l || !l->is_value() || l->pct != 0)
					return std::nullopt;
				xs.push_back(l->px);
			}
			if (xs.empty())
				return std::nullopt;
			if (t.text == "clamp")
				return xs.size() == 3 ? std::optional(Length::of(std::max(xs[0], std::min(xs[1], xs[2])))) : std::nullopt;
			return Length::of(t.text == "min" ? *std::min_element(xs.begin(), xs.end()) : *std::max_element(xs.begin(), xs.end()));
		}
		return std::nullopt;
	}
	case Token::Type::Ident:
	{
		const std::string w = lower(t.text);
		if (w == "auto")
			return Length::keyword(Length::Kind::Auto);
		if (w == "none")
			return Length::keyword(Length::Kind::None);
		if (w == "normal")
			return Length::keyword(Length::Kind::Normal);
		if (w == "fit-content" || w == "-moz-fit-content")
			return Length::keyword(Length::Kind::FitContent);
		if (w == "min-content")
			return Length::keyword(Length::Kind::MinContent);
		if (w == "max-content")
			return Length::keyword(Length::Kind::MaxContent);
		if (w == "content")
			return Length::keyword(Length::Kind::Content);
		return std::nullopt;
	}
	default:
		return std::nullopt;
	}
}

struct NamedColor
{
	const char *name;
	uint32_t rgb;
};

// CSS's named colours
const NamedColor NAMED[] = {
	{ "aliceblue", 0xf0f8ff }, { "antiquewhite", 0xfaebd7 }, { "aqua", 0x00ffff }, { "aquamarine", 0x7fffd4 },
	{ "azure", 0xf0ffff }, { "beige", 0xf5f5dc }, { "bisque", 0xffe4c4 }, { "black", 0x000000 },
	{ "blanchedalmond", 0xffebcd }, { "blue", 0x0000ff }, { "blueviolet", 0x8a2be2 }, { "brown", 0xa52a2a },
	{ "burlywood", 0xdeb887 }, { "cadetblue", 0x5f9ea0 }, { "chartreuse", 0x7fff00 }, { "chocolate", 0xd2691e },
	{ "coral", 0xff7f50 }, { "cornflowerblue", 0x6495ed }, { "cornsilk", 0xfff8dc }, { "crimson", 0xdc143c },
	{ "cyan", 0x00ffff }, { "darkblue", 0x00008b }, { "darkcyan", 0x008b8b }, { "darkgoldenrod", 0xb8860b },
	{ "darkgray", 0xa9a9a9 }, { "darkgreen", 0x006400 }, { "darkgrey", 0xa9a9a9 }, { "darkkhaki", 0xbdb76b },
	{ "darkmagenta", 0x8b008b }, { "darkolivegreen", 0x556b2f }, { "darkorange", 0xff8c00 }, { "darkorchid", 0x9932cc },
	{ "darkred", 0x8b0000 }, { "darksalmon", 0xe9967a }, { "darkseagreen", 0x8fbc8f }, { "darkslateblue", 0x483d8b },
	{ "darkslategray", 0x2f4f4f }, { "darkslategrey", 0x2f4f4f }, { "darkturquoise", 0x00ced1 }, { "darkviolet", 0x9400d3 },
	{ "deeppink", 0xff1493 }, { "deepskyblue", 0x00bfff }, { "dimgray", 0x696969 }, { "dimgrey", 0x696969 },
	{ "dodgerblue", 0x1e90ff }, { "firebrick", 0xb22222 }, { "floralwhite", 0xfffaf0 }, { "forestgreen", 0x228b22 },
	{ "fuchsia", 0xff00ff }, { "gainsboro", 0xdcdcdc }, { "ghostwhite", 0xf8f8ff }, { "gold", 0xffd700 },
	{ "goldenrod", 0xdaa520 }, { "gray", 0x808080 }, { "green", 0x008000 }, { "greenyellow", 0xadff2f },
	{ "grey", 0x808080 }, { "honeydew", 0xf0fff0 }, { "hotpink", 0xff69b4 }, { "indianred", 0xcd5c5c },
	{ "indigo", 0x4b0082 }, { "ivory", 0xfffff0 }, { "khaki", 0xf0e68c }, { "lavender", 0xe6e6fa },
	{ "lavenderblush", 0xfff0f5 }, { "lawngreen", 0x7cfc00 }, { "lemonchiffon", 0xfffacd }, { "lightblue", 0xadd8e6 },
	{ "lightcoral", 0xf08080 }, { "lightcyan", 0xe0ffff }, { "lightgoldenrodyellow", 0xfafad2 }, { "lightgray", 0xd3d3d3 },
	{ "lightgreen", 0x90ee90 }, { "lightgrey", 0xd3d3d3 }, { "lightpink", 0xffb6c1 }, { "lightsalmon", 0xffa07a },
	{ "lightseagreen", 0x20b2aa }, { "lightskyblue", 0x87cefa }, { "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 },
	{ "lightsteelblue", 0xb0c4de }, { "lightyellow", 0xffffe0 }, { "lime", 0x00ff00 }, { "limegreen", 0x32cd32 },
	{ "linen", 0xfaf0e6 }, { "magenta", 0xff00ff }, { "maroon", 0x800000 }, { "mediumaquamarine", 0x66cdaa },
	{ "mediumblue", 0x0000cd }, { "mediumorchid", 0xba55d3 }, { "mediumpurple", 0x9370db }, { "mediumseagreen", 0x3cb371 },
	{ "mediumslateblue", 0x7b68ee }, { "mediumspringgreen", 0x00fa9a }, { "mediumturquoise", 0x48d1cc },
	{ "mediumvioletred", 0xc71585 }, { "midnightblue", 0x191970 }, { "mintcream", 0xf5fffa }, { "mistyrose", 0xffe4e1 },
	{ "moccasin", 0xffe4b5 }, { "navajowhite", 0xffdead }, { "navy", 0x000080 }, { "oldlace", 0xfdf5e6 },
	{ "olive", 0x808000 }, { "olivedrab", 0x6b8e23 }, { "orange", 0xffa500 }, { "orangered", 0xff4500 },
	{ "orchid", 0xda70d6 }, { "palegoldenrod", 0xeee8aa }, { "palegreen", 0x98fb98 }, { "paleturquoise", 0xafeeee },
	{ "palevioletred", 0xdb7093 }, { "papayawhip", 0xffefd5 }, { "peachpuff", 0xffdab9 }, { "peru", 0xcd853f },
	{ "pink", 0xffc0cb }, { "plum", 0xdda0dd }, { "powderblue", 0xb0e0e6 }, { "purple", 0x800080 },
	{ "rebeccapurple", 0x663399 }, { "red", 0xff0000 }, { "rosybrown", 0xbc8f8f }, { "royalblue", 0x4169e1 },
	{ "saddlebrown", 0x8b4513 }, { "salmon", 0xfa8072 }, { "sandybrown", 0xf4a460 }, { "seagreen", 0x2e8b57 },
	{ "seashell", 0xfff5ee }, { "sienna", 0xa0522d }, { "silver", 0xc0c0c0 }, { "skyblue", 0x87ceeb },
	{ "slateblue", 0x6a5acd }, { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "snow", 0xfffafa },
	{ "springgreen", 0x00ff7f }, { "steelblue", 0x4682b4 }, { "tan", 0xd2b48c }, { "teal", 0x008080 },
	{ "thistle", 0xd8bfd8 }, { "tomato", 0xff6347 }, { "turquoise", 0x40e0d0 }, { "violet", 0xee82ee },
	{ "wheat", 0xf5deb3 }, { "white", 0xffffff }, { "whitesmoke", 0xf5f5f5 }, { "yellow", 0xffff00 },
	{ "yellowgreen", 0x9acd32 },
};

int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

} // namespace

std::string_view trim(std::string_view s)
{
	while (!s.empty() && is_space(s.front()))
		s.remove_prefix(1);
	while (!s.empty() && is_space(s.back()))
		s.remove_suffix(1);
	return s;
}

std::string lower(std::string_view s)
{
	std::string out(s);
	for (char &c : out)
		if (c >= 'A' && c <= 'Z')
			c = char(c - 'A' + 'a');
	return out;
}

std::vector<Token> tokenize(std::string_view s)
{
	std::vector<Token> out;
	size_t i = 0;
	const size_t n = s.size();
	while (i < n)
	{
		const char c = s[i];
		if (is_space(c))
		{
			i++;
			continue;
		}
		const bool spaced = i + 1 >= n || is_space(s[i + 1]);
		if (c == ',')
		{
			out.push_back({ Token::Type::Comma, 0, ",", {} });
			i++;
			continue;
		}
		if (c == '/')
		{
			out.push_back({ Token::Type::Slash, 0, "/", {} });
			i++;
			continue;
		}
		if (c == '*' || ((c == '+' || c == '-') && spaced))
		{
			out.push_back({ Token::Type::Op, 0, std::string(1, c), {} });
			i++;
			continue;
		}
		if (c == '"' || c == '\'')
		{
			size_t j = i + 1;
			while (j < n && s[j] != c)
				j++;
			out.push_back({ Token::Type::String, 0, std::string(s.substr(i + 1, j - i - 1)), {} });
			i = j + 1;
			continue;
		}
		if (c == '#')
		{
			size_t j = i + 1;
			while (j < n && is_ident(s[j]))
				j++;
			out.push_back({ Token::Type::Hash, 0, std::string(s.substr(i + 1, j - i - 1)), {} });
			i = j;
			continue;
		}
		const size_t num = number_length(s.substr(i));
		if (num && !(c == '-' && i + 1 < n && (is_alpha(s[i + 1]) || s[i + 1] == '_' || s[i + 1] == '-')))
		{
			Token t { Token::Type::Number, to_double(s.substr(i, num)), {}, {} };
			size_t j = i + num;
			if (j < n && s[j] == '%')
			{
				t.text = "%";
				j++;
			}
			else
			{
				const size_t u = j;
				while (j < n && is_alpha(s[j]))
					j++;
				t.text = lower(s.substr(u, j - u));
			}
			out.push_back(std::move(t));
			i = j;
			continue;
		}
		size_t j = i;
		while (j < n && !is_space(s[j]) && !strchr(",/()\"'*", s[j]))
			j++;
		const std::string_view word = s.substr(i, j - i);
		if (j < n && s[j] == '(')
		{
			int depth = 1;
			size_t k = j + 1;
			while (k < n && depth)
			{
				if (s[k] == '(')
					depth++;
				else if (s[k] == ')')
					depth--;
				k++;
			}
			const size_t end = depth ? k : k - 1;
			out.push_back({ Token::Type::Function, 0, lower(word), std::string(s.substr(j + 1, end - j - 1)) });
			i = k;
			continue;
		}
		if (word.empty())
		{
			i++;
			continue;
		}
		out.push_back({ Token::Type::Ident, 0, std::string(word), {} });
		i = j;
	}
	return out;
}

std::vector<std::string_view> split_commas(std::string_view s)
{
	std::vector<std::string_view> out;
	int depth = 0;
	size_t from = 0;
	char quote = 0;
	for (size_t i = 0; i < s.size(); i++)
	{
		const char c = s[i];
		if (quote)
		{
			if (c == quote)
				quote = 0;
			continue;
		}
		if (c == '"' || c == '\'')
			quote = c;
		else if (c == '(')
			depth++;
		else if (c == ')')
			depth--;
		else if (c == ',' && !depth)
		{
			out.push_back(trim(s.substr(from, i - from)));
			from = i + 1;
		}
	}
	out.push_back(trim(s.substr(from)));
	return out;
}

std::vector<std::string_view> split_words(std::string_view s)
{
	std::vector<std::string_view> out;
	s = trim(s);
	int depth = 0;
	char quote = 0;
	size_t from = std::string_view::npos;
	for (size_t i = 0; i < s.size(); i++)
	{
		const char c = s[i];
		if (quote)
		{
			if (c == quote)
				quote = 0;
			continue;
		}
		if (c == '"' || c == '\'')
			quote = c;
		if (c == '(')
			depth++;
		else if (c == ')')
			depth--;
		if (is_space(c) && !depth)
		{
			if (from != std::string_view::npos)
				out.push_back(s.substr(from, i - from));
			from = std::string_view::npos;
		}
		else if (from == std::string_view::npos)
			from = i;
	}
	if (from != std::string_view::npos)
		out.push_back(s.substr(from));
	return out;
}

std::optional<Length> parse_length(std::string_view s, const LengthEnv &env)
{
	s = trim(s);
	if (s.empty())
		return std::nullopt;
	const std::vector<Token> ts = tokenize(s);
	if (ts.size() == 1)
	{
		// a number without a unit is a length only when it is 0 (calc() takes them as factors)
		if (ts[0].type == Token::Type::Number && ts[0].text.empty() && ts[0].number != 0)
			return std::nullopt;
		return term(ts[0], env);
	}
	return sum(ts, env);
}

std::optional<double> parse_number(std::string_view s)
{
	s = trim(s);
	const size_t n = number_length(s);
	if (!n || n != s.size())
		return std::nullopt;
	return to_double(s);
}

std::optional<Color> parse_color(std::string_view s, bool *current)
{
	if (current)
		*current = false;
	const std::string v = lower(trim(s));
	if (v.empty())
		return std::nullopt;
	if (v == "transparent")
		return Color { 0, 0, 0, 0 };
	if (v == "currentcolor")
	{
		if (current)
			*current = true;
		return std::nullopt;
	}
	if (v[0] == '#')
	{
		std::string h = v.substr(1);
		for (char c : h)
			if (hex_digit(c) < 0)
				return std::nullopt;
		if (h.size() == 3 || h.size() == 4)
		{
			std::string d;
			for (char c : h)
				d += { c, c };
			h = d;
		}
		if (h.size() != 6 && h.size() != 8)
			return std::nullopt;
		auto byte = [&](size_t i) { return float(hex_digit(h[i]) * 16 + hex_digit(h[i + 1])); };
		return Color { byte(0), byte(2), byte(4), h.size() == 8 ? byte(6) / 255 : 1 };
	}
	const size_t open = v.find('(');
	if (open != std::string::npos && v.back() == ')')
	{
		const std::string_view name = std::string_view(v).substr(0, open);
		if (name != "rgb" && name != "rgba")
			return std::nullopt;
		std::vector<double> p;
		std::vector<bool> pct;
		std::string_view args = std::string_view(v).substr(open + 1, v.size() - open - 2);
		size_t i = 0;
		while (i < args.size())
		{
			if (is_space(args[i]) || args[i] == ',' || args[i] == '/')
			{
				i++;
				continue;
			}
			const size_t len = number_length(args.substr(i));
			if (!len)
				return std::nullopt;
			p.push_back(to_double(args.substr(i, len)));
			i += len;
			pct.push_back(i < args.size() && args[i] == '%');
			if (pct.back())
				i++;
		}
		if (p.size() < 3)
			return std::nullopt;
		Color c;
		float *ch[3] = { &c.r, &c.g, &c.b };
		for (int k = 0; k < 3; k++)
			*ch[k] = float(std::clamp(pct[k] ? p[k] * 2.55 : p[k], 0.0, 255.0));
		c.a = p.size() > 3 ? float(std::clamp(pct[3] ? p[3] / 100 : p[3], 0.0, 1.0)) : 1.0f;
		return c;
	}
	for (const NamedColor &nc : NAMED)
		if (v == nc.name)
			return Color { float(nc.rgb >> 16), float((nc.rgb >> 8) & 255), float(nc.rgb & 255), 1 };
	return std::nullopt;
}

std::string format_number(double v)
{
	if (v == 0)
		return "0";
	if (std::floor(v) == v && std::fabs(v) < 1e15)
		return std::to_string((long long)v);
	char buf[64];
	auto r = std::to_chars(buf, buf + sizeof(buf), v);
	return std::string(buf, r.ptr);
}

std::string serialize(const Length &l)
{
	switch (l.kind)
	{
	case Length::Kind::Auto: return "auto";
	case Length::Kind::None: return "none";
	case Length::Kind::Normal: return "normal";
	case Length::Kind::FitContent: return "fit-content";
	case Length::Kind::MinContent: return "min-content";
	case Length::Kind::MaxContent: return "max-content";
	case Length::Kind::Content: return "content";
	case Length::Kind::Value: break;
	}
	if (l.pct == 0 && !l.of_basis)
		return format_number(l.px) + "px";
	if (l.px == 0)
		return format_number(l.pct * 100) + "%";
	return "calc(" + format_number(l.pct * 100) + "% + " + format_number(l.px) + "px)";
}

std::string serialize(const Color &c)
{
	auto ch = [](float v) { return std::to_string(int(std::lround(v))); };
	if (c.a >= 1)
		return "rgb(" + ch(c.r) + ", " + ch(c.g) + ", " + ch(c.b) + ")";
	return "rgba(" + ch(c.r) + ", " + ch(c.g) + ", " + ch(c.b) + ", " + format_number(std::round(double(c.a) * 1000) / 1000) + ")";
}

} // namespace qdom
