// Copyright 2026 Ian Karlsson
//! \file
//! Computed styles and the cascade: an element's style from the rules that match it, its inline style and what it
//! inherits; lengths in pixels where they can be, colours as RGBA. Elements whose parents share a style and that
//! match the same rules with the same inline style share theirs (a computed style is never changed once made).
#pragma once

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "css/properties.h"
#include "css/stylesheet.h"
#include "css/values.h"

namespace qdom
{

class Element;
class FontSet;

enum class Display : uint8_t
{
	Inline, Block, InlineBlock, Flex, InlineFlex, Grid, InlineGrid, Table, InlineTable, TableRow, TableCell,
	TableRowGroup, TableHeaderGroup, TableFooterGroup, ListItem, Contents, None,
};

enum class Position : uint8_t { Static, Relative, Absolute, Fixed, Sticky };
enum class BoxSizing : uint8_t { ContentBox, BorderBox };
enum class Overflow : uint8_t { Visible, Hidden, Clip, Scroll, Auto };
enum class WhiteSpace : uint8_t { Normal, Nowrap, Pre, PreWrap, PreLine, BreakSpaces };
enum class TextAlign : uint8_t { Start, End, Left, Right, Center, Justify };
enum class TextTransform : uint8_t { None, Uppercase, Lowercase, Capitalize };
enum class TextOverflow : uint8_t { Clip, Ellipsis };
enum : uint8_t { DECORATION_UNDERLINE = 1, DECORATION_OVERLINE = 2, DECORATION_LINE_THROUGH = 4 };
enum class FlexDirection : uint8_t { Row, RowReverse, Column, ColumnReverse };
enum class FlexWrap : uint8_t { Nowrap, Wrap, WrapReverse };
enum class Align : uint8_t
{
	Auto, Normal, Stretch, Start, End, FlexStart, FlexEnd, Center, Baseline, SpaceBetween, SpaceAround, SpaceEvenly,
	SelfStart, SelfEnd, Left, Right,
};
enum class BorderStyle : uint8_t { None, Hidden, Solid, Dashed, Dotted, Double, Groove, Ridge, Inset, Outset };
enum class Visibility : uint8_t { Visible, Hidden, Collapse };
enum class FontStyle : uint8_t { Normal, Italic, Oblique };
enum class VerticalAlign : uint8_t { Baseline, Middle, Top, Bottom, TextTop, TextBottom, Sub, Super };
enum class ScrollbarWidth : uint8_t { Auto, Thin, None };
enum class PointerEvents : uint8_t { Auto, None };
enum class GridAutoFlow : uint8_t { Row, Column, RowDense, ColumnDense };
enum class BorderCollapse : uint8_t { Separate, Collapse };

//! The sides of a box, in CSS's order.
enum Side : uint8_t { TOP, RIGHT, BOTTOM, LEFT };

struct LineHeight
{
	enum class Kind : uint8_t { Normal, Number, Px };
	Kind kind = Kind::Normal;
	double value = 0;
	bool operator==(const LineHeight &) const = default;
};

//! The face a style draws its text in, as the font set chose it (FontSet::face).
using FaceId = uint16_t;

//! The custom properties in force: a style's own over its parent's, kept as declared.
struct CustomProperties
{
	std::unordered_map<std::string, std::string> values;
};

struct ComputedStyle
{
	Display display = Display::Inline;
	Position position = Position::Static;
	BoxSizing box_sizing = BoxSizing::ContentBox;
	Length width = Length::keyword(Length::Kind::Auto);
	Length height = Length::keyword(Length::Kind::Auto);
	Length min_width = Length::keyword(Length::Kind::Auto);
	Length min_height = Length::keyword(Length::Kind::Auto);
	Length max_width = Length::keyword(Length::Kind::None);
	Length max_height = Length::keyword(Length::Kind::None);
	std::array<Length, 4> margin {};
	std::array<Length, 4> padding {};
	std::array<double, 4> border_width { 3, 3, 3, 3 };  //!< medium; 0 where the style is none
	std::array<BorderStyle, 4> border_style {};
	std::array<Color, 4> border_color {};
	std::array<Length, 4> radius {};  //!< top-left, top-right, bottom-right, bottom-left
	std::array<Length, 4> inset { Length::keyword(Length::Kind::Auto), Length::keyword(Length::Kind::Auto),
		Length::keyword(Length::Kind::Auto), Length::keyword(Length::Kind::Auto) };

