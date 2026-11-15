// Copyright 2026 Ian Karlsson
#include "css/style.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

#include "dom/node.h"
#include "text/fonts.h"

namespace qdom
{

namespace
{

template <class E>
struct Keyword
{
	const char *name;
	E value;
};

template <class E>
bool keyword(std::string_view v, std::initializer_list<Keyword<E>> list, E &out)
{
	const std::string l = lower(trim(v));
	for (const Keyword<E> &k : list)
		if (l == k.name)
		{
			out = k.value;
			return true;
		}
	return false;
}

bool parse_display(std::string_view v, Display &out)
{
	return keyword<Display>(v, {
		{ "inline", Display::Inline }, { "block", Display::Block }, { "inline-block", Display::InlineBlock },
		{ "flex", Display::Flex }, { "inline-flex", Display::InlineFlex }, { "grid", Display::Grid },
		{ "inline-grid", Display::InlineGrid }, { "table", Display::Table }, { "inline-table", Display::InlineTable },
		{ "table-row", Display::TableRow }, { "table-cell", Display::TableCell },
		{ "table-row-group", Display::TableRowGroup }, { "table-header-group", Display::TableHeaderGroup },
		{ "table-footer-group", Display::TableFooterGroup }, { "list-item", Display::ListItem },
		{ "contents", Display::Contents }, { "none", Display::None }, { "flow-root", Display::Block },
		{ "-webkit-box", Display::Flex },
	}, out);
}

bool parse_align(std::string_view v, Align &out)
{
	std::string_view w = trim(v);
	for (const char *prefix : { "safe ", "unsafe ", "first ", "last " })
		if (w.starts_with(prefix))
			w = trim(w.substr(strlen(prefix)));
	return keyword<Align>(w, {
		{ "auto", Align::Auto }, { "normal", Align::Normal }, { "stretch", Align::Stretch }, { "start", Align::Start },
		{ "end", Align::End }, { "flex-start", Align::FlexStart }, { "flex-end", Align::FlexEnd },
		{ "center", Align::Center }, { "baseline", Align::Baseline }, { "space-between", Align::SpaceBetween },
		{ "space-around", Align::SpaceAround }, { "space-evenly", Align::SpaceEvenly },
		{ "self-start", Align::SelfStart }, { "self-end", Align::SelfEnd }, { "left", Align::Left },
		{ "right", Align::Right },
	}, out);
}

bool parse_border_style(std::string_view v, BorderStyle &out)
{
	return keyword<BorderStyle>(v, {
		{ "none", BorderStyle::None }, { "hidden", BorderStyle::Hidden }, { "solid", BorderStyle::Solid },
		{ "dashed", BorderStyle::Dashed }, { "dotted", BorderStyle::Dotted }, { "double", BorderStyle::Double },
		{ "groove", BorderStyle::Groove }, { "ridge", BorderStyle::Ridge }, { "inset", BorderStyle::Inset },
		{ "outset", BorderStyle::Outset }, { "auto", BorderStyle::Solid },
	}, out);
}

bool parse_overflow(std::string_view v, Overflow &out)
{
	return keyword<Overflow>(v, {
		{ "visible", Overflow::Visible }, { "hidden", Overflow::Hidden }, { "clip", Overflow::Clip },
		{ "scroll", Overflow::Scroll }, { "auto", Overflow::Auto }, { "overlay", Overflow::Auto },
	}, out);
}

const char *name_of(Display d)
{
	static const char *const names[] = { "inline", "block", "inline-block", "flex", "inline-flex", "grid",
		"inline-grid", "table", "inline-table", "table-row", "table-cell", "table-row-group", "table-header-group",
		"table-footer-group", "list-item", "contents", "none" };
	return names[size_t(d)];
}

const char *name_of(Align a)
{
	static const char *const names[] = { "auto", "normal", "stretch", "start", "end", "flex-start", "flex-end",
		"center", "baseline", "space-between", "space-around", "space-evenly", "self-start", "self-end", "left",
		"right" };
	return names[size_t(a)];
}

const char *name_of(Overflow o)
{
	static const char *const names[] = { "visible", "hidden", "clip", "scroll", "auto" };
	return names[size_t(o)];
}

const char *name_of(BorderStyle b)
{
	static const char *const names[] = { "none", "hidden", "solid", "dashed", "dotted", "double", "groove", "ridge",
		"inset", "outset" };
	return names[size_t(b)];
}

//! A border's or an outline's width: thin, medium, thick or a length, as Firefox snaps it (down to whole
//! pixels, at least one).
std::optional<double> parse_line_width(std::string_view v, const LengthEnv &env)
{
	const std::string l = lower(trim(v));
	double w;
	if (l == "thin")
		w = 1;
	else if (l == "medium")
		w = 3;
	else if (l == "thick")
		w = 5;
	else
	{
		std::optional<Length> len = parse_length(v, env);
		if (!len || !len->is_value() || len->pct != 0 || len->px < 0)
			return std::nullopt;
		w = len->px;
	}
	return w > 0 ? std::max(1.0, std::floor(w + 1e-6)) : 0.0;
}

//! var(--name, fallback) replaced by the custom properties in force.
std::string substitute(std::string_view value, const CustomProperties *custom, int depth = 0)
{
	if (value.find("var(") == std::string_view::npos || depth > 16)
		return std::string(value);
	std::string out;
	size_t i = 0;
	while (i < value.size())
	{
		const size_t k = value.find("var(", i);
		if (k == std::string_view::npos)
		{
			out += value.substr(i);
			break;
		}
		out += value.substr(i, k - i);
		int level = 1;
		size_t j = k + 4;
		while (j < value.size() && level)
		{
			if (value[j] == '(')
				level++;
			else if (value[j] == ')')
				level--;
			j++;
		}
		const std::string_view inner = value.substr(k + 4, j - k - 5);
		const size_t comma = inner.find(',');
		const std::string name(trim(comma == std::string_view::npos ? inner : inner.substr(0, comma)));
		const std::string_view fallback = comma == std::string_view::npos ? std::string_view() : trim(inner.substr(comma + 1));
		const std::string *found = nullptr;
		if (custom)
		{
			auto it = custom->values.find(name);
			if (it != custom->values.end())
				found = &it->second;
		}
		out += found ? *found : substitute(fallback, custom, depth + 1);
		i = j;
	}
	return out;
}

struct Declared
{
	std::string_view name;
	std::string_view value;
	int priority;  //!< 0 user agent, 1 author, 2 inline, 3 author !important, 4 user agent !important
};

double font_size_keyword(std::string_view v, double parent, bool &ok)
{
	static const Keyword<double> sizes[] = {
		{ "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 },
		{ "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 },
	};
	const std::string l = lower(trim(v));
	ok = true;
	for (const auto &s : sizes)
		if (l == s.name)
			return s.value;
	if (l == "smaller")
		return parent / 1.2;
	if (l == "larger")
		return parent * 1.2;
	ok = false;
	return 0;
}

//! A property copied from one style to another (for inherit, initial and what is inherited).
void copy(Prop p, ComputedStyle &to, const ComputedStyle &from)
{
	switch (p)
	{
	case Prop::Display: to.display = from.display; break;
	case Prop::Position: to.position = from.position; break;
	case Prop::BoxSizing: to.box_sizing = from.box_sizing; break;
	case Prop::Width: to.width = from.width; break;
	case Prop::Height: to.height = from.height; break;
	case Prop::MinWidth: to.min_width = from.min_width; break;
	case Prop::MinHeight: to.min_height = from.min_height; break;
	case Prop::MaxWidth: to.max_width = from.max_width; break;
	case Prop::MaxHeight: to.max_height = from.max_height; break;
	case Prop::MarginTop: case Prop::MarginRight: case Prop::MarginBottom: case Prop::MarginLeft:
	{
		const int s = int(p) - int(Prop::MarginTop);
		to.margin[s] = from.margin[s];
		break;
	}
	case Prop::PaddingTop: case Prop::PaddingRight: case Prop::PaddingBottom: case Prop::PaddingLeft:
	{
		const int s = int(p) - int(Prop::PaddingTop);
		to.padding[s] = from.padding[s];
		break;
	}
	case Prop::BorderTopWidth: case Prop::BorderRightWidth: case Prop::BorderBottomWidth: case Prop::BorderLeftWidth:
	{
		const int s = int(p) - int(Prop::BorderTopWidth);
		to.border_width[s] = from.border_width[s];
		break;
	}
	case Prop::BorderTopStyle: case Prop::BorderRightStyle: case Prop::BorderBottomStyle: case Prop::BorderLeftStyle:
	{
		const int s = int(p) - int(Prop::BorderTopStyle);
		to.border_style[s] = from.border_style[s];
		break;
	}
	case Prop::BorderTopColor: case Prop::BorderRightColor: case Prop::BorderBottomColor: case Prop::BorderLeftColor:
	{
		const int s = int(p) - int(Prop::BorderTopColor);
		to.border_color[s] = from.border_color[s];
		break;
	}
	case Prop::BorderTopLeftRadius: case Prop::BorderTopRightRadius: case Prop::BorderBottomRightRadius:
	case Prop::BorderBottomLeftRadius:
	{
		const int s = int(p) - int(Prop::BorderTopLeftRadius);
		to.radius[s] = from.radius[s];
		break;
	}
	case Prop::Top: case Prop::Right: case Prop::Bottom: case Prop::Left:
	{
		const int s = int(p) - int(Prop::Top);
		to.inset[s] = from.inset[s];
		break;
	}
	case Prop::FlexDirection: to.flex_direction = from.flex_direction; break;
	case Prop::FlexWrap: to.flex_wrap = from.flex_wrap; break;
	case Prop::FlexGrow: to.flex_grow = from.flex_grow; break;
	case Prop::FlexShrink: to.flex_shrink = from.flex_shrink; break;
	case Prop::FlexBasis: to.flex_basis = from.flex_basis; break;
	case Prop::Order: to.order = from.order; break;
	case Prop::AlignItems: to.align_items = from.align_items; break;
	case Prop::AlignSelf: to.align_self = from.align_self; break;
	case Prop::AlignContent: to.align_content = from.align_content; break;
	case Prop::JustifyContent: to.justify_content = from.justify_content; break;
	case Prop::JustifyItems: to.justify_items = from.justify_items; break;
	case Prop::JustifySelf: to.justify_self = from.justify_self; break;
	case Prop::RowGap: to.row_gap = from.row_gap; break;
	case Prop::ColumnGap: to.column_gap = from.column_gap; break;
	case Prop::OverflowX: to.overflow_x = from.overflow_x; break;
	case Prop::OverflowY: to.overflow_y = from.overflow_y; break;
	case Prop::TextOverflow: to.text_overflow = from.text_overflow; break;
	case Prop::TextDecorationLine: to.text_decoration_line = from.text_decoration_line; break;
	case Prop::TextDecorationColor: to.text_decoration_color = from.text_decoration_color; break;
	case Prop::WhiteSpace: to.white_space = from.white_space; break;
	case Prop::TextAlign: to.text_align = from.text_align; break;
	case Prop::TextTransform: to.text_transform = from.text_transform; break;
	case Prop::FontFamily: to.font_family = from.font_family; break;
	case Prop::FontSize: to.font_size = from.font_size; break;
	case Prop::FontWeight: to.font_weight = from.font_weight; break;
	case Prop::FontStyle: to.font_style = from.font_style; break;
	case Prop::FontVariantNumeric: to.tabular_nums = from.tabular_nums; break;
	case Prop::LineHeight: to.line_height = from.line_height; break;
	case Prop::LetterSpacing: to.letter_spacing = from.letter_spacing; break;
	case Prop::WordSpacing: to.word_spacing = from.word_spacing; break;
	case Prop::VerticalAlign: to.vertical_align = from.vertical_align; break;
	case Prop::Visibility: to.visibility = from.visibility; break;
	case Prop::Color: to.color = from.color; break;
	case Prop::BackgroundColor: to.background_color = from.background_color; break;
	case Prop::BackgroundImage: to.background_image = from.background_image; break;
	case Prop::Opacity: to.opacity = from.opacity; break;
	case Prop::BoxShadow: to.box_shadow = from.box_shadow; break;
	case Prop::OutlineWidth: to.outline_width = from.outline_width; break;
	case Prop::OutlineStyle: to.outline_style = from.outline_style; break;
	case Prop::OutlineColor: to.outline_color = from.outline_color; break;
	case Prop::OutlineOffset: to.outline_offset = from.outline_offset; break;
	case Prop::Cursor: to.cursor = from.cursor; break;
	case Prop::PointerEvents: to.pointer_events = from.pointer_events; break;
	case Prop::UserSelect: to.user_select_none = from.user_select_none; break;
	case Prop::ImageRendering: to.pixelated = from.pixelated; break;
	case Prop::GridTemplateColumns: to.grid_template_columns = from.grid_template_columns; break;
	case Prop::GridTemplateRows: to.grid_template_rows = from.grid_template_rows; break;
	case Prop::GridAutoFlow: to.grid_auto_flow = from.grid_auto_flow; break;
	case Prop::GridAutoRows: to.grid_auto_rows = from.grid_auto_rows; break;
	case Prop::GridAutoColumns: to.grid_auto_columns = from.grid_auto_columns; break;
	case Prop::BorderSpacing: to.border_spacing = from.border_spacing; break;
	case Prop::BorderCollapse: to.border_collapse = from.border_collapse; break;
	case Prop::Content:
		to.content = from.content;
		to.has_content = from.has_content;
		break;
	case Prop::Fill:
		to.fill = from.fill;
		to.fill_none = from.fill_none;
		break;
	case Prop::Stroke:
		to.stroke = from.stroke;
		to.stroke_none = from.stroke_none;
		break;
	case Prop::ScrollbarWidth: to.scrollbar_width = from.scrollbar_width; break;
	case Prop::ScrollbarGutter: to.scrollbar_gutter = from.scrollbar_gutter; break;
	case Prop::ScrollbarColor: to.scrollbar_color = from.scrollbar_color; break;
	case Prop::ZIndex: case Prop::Resize: case Prop::TouchAction: case Prop::Count: break;
	}
}

//! Which colours are currentcolor, resolved once the colour is known.
enum CurrentBits : uint32_t
{
	CURRENT_BORDER = 1,  // four bits, one per side
	CURRENT_OUTLINE = 16,
	CURRENT_BACKGROUND = 32,
	CURRENT_FILL = 64,
	CURRENT_STROKE = 128,
	CURRENT_DECORATION = 256,
};

//! A declared value applied to a style; false when it is not valid for the property (it is then unset).
bool apply(Prop p, std::string_view v, ComputedStyle &cs, const ComputedStyle *parent, const LengthEnv &env,
	uint32_t &current)
{
	auto length = [&](Length &out, bool allow_auto, bool allow_none, bool allow_sizes) {
		std::optional<Length> l = parse_length(v, env);
		if (!l)
			return false;
		switch (l->kind)
		{
		case Length::Kind::Value: break;
		case Length::Kind::Auto: if (!allow_auto) return false; break;
		case Length::Kind::None: if (!allow_none) return false; break;
		case Length::Kind::FitContent: case Length::Kind::MinContent: case Length::Kind::MaxContent:
			if (!allow_sizes) return false;
			break;
		default: return false;
		}
		out = *l;
		return true;
	};
	auto color = [&](Color &out, uint32_t bit) {
		bool cur = false;
		std::optional<Color> c = parse_color(v, &cur);
		if (cur)
		{
			current |= bit;
			return true;
		}
		if (!c)
			return false;
		current &= ~bit;
		out = *c;
		return true;
	};
	auto number = [&](double &out, bool nonneg) {
		std::optional<double> n = parse_number(v);
		if (!n || (nonneg && *n < 0))
			return false;
		out = *n;
		return true;
	};
	switch (p)
	{
	case Prop::Display: return parse_display(v, cs.display);
	case Prop::Position:
		return keyword<Position>(v, { { "static", Position::Static }, { "relative", Position::Relative },
			{ "absolute", Position::Absolute }, { "fixed", Position::Fixed }, { "sticky", Position::Sticky },
			{ "-webkit-sticky", Position::Sticky } }, cs.position);
	case Prop::BoxSizing:
		return keyword<BoxSizing>(v, { { "content-box", BoxSizing::ContentBox }, { "border-box", BoxSizing::BorderBox } },
			cs.box_sizing);
	case Prop::Width: return length(cs.width, true, false, true);
	case Prop::Height: return length(cs.height, true, false, true);
	case Prop::MinWidth: return length(cs.min_width, true, false, true);
	case Prop::MinHeight: return length(cs.min_height, true, false, true);
	case Prop::MaxWidth: return length(cs.max_width, false, true, true);
	case Prop::MaxHeight: return length(cs.max_height, false, true, true);
	case Prop::MarginTop: case Prop::MarginRight: case Prop::MarginBottom: case Prop::MarginLeft:
		return length(cs.margin[int(p) - int(Prop::MarginTop)], true, false, false);
	case Prop::PaddingTop: case Prop::PaddingRight: case Prop::PaddingBottom: case Prop::PaddingLeft:
		return length(cs.padding[int(p) - int(Prop::PaddingTop)], false, false, false);
	case Prop::BorderTopWidth: case Prop::BorderRightWidth: case Prop::BorderBottomWidth: case Prop::BorderLeftWidth:
	{
		std::optional<double> w = parse_line_width(v, env);
		if (!w)
			return false;
		cs.border_width[int(p) - int(Prop::BorderTopWidth)] = *w;
		return true;
	}
	case Prop::BorderTopStyle: case Prop::BorderRightStyle: case Prop::BorderBottomStyle: case Prop::BorderLeftStyle:
		return parse_border_style(v, cs.border_style[int(p) - int(Prop::BorderTopStyle)]);
	case Prop::BorderTopColor: case Prop::BorderRightColor: case Prop::BorderBottomColor: case Prop::BorderLeftColor:
	{
		const int s = int(p) - int(Prop::BorderTopColor);
		return color(cs.border_color[s], CURRENT_BORDER << s);
	}
	case Prop::BorderTopLeftRadius: case Prop::BorderTopRightRadius: case Prop::BorderBottomRightRadius:
	case Prop::BorderBottomLeftRadius:
	{
		// one radius for both axes (an elliptical corner's second is not kept)
		const auto w = split_words(v);
		if (w.empty())
			return false;
		std::optional<Length> l = parse_length(w[0], env);
		if (!l || !l->is_value())
			return false;
		cs.radius[int(p) - int(Prop::BorderTopLeftRadius)] = *l;
		return true;
	}
	case Prop::Top: case Prop::Right: case Prop::Bottom: case Prop::Left:
		return length(cs.inset[int(p) - int(Prop::Top)], true, false, false);
	case Prop::FlexDirection:
		return keyword<FlexDirection>(v, { { "row", FlexDirection::Row }, { "row-reverse", FlexDirection::RowReverse },
			{ "column", FlexDirection::Column }, { "column-reverse", FlexDirection::ColumnReverse } }, cs.flex_direction);
	case Prop::FlexWrap:
		return keyword<FlexWrap>(v, { { "nowrap", FlexWrap::Nowrap }, { "wrap", FlexWrap::Wrap },
			{ "wrap-reverse", FlexWrap::WrapReverse } }, cs.flex_wrap);
	case Prop::FlexGrow: return number(cs.flex_grow, true);
	case Prop::FlexShrink: return number(cs.flex_shrink, true);
	case Prop::FlexBasis:
	{
		std::optional<Length> l = parse_length(v, env);
		if (!l || !(l->is_value() || l->kind == Length::Kind::Auto || l->kind == Length::Kind::Content
			|| l->kind == Length::Kind::MaxContent || l->kind == Length::Kind::MinContent
			|| l->kind == Length::Kind::FitContent))
			return false;
		cs.flex_basis = *l;
		return true;
	}
	case Prop::Order:
	{
		double o;
		if (!number(o, false))
			return false;
		cs.order = int(o);
		return true;
	}
	case Prop::AlignItems: return parse_align(v, cs.align_items);
	case Prop::AlignSelf: return parse_align(v, cs.align_self);
	case Prop::AlignContent: return parse_align(v, cs.align_content);
	case Prop::JustifyContent: return parse_align(v, cs.justify_content);
	case Prop::JustifyItems: return parse_align(v, cs.justify_items);
	case Prop::JustifySelf: return parse_align(v, cs.justify_self);
	case Prop::RowGap:
	case Prop::ColumnGap:
	{
		std::optional<Length> l = parse_length(v, env);
		if (!l || !(l->is_value() || l->kind == Length::Kind::Normal))
			return false;
		(p == Prop::RowGap ? cs.row_gap : cs.column_gap) = *l;
		return true;
	}
	case Prop::OverflowX: return parse_overflow(v, cs.overflow_x);
	case Prop::OverflowY: return parse_overflow(v, cs.overflow_y);
	case Prop::TextDecorationLine:
	{
		uint8_t bits = 0;
		for (std::string_view w : split_words(v))
		{
			const std::string k = lower(w);
			if (k == "none" && split_words(v).size() == 1)
				break;
			if (k == "underline")
				bits |= DECORATION_UNDERLINE;
			else if (k == "overline")
				bits |= DECORATION_OVERLINE;
			else if (k == "line-through")
				bits |= DECORATION_LINE_THROUGH;
			else if (k != "blink")
				return false;
		}
		cs.text_decoration_line = bits;
		return true;
	}
	case Prop::TextDecorationColor: return color(cs.text_decoration_color, CURRENT_DECORATION);
	case Prop::TextOverflow:
		return keyword<TextOverflow>(v, { { "clip", TextOverflow::Clip }, { "ellipsis", TextOverflow::Ellipsis } },
			cs.text_overflow);
	case Prop::WhiteSpace:
		return keyword<WhiteSpace>(v, { { "normal", WhiteSpace::Normal }, { "nowrap", WhiteSpace::Nowrap },
			{ "pre", WhiteSpace::Pre }, { "pre-wrap", WhiteSpace::PreWrap }, { "pre-line", WhiteSpace::PreLine },
			{ "break-spaces", WhiteSpace::BreakSpaces } }, cs.white_space);
	case Prop::TextAlign:
	{
		if (lower(trim(v)) == "match-parent")
		{
			cs.text_align = parent ? parent->text_align : TextAlign::Start;
			return true;
		}
		return keyword<TextAlign>(v, { { "start", TextAlign::Start }, { "end", TextAlign::End },
			{ "left", TextAlign::Left }, { "right", TextAlign::Right }, { "center", TextAlign::Center },
			{ "-moz-center", TextAlign::Center }, { "-webkit-center", TextAlign::Center },
			{ "justify", TextAlign::Justify } }, cs.text_align);
	}
	case Prop::TextTransform:
		return keyword<TextTransform>(v, { { "none", TextTransform::None }, { "uppercase", TextTransform::Uppercase },
			{ "lowercase", TextTransform::Lowercase }, { "capitalize", TextTransform::Capitalize } }, cs.text_transform);
	case Prop::FontFamily:
		if (trim(v).empty())
			return false;
		cs.font_family = trim(v);
		return true;
	case Prop::FontSize:
		return true;  // computed first, in cascade()
	case Prop::FontWeight:
	{
		const std::string l = lower(trim(v));
		const int pw = parent ? parent->font_weight : 400;
		if (l == "normal")
			cs.font_weight = 400;
		else if (l == "bold")
			cs.font_weight = 700;
		else if (l == "bolder")
			cs.font_weight = pw < 350 ? 400 : pw < 550 ? 700 : 900;
		else if (l == "lighter")
			cs.font_weight = pw < 550 ? 100 : pw < 750 ? 400 : 700;
		else
		{
			std::optional<double> n = parse_number(l);
			if (!n || *n < 1 || *n > 1000)
				return false;
			cs.font_weight = int(*n);
		}
		return true;
	}
	case Prop::FontStyle:
	{
		const std::string l = lower(trim(v));
		return keyword<FontStyle>(l.starts_with("oblique") ? "oblique" : l, { { "normal", FontStyle::Normal },
			{ "italic", FontStyle::Italic }, { "oblique", FontStyle::Oblique } }, cs.font_style);
	}
	case Prop::FontVariantNumeric:
		cs.tabular_nums = lower(v).find("tabular-nums") != std::string::npos;
		return true;
	case Prop::LineHeight:
	{
		const std::string l = lower(trim(v));
		if (l == "normal")
		{
			cs.line_height = { LineHeight::Kind::Normal, 0 };
			return true;
		}
		if (std::optional<double> n = parse_number(l))
		{
			if (*n < 0)
				return false;
			cs.line_height = { LineHeight::Kind::Number, *n };
			return true;
		}
		std::optional<Length> len = parse_length(l, env);
		if (!len || !len->is_value())
			return false;
		cs.line_height = { LineHeight::Kind::Px, len->px + len->pct * env.em };
		return true;
	}
	case Prop::LetterSpacing:
	case Prop::WordSpacing:
	{
		double &out = p == Prop::LetterSpacing ? cs.letter_spacing : cs.word_spacing;
		if (lower(trim(v)) == "normal")
		{
			out = 0;
			return true;
		}
		std::optional<Length> len = parse_length(v, env);
		if (!len || !len->is_value())
			return false;
		out = len->px + len->pct * env.em;
		return true;
	}
	case Prop::VerticalAlign:
		if (!keyword<VerticalAlign>(v, { { "baseline", VerticalAlign::Baseline }, { "middle", VerticalAlign::Middle },
			{ "top", VerticalAlign::Top }, { "bottom", VerticalAlign::Bottom }, { "text-top", VerticalAlign::TextTop },
			{ "text-bottom", VerticalAlign::TextBottom }, { "sub", VerticalAlign::Sub }, { "super", VerticalAlign::Super } },
			cs.vertical_align))
			cs.vertical_align = VerticalAlign::Baseline;  // a shift by a length is not kept
		return true;
	case Prop::Visibility:
		return keyword<Visibility>(v, { { "visible", Visibility::Visible }, { "hidden", Visibility::Hidden },
			{ "collapse", Visibility::Collapse } }, cs.visibility);
	case Prop::Color:
		return true;  // computed first, in cascade()
	case Prop::BackgroundColor: return color(cs.background_color, CURRENT_BACKGROUND);
	case Prop::BackgroundImage:
		cs.background_image = lower(trim(v)) == "none" ? "" : std::string(trim(v));
		return true;
	case Prop::Opacity:
	{
		std::optional<Length> l = parse_length(v, env);
		std::optional<double> n = parse_number(v);
		if (n)
			cs.opacity = std::clamp(*n, 0.0, 1.0);
		else if (l && l->is_value() && l->px == 0)
			cs.opacity = std::clamp(l->pct, 0.0, 1.0);
		else
			return false;
		return true;
	}
	case Prop::BoxShadow:
		cs.box_shadow = lower(trim(v)) == "none" ? "" : std::string(trim(v));
		return true;
	case Prop::OutlineWidth:
	{
		std::optional<double> w = parse_line_width(v, env);
		if (!w)
			return false;
		cs.outline_width = *w;
		return true;
	}
	case Prop::OutlineStyle: return parse_border_style(v, cs.outline_style);
	case Prop::OutlineColor:
		if (lower(trim(v)) == "invert" || lower(trim(v)) == "auto")
		{
			current |= CURRENT_OUTLINE;
			return true;
		}
		return color(cs.outline_color, CURRENT_OUTLINE);
	case Prop::OutlineOffset:
	{
		std::optional<Length> l = parse_length(v, env);
		if (!l || !l->is_value())
			return false;
		cs.outline_offset = l->px;
		return true;
	}
	case Prop::Cursor:
	{
		const auto parts = split_commas(v);
		cs.cursor = lower(trim(parts.back()));
		return true;
	}
	case Prop::PointerEvents:
		cs.pointer_events = lower(trim(v)) == "none" ? PointerEvents::None : PointerEvents::Auto;
		return true;
	case Prop::UserSelect:
		cs.user_select_none = lower(trim(v)) == "none";
		return true;
	case Prop::ImageRendering:
	{
		const std::string l = lower(trim(v));
		cs.pixelated = l == "pixelated" || l == "crisp-edges" || l == "-moz-crisp-edges";
		return true;
	}
	case Prop::GridTemplateColumns: cs.grid_template_columns = lower(trim(v)) == "none" ? "" : std::string(trim(v)); return true;
	case Prop::GridTemplateRows: cs.grid_template_rows = lower(trim(v)) == "none" ? "" : std::string(trim(v)); return true;
	case Prop::GridAutoFlow:
	{
		const std::string l = lower(trim(v));
		const bool column = l.find("column") != std::string::npos, dense = l.find("dense") != std::string::npos;
		cs.grid_auto_flow = column ? (dense ? GridAutoFlow::ColumnDense : GridAutoFlow::Column)
			: (dense ? GridAutoFlow::RowDense : GridAutoFlow::Row);
		return true;
	}
	case Prop::GridAutoRows: cs.grid_auto_rows = trim(v); return true;
	case Prop::GridAutoColumns: cs.grid_auto_columns = trim(v); return true;
	case Prop::BorderSpacing:
	{
		const auto w = split_words(v);
		if (w.empty())
			return false;
		std::optional<Length> l = parse_length(w[0], env);
		if (!l || !l->is_value())
			return false;
		cs.border_spacing = l->px;
		return true;
	}
	case Prop::BorderCollapse:
		return keyword<BorderCollapse>(v, { { "separate", BorderCollapse::Separate }, { "collapse", BorderCollapse::Collapse } },
			cs.border_collapse);
	case Prop::Content:
	{
		const std::string_view t = trim(v);
		const std::string l = lower(t);
		if (l == "normal" || l == "none")
		{
			cs.content.clear();
			cs.has_content = false;
			return true;
		}
		std::string text;
		for (const Token &tok : tokenize(t))
			if (tok.type == Token::Type::String)
				text += tok.text;
		cs.content = text;
		cs.has_content = true;
		return true;
	}
	case Prop::Fill:
	case Prop::Stroke:
	{
		const bool fill = p == Prop::Fill;
		if (lower(trim(v)) == "none")
		{
			(fill ? cs.fill_none : cs.stroke_none) = true;
			return true;
		}
		if (!color(fill ? cs.fill : cs.stroke, fill ? CURRENT_FILL : CURRENT_STROKE))
			return false;
		(fill ? cs.fill_none : cs.stroke_none) = false;
		return true;
	}
	case Prop::ScrollbarGutter:
	{
		const std::string k = lower(trim(v));
		if (k != "auto" && k != "stable" && k != "stable both-edges")
			return false;
		cs.scrollbar_gutter = k != "auto";
		return true;
	}
	case Prop::ScrollbarWidth:
		return keyword<ScrollbarWidth>(v, { { "auto", ScrollbarWidth::Auto }, { "thin", ScrollbarWidth::Thin },
			{ "none", ScrollbarWidth::None } }, cs.scrollbar_width);
	case Prop::ScrollbarColor: cs.scrollbar_color = trim(v); return true;
	case Prop::ZIndex: case Prop::Resize: case Prop::TouchAction: return true;
	case Prop::Count: return false;
	}
	return false;
}

Display blockified(Display d)
{
	switch (d)
	{
	case Display::Inline: case Display::InlineBlock: case Display::TableRow: case Display::TableCell:
	case Display::TableRowGroup: case Display::TableHeaderGroup: case Display::TableFooterGroup:
		return Display::Block;
	case Display::InlineFlex: return Display::Flex;
	case Display::InlineGrid: return Display::Grid;
	case Display::InlineTable: return Display::Table;
	default: return d;
	}
}

} // namespace

bool same_layout(const ComputedStyle &a, const ComputedStyle &b)
{
	// b with what is only drawn made a's, compared whole: a property not listed here counts for the layout
	ComputedStyle c = b;
	c.border_style = a.border_style;
	c.border_color = a.border_color;
	c.radius = a.radius;
	c.text_overflow = a.text_overflow;
	c.text_decoration_line = a.text_decoration_line;
	c.text_decoration_color = a.text_decoration_color;
	c.decorations = a.decorations;
	c.decoration_color = a.decoration_color;
	c.visibility = a.visibility;
	c.color = a.color;
	c.background_color = a.background_color;
	c.background_image = a.background_image;
	c.opacity = a.opacity;
	c.box_shadow = a.box_shadow;
	c.outline_width = a.outline_width;
	c.outline_style = a.outline_style;
	c.outline_color = a.outline_color;
	c.outline_offset = a.outline_offset;
	c.cursor = a.cursor;
	c.pointer_events = a.pointer_events;
	c.user_select_none = a.user_select_none;
	c.pixelated = a.pixelated;
	c.fill = a.fill;
	c.fill_none = a.fill_none;
	c.stroke = a.stroke;
	c.stroke_none = a.stroke_none;
	c.scrollbar_color = a.scrollbar_color;
	c.custom = a.custom;
	return c == a;
}

std::string ComputedStyle::value(Prop p) const
{
	switch (p)
	{
	case Prop::Display: return name_of(display);
	case Prop::Position:
	{
		static const char *const names[] = { "static", "relative", "absolute", "fixed", "sticky" };
		return names[size_t(position)];
	}
	case Prop::BoxSizing: return box_sizing == BoxSizing::BorderBox ? "border-box" : "content-box";
	case Prop::Width: return serialize(width);
	case Prop::Height: return serialize(height);
	case Prop::MinWidth: return serialize(min_width);
	case Prop::MinHeight: return serialize(min_height);
	case Prop::MaxWidth: return serialize(max_width);
	case Prop::MaxHeight: return serialize(max_height);
	case Prop::MarginTop: case Prop::MarginRight: case Prop::MarginBottom: case Prop::MarginLeft:
		return serialize(margin[int(p) - int(Prop::MarginTop)]);
	case Prop::PaddingTop: case Prop::PaddingRight: case Prop::PaddingBottom: case Prop::PaddingLeft:
		return serialize(padding[int(p) - int(Prop::PaddingTop)]);
	case Prop::BorderTopWidth: case Prop::BorderRightWidth: case Prop::BorderBottomWidth: case Prop::BorderLeftWidth:
		return format_number(border_width[int(p) - int(Prop::BorderTopWidth)]) + "px";
	case Prop::BorderTopStyle: case Prop::BorderRightStyle: case Prop::BorderBottomStyle: case Prop::BorderLeftStyle:
		return name_of(border_style[int(p) - int(Prop::BorderTopStyle)]);
	case Prop::BorderTopColor: case Prop::BorderRightColor: case Prop::BorderBottomColor: case Prop::BorderLeftColor:
		return serialize(border_color[int(p) - int(Prop::BorderTopColor)]);
	case Prop::BorderTopLeftRadius: case Prop::BorderTopRightRadius: case Prop::BorderBottomRightRadius:
	case Prop::BorderBottomLeftRadius:
		return serialize(radius[int(p) - int(Prop::BorderTopLeftRadius)]);
	case Prop::Top: case Prop::Right: case Prop::Bottom: case Prop::Left:
		return serialize(inset[int(p) - int(Prop::Top)]);
	case Prop::FlexDirection:
	{
		static const char *const names[] = { "row", "row-reverse", "column", "column-reverse" };
		return names[size_t(flex_direction)];
	}
	case Prop::FlexWrap:
	{
		static const char *const names[] = { "nowrap", "wrap", "wrap-reverse" };
		return names[size_t(flex_wrap)];
	}
	case Prop::FlexGrow: return format_number(flex_grow);
	case Prop::FlexShrink: return format_number(flex_shrink);
	case Prop::FlexBasis: return serialize(flex_basis);
	case Prop::Order: return std::to_string(order);
	case Prop::AlignItems: return name_of(align_items);
	case Prop::AlignSelf: return name_of(align_self);
	case Prop::AlignContent: return name_of(align_content);
	case Prop::JustifyContent: return name_of(justify_content);
	case Prop::JustifyItems: return name_of(justify_items);
	case Prop::JustifySelf: return name_of(justify_self);
	case Prop::RowGap: return serialize(row_gap);
	case Prop::ColumnGap: return serialize(column_gap);
	case Prop::OverflowX: return name_of(overflow_x);
	case Prop::OverflowY: return name_of(overflow_y);
	case Prop::TextOverflow: return text_overflow == TextOverflow::Ellipsis ? "ellipsis" : "clip";
	case Prop::TextDecorationLine:
	{
		std::string out;
		for (const auto &[bit, name] : { std::pair { DECORATION_UNDERLINE, "underline" }, { DECORATION_OVERLINE, "overline" },
				 { DECORATION_LINE_THROUGH, "line-through" } })
			if (text_decoration_line & bit)
				out += (out.empty() ? "" : " ") + std::string(name);
		return out.empty() ? "none" : out;
	}
	case Prop::TextDecorationColor: return serialize(text_decoration_color);
	case Prop::WhiteSpace:
	{
		static const char *const names[] = { "normal", "nowrap", "pre", "pre-wrap", "pre-line", "break-spaces" };
		return names[size_t(white_space)];
	}
	case Prop::TextAlign:
	{
		static const char *const names[] = { "start", "end", "left", "right", "center", "justify" };
		return names[size_t(text_align)];
	}
	case Prop::TextTransform:
	{
		static const char *const names[] = { "none", "uppercase", "lowercase", "capitalize" };
		return names[size_t(text_transform)];
	}
	case Prop::FontFamily: return font_family;
	case Prop::FontSize: return format_number(font_size) + "px";
	case Prop::FontWeight: return std::to_string(font_weight);
	case Prop::FontStyle: return font_style == FontStyle::Normal ? "normal" : font_style == FontStyle::Italic ? "italic" : "oblique";
	case Prop::LineHeight:
		return line_height.kind == LineHeight::Kind::Normal ? "normal"
			: line_height.kind == LineHeight::Kind::Number ? format_number(line_height.value)
			: format_number(line_height.value) + "px";
	case Prop::LetterSpacing: return letter_spacing ? format_number(letter_spacing) + "px" : "normal";
	case Prop::WordSpacing: return format_number(word_spacing) + "px";
	case Prop::Visibility: return visibility == Visibility::Visible ? "visible" : visibility == Visibility::Hidden ? "hidden" : "collapse";
	case Prop::Color: return serialize(color);
	case Prop::BackgroundColor: return serialize(background_color);
	case Prop::BackgroundImage: return background_image.empty() ? "none" : background_image;
	case Prop::Opacity: return format_number(opacity);
	case Prop::BoxShadow: return box_shadow.empty() ? "none" : box_shadow;
	case Prop::OutlineWidth: return format_number(outline_width) + "px";
	case Prop::OutlineStyle: return name_of(outline_style);
	case Prop::OutlineColor: return serialize(outline_color);
	case Prop::OutlineOffset: return format_number(outline_offset) + "px";
	case Prop::Cursor: return cursor;
	case Prop::PointerEvents: return pointer_events == PointerEvents::None ? "none" : "auto";
	case Prop::UserSelect: return user_select_none ? "none" : "auto";
	case Prop::GridTemplateColumns: return grid_template_columns.empty() ? "none" : grid_template_columns;
	case Prop::GridTemplateRows: return grid_template_rows.empty() ? "none" : grid_template_rows;
	case Prop::Content: return has_content ? "\"" + content + "\"" : "none";
	case Prop::ScrollbarGutter: return scrollbar_gutter ? "stable" : "auto";
	case Prop::ScrollbarWidth: return scrollbar_width == ScrollbarWidth::Thin ? "thin" : scrollbar_width == ScrollbarWidth::None ? "none" : "auto";
	default: return "";
	}
}

StyleEngine::StyleEngine(Names &names, FontSet &fonts) : m_names(names), m_fonts(fonts)
{
	m_initial = std::make_shared<ComputedStyle>();
	set_sheets({});
}

void StyleEngine::set_viewport(double width, double height)
{
	if (width == m_vw && height == m_vh)
		return;
	m_vw = width;
	m_vh = height;
	m_shared.clear();
	m_anonymous.clear();
}

void StyleEngine::set_sheets(const std::vector<std::string> &sheets)
{
	uint32_t order = 0;
	m_rules = parse_stylesheet(USER_AGENT_SHEET, m_names, Origin::UserAgent, order);
	for (const std::string &s : sheets)
	{
		std::vector<Rule> r = parse_stylesheet(s, m_names, Origin::Author, order);
		std::move(r.begin(), r.end(), std::back_inserter(m_rules));
	}
	rebuild();
}

void StyleEngine::rebuild()
{
	m_index = Index();
	m_hover.clear();
	m_attrs.clear();
	m_class_reach.clear();
	m_id_reach.clear();
	m_attr_reach.clear();
	m_pseudo_reach.fill(Reach::None);
	m_reads_siblings = false;
	m_shared.clear();
	m_anonymous.clear();
	for (const Rule &r : m_rules)
	{
		const Compound &k = r.selector.parts.front();
		if (k.id)
			m_index.id[k.id].push_back(&r);
		else if (!k.classes.empty())
			m_index.cls[k.classes.front()].push_back(&r);
		else if (k.tag)
			m_index.tag[k.tag].push_back(&r);
		else
			m_index.any.push_back(&r);
		m_reads_siblings = m_reads_siblings || r.selector.reads_siblings;
		// the attributes the selectors read, and the compounds with :hover (the hover taken out), which find
		// the elements a change of hover restyles
		auto walk = [&](const Selector &s, auto &self) -> void {
			for (const Compound &c : s.parts)
			{
				for (const AttributeSelector &a : c.attrs)
					if (std::find(m_attrs.begin(), m_attrs.end(), a.name) == m_attrs.end())
						m_attrs.push_back(a.name);
				for (const auto &list : c.nots)
					for (const Selector &x : list)
						self(x, self);
				for (const auto &list : c.ises)
					for (const Selector &x : list)
						self(x, self);
				bool hover = false;
				Compound plain = c;
				plain.combinator = 0;
				plain.pseudos.clear();
				for (const PseudoSelector &p : c.pseudos)
					if (p.kind == PseudoClass::Hover)
						hover = true;
					else
						plain.pseudos.push_back(p);
				if (hover)
				{
					Selector h;
					h.parts.push_back(std::move(plain));
					m_hover.push_back(std::move(h));
				}
			}
		};
		walk(r.selector, walk);
		// how far a change of what each compound reads reaches: a subject's the element, a compound left of a
		// descendant or child combinator what is under it, left of a sibling combinator what is under its parent;
		// what is inside :is(), :where() and :not() as far as the compound holding it, or farther
		auto widen = [](Reach &r, Reach to) { r = std::max(r, to); };
		auto reach = [&](const Selector &sel, Reach outer, auto &self) -> void {
			Reach role = outer;
			for (size_t i = 0; i < sel.parts.size(); i++)
			{
				if (i > 0)
				{
					const char comb = sel.parts[i - 1].combinator;
					role = std::max(role, comb == '+' || comb == '~' ? Reach::Parent : Reach::Subtree);
				}
				const Compound &c = sel.parts[i];
				for (Name k : c.classes)
					widen(m_class_reach[k], role);
				if (c.id)
					widen(m_id_reach[c.id], role);
				for (const AttributeSelector &a : c.attrs)
					widen(m_attr_reach[a.name], role);
				for (const PseudoSelector &p : c.pseudos)
					widen(m_pseudo_reach[size_t(p.kind)], role);
				for (const auto &list : c.nots)
					for (const Selector &x : list)
						self(x, role, self);
				for (const auto &list : c.ises)
					for (const Selector &x : list)
						self(x, role, self);
			}
		};
		reach(r.selector, Reach::Self, reach);
	}
	// :empty reads the parent itself; its children's places (sibling combinators and the structural pseudo-classes)
	// reach its children at least
	m_children_reach = pseudo_reach(PseudoClass::Empty);
	for (PseudoClass p : { PseudoClass::FirstChild, PseudoClass::LastChild, PseudoClass::OnlyChild, PseudoClass::NthChild,
			 PseudoClass::NthLastChild, PseudoClass::FirstOfType, PseudoClass::LastOfType })
		if (pseudo_reach(p) != Reach::None)
			m_children_reach = std::max({ m_children_reach, Reach::Subtree, pseudo_reach(p) });
	if (m_reads_siblings)
		m_children_reach = std::max(m_children_reach, Reach::Subtree);
}

StyleEngine::Reach StyleEngine::class_reach(Name c) const
{
	auto it = m_class_reach.find(c);
	return it == m_class_reach.end() ? Reach::None : it->second;
}

StyleEngine::Reach StyleEngine::id_reach(Name id) const
{
	auto it = m_id_reach.find(id);
	return it == m_id_reach.end() ? Reach::None : it->second;
}

StyleEngine::Reach StyleEngine::attribute_reach(Name a) const
{
	auto it = m_attr_reach.find(a);
	Reach r = it == m_attr_reach.end() ? Reach::None : it->second;
	auto with = [&](PseudoClass p) { r = std::max(r, pseudo_reach(p)); };
	if (a == n::disabled)
	{
		with(PseudoClass::Disabled);
		with(PseudoClass::Enabled);
	}
	else if (a == n::checked)
		with(PseudoClass::Checked);
	else if (a == n::open)
		with(PseudoClass::Open);
	else if (a == n::placeholder || a == n::value)
		with(PseudoClass::PlaceholderShown);
	return r;
}

bool StyleEngine::reads_hover(const Element &el) const
{
	for (const Selector &s : m_hover)
		if (matches(s, el))
			return true;
	return false;
}

bool StyleEngine::reads_attribute(Name n) const
{
	return std::find(m_attrs.begin(), m_attrs.end(), n) != m_attrs.end();
}

void StyleEngine::collect(const Element &el, Name pseudo, std::vector<const Rule *> &out) const
{
	out.clear();
	auto add = [&](const std::vector<const Rule *> &list) {
		for (const Rule *r : list)
			if (r->selector.pseudo_element == pseudo && matches(r->selector, el))
				out.push_back(r);
	};
	add(m_index.any);
	if (el.id())
	{
		auto it = m_index.id.find(el.id());
		if (it != m_index.id.end())
			add(it->second);
	}
	for (Name c : el.classes())
	{
		auto it = m_index.cls.find(c);
		if (it != m_index.cls.end())
			add(it->second);
	}
	auto it = m_index.tag.find(el.tag());
	if (it != m_index.tag.end())
		add(it->second);
	// a class listed twice finds its rules twice
	std::sort(out.begin(), out.end(), [](const Rule *a, const Rule *b) {
		if (a->origin != b->origin)
			return a->origin < b->origin;
		if (a->selector.specificity != b->selector.specificity)
			return a->selector.specificity < b->selector.specificity;
		return a->order < b->order;
	});
	out.erase(std::unique(out.begin(), out.end()), out.end());
}

StylePtr StyleEngine::compute(const Element &el, const ComputedStyle *parent, Name pseudo)
{
	std::vector<const Rule *> rules;
	collect(el, pseudo, rules);
	if (pseudo && pseudo != n::backdrop && rules.empty())
		return nullptr;
	// the key of a shared style: the parent's, the rules, the inline style, an svg's presentation attributes
	std::string key;
	key.reserve(64);
	key.append(reinterpret_cast<const char *>(&parent), sizeof(parent));
	key.append(reinterpret_cast<const char *>(&pseudo), sizeof(pseudo));
	for (const Rule *r : rules)
		key.append(reinterpret_cast<const char *>(&r->order), sizeof(r->order));
	if (!pseudo)
	{
		key += '|';
		for (const auto &[k, v] : el.inline_style())
		{
			key += k;
			key += ':';
			key += v;
			key += ';';
		}
		if (el.ns() == Namespace::Svg)
			for (const Attribute &a : el.attributes())
				if (a.name == n::width || a.name == n::height || m_names.str(a.name) == "fill" || m_names.str(a.name) == "stroke")
				{
					key += m_names.str(a.name);
					key += '=';
					key += a.value;
					key += ';';
				}
		key += el.parent() && el.parent()->is_document() ? "r" : "";
	}
	auto it = m_shared.find(key);
	if (it != m_shared.end())
		return it->second;
	StylePtr s = cascade(pseudo ? nullptr : &el, parent, rules, pseudo);
	if (pseudo && pseudo != n::backdrop && !s->has_content)
		s = nullptr;
	if (m_shared.size() > 30000)
		m_shared.clear();
	m_shared.emplace(std::move(key), s);
	return s;
}

StylePtr StyleEngine::anonymous(const StylePtr &from)
{
	const ComputedStyle &parent = *from;
	auto it = m_anonymous.find(&parent);
	if (it != m_anonymous.end())
		return it->second.second;
	auto s = std::make_shared<ComputedStyle>();
	for (size_t i = 0; i < PROP_COUNT; i++)
		if (prop_inherited(Prop(i)))
			copy(Prop(i), *s, parent);
	s->display = Display::Block;
	s->border_width = { 0, 0, 0, 0 };
	s->outline_width = 0;
	s->custom = parent.custom;
	s->face = parent.face;
	m_anonymous.emplace(&parent, std::pair<StylePtr, StylePtr> { from, s });
	return s;
}

StylePtr StyleEngine::cascade(const Element *el, const ComputedStyle *parent, const std::vector<const Rule *> &rules,
	Name pseudo)
{
	// the browser's rules under the stylesheets', the inline style above them, their !important ones above that
	// and the browser's !important ones above all; an svg's presentation attributes under everything
	std::vector<Declared> declared;
	std::vector<std::pair<std::string_view, std::string>> presentation;
	if (el && el->ns() == Namespace::Svg)
	{
		for (const Attribute &a : el->attributes())
		{
			const std::string_view name = m_names.str(a.name);
			const bool size = el->tag() == n::svg && (a.name == n::width || a.name == n::height);
			if (size || (el->tag() != n::svg && (name == "fill" || name == "stroke")))
				presentation.emplace_back(name, parse_number(a.value) ? a.value + "px" : a.value);
		}
		for (const auto &[name, value] : presentation)
			declared.push_back({ name, value, -1 });
	}
	for (const Rule *r : rules)
		for (const Declaration &d : *r->declarations)
		{
			const bool ua = r->origin == Origin::UserAgent;
			declared.push_back({ d.name, d.value, d.important ? (ua ? 4 : 3) : (ua ? 0 : 1) });
		}
	if (el)
		for (const auto &[k, v] : el->inline_style())
			declared.push_back({ k, v, 2 });
	std::stable_sort(declared.begin(), declared.end(), [](const Declared &a, const Declared &b) {
		return a.priority < b.priority;
	});

	auto cs = std::make_shared<ComputedStyle>();
	// custom properties first: they are inherited, and the rest may use them
	std::shared_ptr<CustomProperties> own;
	for (const Declared &d : declared)
		if (d.name.starts_with("--"))
		{
			if (!own)
				own = parent && parent->custom ? std::make_shared<CustomProperties>(*parent->custom)
				                               : std::make_shared<CustomProperties>();
			own->values[std::string(d.name)] = substitute(d.value, own.get());
		}
	if (own)
		cs->custom = own;
	else if (parent)
		cs->custom = parent->custom;
	const CustomProperties *custom = cs->custom.get();

	// every declaration as the longhands it sets, var() replaced
	std::vector<std::string> raw(PROP_COUNT);
	std::vector<bool> set(PROP_COUNT, false);
	std::vector<std::pair<Prop, std::string>> longhands;
	for (const Declared &d : declared)
	{
		if (d.name.starts_with("--"))
			continue;
		longhands.clear();
		const std::string value = substitute(d.value, custom);
		if (!expand(d.name, value, longhands))
			continue;
		for (auto &[p, v] : longhands)
		{
			raw[size_t(p)] = std::move(v);
			set[size_t(p)] = true;
		}
	}

	// what it inherits
	if (parent)
		for (size_t i = 0; i < PROP_COUNT; i++)
			if (prop_inherited(Prop(i)))
				copy(Prop(i), *cs, *parent);

	auto keyword_of = [&](Prop p) -> int {
		const std::string l = lower(trim(raw[size_t(p)]));
		return l == "inherit" ? 1 : l == "initial" ? 2 : l == "unset" ? 3 : l == "revert" ? 3 : 0;
	};
	auto wide = [&](Prop p) {
		// inherit, initial and unset
		const int k = keyword_of(p);
		if (k == 1 || (k == 3 && prop_inherited(p)))
			copy(p, *cs, parent ? *parent : *m_initial);
		else
			copy(p, *cs, *m_initial);
	};

	// the font's size first: ems are relative to it
	const double pfs = parent ? parent->font_size : 16;
	if (set[size_t(Prop::FontSize)])
	{
		if (keyword_of(Prop::FontSize))
			wide(Prop::FontSize);
		else
		{
			bool ok;
			double fs = font_size_keyword(raw[size_t(Prop::FontSize)], pfs, ok);
			if (!ok)
			{
				std::optional<Length> l = parse_length(raw[size_t(Prop::FontSize)], { pfs, m_root_font_size, m_vw, m_vh });
				if (l && l->is_value())
				{
					fs = l->px + l->pct * pfs;
					ok = fs >= 0;
				}
			}
			if (ok)
				cs->font_size = fs;
		}
	}
	// sizes in 64ths of a pixel, as the browser keeps them
	cs->font_size = std::floor(cs->font_size * 64 + 1e-6) / 64;
	if (el && el->parent() && el->parent()->is_document())
		m_root_font_size = cs->font_size;

	const LengthEnv env { cs->font_size, m_root_font_size, m_vw, m_vh };
	uint32_t current = CURRENT_BORDER * 15 | CURRENT_OUTLINE | CURRENT_DECORATION;
	if (parent)
	{
		// an inherited colour property that was currentcolor stays so
		if (parent->fill_none && !set[size_t(Prop::Fill)])
			cs->fill_none = true;
	}
	if (set[size_t(Prop::Color)])
	{
		if (keyword_of(Prop::Color))
			wide(Prop::Color);
		else
		{
			bool cur = false;
			std::optional<Color> c = parse_color(raw[size_t(Prop::Color)], &cur);
			if (c)
				cs->color = *c;
		}
	}
	for (size_t i = 0; i < PROP_COUNT; i++)
	{
		const Prop p = Prop(i);
		if (!set[i] || p == Prop::FontSize || p == Prop::Color)
			continue;
		if (keyword_of(p))
		{
			wide(p);
			if (p == Prop::TextDecorationColor)
			{
				if (keyword_of(p) != 1)
					current |= CURRENT_DECORATION;
				else
					current &= ~CURRENT_DECORATION;
			}
			if (p >= Prop::BorderTopColor && p <= Prop::BorderLeftColor)
			{
				// initial is currentcolor
				const int s = int(p) - int(Prop::BorderTopColor);
				if (keyword_of(p) != 1)
					current |= CURRENT_BORDER << s;
				else
					current &= ~(CURRENT_BORDER << s);
			}
			continue;
		}
		if (!apply(p, raw[i], *cs, parent, env, current))
		{
			// not valid for the property: unset
			if (prop_inherited(p) && parent)
				copy(p, *cs, *parent);
			else
				copy(p, *cs, *m_initial);
		}
	}

	// what has to be read against the element's own colour
	for (int s = 0; s < 4; s++)
	{
		if (current & (CURRENT_BORDER << s))
			cs->border_color[s] = cs->color;
		if (cs->border_style[s] == BorderStyle::None || cs->border_style[s] == BorderStyle::Hidden)
			cs->border_width[s] = 0;
	}
	if (current & CURRENT_OUTLINE)
		cs->outline_color = cs->color;
	if (current & CURRENT_BACKGROUND)
		cs->background_color = cs->color;
	if (current & CURRENT_FILL)
		cs->fill = cs->color;
	if (current & CURRENT_STROKE)
		cs->stroke = cs->color;
	if (current & CURRENT_DECORATION)
		cs->text_decoration_color = cs->color;
	if (cs->outline_style == BorderStyle::None)
		cs->outline_width = 0;
	// visible or clip beside an axis that scrolls or hides becomes auto or hidden
	auto contained = [](Overflow o) { return o == Overflow::Visible || o == Overflow::Clip; };
	if (contained(cs->overflow_x) != contained(cs->overflow_y))
	{
		Overflow &o = contained(cs->overflow_x) ? cs->overflow_x : cs->overflow_y;
		o = o == Overflow::Visible ? Overflow::Auto : Overflow::Hidden;
	}
	// the lines through its text: its parent's, unless it is an inline block or out of flow, and its own
	const bool atomic = (cs->display == Display::InlineBlock || cs->display == Display::InlineFlex
		|| cs->display == Display::InlineGrid || cs->display == Display::InlineTable)
		&& !(parent && (parent->is_flex() || parent->is_grid()));
	cs->decorations = parent && !cs->out_of_flow() && !atomic ? parent->decorations : 0;
	cs->decoration_color = parent ? parent->decoration_color : cs->color;
	if (cs->text_decoration_line)
	{
		cs->decorations |= cs->text_decoration_line;
		cs->decoration_color = cs->text_decoration_color;
	}
	// what floats out of flow, an item of a flex or grid container and the root are blocks
	if (cs->out_of_flow())
		cs->inline_static = cs->display == Display::Inline || cs->display == Display::InlineBlock
			|| cs->display == Display::InlineFlex || cs->display == Display::InlineGrid;
	if (cs->out_of_flow() || (parent && (parent->is_flex() || parent->is_grid()) && !pseudo)
		|| (el && el->parent() && el->parent()->is_document()))
		cs->display = blockified(cs->display);
	if (pseudo == n::after || pseudo == n::before)
	{
		if (parent && (parent->is_flex() || parent->is_grid()))
			cs->display = blockified(cs->display);
	}
	cs->face = m_fonts.face(cs->font_family, cs->font_weight, cs->font_style);
	return cs;
}

} // namespace qdom
