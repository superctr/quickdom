// Copyright 2026 Ian Karlsson
// Inline content: words, atoms and breaks laid out in lines, with wrapping, baselines and alignment, and the
// boxes of the inline elements as the union of their fragments.
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "dom/dom.h"
#include "layout/layout.h"

namespace qdom
{

namespace
{

bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

uint32_t decode(const std::string &s, size_t &i)
{
	const unsigned char c = static_cast<unsigned char>(s[i++]);
	if (c < 0x80)
		return c;
	int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
	uint32_t cp = c & (0x3fu >> more);
	while (more-- && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80)
		cp = cp << 6 | (static_cast<unsigned char>(s[i++]) & 0x3f);
	return cp;
}

void encode(uint32_t cp, std::string &out)
{
	if (cp < 0x80)
		out += char(cp);
	else if (cp < 0x800)
	{
		out += char(0xc0 | cp >> 6);
		out += char(0x80 | (cp & 0x3f));
	}
	else if (cp < 0x10000)
	{
		out += char(0xe0 | cp >> 12);
		out += char(0x80 | ((cp >> 6) & 0x3f));
		out += char(0x80 | (cp & 0x3f));
	}
	else
	{
		out += char(0xf0 | cp >> 18);
		out += char(0x80 | ((cp >> 12) & 0x3f));
		out += char(0x80 | ((cp >> 6) & 0x3f));
		out += char(0x80 | (cp & 0x3f));
	}
}

//! A letter's case changed: ASCII, Latin-1 and Latin Extended-A, which the bundled fonts hold.
uint32_t to_upper(uint32_t c)
{
	if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7))
		return c - 32;
	if (c >= 0x100 && c <= 0x17f && (c & 1) && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17f)
		return c - 1;
	return c;
}

uint32_t to_lower(uint32_t c)
{
	if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
		return c + 32;
	if (c >= 0x100 && c <= 0x17f && !(c & 1) && c != 0x130 && c != 0x138)
		return c + 1;
	return c;
}

std::string transform(const ComputedStyle &cs, const std::string &s)
{
	if (cs.text_transform == TextTransform::None)
		return s;
	std::string out;
	out.reserve(s.size());
	bool start = true;
	for (size_t i = 0; i < s.size();)
	{
		const uint32_t c = decode(s, i);
		uint32_t t = c;
		if (cs.text_transform == TextTransform::Uppercase)
			t = to_upper(c);
		else if (cs.text_transform == TextTransform::Lowercase)
			t = to_lower(c);
		else if (start)
			t = to_upper(c);
		start = c == ' ' || c == '\t' || c == '\n';
		encode(t, out);
	}
	return out;
}

std::vector<std::string> split(const std::string &s, const std::string &by)
{
	std::vector<std::string> out;
	size_t from = 0;
	for (;;)
	{
		const size_t k = s.find(by, from);
		if (k == std::string::npos)
		{
			out.push_back(s.substr(from));
			return out;
		}
		out.push_back(s.substr(from, k - from));
		from = k + by.size();
	}
}

} // namespace