	FlexDirection flex_direction = FlexDirection::Row;
	FlexWrap flex_wrap = FlexWrap::Nowrap;
	double flex_grow = 0;
	double flex_shrink = 1;
	Length flex_basis = Length::keyword(Length::Kind::Auto);
	int order = 0;
	Align align_items = Align::Normal;
	Align align_self = Align::Auto;
	Align align_content = Align::Normal;
	Align justify_content = Align::Normal;
	Align justify_items = Align::Normal;
	Align justify_self = Align::Auto;
	Length row_gap = Length::keyword(Length::Kind::Normal);
	Length column_gap = Length::keyword(Length::Kind::Normal);

	Overflow overflow_x = Overflow::Visible;
	Overflow overflow_y = Overflow::Visible;
	TextOverflow text_overflow = TextOverflow::Clip;
	uint8_t text_decoration_line = 0;  //!< DECORATION_* bits
	Color text_decoration_color {};
	//! The lines drawn through its text: its own and those of the boxes it is in, as far as they propagate (not into
	//! an inline block or what is out of flow), in the colour of the nearest box that has them
	uint8_t decorations = 0;
	Color decoration_color {};
	WhiteSpace white_space = WhiteSpace::Normal;
	TextAlign text_align = TextAlign::Start;
	TextTransform text_transform = TextTransform::None;
	std::string font_family = "sans-serif";
	double font_size = 16;
	int font_weight = 400;
	FontStyle font_style = FontStyle::Normal;
	bool tabular_nums = false;
	LineHeight line_height;
	double letter_spacing = 0;
	double word_spacing = 0;
	VerticalAlign vertical_align = VerticalAlign::Baseline;
	Visibility visibility = Visibility::Visible;

	Color color { 0, 0, 0, 1 };
	Color background_color {};
	std::string background_image;  //!< empty for none
	double opacity = 1;
	std::string box_shadow;         //!< empty for none
	double outline_width = 3;
	BorderStyle outline_style = BorderStyle::None;
	Color outline_color {};
	double outline_offset = 0;
	std::string cursor = "auto";
	PointerEvents pointer_events = PointerEvents::Auto;
	bool user_select_none = false;
	bool pixelated = false;          //!< image-rendering: pixelated or crisp-edges

	std::string grid_template_columns;
	std::string grid_template_rows;
	GridAutoFlow grid_auto_flow = GridAutoFlow::Row;
	std::string grid_auto_rows = "auto";
	std::string grid_auto_columns = "auto";
	double border_spacing = 0;
	BorderCollapse border_collapse = BorderCollapse::Separate;

	//! ::before and ::after's content; has_content false for normal and none.
	std::string content;
	bool has_content = false;
	Color fill { 0, 0, 0, 1 };
	bool fill_none = false;
	Color stroke {};
	bool stroke_none = true;
	ScrollbarWidth scrollbar_width = ScrollbarWidth::Auto;
	bool scrollbar_gutter = false;  //!< scrollbar-gutter: stable, the vertical bar's room kept while it may scroll
	std::string scrollbar_color = "auto";

	//! An absolutely placed box that was inline-level before it was made a block: its static place is in a line.
	bool inline_static = false;
	//! The face its text is drawn in.
	FaceId face = 0;
	std::shared_ptr<const CustomProperties> custom;

	bool is_flex() const { return display == Display::Flex || display == Display::InlineFlex; }
	bool is_grid() const { return display == Display::Grid || display == Display::InlineGrid; }
	bool row() const { return flex_direction == FlexDirection::Row || flex_direction == FlexDirection::RowReverse; }
	bool out_of_flow() const { return position == Position::Absolute || position == Position::Fixed; }
	//! Whether it clips its content (a region that scrolls, or one that hides what overflows).
	bool clips() const { return overflow_x != Overflow::Visible || overflow_y != Overflow::Visible; }
	//! Whether the user can scroll it.
	bool scrolls() const
	{
		return overflow_x == Overflow::Auto || overflow_x == Overflow::Scroll || overflow_y == Overflow::Auto
			|| overflow_y == Overflow::Scroll;
	}
	//! A property's computed value as getComputedStyle gives it.
	std::string value(Prop p) const;

