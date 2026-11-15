// Copyright 2026 Ian Karlsson
//! \file
//! The layout: every element's box as Firefox lays out a page. Block flow with collapsing
//! margins, lines of text with wrapping and baselines, flex, absolutely placed and fixed boxes, regions that
//! scroll with 12-pixel scrollbars (6 for scrollbar-width: thin), grid (tracks, items placed in order) and tables.
//!
//! The boxes match Firefox's to half a pixel on the pages tested.
//! The box tree (texts, ::before and ::after, anonymous boxes around the texts of a flex container) is kept across
//! passes with each box's layout: a pass builds again only the children of the elements marked so, forgets the
//! layouts of the boxes marked and of those holding them, and lays the root out, taking each box asked for at the
//! key it was laid out at as it is (docs/design.md §3.9). A box is kept relative to its element's parent's border
//! box; text is kept as pieces of lines on the box that holds them, each at its baseline.
#pragma once

#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "css/style.h"

namespace qdom
{

class Dom;
class Element;
class Text;
struct LayoutNode;

//! Vertical margins collapsed together: the largest positive one and the most negative one.
struct Margins
{
	double pos = 0, neg = 0;

	void add(double m)
	{
		if (m > 0)
			pos = std::max(pos, m);
		else
			neg = std::min(neg, m);
	}
	void add(const Margins &o)
	{
		pos = std::max(pos, o.pos);
		neg = std::min(neg, o.neg);
	}
	double value() const { return pos + neg; }
};

//! A piece of a line of text, at its baseline, relative to the border box of the box that holds it.
struct TextPiece
{
	double x = 0, y = 0;
	double w = 0;
	std::string text;
	FaceId face = 0;
	double size = 0;
	double spacing = 0;
	//! The text's box: its style (the colour) is read when it is drawn, so that a change of colour alone needs no
	//! layout.
	const LayoutNode *owner = nullptr;
	double space = 0;  //!< the space after it on its line (none at the line's end), which its decorations cross
};

struct LayoutNode
{
	enum class Kind : uint8_t
	{
		Element,
		Text,       //!< a text among inline content
		Anonymous,  //!< a text that is a flex item, in a block of its own
		Pseudo,     //!< ::before or ::after
	};
	Kind kind = Kind::Element;
	Element *el = nullptr;              //!< null once its element is freed (the box is let go at the next pass)
	Text *text = nullptr;               //!< a document's text (null for a pseudo-element's, or once it is freed)
	const std::string *data = nullptr;  //!< a text's characters (a Text node's, or a pseudo-element's content)
	LayoutNode *anon = nullptr;         //!< a text's anonymous box, when it is a flex item
	StylePtr style;
	LayoutNode *parent = nullptr;
	std::vector<LayoutNode *> kids;

	// the box: border box relative to the parent element's border box
	double x = 0, y = 0, w = 0, h = 0;
	double bt = 0, br = 0, bb = 0, bl = 0;
	double pt = 0, pr = 0, pb = 0, pl = 0;
	std::optional<double> mt, mr, mb, ml;  //!< none for auto
	double cw = 0, ch = 0;  //!< the padding box less the scrollbars
	double sw = 0, sh = 0;  //!< what scrolls
	double ow = 0, oh = 0;  //!< how far its content reaches past it
	double bar_x = 0, bar_y = 0;  //!< the scrollbars' room
	std::optional<double> base;   //!< the first baseline from the top
	double content_h = 0;
	std::vector<TextPiece> pieces;
	bool inline_box = false;      //!< an inline element: the union of its fragments
	bool scrolls = false;
	double static_x = 0, static_y = 0;  //!< where an absolutely placed box would have been in flow
	//! The margins of its first and last children that collapse with its own top and bottom margins, and
	//! whether it is empty and collapses through (its top and bottom margins adjoin).
	Margins escape_top, escape_bottom;
	bool escapes_top = false, escapes_bottom = false, through = false;
	bool fixed = false;                 //!< a fixed box, placed in the viewport at fx, fy
	double fx = 0, fy = 0;
	std::string content;                //!< a pseudo-element's text
	double cell_dy = 0;                 //!< a table cell's: how far its table moved its content (vertical-align)
	bool free = false;                  //!< in the pool
	bool keep = false;                  //!< kept while its parent's children are built again

	// its layout, kept until it is forgotten: the key it was laid out at, its intrinsic widths, its heights at the
	// widths it was measured at, its own height
	struct Key
	{
		double w;
		std::optional<double> h, cbw, cbh;
		bool operator==(const Key &) const = default;
	};
	bool laid = false;
	uint32_t pass = 0;  //!< the pass that laid it out last or took its layout as it was
	Key key {};
	bool iw_valid = false;
	double iw_min = 0, iw_max = 0;
	std::vector<std::pair<Key, double>> heights;
	bool auto_valid = false;
	Key auto_key {};
	double auto_h = 0;

