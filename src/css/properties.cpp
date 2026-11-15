// Copyright 2026 Ian Karlsson
#include "css/properties.h"

#include <array>
#include <unordered_map>

#include "css/values.h"

namespace qdom
{

namespace
{

struct PropInfo
{
	const char *name;
	bool inherited;
};

const PropInfo PROPS[] = {
#define QDOM_PROP_INFO(id, name, inherited) { name, inherited },
	QDOM_PROPERTIES(QDOM_PROP_INFO)
#undef QDOM_PROP_INFO
};

const char *const SHORTHANDS[] = {
	"margin", "padding", "inset", "border", "border-top", "border-right", "border-bottom", "border-left", "border-width",
	"border-style", "border-color", "border-radius", "background", "outline", "overflow", "gap", "grid-gap", "flex",
	"flex-flow", "font", "inset-inline", "inset-block", "padding-inline", "padding-block", "margin-inline",
	"margin-block", "place-items", "place-content", "place-self", "grid-template", "text-decoration",
};

const std::unordered_map<std::string_view, Prop> &prop_map()
{
	static const std::unordered_map<std::string_view, Prop> map = [] {
		std::unordered_map<std::string_view, Prop> m;
		for (size_t i = 0; i < PROP_COUNT; i++)
			m.emplace(PROPS[i].name, Prop(i));
		return m;
	}();
	return map;
}

const char *const SIDES[] = { "top", "right", "bottom", "left" };
const char *const CORNERS[] = { "top-left", "top-right", "bottom-right", "bottom-left" };

//! One to four values as top, right, bottom, left.
std::array<std::string, 4> four(std::string_view v)
{
	const std::vector<std::string_view> w = split_words(v);
	std::array<std::string, 4> out;
	if (w.empty())
		return out;
	out[0] = w[0];
	out[1] = w.size() > 1 ? w[1] : w[0];
	out[2] = w.size() > 2 ? w[2] : w[0];
	out[3] = w.size() > 3 ? w[3] : out[1];
	return out;
}

bool is_border_style(std::string_view w)
{
	static const char *const styles[] = { "none", "solid", "dashed", "dotted", "inset", "outset", "double", "groove",
		"ridge", "hidden" };
	const std::string l = lower(w);
	for (const char *s : styles)
		if (l == s)
			return true;
	return false;
}

bool is_color(std::string_view w)
{
	bool current = false;
	return parse_color(w, &current) || current || lower(w) == "none";
}

void add(std::vector<std::pair<Prop, std::string>> &out, std::string_view name, std::string_view value)
{
	if (auto p = find_prop(name))
		out.emplace_back(*p, std::string(value));
}

//! A border side, or an outline: width, style and colour in any order.
void line(std::vector<std::pair<Prop, std::string>> &out, const std::string &prefix, std::string_view value)
{
	std::string width = "medium", style = "none", color = "currentcolor";
	if (value == "none" || value == "0")
		width = "0";
	else
		for (std::string_view w : split_words(value))
		{
			if (is_border_style(w))
				style = w;
			else if (is_color(w))
				color = w;
			else
				width = w;
		}
	add(out, prefix + "-width", width);
	add(out, prefix + "-style", style);
	add(out, prefix + "-color", color);
}

bool is_number_word(std::string_view w)
{
	return parse_number(w).has_value();
}

} // namespace

const char *prop_name(Prop p)
{
	return PROPS[size_t(p)].name;
}

bool prop_inherited(Prop p)
{
	return PROPS[size_t(p)].inherited;
}

std::string dashed(std::string_view name)
{
	if (name.starts_with("--"))
		return std::string(name);
	std::string out;
	for (char c : name)
	{
		if (c >= 'A' && c <= 'Z')
		{
			out += '-';
			out += char(c - 'A' + 'a');
		}
		else
			out += c;
	}
	return out;
}

std::string camel(std::string_view name)
{
	std::string out;
	bool up = false;
	for (char c : name)
	{
		if (c == '-')
		{
			up = true;
			continue;
		}
		out += up && c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
		up = false;
	}
	return out;
}

std::optional<Prop> find_prop(std::string_view name)
{
	const auto &m = prop_map();
	auto it = m.find(name);
	if (it != m.end())
		return it->second;
	if (name.find('-') == std::string_view::npos)
	{
		const std::string d = dashed(name);
		it = m.find(d);
		if (it != m.end())
			return it->second;
	}
	return std::nullopt;
}

bool is_shorthand(std::string_view name)
{
	for (const char *s : SHORTHANDS)
		if (name == s)
			return true;
	return false;
}

const std::vector<std::string> &style_property_names()
{
	static const std::vector<std::string> names = [] {
		std::vector<std::string> v;
		for (const PropInfo &p : PROPS)
			v.emplace_back(p.name);
		for (const char *s : SHORTHANDS)
			v.emplace_back(s);
		return v;
	}();
	return names;
}

bool expand(std::string_view name, std::string_view value, std::vector<std::pair<Prop, std::string>> &out)
{
	if (auto p = find_prop(name))
	{
		out.emplace_back(*p, std::string(value));
		return true;
	}
	if (!is_shorthand(name))
		return false;
	const size_t first = out.size();
	const std::string v = lower(trim(value));
	const bool wide = v == "inherit" || v == "initial" || v == "unset";
	if (name == "margin" || name == "padding")
	{
		const auto f = four(value);
		for (int i = 0; i < 4; i++)
			add(out, std::string(name) + "-" + SIDES[i], f[i]);
	}
	else if (name == "inset")
	{
		const auto f = four(value);
		for (int i = 0; i < 4; i++)
			add(out, SIDES[i], f[i]);
	}
	else if (name == "border-width" || name == "border-style" || name == "border-color")
	{
		const auto f = four(value);
		const std::string part(name.substr(7));
		for (int i = 0; i < 4; i++)
			add(out, std::string("border-") + SIDES[i] + "-" + part, f[i]);
	}
	else if (name == "border")
	{
		for (const char *s : SIDES)
			line(out, std::string("border-") + s, value);
	}
	else if (name == "border-top" || name == "border-right" || name == "border-bottom" || name == "border-left")
	{
		line(out, std::string(name), value);
	}
	else if (name == "outline")
	{
		line(out, "outline", value);
	}
	else if (name == "text-decoration")
	{
		// its lines and its colour; its style and thickness are not kept (every line is drawn solid, from the font)
		std::string lines, color = "currentcolor";
		for (std::string_view w : split_words(value))
		{
			const std::string k = lower(w);
			if (k == "underline" || k == "overline" || k == "line-through" || k == "blink")
				lines += (lines.empty() ? "" : " ") + k;
			else if (k != "none" && is_color(w))
				color = w;
		}
		add(out, "text-decoration-line", wide ? v : lines.empty() ? "none" : lines);
		add(out, "text-decoration-color", wide ? v : color);
	}
	else if (name == "border-radius")
	{
		const std::string_view head = value.substr(0, value.find('/'));
		const auto f = four(head);
		for (int i = 0; i < 4; i++)
			add(out, std::string("border-") + CORNERS[i] + "-radius", f[i]);
	}
	else if (name == "background")
	{
		std::string color = "transparent", image = "none";
		for (std::string_view w : split_words(value))
		{
			const std::string l = lower(w);
			if (l.starts_with("linear-gradient(") || l.starts_with("radial-gradient(") || l.starts_with("url("))
				image = w;
			else if (is_color(w) && l != "none")
				color = w;
		}
		add(out, "background-color", color);
		add(out, "background-image", image);
	}
	else if (name == "overflow")
	{
		const auto w = split_words(value);
		if (w.empty())
			return true;
		add(out, "overflow-x", w[0]);
		add(out, "overflow-y", w.size() > 1 ? w[1] : w[0]);
	}
	else if (name == "gap" || name == "grid-gap")
	{
		const auto w = split_words(value);
		if (w.empty())
			return true;
		add(out, "row-gap", w[0]);
		add(out, "column-gap", w.size() > 1 ? w[1] : w[0]);
	}
	else if (name == "flex")
	{
		if (v == "none")
		{
			add(out, "flex-grow", "0");
			add(out, "flex-shrink", "0");
			add(out, "flex-basis", "auto");
		}
		else if (v == "auto")
		{
			add(out, "flex-grow", "1");
			add(out, "flex-shrink", "1");
			add(out, "flex-basis", "auto");
		}
		else if (!wide)
		{
			std::vector<std::string_view> nums;
			std::string_view basis;
			for (std::string_view w : split_words(value))
			{
				if (is_number_word(w) && nums.size() < 2)
					nums.push_back(w);
				else
					basis = w;
			}
			add(out, "flex-grow", nums.size() > 0 ? nums[0] : "1");
			add(out, "flex-shrink", nums.size() > 1 ? nums[1] : "1");
			add(out, "flex-basis", basis.empty() ? "0%" : basis);
		}
	}
	else if (name == "flex-flow")
	{
		for (std::string_view w : split_words(value))
			add(out, lower(w).find("wrap") != std::string::npos ? "flex-wrap" : "flex-direction", w);
	}
	else if (name == "font")
	{
		if (!wide)
		{
			// [style] [variant] [weight] size[/line-height] family
			const auto w = split_words(value);
			std::string style = "normal", weight = "normal", height = "normal";
			size_t k = 0;
			for (; k < w.size(); k++)
			{
				const std::string l = lower(w[k]);
				if (l == "italic" || l == "oblique")
					style = l;
				else if (l == "bold" || l == "bolder" || l == "lighter" || (l.size() == 3 && l[1] == '0' && l[2] == '0'))
					weight = l;
				else if (l == "normal" || l == "small-caps")
					continue;
				else
					break;
			}
			if (k >= w.size())
				return true;
			std::string_view size = w[k];
			const size_t slash = size.find('/');
			if (slash != std::string_view::npos)
			{
				height = size.substr(slash + 1);
				size = size.substr(0, slash);
			}
			std::string family;
			for (size_t i = k + 1; i < w.size(); i++)
			{
				if (!family.empty())
					family += ' ';
				family += w[i];
			}
			add(out, "font-style", style);
			add(out, "font-weight", weight);
			add(out, "line-height", height);
			add(out, "font-size", size);
			add(out, "font-family", family);
		}
	}
	else if (name == "inset-inline" || name == "inset-block" || name == "padding-inline" || name == "padding-block"
		|| name == "margin-inline" || name == "margin-block")
	{
		const auto w = split_words(value);
		if (w.empty())
			return true;
		const bool inl = name.ends_with("inline");
		const std::string base = name.starts_with("inset") ? "" : std::string(name.substr(0, name.find('-'))) + "-";
		add(out, base + (inl ? "left" : "top"), w[0]);
		add(out, base + (inl ? "right" : "bottom"), w.size() > 1 ? w[1] : w[0]);
	}
	else if (name == "place-items" || name == "place-content" || name == "place-self")
	{
		const auto w = split_words(value);
		if (w.empty())
			return true;
		const std::string part(name.substr(6));
		add(out, "align-" + part, w[0]);
		add(out, "justify-" + part, w.size() > 1 ? w[1] : w[0]);
	}
	else if (name == "grid-template")
	{
		const size_t slash = value.find('/');
		add(out, "grid-template-rows", trim(value.substr(0, slash)));
		if (slash != std::string_view::npos)
			add(out, "grid-template-columns", trim(value.substr(slash + 1)));
	}
	if (wide)
	{
		// a CSS-wide keyword sets every longhand the shorthand has
		std::vector<std::pair<Prop, std::string>> probe;
		const char *sample = name == "font" ? "italic bold 1px/1 x" : name == "flex" ? "1 1 0" : "1px solid red";
		expand(name, sample, probe);
		out.resize(first);
		for (auto &p : probe)
			out.emplace_back(p.first, v);
	}
	return true;
}

} // namespace qdom