	bool operator==(const ComputedStyle &) const = default;
};

using StylePtr = std::shared_ptr<const ComputedStyle>;

//! Whether two styles lay a box out alike: they differ at most in what is drawn (colours, backgrounds, borders' colours
//! and styles, radii, shadows, outlines, opacity, visibility, the cursor, pointer-events, user-select, fill and
//! stroke, image-rendering, the scrollbar's colour, text-overflow, the custom properties as declared).
bool same_layout(const ComputedStyle &a, const ComputedStyle &b);

//! The cascade: the user agent's sheet and the author's, an index of their rules, and the shared styles.
class StyleEngine
{
public:
	StyleEngine(Names &names, FontSet &fonts);

	//! The author's sheets, in order (the host's first, then the document's style elements).
	void set_sheets(const std::vector<std::string> &sheets);
	//! The viewport, which vw and vh are relative to.
	void set_viewport(double width, double height);
	//! The styles made so far forgotten (the fonts changed: the faces they chose).
	void forget_shared()
	{
		m_shared.clear();
		m_anonymous.clear();
	}

	//! An element's style (pseudo 0), or its pseudo-element's (n::after, n::before, n::backdrop); for a
	//! pseudo-element, null when it has no content.
	StylePtr compute(const Element &el, const ComputedStyle *parent, Name pseudo = 0);
	//! The style of anonymous boxes inside a parent's: what they inherit, the rest initial.
	StylePtr anonymous(const StylePtr &parent);

	//! Whether a selector reads a sibling before its subject (a change to an element may then restyle its siblings).
	bool reads_siblings() const { return m_reads_siblings; }
	//! How far a change of what selectors read restyles (docs/design.md §3.9): nothing (no selector reads it), the
	//! element alone (read only in subjects), what is under it (read left of a descendant or child combinator), or
	//! what is under its parent (left of a sibling combinator).
	enum class Reach : uint8_t { None, Self, Subtree, Parent };
	Reach class_reach(Name c) const;
	Reach id_reach(Name id) const;
	//! An attribute's, with the pseudo-classes that read it (:disabled, :checked, :open, :placeholder-shown).
	Reach attribute_reach(Name a) const;
	Reach pseudo_reach(PseudoClass p) const { return m_pseudo_reach[size_t(p)]; }
	//! How far a child inserted or removed reaches its parent: :empty the parent, what reads its children's places
	//! (sibling combinators, :first-child, :nth-child() and the like) what is under it.
	Reach children_reach() const { return m_children_reach; }
	//! Whether an element's styles depend on whether the pointer is over it or over something in it.
	bool reads_hover(const Element &el) const;
	//! The attributes the selectors read besides class and id.
	bool reads_attribute(Name n) const;

private:
	struct Index
	{
		std::unordered_map<Name, std::vector<const Rule *>> id, cls, tag;
		std::vector<const Rule *> any;
	};

	void rebuild();
	void collect(const Element &el, Name pseudo, std::vector<const Rule *> &out) const;
	StylePtr cascade(const Element *el, const ComputedStyle *parent, const std::vector<const Rule *> &rules, Name pseudo);

	Names &m_names;
	FontSet &m_fonts;
	std::vector<Rule> m_rules;
	Index m_index;
	std::vector<Selector> m_hover;  //!< the compounds with :hover, the hover left out
	std::vector<Name> m_attrs;
	std::unordered_map<Name, Reach> m_class_reach, m_id_reach, m_attr_reach;
	std::array<Reach, 32> m_pseudo_reach {};
	Reach m_children_reach = Reach::None;
	bool m_reads_siblings = false;
	double m_vw = 0, m_vh = 0;
	double m_root_font_size = 16;
	std::unordered_map<std::string, StylePtr> m_shared;
	//! by their parents' styles, each kept with its parent's (so that no other style takes its address)
	std::unordered_map<const ComputedStyle *, std::pair<StylePtr, StylePtr>> m_anonymous;
	StylePtr m_initial;
};

} // namespace qdom
