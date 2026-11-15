// Copyright 2026 Ian Karlsson
// Flex layout: rows and columns, wrapping, the flexible lengths with their min and max, stretching, auto margins,
// justify-content and align-items, gaps; a text among the items in an anonymous block of its own.
#include <algorithm>
#include <cmath>
#include <limits>

#include "dom/dom.h"
#include "layout/layout.h"

namespace qdom
{

namespace
{

double gap_of(const Length &l, std::optional<double> basis)
{
	if (l.kind == Length::Kind::Normal)
		return 0;
	return l.resolve(basis).value_or(0);
}

struct FlexItem
{
	LayoutNode *it;
	double ml = 0, mr = 0, mt = 0, mb = 0;
	bool auto_s, auto_e, auto_cs, auto_ce;
	double m_main, m_cross;
	Align align;
	double grow, shrink;
	double base = 0, min = 0, max = 0, hyp = 0, main = 0, cross = 0;
	bool frozen = false;
	double viol = 0;
	//! a column item of a definite height whose least is the height its content needs when that is less: its min
	//! is its height until it is measured (only when its line shrinks, or its basis is less)
	bool lazy_min = false;
};

bool is_end(Align a)
{
	return a == Align::FlexEnd || a == Align::End || a == Align::SelfEnd || a == Align::Right;
}

void resolve_flexible(std::vector<FlexItem *> &line, double size, double gap)
{
	auto outer = [&](auto f) {
		double s = 0;
		for (FlexItem *d : line)
			s += f(*d) + d->m_main;
		return s + gap * double(line.size() - 1);
	};
	const bool growing = outer([](const FlexItem &d) { return d.hyp; }) < size;
	for (FlexItem *d : line)
	{
		d->frozen = (growing ? d->grow == 0 : d->shrink == 0) || (growing ? d->base > d->hyp : d->base < d->hyp);
		d->main = d->hyp;
	}
	const double initial = size - outer([](const FlexItem &d) { return d.frozen ? d.hyp : d.base; });
	for (int loop = 0; loop < 12; loop++)
	{
		std::vector<FlexItem *> open;
		for (FlexItem *d : line)
			if (!d->frozen)
				open.push_back(d);
		if (open.empty())
			break;
		double free = size - outer([](const FlexItem &d) { return d.frozen ? d.main : d.base; });
		double sum_f = 0, sum_s = 0;
		for (FlexItem *d : open)
		{
			sum_f += growing ? d->grow : d->shrink;
			sum_s += d->shrink * d->base;
		}
		if (sum_f < 1 && std::fabs(initial * sum_f) < std::fabs(free))
			free = initial * sum_f;
		for (FlexItem *d : open)
		{
			if (growing)
				d->main = d->base + (sum_f ? free * d->grow / sum_f : 0);
			else
				d->main = d->base + (sum_s ? free * d->shrink * d->base / sum_s : 0);
		}
		double violation = 0;
		for (FlexItem *d : open)
		{
			const double c = std::max(d->min, std::min(d->max, d->main));
			d->viol = c - d->main;
			violation += d->viol;
			d->main = c;
		}
		if (std::fabs(violation) < 0.001)
			break;
		for (FlexItem *d : open)
			if ((violation > 0 && d->viol > 0) || (violation < 0 && d->viol < 0))
				d->frozen = true;
	}
}

} // namespace

std::vector<LayoutNode *> Layout::flow_items(LayoutNode &n)
{
	std::vector<LayoutNode *> out;
	for (LayoutNode *c : n.kids)
	{
		if (c->kind == LayoutNode::Kind::Text)
		{
			if (blank(*c))
				continue;
			out.push_back(anonymous(&n, c));
			continue;
		}
		if (out_of_flow(*c))
		{
			c->static_x = c->static_y = 0;
			continue;
		}
		out.push_back(c);
	}
	return out;
}

Layout::Intrinsic Layout::anon_intrinsic(LayoutNode &a)
{
	return intrinsic(a);
}

void Layout::adopt_anon(LayoutNode &container, LayoutNode &a)
{
	for (const TextPiece &p : a.pieces)
	{
		TextPiece q = p;
		q.x += a.x;
		q.y += a.y;
		container.pieces.push_back(std::move(q));
	}
}

Layout::Intrinsic Layout::flex_intrinsic(LayoutNode &n)
{
	const ComputedStyle &cs = n.cs();
	const bool row = cs.row();
	const bool wrap = cs.flex_wrap != FlexWrap::Nowrap;
	const double gap = gap_of(row ? cs.column_gap : cs.row_gap, std::nullopt);
	double mn = 0, mx = 0;
	bool first = true;
	for (LayoutNode *it : flow_items(n))
	{
		const Intrinsic o = it->kind == LayoutNode::Kind::Anonymous ? anon_intrinsic(*it) : outer_intrinsic(*it);
		if (row)
		{
			mx += o.max + (first ? 0 : gap);
			mn = wrap ? std::max(mn, o.min) : mn + o.min + (first ? 0 : gap);
		}
		else
		{
			mx = std::max(mx, o.max);
			mn = std::max(mn, o.min);
		}
		first = false;
	}
	return { mn, mx };
}

Layout::Content Layout::layout_flex(LayoutNode &n, double cw, std::optional<double> ch_def)
{
	const ComputedStyle &cs = n.cs();
	const bool row = cs.row();
	const bool reverse = cs.flex_direction == FlexDirection::RowReverse || cs.flex_direction == FlexDirection::ColumnReverse;
	const bool wrap = cs.flex_wrap != FlexWrap::Nowrap;
	std::vector<LayoutNode *> items = flow_items(n);
	if (reverse)
		std::reverse(items.begin(), items.end());
	const std::optional<double> main_size = row ? std::optional<double>(cw) : ch_def;
	const std::optional<double> cross_size = row ? ch_def : std::optional<double>(cw);
	const double gap_main = gap_of(row ? cs.column_gap : cs.row_gap, main_size);
	const double gap_cross = gap_of(row ? cs.row_gap : cs.column_gap, cross_size);
	const Align ai = cs.align_items;
	const double inf = std::numeric_limits<double>::infinity();

	// each item's base size and its bounds along the main axis
	std::vector<FlexItem> data;
	data.reserve(items.size());
	for (LayoutNode *it : items)
	{
		const Edges e = edges(*it, cw);
		const ComputedStyle &ics = it->cs();
		FlexItem d {};
		d.it = it;
		d.ml = e.ml.value_or(0);
		d.mr = e.mr.value_or(0);
		d.mt = e.mt.value_or(0);
		d.mb = e.mb.value_or(0);
		d.auto_s = row ? !e.ml : !e.mt;
		d.auto_e = row ? !e.mr : !e.mb;
		d.auto_cs = row ? !e.mt : !e.ml;
		d.auto_ce = row ? !e.mb : !e.mr;
		d.m_main = row ? d.ml + d.mr : d.mt + d.mb;
		d.m_cross = row ? d.mt + d.mb : d.ml + d.mr;
		const Align self = ics.align_self == Align::Auto ? ai : ics.align_self;
		d.align = self == Align::Normal ? Align::Stretch : self;
		d.grow = ics.flex_grow;
		d.shrink = ics.flex_shrink;
		std::optional<double> base;
		const Length &basis = ics.flex_basis;
		if (basis.is_value())
			if (std::optional<double> b = basis.resolve(main_size))
				base = ics.box_sizing == BoxSizing::BorderBox ? *b : *b + (row ? e.hx() : e.vx());
		if (!base)
			base = size_of(*it, row ? ics.width : ics.height, row ? std::optional<double>(cw) : ch_def, e, row);
		// the cross size a column item is laid out at
		if (!row)
		{
			std::optional<double> w = size_of(*it, ics.width, cw, e, true);
			if (!w && (ics.width.kind == Length::Kind::MaxContent || ics.width.kind == Length::Kind::MinContent))
				w = ics.width.kind == Length::Kind::MaxContent ? intrinsic(*it).max : intrinsic(*it).min;
			if (!w)
			{
				if (d.align == Align::Stretch && !d.auto_cs && !d.auto_ce)
					w = cw - d.m_cross;
				else if (replaced(*it))
				{
					const ReplacedSize rs = replaced_size(*it, e);
					w = rs.fill ? cw - d.m_cross : rs.w;
				}
				else
				{
					const Intrinsic i = intrinsic(*it);
					w = std::min(std::max(i.min, cw - d.m_cross), i.max);
				}
			}
			d.cross = clamp_w(*it, *w, cw, e);
		}
		if (!base)
		{
			if (row)
			{
				if (replaced(*it))
				{
					const ReplacedSize rs = replaced_size(*it, e);
					base = rs.fill ? size_of(*it, ics.min_width, cw, e, true).value_or(0) : rs.w;
				}
				else
					base = intrinsic(*it).max;
			}
			else
				base = height_at(*it, d.cross, cw, ch_def);
		}
		d.base = *base;
		// the least it takes: its min-width, or what its content needs unless it may scroll
		std::optional<double> mn = size_of(*it, row ? ics.min_width : ics.min_height, row ? std::optional<double>(cw) : ch_def, e, row);
		if (!mn)
		{
			// a scroll container's least is nothing (an svg's overflow only clips its drawing)
			if (ics.clips() && !replaced(*it))
				mn = row ? e.hx() : e.vx();
			else if (row)
			{
				double imin;
				if (replaced(*it))
				{
					const ReplacedSize rs = replaced_size(*it, e);
					imin = rs.fill ? 0 : rs.w;
				}
				else
					imin = content_min(*it);
				const std::optional<double> spec = size_of(*it, ics.width, cw, e, true);
				mn = spec ? std::min(*spec, imin) : imin;
			}
			else
			{
				const double at = height_at(*it, d.cross, cw, ch_def);
				const std::optional<double> spec = size_of(*it, ics.height, ch_def, e, false);
				mn = spec ? std::min(*spec, at) : at;
				d.lazy_min = spec.has_value();
			}
		}
		const std::optional<double> mx = size_of(*it, row ? ics.max_width : ics.max_height, row ? std::optional<double>(cw) : ch_def, e, row);
		d.min = *mn;
		d.max = mx ? *mx : inf;
		d.hyp = std::max(d.min, std::min(d.max, d.base));
		data.push_back(d);
	}
	// a column item's least: the lesser of its height and the height of its content (laid out as if its height were
	// auto), measured where it counts
	auto measure_min = [&](FlexItem &d) {
		if (!d.lazy_min)
			return;
		d.lazy_min = false;
		d.min = std::min(d.min, content_height(*d.it, d.cross, cw, ch_def));
		d.hyp = std::max(d.min, std::min(d.max, d.base));
	};
	for (FlexItem &d : data)
		if (d.base < d.min)
			measure_min(d);

	// the lines
	std::vector<std::vector<FlexItem *>> lines;
	if (!wrap || !main_size)
	{
		lines.emplace_back();
		for (FlexItem &d : data)
			lines.back().push_back(&d);
	}
	else
	{
		std::vector<FlexItem *> cur;
		double used = 0;
		for (FlexItem &d : data)
		{
			const double o = d.hyp + d.m_main;
			if (!cur.empty() && used + gap_main + o > *main_size + 0.01)
			{
				lines.push_back(std::move(cur));
				cur.clear();
				used = 0;
			}
			used += (cur.empty() ? 0 : gap_main) + o;
			cur.push_back(&d);
		}
		if (!cur.empty())
			lines.push_back(std::move(cur));
	}
	// the flexible lengths
	for (auto &line : lines)
	{
		if (line.empty())
			continue;
		if (!main_size)
		{
			for (FlexItem *d : line)
				d->main = d->hyp;
			continue;
		}
		double outer = gap_main * double(line.size() - 1);
		for (FlexItem *d : line)
			outer += d->hyp + d->m_main;
		if (outer > *main_size)
			for (FlexItem *d : line)
				measure_min(*d);
		resolve_flexible(line, *main_size, gap_main);
	}
	// cross sizes: each item laid out at its main size
	std::vector<double> line_cross;
	for (auto &line : lines)
	{
		double lc = 0;
		for (FlexItem *d : line)
		{
			LayoutNode &it = *d->it;
			if (row)
			{
				const Edges e = edges(it, cw);
				const std::optional<double> spec = size_of(it, it.cs().height, ch_def, e, false);
				layout_box(it, d->main, spec, cw, ch_def);
				d->cross = it.h;
			}
			else
				layout_box(it, d->cross, d->main, cw, ch_def);
			lc = std::max(lc, d->cross + d->m_cross);
		}
		line_cross.push_back(lc);
	}
	if (lines.size() == 1 && cross_size)
		line_cross[0] = *cross_size;
	else if (cross_size && lines.size() > 1)
	{
		const Align ac = cs.align_content;
		double used = gap_cross * double(lines.size() - 1);
		for (double x : line_cross)
			used += x;
		if ((ac == Align::Normal || ac == Align::Stretch) && used < *cross_size)
			for (double &x : line_cross)
				x += (*cross_size - used) / double(lines.size());
	}
	// stretched items take their line's cross size
	for (size_t li = 0; li < lines.size(); li++)
		for (FlexItem *d : lines[li])
		{
			LayoutNode &it = *d->it;
			const Edges e = edges(it, cw);
			const std::optional<double> spec = size_of(it, row ? it.cs().height : it.cs().width,
				row ? ch_def : std::optional<double>(cw), e, !row);
			const bool cross_auto = (row ? it.cs().height : it.cs().width).is_auto();
			if (d->align == Align::Stretch && !spec && cross_auto && !d->auto_cs && !d->auto_ce)
			{
				double c = line_cross[li] - d->m_cross;
				c = row ? clamp_h(it, c, ch_def, e) : clamp_w(it, c, cw, e);
				if (std::fabs(c - d->cross) > 0.001)
				{
					if (row)
						layout_box(it, d->main, c, cw, ch_def);
					else
						layout_box(it, c, d->main, cw, ch_def);
					d->cross = c;
				}
			}
		}
	// positions
	const Align jc = cs.justify_content;
	double cross = 0;
	std::optional<double> base;
	for (size_t li = 0; li < lines.size(); li++)
	{
		const auto &line = lines[li];
		double used = gap_main * (line.empty() ? 0 : double(line.size() - 1));
		int autos = 0;
		for (FlexItem *d : line)
		{
			used += d->main + d->m_main;
			autos += int(d->auto_s) + int(d->auto_e);
		}
		const double free = main_size ? *main_size - used : 0;
		double pos = 0, between = 0, auto_each = 0;
		if (free > 0 && autos)
			auto_each = free / autos;
		else
		{
			Align j = jc;
			if (reverse && (j == Align::FlexStart || j == Align::Normal || j == Align::Start))
				j = Align::FlexEnd;
			else if (reverse && (j == Align::FlexEnd || j == Align::End))
				j = Align::FlexStart;
			if (j == Align::FlexEnd || j == Align::End || j == Align::Right)
				pos = free;
			else if (j == Align::Center)
				pos = free / 2;
			else if (j == Align::SpaceBetween && line.size() > 1 && free > 0)
				between = free / double(line.size() - 1);
			else if (j == Align::SpaceAround && free > 0)
			{
				between = free / double(line.size());
				pos = between / 2;
			}
			else if (j == Align::SpaceEvenly && free > 0)
			{
				between = free / double(line.size() + 1);
				pos = between;
			}
		}
		for (FlexItem *d : line)
		{
			LayoutNode &it = *d->it;
			pos += d->auto_s ? auto_each : 0;
			const double ms = row ? d->ml : d->mt;
			const double lc = line_cross[li];
			const double free_c = lc - d->cross - d->m_cross;
			double off = 0;
			if (d->auto_cs && d->auto_ce)
				off = free_c / 2;
			else if (d->auto_cs)
				off = free_c;
			else if (d->align == Align::Center)
				off = free_c / 2;
			else if (is_end(d->align))
				off = free_c;
			const double mc = row ? d->mt : d->ml;
			if (row)
			{
				it.x = pos + ms;
				it.y = cross + mc + off;
			}
			else
			{
				it.y = pos + ms;
				it.x = cross + mc + off;
			}
			// the first item's baseline, or the bottom of a first item without one
			if (!base && li == 0)
				base = it.y + (it.base ? *it.base : it.h);
			pos += ms + d->main + (row ? d->mr : d->mb) + gap_main + between + (d->auto_e ? auto_each : 0);
		}
		cross += line_cross[li] + gap_cross;
	}
	// text items: their text kept on the anonymous box, drawn from its element
	for (FlexItem &d : data)
		if (d.it->kind == LayoutNode::Kind::Anonymous)
			adopt_anon(n, *d.it);
	if (!base && !data.empty())
		base = data[0].it->y + (data[0].it->base ? *data[0].it->base : data[0].it->h);
	const double content_cross = cross - (lines.empty() ? 0 : gap_cross);
	if (row)
		return { content_cross, base };
	double main_used = 0;
	for (auto &line : lines)
	{
		double s = gap_main * (line.empty() ? 0 : double(line.size() - 1));
		for (FlexItem *d : line)
			s += d->main + d->m_main;
		main_used = std::max(main_used, s);
	}
	return { ch_def ? *ch_def : main_used, base };
}

} // namespace qdom