void Layout::inline_items(const std::vector<LayoutNode *> &nodes, std::vector<InlineItem> &items)
{
	bool last_space = true;
	// the inline elements a text is inside, outermost first
	std::vector<LayoutNode *> owners;
	auto push = [&](InlineItem it) {
		it.owners = owners;
		items.push_back(std::move(it));
	};
	auto walk = [&](LayoutNode *n, auto &self) -> void {
		if (n->kind == LayoutNode::Kind::Text)
		{
			const ComputedStyle &cs = n->cs();
			const WhiteSpace ws = cs.white_space;
			const FontInfo f = font(cs);
			std::string s = transform(cs, *n->data);
			if (ws == WhiteSpace::Pre || ws == WhiteSpace::PreWrap || ws == WhiteSpace::BreakSpaces)
			{
				const std::vector<std::string> lines = split(s, "\n");
				for (size_t i = 0; i < lines.size(); i++)
				{
					if (i)
						push({ InlineItem::Kind::Break });
					if (!lines[i].empty())
					{
						InlineItem it { InlineItem::Kind::Text };
						it.text = lines[i];
						it.w = measure(f.face, f.size, f.spacing, lines[i]);
						it.cs = &cs;
						it.node = n;
						it.nowrap = ws == WhiteSpace::Pre;
						push(std::move(it));
					}
				}
				last_space = false;
				return;
			}
			// white space collapsed
			std::string c;
			c.reserve(s.size());
			for (char ch : s)
			{
				if (is_space(ch))
				{
					if (c.empty() ? !last_space : c.back() != ' ')
						c += ' ';
				}
				else
					c += ch;
			}
			if (c.empty())
				return;
			const bool nowrap = ws == WhiteSpace::Nowrap;
			const std::vector<std::string> parts = split(c, " ");
			const double sp = measure(f.face, f.size, f.spacing, " ");
			for (size_t i = 0; i < parts.size(); i++)
			{
				const bool last = i + 1 == parts.size();
				if (parts[i].empty() && last)
					continue;
				// a zero-width space breaks a word where it may (a path after its slashes)
				const std::vector<std::string> bits = split(parts[i], "​");
				for (size_t j = 0; j < bits.size(); j++)
				{
					const bool end = j + 1 == bits.size();
					if (!end && bits[j].empty())
						continue;
					InlineItem it { InlineItem::Kind::Text };
					it.text = bits[j];
					it.w = bits[j].empty() ? 0 : measure(f.face, f.size, f.spacing, bits[j]);
					it.cs = &cs;
					it.node = n;
					it.nowrap = nowrap;
					it.space = end && !last ? sp : 0;
					it.break_after = (!end || !last) && !nowrap;
					push(std::move(it));
				}
			}
			last_space = c.back() == ' ';
			return;
		}
		if (n->kind == LayoutNode::Kind::Element && n->el->is(n::br))
		{
			InlineItem br { InlineItem::Kind::Break };
			br.node = n;
			push(std::move(br));
			last_space = true;
			return;
		}
		if (out_of_flow(*n))
		{
			// where an absolutely placed box would have been
			InlineItem at { InlineItem::Kind::Static };
			at.node = n;
			push(std::move(at));
			return;
		}
		const Display d = n->cs().display;
		if (d == Display::Inline && !replaced(*n))
		{
			const Edges e = edges(*n, std::nullopt);
			if (e.ml.value_or(0) || e.pl || e.bl)
			{
				InlineItem gap { InlineItem::Kind::Gap };
				gap.w = e.ml.value_or(0) + e.pl + e.bl;
				push(std::move(gap));
			}
			owners.push_back(n);
			InlineItem mark { InlineItem::Kind::Mark };
			mark.node = n;
			push(std::move(mark));
			for (LayoutNode *c : n->kids)
				if (c->kind == LayoutNode::Kind::Text || !out_of_flow(*c) || c->cs().inline_static)
					self(c, self);
			owners.pop_back();
			if (e.mr.value_or(0) || e.pr || e.br)
			{
				InlineItem gap { InlineItem::Kind::Gap };
				gap.w = e.mr.value_or(0) + e.pr + e.br;
				push(std::move(gap));
			}
			return;
		}
		// an atom: an inline block or a replaced element, its margins about it; the lines break about it unless the
		// white space it stands in keeps them whole (a button's icon beside its text)
		const Intrinsic o = outer_intrinsic(*n);
		InlineItem atom { InlineItem::Kind::Atom };
		atom.node = n;
		atom.w = o.max;
		atom.min = o.min;
		const WhiteSpace around = (n->parent ? n->parent : n)->cs().white_space;
		atom.nowrap = around == WhiteSpace::Nowrap || around == WhiteSpace::Pre;
		atom.break_after = !atom.nowrap;
		push(std::move(atom));
		last_space = false;
	};
	for (LayoutNode *n : nodes)
		walk(n, walk);
}

Layout::Intrinsic Layout::inline_intrinsic(const std::vector<LayoutNode *> &nodes)
{
	std::vector<InlineItem> items;
	inline_items(nodes, items);
	double mn = 0, mx = 0, line = 0, word = 0;
	for (const InlineItem &it : items)
	{
		if (it.kind == InlineItem::Kind::Break)
		{
			mx = std::max(mx, line);
			line = 0;
			mn = std::max(mn, word);
			word = 0;
			continue;
		}
		line += it.w + it.space;
		if (it.nowrap)
			word += it.w + it.space;
		else
		{
			word += it.w;
			mn = std::max(mn, word);
			word = it.break_after ? 0 : word + it.space;
		}
	}
	mn = std::max(mn, word);
	mx = std::max(mx, line);
	return { mn, mx };
}