	bool is_element() const { return kind == Kind::Element; }
	bool is_text() const { return kind == Kind::Text; }
	const ComputedStyle &cs() const { return *style; }
};

class Layout
{
public:
	explicit Layout(Dom &dom);
	~Layout();
	Layout(const Layout &) = delete;
	Layout &operator=(const Layout &) = delete;

	//! The document laid out in a viewport: what is marked laid out again, or everything after relayout_all.
	void run(double vw, double vh);
	LayoutNode *root() const { return m_root; }
	//! The next pass builds and lays out everything anew (the fonts, the top layer, the viewport changed).
	void relayout_all() { m_full = true; }
	//! A layout that writes nothing to the document, laid out anew to check the kept one against (QDOM_CHECK_LAYOUT):
	//! the scrollbars each element started from in the pass checked.
	struct Bars
	{
		double x, y;
	};
	void run_shadow(double vw, double vh, const std::unordered_map<const Element *, Bars> &bars);
	//! The differences between two layouts of the same document, one line each (none when they agree).
	static std::vector<std::string> compare(const Layout &a, const Layout &b);
	//! The boxes in the pool and in use, for the tests.
	size_t boxes_in_use() const { return m_nodes.size() - m_free.size(); }

	//! How far a sticky box is moved to stay in view of the region that scrolls it.
	double stuck(const LayoutNode &n) const;

private:
	friend struct InlineLayout;
	friend struct FlexLayout;

	struct Edges
	{
		double bt, br, bb, bl, pt, pr, pb, pl;
		std::optional<double> mt, mr, mb, ml;
		double hx() const { return bl + br + pl + pr; }
		double vx() const { return bt + bb + pt + pb; }
	};
	struct Intrinsic
	{
		double min, max;
	};
	struct Content
	{
		double h;
		std::optional<double> base;
	};

	LayoutNode *build(Element *el, LayoutNode *parent);
	LayoutNode *make(LayoutNode::Kind kind, LayoutNode *parent);
	LayoutNode *anonymous(LayoutNode *container, LayoutNode *text);
	//! A pseudo-element's box from its style, or none.
	LayoutNode *pseudo(LayoutNode *n, const StylePtr &s);
	//! A text's box, among an element's.
	LayoutNode *text_box(LayoutNode *n, Text *t);
	//! The marked brought up to date under a box: its children built again where its element's are marked, the layouts
	//! of what changed forgotten. True when something under it (or it) changed.
	bool sync(LayoutNode &n);
	void rebuild_kids(LayoutNode &n);
	//! A box's layout forgotten (and, own: its texts' anonymous boxes' and pseudo-elements').
	void forget(LayoutNode &n, bool own);
	//! A box and what is in it back to the pool, their nodes forgetting them.
	void release(LayoutNode *n);

	Edges edges(const LayoutNode &n, std::optional<double> cbw) const;
	std::optional<double> size_of(const LayoutNode &n, const Length &l, std::optional<double> basis, const Edges &e,
		bool horizontal) const;
	double clamp_w(const LayoutNode &n, double w, std::optional<double> basis, const Edges &e) const;
	double clamp_h(const LayoutNode &n, double h, std::optional<double> basis, const Edges &e) const;

	bool replaced(const LayoutNode &n) const;
	struct ReplacedSize
	{
		double w, h;
		bool fill;
	};
	ReplacedSize replaced_size(const LayoutNode &n, const Edges &e);

	Intrinsic intrinsic(LayoutNode &n);
	//! The least and most widths: own_width false leaves its own width and min-width out (its content's).
	Intrinsic intrinsic_of(LayoutNode &n, bool own_width);
	//! The width its content needs at the narrowest (a flex item's automatic minimum).
	double content_min(LayoutNode &n);
	Intrinsic outer_intrinsic(LayoutNode &n);
	Intrinsic flow_intrinsic(LayoutNode &n);

