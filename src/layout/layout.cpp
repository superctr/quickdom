// Copyright 2026 Ian Karlsson
// The layout's core: the box tree, edges and sizes, replaced elements, intrinsic widths, a box laid out with its
// scrollbars, block flow with collapsing margins, absolutely placed and fixed boxes.
#include "layout/layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "dom/dom.h"
#include "text/fonts.h"

namespace qdom
{

Layout::Layout(Dom &dom) : m_dom(dom)
{
}

Layout::~Layout()
{
	// the elements outlive the boxes (the collector frees them later): they forget theirs
	if (m_root)
		release(m_root);
	for (LayoutNode *d : m_top)
		release(d);
}

// ---------------------------------------------------------------- the box tree

namespace
{

// an element whose children have no boxes of their own: a replaced element's are its own (an svg's items, a list's
// options, a field's text)
bool leaf(const Element *el)
{
	return el->ns() == Namespace::Svg || el->is(n::select) || el->is(n::input) || el->is(n::textarea) || el->is(n::canvas)
		|| el->is(n::img);
}

bool shown(const Element *c)
{
	return c->style && c->style->display != Display::None && !c->modal;
}

} // namespace

LayoutNode *Layout::make(LayoutNode::Kind kind, LayoutNode *parent)
{
	LayoutNode *n;
	if (!m_free.empty())
	{
		n = m_free.back();
		m_free.pop_back();
		*n = LayoutNode {};
	}
	else
	{
		m_nodes.emplace_back();
		n = &m_nodes.back();
	}
	n->kind = kind;
	n->parent = parent;
	return n;
}

LayoutNode *Layout::pseudo(LayoutNode *n, const StylePtr &s)
{
	if (!s || s->display == Display::None)
		return nullptr;
	LayoutNode *p = make(LayoutNode::Kind::Pseudo, n);
	p->style = s;
	p->content = s->content;
	if (!p->content.empty())
	{
		LayoutNode *t = make(LayoutNode::Kind::Text, p);
		t->style = s;
		t->data = &p->content;
		p->kids.push_back(t);
	}
	return p;
}

LayoutNode *Layout::text_box(LayoutNode *n, Text *t)
{
	LayoutNode *k = make(LayoutNode::Kind::Text, n);
	k->style = n->el->style;
	k->text = t;
	k->data = &t->data();
	if (!m_shadow)
		t->layout = k;
	return k;
}

LayoutNode *Layout::build(Element *el, LayoutNode *parent)
{
	LayoutNode *n = make(LayoutNode::Kind::Element, parent);
	n->el = el;
	n->style = el->style;
	if (m_shadow)
		m_map[el] = n;
	else
	{
		el->layout = n;
		el->layout_dirty = el->kids_dirty = el->layout_below = false;
	}
	const bool lf = leaf(el);
	if (el->ns() == Namespace::Html && !lf)
		if (LayoutNode *p = pseudo(n, el->before_style))
			n->kids.push_back(p);
	if (!lf)
		for (const Link &l : el->links())
		{
			if (Text *t = l.node->as_text())
				n->kids.push_back(text_box(n, t));
			else if (Element *c = l.node->as_element(); c && shown(c))
				n->kids.push_back(build(c, n));
		}
	if (el->ns() == Namespace::Html && !lf)
		if (LayoutNode *p = pseudo(n, el->after_style))
			n->kids.push_back(p);
	return n;
}

LayoutNode *Layout::anonymous(LayoutNode *container, LayoutNode *text)
{
	if (text->anon)
		return text->anon;
	LayoutNode *a = make(LayoutNode::Kind::Anonymous, container);
	a->style = m_dom.styles.anonymous(container->style);
	a->kids.push_back(text);
	text->anon = a;
	return a;
}

void Layout::release(LayoutNode *n)
{
	for (LayoutNode *k : n->kids)
		release(k);
	if (n->anon)
	{
		// an anonymous box holds the text that holds it
		n->anon->kids.clear();
		release(n->anon);
	}
	if (!m_shadow)
	{
		if (n->el && n->el->layout == n)
			n->el->layout = nullptr;
		if (n->text && n->text->layout == n)
			n->text->layout = nullptr;
	}
	*n = LayoutNode {};
	n->free = true;
	m_free.push_back(n);
}

void Layout::forget(LayoutNode &n, bool own)
{
	n.laid = false;
	n.iw_valid = false;
	n.heights.clear();
	n.auto_valid = false;
	if (!own)
		return;
	for (LayoutNode *k : n.kids)
	{
		if (k->kind == LayoutNode::Kind::Text && k->anon)
		{
			k->anon->style = m_dom.styles.anonymous(n.style);
			forget(*k->anon, false);
		}
		else if (k->kind == LayoutNode::Kind::Pseudo)
			forget(*k, true);
	}
}

void Layout::rebuild_kids(LayoutNode &n)
{
	Element *el = n.el;
	std::vector<LayoutNode *> now;
	// a box kept: an element's or a text's that was among these children
	auto kept = [&](LayoutNode *k) { return k && !k->free && k->parent == &n; };
	const bool lf = leaf(el);
	if (el->ns() == Namespace::Html && !lf)
		if (LayoutNode *p = pseudo(&n, el->before_style))
			now.push_back(p);
	if (!lf)
		for (const Link &l : el->links())
		{
			if (Text *t = l.node->as_text())
			{
				LayoutNode *k = t->layout;
				if (kept(k))
				{
					k->style = el->style;
					k->keep = true;
					now.push_back(k);
				}
				else
					now.push_back(text_box(&n, t));
			}
			else if (Element *c = l.node->as_element(); c && shown(c))
			{
				LayoutNode *k = c->layout;
				if (kept(k))
				{
					k->keep = true;
					now.push_back(k);
					sync(*k);
				}
				else
					now.push_back(build(c, &n));
			}
		}
	if (el->ns() == Namespace::Html && !lf)
		if (LayoutNode *p = pseudo(&n, el->after_style))
			now.push_back(p);
	for (LayoutNode *k : n.kids)
	{
		if (k->keep)
			k->keep = false;
		else
			release(k);
	}
	n.kids = std::move(now);
}

bool Layout::sync(LayoutNode &n)
{
	Element *el = n.el;
	if (!el)
		return true;
	const bool own = el->layout_dirty || el->kids_dirty;
	bool changed = own;
	if (el->kids_dirty)
	{
		el->kids_dirty = false;
		rebuild_kids(n);
	}
	else if (el->layout_below)
		for (LayoutNode *k : n.kids)
			if (k->kind == LayoutNode::Kind::Element && sync(*k))
				changed = true;
	el->layout_dirty = el->layout_below = false;
	if (changed)
		forget(n, own);
	return changed;
}

void Layout::run(double vw, double vh)
{
	m_pass++;
	if (vw != m_vw || vh != m_vh)
		m_full = true;
	m_vw = vw;
	m_vh = vh;
	Document *doc = m_dom.document();
	Element *html = doc->document_element();
	const bool on = html && html->style && html->style->display != Display::None;
	if (m_shadow || m_full || !m_root || !on || m_root->el != html)
	{
		// everything anew
		m_full = false;
		if (m_root)
			release(m_root);
		m_root = nullptr;
		for (LayoutNode *d : m_top)
			release(d);
		m_top.clear();
		if (!on)
			return;
		m_root = build(html, nullptr);
		for (const ElementRef &r : doc->top_layer())
			if (r.el->style && r.el->style->display != Display::None && r.el->connected())
				m_top.push_back(build(r.el, nullptr));
	}
	else
	{
		sync(*m_root);
		for (LayoutNode *d : m_top)
			sync(*d);
	}

	LayoutNode &root = *m_root;
	const Edges e = edges(root, vw);
	const std::optional<double> h = size_of(root, root.cs().height, vh, e, false);
	layout_box(root, vw - e.ml.value_or(0) - e.mr.value_or(0), h, vw, vh);
	root.x = e.ml.value_or(0);
	root.y = e.mt.value_or(0);
	// the fixed boxes in the viewport, then the dialogs above the page
	place_fixed(root);
	for (LayoutNode *d : m_top)
	{
		const Element *p = d->el ? d->el->parent_element() : nullptr;
		LayoutNode *pn = nullptr;
		if (p && m_shadow)
		{
			auto it = m_map.find(p);
			pn = it == m_map.end() ? nullptr : it->second;
		}
		else if (p)
			pn = p->layout;
		d->parent = pn;
		place_one(*d, root, vw, vh, 0, 0);
		double px, py;
		offset_to(d->parent, nullptr, px, py);
		d->fixed = true;
		d->fx = d->x + px;
		d->fy = d->y + py;
	}
}

std::vector<std::string> Layout::compare(const Layout &a, const Layout &b)
{
	std::vector<std::string> out;
	const Names &names = a.m_dom.names;
	auto name = [&](const LayoutNode *n) -> std::string {
		switch (n->kind)
		{
		case LayoutNode::Kind::Text: return "#text";
		case LayoutNode::Kind::Anonymous: return "#anonymous";
		case LayoutNode::Kind::Pseudo: return "::pseudo";
		default:
			if (!n->el)
				return "(freed)";
			std::string t(names.str(n->el->tag()));
			if (n->el->id())
				t += "#" + std::string(names.str(n->el->id()));
			for (Name c : n->el->classes())
				t += "." + std::string(names.str(c));
			return t;
		}
	};
	auto num = [](double v) {
		char t[32];
		snprintf(t, sizeof(t), "%.4f", v);
		return std::string(t);
	};
	auto walk = [&](const LayoutNode *p, const LayoutNode *q, const std::string &at, auto &self) -> void {
		if (out.size() > 64)
			return;
		if (!p || !q)
		{
			if (p != q)
				out.push_back(at + ": a box in " + (p ? "the kept layout" : "the new layout") + " only");
			return;
		}
		if (p->kind != q->kind || p->el != q->el)
		{
			out.push_back(at + ": " + name(p) + " kept, " + name(q) + " anew");
			return;
		}
		auto field = [&](const char *f, double x, double y) {
			if (std::fabs(x - y) > 1e-6)
				out.push_back(at + ": " + f + " " + num(x) + " kept, " + num(y) + " anew");
		};
		auto opt = [&](const char *f, const std::optional<double> &x, const std::optional<double> &y) {
			if (x.has_value() != y.has_value())
				out.push_back(at + ": " + f + (x ? " set kept, none anew" : " none kept, set anew"));
			else if (x)
				field(f, *x, *y);
		};
		field("laid", p->laid, q->laid);
		if (p->laid && q->laid)
		{
			field("x", p->x, q->x);
			field("y", p->y, q->y);
			field("w", p->w, q->w);
			field("h", p->h, q->h);
			field("bt", p->bt, q->bt);
			field("br", p->br, q->br);
			field("bb", p->bb, q->bb);
			field("bl", p->bl, q->bl);
			field("pt", p->pt, q->pt);
			field("pr", p->pr, q->pr);
			field("pb", p->pb, q->pb);
			field("pl", p->pl, q->pl);
			opt("mt", p->mt, q->mt);
			opt("mr", p->mr, q->mr);
			opt("mb", p->mb, q->mb);
			opt("ml", p->ml, q->ml);
			field("cw", p->cw, q->cw);
			field("ch", p->ch, q->ch);
			field("sw", p->sw, q->sw);
			field("sh", p->sh, q->sh);
			field("ow", p->ow, q->ow);
			field("oh", p->oh, q->oh);
			field("bar_x", p->bar_x, q->bar_x);
			field("bar_y", p->bar_y, q->bar_y);
			opt("base", p->base, q->base);
			field("inline_box", p->inline_box, q->inline_box);
			field("scrolls", p->scrolls, q->scrolls);
		}
		field("fixed", p->fixed, q->fixed);
		if (p->fixed && q->fixed)
		{
			field("fx", p->fx, q->fx);
			field("fy", p->fy, q->fy);
		}
		if ((p->kind == LayoutNode::Kind::Element || p->kind == LayoutNode::Kind::Pseudo) && p->style && p->style->out_of_flow())
		{
			field("static_x", p->static_x, q->static_x);
			field("static_y", p->static_y, q->static_y);
		}
		if (p->pieces.size() != q->pieces.size())
			out.push_back(at + ": " + std::to_string(p->pieces.size()) + " pieces of text kept, " + std::to_string(q->pieces.size())
				+ " anew");
		else
			for (size_t i = 0; i < p->pieces.size(); i++)
			{
				const TextPiece &x = p->pieces[i], &y = q->pieces[i];
				if (x.text != y.text || std::fabs(x.x - y.x) > 1e-6 || std::fabs(x.y - y.y) > 1e-6 || std::fabs(x.w - y.w) > 1e-6
					|| x.face != y.face || x.size != y.size || x.spacing != y.spacing)
					out.push_back(at + ": the text piece \"" + x.text + "\" at " + num(x.x) + "," + num(x.y) + " kept, \"" + y.text
						+ "\" at " + num(y.x) + "," + num(y.y) + " anew");
			}
		// an anonymous box's child is the text that holds it
		if (p->kind == LayoutNode::Kind::Anonymous)
			return;
		if (p->kids.size() != q->kids.size())
		{
			out.push_back(at + ": " + std::to_string(p->kids.size()) + " children kept, " + std::to_string(q->kids.size()) + " anew");
			return;
		}
		for (size_t i = 0; i < p->kids.size(); i++)
			self(p->kids[i], q->kids[i], at + "/" + name(p->kids[i]) + "[" + std::to_string(i) + "]", self);
		if (p->anon && q->anon)
			self(p->anon, q->anon, at + "/#anonymous", self);
	};
	walk(a.m_root, b.m_root, "", walk);
	if (a.m_top.size() != b.m_top.size())
		out.push_back("the top layer: " + std::to_string(a.m_top.size()) + " kept, " + std::to_string(b.m_top.size()) + " anew");
	else
		for (size_t i = 0; i < a.m_top.size(); i++)
			walk(a.m_top[i], b.m_top[i], "/(top layer " + std::to_string(i) + ")", walk);
	return out;
}

void Layout::run_shadow(double vw, double vh, const std::unordered_map<const Element *, Bars> &bars)
{
	m_shadow = true;
	m_bars = &bars;
	m_map.clear();
	run(vw, vh);
}

// ---------------------------------------------------------------- helpers

bool Layout::out_of_flow(const LayoutNode &n) const
{
	return (n.kind == LayoutNode::Kind::Element || n.kind == LayoutNode::Kind::Pseudo) && n.cs().out_of_flow();
}

bool Layout::inline_level(const LayoutNode &n) const
{
	if (n.kind == LayoutNode::Kind::Text)
		return true;
	if (n.kind == LayoutNode::Kind::Anonymous)
		return false;
	const Display d = n.cs().display;
	return d == Display::Inline || d == Display::InlineBlock || d == Display::InlineFlex || d == Display::InlineGrid
		|| d == Display::InlineTable;
}

bool Layout::blank(const LayoutNode &n) const
{
	if (n.kind != LayoutNode::Kind::Text)
		return false;
	const WhiteSpace ws = n.cs().white_space;
	if (ws == WhiteSpace::Pre || ws == WhiteSpace::PreWrap || ws == WhiteSpace::BreakSpaces)
		return false;
	for (char c : *n.data)
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f')
			return false;
	return true;
}

bool Layout::replaced(const LayoutNode &n) const
{
	if (n.kind != LayoutNode::Kind::Element)
		return false;
	const Element *el = n.el;
	return (el->ns() == Namespace::Svg && el->tag() == n::svg) || el->is(n::canvas) || el->is(n::img) || el->is(n::input)
		|| el->is(n::select) || el->is(n::textarea);
}

double Layout::scrollbar(const ComputedStyle &cs) const
{
	return cs.scrollbar_width == ScrollbarWidth::Thin ? SCROLLBAR_THIN
		: cs.scrollbar_width == ScrollbarWidth::None ? 0 : SCROLLBAR;
}

Layout::FontInfo Layout::font(const ComputedStyle &cs)
{
	const FontMetrics m = m_dom.fonts.metrics(cs.face, cs.font_size);
	double line;
	switch (cs.line_height.kind)
	{
	case LineHeight::Kind::Normal: line = m.normal; break;
	case LineHeight::Kind::Number: line = cs.line_height.value * cs.font_size; break;
	default: line = cs.line_height.value; break;
	}
	return { cs.face, cs.font_size, cs.letter_spacing, line, m.ascent, m.descent, m.normal, m.average };
}

double Layout::measure(FaceId face, double size, double spacing, std::string_view s)
{
	return m_dom.fonts.width(face, size, spacing, s);
}

Layout::Edges Layout::edges(const LayoutNode &n, std::optional<double> cbw) const
{
	const ComputedStyle &cs = n.cs();
	Edges e {};
	if (n.kind == LayoutNode::Kind::Text)
		return e;
	e.bt = cs.border_width[TOP];
	e.br = cs.border_width[RIGHT];
	e.bb = cs.border_width[BOTTOM];
	e.bl = cs.border_width[LEFT];
	auto p = [&](int s) { return cs.padding[s].resolve(cbw).value_or(0); };
	auto m = [&](int s) -> std::optional<double> {
		if (cs.margin[s].is_auto())
			return std::nullopt;
		return cs.margin[s].resolve(cbw).value_or(0);
	};
	e.pt = p(TOP);
	e.pr = p(RIGHT);
	e.pb = p(BOTTOM);
	e.pl = p(LEFT);
	e.mt = m(TOP);
	e.mr = m(RIGHT);
	e.mb = m(BOTTOM);
	e.ml = m(LEFT);
	return e;
}

std::optional<double> Layout::size_of(const LayoutNode &n, const Length &l, std::optional<double> basis, const Edges &e,
	bool horizontal) const
{
	if (!l.is_value())
		return std::nullopt;
	std::optional<double> v = l.resolve(basis);
	if (!v)
		return std::nullopt;
	if (n.cs().box_sizing == BoxSizing::BorderBox)
		return *v;
	return *v + (horizontal ? e.hx() : e.vx());
}

double Layout::clamp_w(const LayoutNode &n, double w, std::optional<double> basis, const Edges &e) const
{
	if (std::optional<double> mx = size_of(n, n.cs().max_width, basis, e, true))
		w = std::min(w, *mx);
	if (std::optional<double> mn = size_of(n, n.cs().min_width, basis, e, true))
		w = std::max(w, *mn);
	return std::max(w, e.hx());
}

double Layout::clamp_h(const LayoutNode &n, double h, std::optional<double> basis, const Edges &e) const
{
	if (std::optional<double> mx = size_of(n, n.cs().max_height, basis, e, false))
		h = std::min(h, *mx);
	if (std::optional<double> mn = size_of(n, n.cs().min_height, basis, e, false))
		h = std::max(h, *mn);
	return std::max(h, e.vx());
}

// ---------------------------------------------------------------- replaced elements

Layout::ReplacedSize Layout::replaced_size(const LayoutNode &n, const Edges &e)
{
	const Element *el = n.el;
	const ComputedStyle &cs = n.cs();
	if (el->ns() == Namespace::Svg || el->is(n::canvas) || el->is(n::img))
	{
		const std::string *aw = el->attribute(n::width), *ah = el->attribute(n::height);
		std::optional<double> w, h;
		if (aw)
			if (std::optional<double> v = parse_number(*aw))
				w = *v;
		if (ah)
			if (std::optional<double> v = parse_number(*ah))
				h = *v;
		if (el->is(n::canvas))
		{
			w = w.value_or(300);
			h = h.value_or(150);
		}
		std::vector<double> vb;
		if (const std::string *v = el->attribute(n::viewBox))
		{
			std::string t = *v;
			std::replace(t.begin(), t.end(), ',', ' ');
			for (std::string_view x : split_words(t))
				vb.push_back(parse_number(x).value_or(0));
		}
		if (!w && h && vb.size() == 4 && vb[3] != 0)
			w = *h * vb[2] / vb[3];
		if (!h && w && vb.size() == 4 && vb[2] != 0)
			h = *w * vb[3] / vb[2];
		if (el->is(n::img))
		{
			// its picture's natural size, and its ratio for a missing side; nothing until it has loaded
			const Picture *p = m_dom.picture_of(el);
			if (p && p->state == Picture::State::Ready && p->width > 0 && p->height > 0)
			{
				if (!w && !h)
				{
					w = p->width;
					h = p->height;
				}
				else if (!h)
					h = *w * p->height / p->width;
				else if (!w)
					w = *h * p->width / p->height;
			}
			else if (!w)
				w = h = 0;
		}
		return { w.value_or(300), h.value_or(150), el->ns() == Namespace::Svg && !aw };
	}
	const FontInfo f = font(cs);
	const std::string *type = el->attribute(n::type);
	if (el->is(n::input) && type && lower(*type) == "range")
		return { 12 * f.size, 20, false };
	// a text field as many average characters wide as its size, a proportional font's with two more less four
	// pixels; never shorter than the font's normal line
	if (el->is(n::input) || el->is(n::textarea))
	{
		const double cw = f.average;
		const bool mono = m_dom.fonts.metrics(cs.face, cs.font_size).average > 0
			&& lower(cs.font_family).find("mono") != std::string::npos;
		const double size = el->is(n::textarea) ? el->number_attribute(n::cols, 20) : el->number_attribute(n::size, 20);
		const double w = size * cw + (mono ? 1.0 / 60 : 2 * cw - 4);
		double h = std::max(f.line, f.normal);
		if (el->is(n::textarea))
			h = el->number_attribute(n::rows, 2) * f.line;
		return { w + e.hx(), h + e.vx(), false };
	}
	if (el->is(n::select))
	{
		double w = 0;
		for (const Element *o : m_dom.options(el))
			w = std::max(w, measure(f.face, f.size, f.spacing, o->text_content()));
		return { w + e.hx() + SELECT_ARROW, f.line + e.vx(), false };
	}
	return { 0, 0, false };
}

// ---------------------------------------------------------------- intrinsic widths

Layout::Intrinsic Layout::intrinsic(LayoutNode &n)
{
	if (n.iw_valid)
		return { n.iw_min, n.iw_max };
	const Intrinsic i = intrinsic_of(n, true);
	n.iw_valid = true;
	n.iw_min = i.min;
	n.iw_max = i.max;
	return i;
}

double Layout::content_min(LayoutNode &n)
{
	return intrinsic_of(n, false).min;
}

Layout::Intrinsic Layout::intrinsic_of(LayoutNode &n, bool own_width)
{
	const Edges e = edges(n, std::nullopt);
	const ComputedStyle &cs = n.cs();
	double mn, mx;
	std::optional<double> fixed;
	if (own_width)
		fixed = size_of(n, cs.width, std::nullopt, e, true);
	if (fixed)
		mn = mx = *fixed;
	else if (replaced(n))
	{
		const ReplacedSize r = replaced_size(n, e);
		const double min_w = size_of(n, cs.min_width, std::nullopt, e, true).value_or(0);
		mn = mx = r.fill ? min_w : r.w;
	}
	else
	{
		Intrinsic c;
		if (n.kind == LayoutNode::Kind::Anonymous)
			c = inline_intrinsic(n.kids);
		else if (cs.is_flex())
			c = flex_intrinsic(n);
		else if (cs.is_grid())
			c = grid_intrinsic(n);
		else if (cs.display == Display::Table || cs.display == Display::InlineTable)
			c = table_intrinsic(n);
		else
			c = flow_intrinsic(n);
		mn = c.min + e.hx();
		mx = c.max + e.hx();
	}
	if (std::optional<double> v = size_of(n, cs.max_width, std::nullopt, e, true))
	{
		mn = std::min(mn, *v);
		mx = std::min(mx, *v);
	}
	if (std::optional<double> v = size_of(n, cs.min_width, std::nullopt, e, true); v && own_width)
	{
		mn = std::max(mn, *v);
		mx = std::max(mx, *v);
	}
	return { mn, std::max(mn, mx) };
}

Layout::Intrinsic Layout::outer_intrinsic(LayoutNode &n)
{
	const Intrinsic i = intrinsic(n);
	const Edges e = edges(n, std::nullopt);
	const double m = e.ml.value_or(0) + e.mr.value_or(0);
	return { i.min + m, i.max + m };
}

Layout::Intrinsic Layout::flow_intrinsic(LayoutNode &n)
{
	double mn = 0, mx = 0;
	std::vector<LayoutNode *> run;
	auto flush = [&] {
		if (run.empty())
			return;
		const Intrinsic r = inline_intrinsic(run);
		mn = std::max(mn, r.min);
		mx = std::max(mx, r.max);
		run.clear();
	};
	for (LayoutNode *c : n.kids)
	{
		if (out_of_flow(*c))
			continue;
		if (inline_level(*c))
		{
			run.push_back(c);
			continue;
		}
		flush();
		const Intrinsic o = outer_intrinsic(*c);
		mn = std::max(mn, o.min);
		mx = std::max(mx, o.max);
	}
	flush();
	return { mn, mx };
}

// ---------------------------------------------------------------- a box

void Layout::layout_box(LayoutNode &n, double w, std::optional<double> h, std::optional<double> cbw,
	std::optional<double> cbh)
{
	const ComputedStyle &cs = n.cs();
	// the containing block reaches the layout through the box's own heights (its height) and its padding and
	// margins (its width) alone, where they are percentages
	const bool heights = cs.height.has_percent() || cs.min_height.has_percent() || cs.max_height.has_percent();
	bool edges_pct = false;
	for (int s = 0; s < 4; s++)
		edges_pct = edges_pct || cs.padding[s].has_percent() || cs.margin[s].has_percent();
	const LayoutNode::Key key { w, h, edges_pct ? cbw : std::nullopt, heights ? cbh : std::nullopt };
	if (n.laid && n.key == key)
	{
		n.pass = m_pass;
		return;
	}
	// laid out again at the height its content gave it: the same layout
	if (h && n.auto_valid && n.laid && n.key == n.auto_key && n.auto_key.w == w && n.auto_key.cbw == key.cbw
		&& n.auto_key.cbh == key.cbh && std::fabs(n.auto_h - *h) < 0.001)
	{
		n.pass = m_pass;
		return;
	}

	const Edges e = edges(n, cbw);
	if (!m_shadow)
		m_dom.stats.boxes++;
	n.laid = true;
	n.pass = m_pass;
	n.key = key;
	n.cell_dy = 0;
	n.w = w;
	n.h = 0;
	n.bt = e.bt;
	n.br = e.br;
	n.bb = e.bb;
	n.bl = e.bl;
	n.pt = e.pt;
	n.pr = e.pr;
	n.pb = e.pb;
	n.pl = e.pl;
	n.mt = e.mt;
	n.mr = e.mr;
	n.mb = e.mb;
	n.ml = e.ml;
	n.base.reset();
	n.pieces.clear();
	n.scrolls = cs.scrolls();
	n.escapes_top = n.escapes_bottom = n.through = false;

	const bool own_height = &n != m_auto_height;
	std::optional<double> hdef = h ? h : own_height ? size_of(n, cs.height, cbh, e, false) : std::nullopt;
	if (hdef)
		hdef = clamp_h(n, *hdef, cbh, e);
	// a region that scrolls gives its scrollbars room, each once its content overflows; it starts from the bars
	// its last layout had, as the browser does, and changes them only where they no longer fit
	const Overflow ox = cs.overflow_x, oy = cs.overflow_y;
	const double bar = scrollbar(cs);
	double had_x = n.el ? n.el->last_bar_x : 0, had_y = n.el ? n.el->last_bar_y : 0;
	if (m_shadow && n.el)
		if (auto it = m_bars->find(n.el); it != m_bars->end())
		{
			had_x = it->second.x;
			had_y = it->second.y;
		}
	// scrollbar-gutter: stable keeps the vertical bar's room whenever the box may scroll
	const bool gutter = cs.scrollbar_gutter && (oy == Overflow::Auto || oy == Overflow::Hidden || oy == Overflow::Scroll);
	double bar_y = oy == Overflow::Scroll || gutter || (oy == Overflow::Auto && had_y > 0) ? bar : 0;
	double bar_x = ox == Overflow::Scroll || (ox == Overflow::Auto && had_x > 0) ? bar : 0;
	Content r { 0, std::nullopt };
	double hh = 0, sw = 0, sh = 0;
	const bool sized = (cs.is_flex() && !cs.row()) || cs.is_grid();
	for (int k = 0; k < 4; k++)
	{
		n.pieces.clear();
		const double cw = std::max(0.0, w - e.hx() - bar_y);
		std::optional<double> ch_def;
		if (hdef)
			ch_def = std::max(0.0, *hdef - e.vx() - bar_x);
		r = content(n, e, cw, ch_def, hdef, w);
		hh = hdef ? *hdef : own_height ? clamp_h(n, r.h + e.vx() + bar_x, cbh, e) : r.h + e.vx() + bar_x;
		// a column its min-height or max-height holds: laid out again in that height
		if (!hdef && sized && std::fabs(hh - (r.h + e.vx() + bar_x)) > 0.01)
		{
			hdef = hh;
			continue;
		}
		// what overflows: the children's far edges and the text's, with the padding after them
		sw = 0;
		sh = r.h;
		for (LayoutNode *c : n.kids)
		{
			// the boxes in flow (a box placed by its containing block comes after: its place is not this one's yet)
			if (c->kind != LayoutNode::Kind::Element || !c->laid || c->pass != m_pass || out_of_flow(*c))
				continue;
			// a child's own overflow too, unless it clips it
			sw = std::max(sw, c->x + std::max(c->w, c->ow) + c->mr.value_or(0));
			sh = std::max(sh, c->y + std::max(c->h, c->oh) + c->mb.value_or(0));
		}
		for (const TextPiece &p : n.pieces)
			sw = std::max(sw, p.x + p.w);
		const bool keep_y = !hdef && oy == Overflow::Auto && had_y > 0;
		const double want_y = oy == Overflow::Scroll || gutter || keep_y
			|| (oy == Overflow::Auto && sh + e.pt + e.pb > hh - e.bt - e.bb - bar_x + 0.5) ? bar : 0;
		const double want_x = ox == Overflow::Scroll
			|| (ox == Overflow::Auto && sw + e.pl + e.pr > w - e.bl - e.br - want_y + 0.5) ? bar : 0;
		if (want_y == bar_y && want_x == bar_x)
			break;
		bar_y = want_y;
		bar_x = want_x;
	}
	n.h = hh;
	if (r.base)
		n.base = *r.base + e.bt + e.pt;
	// a button's content stands in its middle
	if (n.el && n.el->is(n::button) && !cs.is_flex() && !cs.is_grid())
	{
		const double extra = hh - e.vx() - r.h;
		if (extra > 0)
		{
			for (TextPiece &p : n.pieces)
				p.y += extra / 2;
			for (LayoutNode *c : n.kids)
				if (c->kind != LayoutNode::Kind::Text && c->laid && !out_of_flow(*c))
					c->y += extra / 2;
			if (n.base)
				*n.base += extra / 2;
		}
	}
	n.bar_x = bar_x;
	n.bar_y = bar_y;
	if (n.el && !m_shadow)
	{
		n.el->last_bar_x = bar_x;
		n.el->last_bar_y = bar_y;
	}
	n.cw = std::max(0.0, w - e.bl - e.br - bar_y);
	n.ch = std::max(0.0, hh - e.bt - e.bb - bar_x);
	n.sw = sw + e.pl + e.pr;
	n.sh = sh + e.pt + e.pb;
	// how far its content reaches past it, for a region scrolling it
	const bool clips = cs.clips();
	n.ow = clips ? n.w : std::max(n.w, sw + e.bl + e.pl);
	n.oh = clips ? n.h : std::max(n.h, sh + e.bt + e.pt);
	n.content_h = r.h;
	// the children were laid out at the content box's origin: moved to the border box's
	shift_kids(n, e.bl + e.pl, e.bt + e.pt);
	if (cs.position != Position::Static || &n == m_root)
		place_absolute(n);
	if (!h)
	{
		n.auto_valid = true;
		n.auto_key = key;
		n.auto_h = n.h;
	}
}

double Layout::height_at(LayoutNode &n, double w, std::optional<double> cbw, std::optional<double> cbh)
{
	const ComputedStyle &cs = n.cs();
	const bool heights = cs.height.has_percent() || cs.min_height.has_percent() || cs.max_height.has_percent();
	bool edges_pct = false;
	for (int s = 0; s < 4; s++)
		edges_pct = edges_pct || cs.padding[s].has_percent() || cs.margin[s].has_percent();
	const LayoutNode::Key key { w, std::nullopt, edges_pct ? cbw : std::nullopt, heights ? cbh : std::nullopt };
	if (n.laid && n.key == key)
		return n.h;
	for (const auto &[k, v] : n.heights)
		if (k == key)
			return v;
	layout_box(n, w, std::nullopt, cbw, cbh);
	n.heights.emplace_back(key, n.h);
	return n.h;
}

double Layout::content_height(LayoutNode &n, double w, std::optional<double> cbw, std::optional<double> cbh)
{
	// kept among its heights under a key no layout has (a height of -1); the box is laid out again at its own height
	// when it is next laid out
	const LayoutNode::Key key { w, -1.0, cbw, cbh };
	for (const auto &[k, v] : n.heights)
		if (k == key)
			return v;
	const LayoutNode *was = m_auto_height;
	m_auto_height = &n;
	n.laid = false;
	layout_box(n, w, std::nullopt, cbw, cbh);
	m_auto_height = was;
	n.laid = false;
	n.auto_valid = false;
	n.heights.emplace_back(key, n.h);
	return n.h;
}

Layout::Content Layout::content(LayoutNode &n, const Edges &e, double cw, std::optional<double> ch,
	std::optional<double> hdef, double w)
{
	if (replaced(n))
	{
		const ReplacedSize rs = replaced_size(n, e);
		double h;
		if (hdef)
			h = *hdef;
		else
		{
			const Element *el = n.el;
			const bool picture = el->ns() == Namespace::Svg || el->is(n::canvas) || el->is(n::img);
			h = picture ? (rs.fill ? rs.h : rs.w ? rs.h * (w / rs.w) : rs.h) : rs.h;
			if (el->ns() == Namespace::Svg && !rs.fill && el->has_attribute(n::height))
				h = el->number_attribute(n::height, h);
		}
		return { std::max(0.0, h - e.vx()), std::nullopt };
	}
	const ComputedStyle &cs = n.cs();
	if (cs.is_flex())
		return layout_flex(n, cw, ch);
	if (cs.is_grid())
		return layout_grid(n, cw, ch);
	if (cs.display == Display::Table || cs.display == Display::InlineTable)
		return layout_table(n, cw);
	return layout_flow(n, cw, ch);
}

void Layout::shift_kids(LayoutNode &n, double dx, double dy)
{
	for (LayoutNode *c : n.kids)
	{
		if (c->kind == LayoutNode::Kind::Text)
			continue;
		if (out_of_flow(*c))
		{
			c->static_x += dx;
			c->static_y += dy;
			continue;
		}
		if (!c->laid)
			continue;
		c->x += dx;
		c->y += dy;
	}
	for (TextPiece &p : n.pieces)
	{
		p.x += dx;
		p.y += dy;
	}
}

// ---------------------------------------------------------------- block flow

bool Layout::bfc_root(const LayoutNode &n) const
{
	if (&n == m_root || n.kind == LayoutNode::Kind::Anonymous)
		return true;
	const ComputedStyle &cs = n.cs();
	if (cs.out_of_flow() || replaced(n) || (n.el && n.el->is(n::button)))
		return true;
	switch (cs.display)
	{
	case Display::Block: case Display::ListItem: case Display::Inline: break;
	default: return true;
	}
	if ((cs.overflow_x != Overflow::Visible && cs.overflow_x != Overflow::Clip)
		|| (cs.overflow_y != Overflow::Visible && cs.overflow_y != Overflow::Clip))
		return true;
	return n.parent && (n.parent->cs().is_flex() || n.parent->cs().is_grid());
}

Layout::Content Layout::layout_flow(LayoutNode &n, double cw, std::optional<double> ch)
{
	n.pieces.clear();
	// the out-of-flow boxes that were inline stand in the lines, where they would have been
	auto in_line = [&](const LayoutNode *c) { return !out_of_flow(*c) || c->cs().inline_static; };
	std::vector<LayoutNode *> flow;
	for (LayoutNode *c : n.kids)
		if (in_line(c))
			flow.push_back(c);
	if (!flow.empty() && std::all_of(flow.begin(), flow.end(), [&](LayoutNode *c) { return out_of_flow(*c) || inline_level(*c); }))
	{
		for (LayoutNode *c : n.kids)
			if (out_of_flow(*c))
				c->static_x = c->static_y = 0;
		const Content r = layout_inline(n, flow, cw, 0, n.pieces);
		return r;
	}
	// margins collapse between siblings, with the parent's where nothing stands between them, and through
	// empty boxes
	const ComputedStyle &cs = n.cs();
	const bool root = bfc_root(n);
	const bool zero_min = !cs.min_height.is_value() || cs.min_height.resolve(0).value_or(1) == 0;
	const bool top_open = !root && n.bt == 0 && n.pt == 0;
	const bool bottom_open = !root && n.bb == 0 && n.pb == 0 && !ch && cs.height.is_auto() && zero_min;
	double y = 0;
	Margins pending;
	bool content = false;
	std::optional<Margins> escaped;
	std::optional<double> base;
	std::vector<LayoutNode *> run;
	auto flush = [&] {
		const bool all_blank = std::all_of(run.begin(), run.end(), [&](LayoutNode *c) {
			return blank(*c) || out_of_flow(*c);
		});
		if (!all_blank)
		{
			if (!content && top_open)
				escaped = pending;
			else
				y += pending.value();
			pending = Margins();
			std::vector<TextPiece> pieces;
			const Content r = layout_inline(n, run, cw, y, pieces);
			for (TextPiece &p : pieces)
				n.pieces.push_back(std::move(p));
			if (!base && r.base)
				base = r.base;
			y += r.h;
			content = true;
		}
		else
			for (LayoutNode *c : run)
				if (out_of_flow(*c))
				{
					c->static_x = 0;
					c->static_y = y;
				}
		run.clear();
	};
	for (LayoutNode *c : n.kids)
	{
		if (out_of_flow(*c) && !c->cs().inline_static)
		{
			c->static_x = 0;
			c->static_y = y;
			continue;
		}
		if (out_of_flow(*c) || inline_level(*c))
		{
			run.push_back(c);
			continue;
		}
		flush();
		const Edges e = edges(*c, cw);
		layout_block(*c, cw, ch, e);
		Margins top, bottom;
		top.add(e.mt.value_or(0));
		if (c->escapes_top)
			top.add(c->escape_top);
		bottom.add(e.mb.value_or(0));
		if (c->escapes_bottom)
			bottom.add(c->escape_bottom);
		if (c->through)
		{
			// an empty box: where it would be with a bottom border, its margins collapsing on through it
			Margins m = pending;
			m.add(top);
			c->y = !content && top_open ? y : y + m.value();
			pending = m;
			pending.add(bottom);
			continue;
		}
		pending.add(top);
		if (!content && top_open)
			escaped = pending;
		else
			y += pending.value();
		c->y = y;
		content = true;
		if (!base && c->base)
			base = y + *c->base;
		y += c->h;
		pending = bottom;
	}
	flush();
	n.escapes_top = n.escapes_bottom = false;
	n.through = false;
	if (!content)
	{
		// nothing in it: its children's margins all collapse with its own
		n.through = top_open && bottom_open;
		n.escapes_top = top_open;
		n.escape_top = pending;
		if (!top_open)
			y += pending.value();
	}
	else
	{
		if (escaped)
		{
			n.escapes_top = true;
			n.escape_top = *escaped;
		}
		if (bottom_open)
		{
			n.escapes_bottom = true;
			n.escape_bottom = pending;
		}
		else
			y += pending.value();
	}
	return { y, base };
}

void Layout::layout_block(LayoutNode &c, double cw, std::optional<double> ch, const Edges &e)
{
	const ComputedStyle &cs = c.cs();
	std::optional<double> w = size_of(c, cs.width, cw, e, true);
	const bool shrink = cs.display == Display::Table || cs.display == Display::InlineTable || replaced(c)
		|| cs.width.kind == Length::Kind::FitContent;
	if (!w && (cs.width.kind == Length::Kind::MaxContent || cs.width.kind == Length::Kind::MinContent))
		w = cs.width.kind == Length::Kind::MaxContent ? intrinsic(c).max : intrinsic(c).min;
	if (!w)
	{
		if (replaced(c))
		{
			const ReplacedSize r = replaced_size(c, e);
			w = r.fill ? cw - e.ml.value_or(0) - e.mr.value_or(0) : r.w;
		}
		else if (shrink)
		{
			const Intrinsic i = intrinsic(c);
			w = std::min(std::max(i.min, cw - e.ml.value_or(0) - e.mr.value_or(0)), i.max);
		}
		else
			w = cw - e.ml.value_or(0) - e.mr.value_or(0);
	}
	const double ww = clamp_w(c, *w, cw, e);
	layout_box(c, ww, std::nullopt, cw, ch);
	// auto margins centre it
	double x = e.ml.value_or(0);
	if (!e.ml && !e.mr)
		x = (cw - ww) / 2;
	else if (!e.ml)
		x = cw - ww - e.mr.value_or(0);
	c.x = x;
}

// ---------------------------------------------------------------- absolutely placed boxes

void Layout::offset_to(const LayoutNode *n, const LayoutNode *anc, double &x, double &y) const
{
	x = y = 0;
	for (const LayoutNode *e = n; e && e != anc; e = e->parent)
	{
		if (!e->laid)
			break;
		x += e->x;
		y += e->y;
	}
}

void Layout::place_absolute(LayoutNode &cb)
{
	std::vector<LayoutNode *> found;
	auto walk = [&](LayoutNode &n, auto &self) -> void {
		for (LayoutNode *c : n.kids)
		{
			if (c->kind == LayoutNode::Kind::Text || c->kind == LayoutNode::Kind::Anonymous)
				continue;
			const Position p = c->cs().position;
			if (p == Position::Absolute)
				found.push_back(c);
			if (p == Position::Static)
				self(*c, self);
		}
	};
	walk(cb, walk);
	const double W = cb.w - cb.bl - cb.br, H = cb.h - cb.bt - cb.bb;
	for (LayoutNode *a : found)
		place_one(*a, cb, W, H, cb.bl, cb.bt);
}

void Layout::place_one(LayoutNode &a, LayoutNode &cb, double W, double H, double ox, double oy)
{
	const ComputedStyle &cs = a.cs();
	const Edges e = edges(a, W);
	const std::optional<double> L = cs.inset[LEFT].resolve(W), R = cs.inset[RIGHT].resolve(W);
	const std::optional<double> T = cs.inset[TOP].resolve(H), B = cs.inset[BOTTOM].resolve(H);
	std::optional<double> w = size_of(a, cs.width, W, e, true);
	if (!w && (cs.width.kind == Length::Kind::MaxContent || cs.width.kind == Length::Kind::MinContent))
		w = cs.width.kind == Length::Kind::MaxContent ? intrinsic(a).max : intrinsic(a).min;
	if (!w)
	{
		if (L && R && cs.width.kind != Length::Kind::FitContent)
			w = W - *L - *R - e.ml.value_or(0) - e.mr.value_or(0);
		else if (replaced(a))
			w = replaced_size(a, e).w;
		else
		{
			const Intrinsic i = intrinsic(a);
			const double avail = W - L.value_or(0) - R.value_or(0) - e.ml.value_or(0) - e.mr.value_or(0);
			w = std::min(std::max(i.min, avail), i.max);
		}
	}
	const double ww = clamp_w(a, *w, W, e);
	std::optional<double> h = size_of(a, cs.height, H, e, false);
	if (!h && T && B && cs.height.kind != Length::Kind::FitContent)
		h = H - *T - *B - e.mt.value_or(0) - e.mb.value_or(0);
	if (h)
		h = clamp_h(a, *h, H, e);
	layout_box(a, ww, h, W, H);
	const double hh = a.h;
	// where it would have been, from its parent's flow
	double px, py;
	offset_to(a.parent, &cb, px, py);
	double x, y;
	if (L && R && !e.ml && !e.mr)
		x = *L + (W - *L - *R - ww) / 2;
	else if (L)
		x = *L + e.ml.value_or(0);
	else if (R)
		x = W - *R - e.mr.value_or(0) - ww;
	else
		x = px - ox + a.static_x + e.ml.value_or(0);
	if (T && B && !e.mt && !e.mb)
		y = *T + (H - *T - *B - hh) / 2;
	else if (T)
		y = *T + e.mt.value_or(0);
	else if (B)
		y = H - *B - e.mb.value_or(0) - hh;
	else
		y = py - oy + a.static_y + e.mt.value_or(0);
	// relative to its parent's border box
	a.x = x + ox - px;
	a.y = y + oy - py;
}

void Layout::place_fixed(LayoutNode &n)
{
	for (LayoutNode *c : n.kids)
	{
		if (c->kind == LayoutNode::Kind::Text || c->kind == LayoutNode::Kind::Anonymous)
			continue;
		if (c->cs().position == Position::Fixed)
		{
			place_one(*c, *m_root, m_vw, m_vh, 0, 0);
			// in the viewport, wherever its ancestors scroll
			double px, py;
			offset_to(c->parent, nullptr, px, py);
			c->fixed = true;
			c->fx = c->x + px;
			c->fy = c->y + py;
		}
		place_fixed(*c);
	}
}

double Layout::stuck(const LayoutNode &n) const
{
	const std::optional<double> t = n.cs().inset[TOP].resolve(std::nullopt);
	if (!t || !n.parent)
		return 0;
	const LayoutNode *sc = n.parent;
	double y = n.y;
	while (sc && !sc->scrolls)
	{
		y += sc->y;
		sc = sc->parent;
	}
	if (!sc || !sc->el)
		return 0;
	// held inside the scrolling box's padding
	const double at = y - sc->bt - sc->pt;
	const double want = sc->el->scroll_y + *t - at;
	if (want <= 0)
		return 0;
	const LayoutNode &p = *n.parent;
	const double room = p.h - p.bb - p.pb - (n.y + n.h + n.mb.value_or(0));
	return std::max(0.0, std::min(want, room));
}

double Layout::atom_baseline(const LayoutNode &a)
{
	const std::string *type = a.el && a.el->is(n::input) ? a.el->attribute(n::type) : nullptr;
	const bool field = a.el && ((a.el->is(n::input) && (!type || (lower(*type) != "range" && lower(*type) != "checkbox"
		&& lower(*type) != "radio"))) || a.el->is(n::select));
	if (replaced(a) && field)
	{
		// the text stands in the middle of the content box
		const FontInfo f = font(a.cs());
		const double content = a.h - a.bt - a.bb - a.pt - a.pb;
		return a.bt + a.pt + (content - f.ascent - f.descent) / 2 + f.ascent;
	}
	// a replaced box, or one with no line or that clips, sits on its bottom margin edge
	if (replaced(a) || !a.base || a.cs().clips())
		return a.h + a.mb.value_or(0);
	return *a.base;
}

} // namespace qdom