Layout::Content Layout::layout_inline(LayoutNode &el, const std::vector<LayoutNode *> &nodes, double cw, double y0,
	std::vector<TextPiece> &pieces)
{
	std::vector<InlineItem> items;
	inline_items(nodes, items);
	const ComputedStyle &cs = el.cs();
	const FontInfo strut = font(cs);
	std::vector<std::vector<InlineItem *>> lines;
	std::vector<InlineItem *> cur;
	double x = 0;
	const bool wraps = cs.white_space != WhiteSpace::Nowrap && cs.white_space != WhiteSpace::Pre;
	for (InlineItem &it : items)
	{
		if (it.kind == InlineItem::Kind::Break)
		{
			if (it.node)
				cur.push_back(&it);
			lines.push_back(std::move(cur));
			cur.clear();
			x = 0;
			continue;
		}
		if (it.kind == InlineItem::Kind::Atom)
		{
			LayoutNode &a = *it.node;
			const Edges e = edges(a, cw);
			std::optional<double> w = size_of(a, a.cs().width, cw, e, true);
			if (!w)
			{
				if (replaced(a))
				{
					const ReplacedSize r = replaced_size(a, e);
					w = r.fill ? cw - e.ml.value_or(0) - e.mr.value_or(0) : r.w;
				}
				else
				{
					const Intrinsic i = intrinsic(a);
					w = std::min(std::max(i.min, cw - e.ml.value_or(0) - e.mr.value_or(0)), i.max);
				}
			}
			const double ww = clamp_w(a, *w, cw, e);
			layout_box(a, ww, std::nullopt, cw, std::nullopt);
			it.w = ww + e.ml.value_or(0) + e.mr.value_or(0);
			it.e = e;
		}
		// the item before, past the marks where inline elements start
		InlineItem *prev = nullptr;
		for (size_t i = cur.size(); i-- > 0 && !prev;)
			if (cur[i]->kind != InlineItem::Kind::Mark && cur[i]->kind != InlineItem::Kind::Static)
				prev = cur[i];
		const bool zero = it.kind == InlineItem::Kind::Mark || it.kind == InlineItem::Kind::Static;
		const bool can_break = prev && !zero && (wraps || it.kind == InlineItem::Kind::Atom)
			&& !(it.nowrap && prev->nowrap && !prev->break_after);
		if (can_break && x + it.w > cw + 0.01
			&& (prev->break_after || it.kind == InlineItem::Kind::Atom || prev->kind == InlineItem::Kind::Atom))
		{
			lines.push_back(std::move(cur));
			cur.clear();
			x = 0;
		}
		cur.push_back(&it);
		x += it.w + it.space;
	}
	if (!cur.empty())
		lines.push_back(std::move(cur));

	// each line's baseline: the strut's, its text's and its atoms'
	struct Span
	{
		double x0, y0, x1, y1;
	};
	std::vector<std::pair<LayoutNode *, Span>> spans;
	auto extend = [&](LayoutNode *o, double x0, double ya, double x1, double yb) {
		for (auto &[k, r] : spans)
			if (k == o)
			{
				r = { std::min(r.x0, x0), std::min(r.y0, ya), std::max(r.x1, x1), std::max(r.y1, yb) };
				return;
			}
		spans.push_back({ o, { x0, ya, x1, yb } });
	};
	double y = y0;
	std::optional<double> first;
	const TextAlign align = cs.text_align;
	for (auto &line : lines)
	{
		// spaces at a line's end hang
		while (!line.empty() && line.back()->kind == InlineItem::Kind::Text && line.back()->text.empty())
			line.pop_back();
		if (line.empty())
			continue;
		auto half = [](const FontInfo &f) { return (f.line - (f.ascent + f.descent)) / 2; };
		double above = strut.ascent + half(strut), below = strut.descent + half(strut);
		for (InlineItem *it : line)
		{
			if (it->kind == InlineItem::Kind::Text)
			{
				const FontInfo f = font(*it->cs);
				double a = f.ascent, d = f.descent;
				// with line-height: normal, the line makes room for the fallback fonts' glyphs too, as Firefox's
				if (it->cs->line_height.kind == LineHeight::Kind::Normal && !it->text.empty())
					m_dom.fonts.extent(f.face, f.size, it->text, a, d);
				above = std::max(above, a + half(f));
				below = std::max(below, d + half(f));
			}
			else if (it->kind == InlineItem::Kind::Atom)
			{
				const LayoutNode &b = *it->node;
				const double base = atom_baseline(b) + it->e.mt.value_or(0);
				const double outer = b.h + it->e.mt.value_or(0) + it->e.mb.value_or(0);
				above = std::max(above, base);
				below = std::max(below, outer - base);
			}
		}
		double width = 0;
		for (size_t i = 0; i < line.size(); i++)
			width += line[i]->w + (i + 1 < line.size() ? line[i]->space : 0);
		double lx = 0;
		if (align == TextAlign::Right || align == TextAlign::End)
			lx = cw - width;
		else if (align == TextAlign::Center)
			lx = (cw - width) / 2;
		const double baseline = y + above;
		if (!first)
			first = baseline;
		for (size_t i = 0; i < line.size(); i++)
		{
			InlineItem *it = line[i];
			// an inline element's box: its fragments', each its font's height about the baseline, a space inside it
			// counted unless the line ends there
			const double w = it->w + (i + 1 < line.size() ? it->space : 0);
			for (LayoutNode *o : it->owners)
			{
				const FontInfo f = font(o->cs());
				extend(o, lx, baseline - f.ascent, lx + w, baseline + f.descent);
			}
			if (it->kind == InlineItem::Kind::Mark)
			{
				const FontInfo f = font(it->node->cs());
				extend(it->node, lx, baseline - f.ascent, lx, baseline + f.descent);
			}
			if (it->kind == InlineItem::Kind::Break || it->kind == InlineItem::Kind::Static)
			{
				// a br: a box of no width as tall as the line; an absolutely placed box: its static place
				LayoutNode &b = *it->node;
				if (it->kind == InlineItem::Kind::Break)
				{
					b.laid = true;
					b.pass = m_pass;
					b.inline_box = true;
					b.x = lx;
					b.y = baseline - above;
					b.w = b.cw = 0;
					b.h = b.ch = above + below;
				}
				else
				{
					b.static_x = lx;
					b.static_y = baseline - above;
				}
				continue;
			}
			// a text, or a space alone (between two inline elements), which a decoration crosses
			const bool crossed = it->text.empty() && i + 1 < line.size() && it->space > 0;
			if (it->kind == InlineItem::Kind::Text && (!it->text.empty() || crossed))
			{
				const FontInfo f = font(*it->cs);
				pieces.push_back({ lx, baseline, it->w, it->text, f.face, f.size, f.spacing, it->node,
					i + 1 < line.size() ? it->space : 0 });
			}
			else if (it->kind == InlineItem::Kind::Atom)
			{
				LayoutNode &b = *it->node;
				b.x = lx + it->e.ml.value_or(0);
				b.y = baseline - atom_baseline(b);
			}
			lx += it->w + it->space;
		}
		y = baseline + below;
	}
	// the inline elements' boxes, each relative to its parent's (an inline parent's, or the block's content box)
	std::unordered_map<LayoutNode *, std::pair<double, double>> origin;
	for (auto &[o, r] : spans)
	{
		// the border box: the content's fragments with the padding and the border about them
		const Edges e = edges(*o, cw);
		r.x0 -= e.pl + e.bl;
		r.x1 += e.pr + e.br;
		r.y0 -= e.pt + e.bt;
		r.y1 += e.pb + e.bb;
		LayoutNode &b = *o;
		b.laid = true;
		b.pass = m_pass;
		b.inline_box = true;
		b.x = r.x0;
		b.y = r.y0;
		b.w = b.cw = r.x1 - r.x0;
		b.h = b.ch = r.y1 - r.y0;
		b.bt = e.bt;
		b.br = e.br;
		b.bb = e.bb;
		b.bl = e.bl;
		b.pt = e.pt;
		b.pr = e.pr;
		b.pb = e.pb;
		b.pl = e.pl;
		b.sw = b.sh = 0;
		b.ow = b.w;
		b.oh = b.h;
		b.base.reset();
		b.pieces.clear();
		origin[o] = { r.x0, r.y0 };
	}
	for (auto &[o, r] : spans)
		if (o->parent && origin.count(o->parent))
		{
			o->x -= origin[o->parent].first;
			o->y -= origin[o->parent].second;
		}
	// the atoms, breaks and placed boxes inside an inline element, relative to it
	for (auto &line : lines)
		for (InlineItem *it : line)
			if ((it->kind == InlineItem::Kind::Atom || it->kind == InlineItem::Kind::Break
				|| it->kind == InlineItem::Kind::Static) && it->node->parent && origin.count(it->node->parent))
			{
				const auto [ox, oy] = origin[it->node->parent];
				if (it->kind == InlineItem::Kind::Static)
				{
					it->node->static_x -= ox;
					it->node->static_y -= oy;
				}
				else
				{
					it->node->x -= ox;
					it->node->y -= oy;
				}
			}
	return { y - y0, first };
}

} // namespace qdom