	void layout_box(LayoutNode &n, double w, std::optional<double> h, std::optional<double> cbw,
		std::optional<double> cbh);
	double height_at(LayoutNode &n, double w, std::optional<double> cbw, std::optional<double> cbh);
	//! The border box's height its content gives it at a width, its own height, min-height and max-height left out (a
	//! column flex item's content size suggestion).
	double content_height(LayoutNode &n, double w, std::optional<double> cbw, std::optional<double> cbh);
	Content content(LayoutNode &n, const Edges &e, double cw, std::optional<double> ch, std::optional<double> hdef,
		double w);
	Content layout_flow(LayoutNode &n, double cw, std::optional<double> ch);
	void layout_block(LayoutNode &c, double cw, std::optional<double> ch, const Edges &e);
	void shift_kids(LayoutNode &n, double dx, double dy);
	void place_absolute(LayoutNode &cb);
	void place_one(LayoutNode &a, LayoutNode &cb, double W, double H, double ox, double oy);
	void place_fixed(LayoutNode &n);
	//! A box's border box from an ancestor's.
	void offset_to(const LayoutNode *n, const LayoutNode *anc, double &x, double &y) const;
	//! The items of a flex container: its children in flow, a text among them in an anonymous block.
	std::vector<LayoutNode *> flow_items(LayoutNode &n);
	Intrinsic flex_intrinsic(LayoutNode &n);
	Content layout_flex(LayoutNode &n, double cw, std::optional<double> ch);
	Intrinsic anon_intrinsic(LayoutNode &a);
	void adopt_anon(LayoutNode &container, LayoutNode &a);
	double scrollbar(const ComputedStyle &cs) const;

	// grid
	struct Track
	{
		enum class Kind : uint8_t { Fixed, Auto, MinContent, MaxContent, Fr };
		Kind kind;
		double v;
	};
	struct TrackSize
	{
		Track min, max;
	};
	struct Placed
	{
		LayoutNode *it;
		size_t r, c;
	};
	struct Grid
	{
		std::vector<LayoutNode *> items;
		std::vector<TrackSize> cols, rows;
		std::vector<Placed> place;
		bool stretch = false;
	};
	Track track_one(std::string_view x, std::optional<double> basis, double em) const;
	TrackSize track(std::string_view w, std::optional<double> basis, double em) const;
	std::vector<TrackSize> tracks(std::string_view s, std::optional<double> basis, double em) const;
	Grid grid_setup(LayoutNode &n, std::optional<double> cw);
	double contribution(LayoutNode &it, bool min);
	std::vector<double> size_columns(Grid &g, std::optional<double> cw, double gap);
	Intrinsic grid_intrinsic(LayoutNode &n);
	Content layout_grid(LayoutNode &n, double cw, std::optional<double> ch);

	// tables
	struct TableRow
	{
		LayoutNode *row;
		LayoutNode *group;
		std::vector<LayoutNode *> cells;
	};
	std::vector<TableRow> table_rows(LayoutNode &n);
	double spacing_of(const LayoutNode &n) const;
	Intrinsic table_intrinsic(LayoutNode &n);
	Content layout_table(LayoutNode &n, double cw);

	//! A style's font: its face, size and letter spacing; its line's height, the ascent and descent of its glyphs.
	struct FontInfo
	{
		FaceId face;
		double size, spacing, line, ascent, descent, normal, average;
	};
	FontInfo font(const ComputedStyle &cs);
	double measure(FaceId face, double size, double spacing, std::string_view s);

	//! An item of inline content: a word with the space after it, an atom (an inline block, a replaced
	//! element), a forced break, an inline element's edge (gap) or start (mark).
	struct InlineItem
	{
		enum class Kind : uint8_t { Text, Atom, Break, Gap, Mark, Static };
		Kind kind;
		std::string text;
		double w = 0, space = 0, min = 0;
		bool nowrap = false, break_after = false;
		const ComputedStyle *cs = nullptr;
		LayoutNode *node = nullptr;
		std::vector<LayoutNode *> owners;
		Edges e {};
	};
	void inline_items(const std::vector<LayoutNode *> &nodes, std::vector<InlineItem> &out);
	Intrinsic inline_intrinsic(const std::vector<LayoutNode *> &nodes);
	Content layout_inline(LayoutNode &el, const std::vector<LayoutNode *> &nodes, double cw, double y0,
		std::vector<TextPiece> &pieces);

	double atom_baseline(const LayoutNode &a);
	//! Whether a box's margins stay apart from its children's (it starts a block formatting context).
	bool bfc_root(const LayoutNode &n) const;
	bool out_of_flow(const LayoutNode &n) const;
	bool inline_level(const LayoutNode &n) const;
	bool blank(const LayoutNode &n) const;

	Dom &m_dom;
	std::deque<LayoutNode> m_nodes;
	std::vector<LayoutNode *> m_free;
	LayoutNode *m_root = nullptr;
	std::vector<LayoutNode *> m_top;
	double m_vw = 0, m_vh = 0;
	bool m_full = true;
	uint32_t m_pass = 0;
	//! A shadow layout's: no element written (their boxes and scrollbars are the real layout's), its own map.
	bool m_shadow = false;
	const LayoutNode *m_auto_height = nullptr;  //!< the box content_height lays out without its own height
	const std::unordered_map<const Element *, Bars> *m_bars = nullptr;
	std::unordered_map<const Element *, LayoutNode *> m_map;
};

constexpr double SCROLLBAR = 12;
constexpr double SCROLLBAR_THIN = 6;
constexpr double SELECT_ARROW = 18;

} // namespace qdom
