// Copyright 2026 Ian Karlsson
// Grid layout as supported: track lists with repeat(), minmax(), fr and the content
// keywords, items placed in order (by rows, or by columns with grid-auto-flow: column), gaps, stretching and the
// alignment of items; no explicit placement.
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

const double INF = std::numeric_limits<double>::infinity();

} // namespace

Layout::Track Layout::track_one(std::string_view x, std::optional<double> basis, double em) const
{
	const std::string l = lower(trim(x));
	if (l.ends_with("fr"))
		return { Track::Kind::Fr, parse_number(std::string_view(l).substr(0, l.size() - 2)).value_or(1) };
	if (l == "auto")
		return { Track::Kind::Auto, 0 };
	if (l == "min-content")
		return { Track::Kind::MinContent, 0 };
	if (l == "max-content")
		return { Track::Kind::MaxContent, 0 };
	std::optional<Length> len = parse_length(l, { em, 16, m_vw, m_vh });
	if (len && len->is_value())
		if (std::optional<double> v = len->resolve(basis))
			return { Track::Kind::Fixed, *v };
	return { Track::Kind::Auto, 0 };
}

Layout::TrackSize Layout::track(std::string_view w, std::optional<double> basis, double em) const
{
	const std::string l = lower(trim(w));
	if (l.starts_with("minmax(") && l.back() == ')')
	{
		const auto parts = split_commas(std::string_view(l).substr(7, l.size() - 8));
		if (parts.size() == 2)
			return { track_one(parts[0], basis, em), track_one(parts[1], basis, em) };
	}
	const Track t = track_one(l, basis, em);
	if (t.kind == Track::Kind::Fr)
		return { { Track::Kind::Auto, 0 }, t };
	return { t, t };
}

std::vector<Layout::TrackSize> Layout::tracks(std::string_view s, std::optional<double> basis, double em) const
{
	std::vector<TrackSize> out;
	if (trim(s).empty() || lower(trim(s)) == "none")
		return out;
	for (std::string_view w : split_words(s))
	{
		const std::string l = lower(w);
		if (l.starts_with("repeat(") && l.back() == ')')
		{
			const std::string_view inner = std::string_view(w).substr(7, w.size() - 8);
			const size_t comma = inner.find(',');
			if (comma == std::string_view::npos)
				continue;
			const int n = int(parse_number(trim(inner.substr(0, comma))).value_or(1));
			const std::vector<TrackSize> list = tracks(inner.substr(comma + 1), basis, em);
			for (int i = 0; i < n; i++)
				out.insert(out.end(), list.begin(), list.end());
			continue;
		}
		out.push_back(track(w, basis, em));
	}
	return out;
}

Layout::Grid Layout::grid_setup(LayoutNode &n, std::optional<double> cw)
{
	const ComputedStyle &cs = n.cs();
	Grid g;
	g.items = flow_items(n);
	g.cols = tracks(cs.grid_template_columns, cw, cs.font_size);
	g.rows = tracks(cs.grid_template_rows, std::nullopt, cs.font_size);
	const bool column = cs.grid_auto_flow == GridAutoFlow::Column || cs.grid_auto_flow == GridAutoFlow::ColumnDense;
	const TrackSize auto_row = track(cs.grid_auto_rows.empty() ? "auto" : cs.grid_auto_rows, std::nullopt, cs.font_size);
	const size_t count = g.items.size();
	if (column)
	{
		// by columns: down the rows there are (one when none is given), then into implicit columns
		const TrackSize auto_col = track(cs.grid_auto_columns.empty() ? "auto" : cs.grid_auto_columns, cw, cs.font_size);
		if (g.rows.empty())
			g.rows.push_back(auto_row);
		const size_t nr = g.rows.size();
		const size_t nc = std::max<size_t>(1, (count + nr - 1) / nr);
		while (g.cols.size() < nc)
			g.cols.push_back(auto_col);
		for (size_t i = 0; i < count; i++)
			g.place.push_back({ g.items[i], i % nr, i / nr });
	}
	else
	{
		if (g.cols.empty())
			g.cols.push_back({ { Track::Kind::Auto, 0 }, { Track::Kind::Auto, 0 } });
		const size_t nc = g.cols.size();
		for (size_t i = 0; i < count; i++)
			g.place.push_back({ g.items[i], i / nc, i % nc });
		const size_t nr = (count + nc - 1) / nc;
		while (g.rows.size() < nr)
			g.rows.push_back(auto_row);
	}
	return g;
}

double Layout::contribution(LayoutNode &it, bool min)
{
	const Intrinsic o = it.kind == LayoutNode::Kind::Anonymous ? anon_intrinsic(it) : outer_intrinsic(it);
	if (min && it.kind != LayoutNode::Kind::Anonymous)
	{
		const Edges e = edges(it, std::nullopt);
		const double m = e.ml.value_or(0) + e.mr.value_or(0);
		if (std::optional<double> mn = size_of(it, it.cs().min_width, std::nullopt, e, true))
			return std::min(o.min, *mn + m);
		if (it.cs().clips() && !replaced(it))
			return e.hx() + m;
	}
	return min ? o.min : o.max;
}

std::vector<double> Layout::size_columns(Grid &g, std::optional<double> cw, double gap)
{
	const size_t n = g.cols.size();
	std::vector<double> min_c(n, 0), max_c(n, 0);
	for (const Placed &p : g.place)
	{
		min_c[p.c] = std::max(min_c[p.c], contribution(*p.it, true));
		max_c[p.c] = std::max(max_c[p.c], contribution(*p.it, false));
	}
	std::vector<double> base(n), limit(n), flex(n);
	for (size_t i = 0; i < n; i++)
	{
		const Track lo = g.cols[i].min, hi = g.cols[i].max;
		base[i] = lo.kind == Track::Kind::Fixed ? lo.v : lo.kind == Track::Kind::MaxContent ? max_c[i] : min_c[i];
		limit[i] = hi.kind == Track::Kind::Fr ? INF : hi.kind == Track::Kind::Fixed ? hi.v
			: hi.kind == Track::Kind::MinContent ? min_c[i] : max_c[i];
		if (limit[i] < base[i])
			limit[i] = base[i];
		flex[i] = hi.kind == Track::Kind::Fr ? hi.v : 0;
	}
	std::vector<double> sizes = base;
	const double gaps = gap * double(n > 0 ? n - 1 : 0);
	// the tracks that do not flex grow to their limits as far as the width lets them
	const double avail = cw ? *cw - gaps : INF;
	double free = avail;
	for (double s : sizes)
		free -= s;
	for (int k = 0; k < 20 && free > 0.001; k++)
	{
		std::vector<size_t> open;
		for (size_t i = 0; i < n; i++)
			if (!flex[i] && sizes[i] < limit[i])
				open.push_back(i);
		if (open.empty())
			break;
		const double each = free / double(open.size());
		for (size_t i : open)
		{
			const double add = std::min(each, limit[i] - sizes[i]);
			sizes[i] += add;
			free -= add;
		}
		if (std::isinf(free))
			break;
	}
	if (!cw)
		for (size_t i = 0; i < n; i++)
			if (!flex[i] && !std::isinf(limit[i]))
				sizes[i] = std::max(sizes[i], limit[i]);
	// the flexible tracks share what is left, none under its base
	if (std::any_of(flex.begin(), flex.end(), [](double f) { return f > 0; }))
	{
		if (!cw)
		{
			double fr = 0;
			for (size_t i = 0; i < n; i++)
				if (flex[i])
					fr = std::max({ fr, max_c[i] / flex[i], base[i] / flex[i] });
			for (size_t i = 0; i < n; i++)
				if (flex[i])
					sizes[i] = std::max(base[i], fr * flex[i]);
			return sizes;
		}
		std::vector<bool> fixed(n, false);
		double fr = 0;
		for (size_t k = 0; k <= n; k++)
		{
			double left = avail, sum_f = 0;
			for (size_t i = 0; i < n; i++)
			{
				if (!(flex[i] && !fixed[i]))
					left -= sizes[i];
				if (!fixed[i])
					sum_f += flex[i];
			}
			fr = sum_f ? std::max(0.0, left) / std::max(sum_f, 1.0) : 0;
			bool changed = false;
			for (size_t i = 0; i < n; i++)
				if (flex[i] && !fixed[i] && fr * flex[i] < base[i])
				{
					fixed[i] = true;
					changed = true;
				}
			if (!changed)
				break;
		}
		for (size_t i = 0; i < n; i++)
			if (flex[i] && !fixed[i])
				sizes[i] = fr * flex[i];
		return sizes;
	}
	if (cw && free > 0.001 && g.stretch)
	{
		// auto tracks stretch to fill
		std::vector<size_t> autos;
		for (size_t i = 0; i < n; i++)
			if (g.cols[i].max.kind == Track::Kind::Auto)
				autos.push_back(i);
		for (size_t i : autos)
			sizes[i] += free / double(autos.size());
	}
	return sizes;
}

Layout::Intrinsic Layout::grid_intrinsic(LayoutNode &n)
{
	Grid g = grid_setup(n, std::nullopt);
	const double gap = gap_of(n.cs().column_gap, std::nullopt);
	const std::vector<double> max_s = size_columns(g, std::nullopt, gap);
	const size_t nc = g.cols.size();
	std::vector<double> min_c(nc, 0);
	for (const Placed &p : g.place)
		min_c[p.c] = std::max(min_c[p.c], contribution(*p.it, true));
	double mn = 0, mx = 0;
	for (size_t i = 0; i < nc; i++)
	{
		const TrackSize &t = g.cols[i];
		mn += t.min.kind == Track::Kind::Fixed ? t.min.v : t.max.kind == Track::Kind::Fixed ? t.max.v : min_c[i];
		mx += max_s[i];
	}
	const double gaps = gap * double(nc > 0 ? nc - 1 : 0);
	return { mn + gaps, mx + gaps };
}

Layout::Content Layout::layout_grid(LayoutNode &n, double cw, std::optional<double> ch_def)
{
	const ComputedStyle &cs = n.cs();
	Grid g = grid_setup(n, cw);
	g.stretch = cs.justify_content == Align::Normal || cs.justify_content == Align::Stretch;
	const double cgap = gap_of(cs.column_gap, cw), rgap = gap_of(cs.row_gap, ch_def);
	const std::vector<double> col_w = size_columns(g, cw, cgap);
	std::vector<double> col_x(col_w.size());
	double xx = 0;
	for (size_t i = 0; i < col_w.size(); i++)
	{
		col_x[i] = xx;
		xx += col_w[i] + cgap;
	}
	// rows: each item laid out at its column's width
	const size_t nr = g.rows.size();
	std::vector<double> row_h(nr, 0);
	const Align ai = cs.align_items, ji = cs.justify_items;
	struct Data
	{
		Edges e;
		double w, content, h;
		std::optional<double> fixed_h;
	};
	std::vector<Data> data(g.place.size());
	for (size_t k = 0; k < g.place.size(); k++)
	{
		const Placed &p = g.place[k];
		LayoutNode &it = *p.it;
		Data &d = data[k];
		d.e = edges(it, col_w[p.c]);
		const double avail = col_w[p.c] - d.e.ml.value_or(0) - d.e.mr.value_or(0);
		std::optional<double> w = size_of(it, it.cs().width, col_w[p.c], d.e, true);
		const Align js = it.cs().justify_self != Align::Auto ? it.cs().justify_self : ji;
		if (!w)
		{
			if (replaced(it))
			{
				const ReplacedSize rs = replaced_size(it, d.e);
				w = rs.fill ? avail : rs.w;
			}
			else if (js == Align::Normal || js == Align::Stretch)
				w = avail;
			else
			{
				const Intrinsic i = it.kind == LayoutNode::Kind::Anonymous ? anon_intrinsic(it) : intrinsic(it);
				w = std::min(std::max(i.min, avail), i.max);
			}
		}
		d.w = clamp_w(it, *w, col_w[p.c], d.e);
		const TrackSize &t = g.rows[p.r];
		if (t.min.kind == Track::Kind::Fixed && t.max.kind == Track::Kind::Fixed)
			d.fixed_h = t.min.v;
		d.content = height_at(it, d.w, col_w[p.c], d.fixed_h);
		d.h = d.content + d.e.mt.value_or(0) + d.e.mb.value_or(0);
		row_h[p.r] = std::max(row_h[p.r], d.h);
	}
	// each row from its items' least and most heights (a box that scrolls, or sets its min-height, may be less
	// than its content), grown into the room there is: every row toward its most, the flexible ones sharing what
	// is left, then the auto ones stretched
	std::vector<double> min_r(nr, 0);
	for (size_t k = 0; k < g.place.size(); k++)
	{
		const Placed &p = g.place[k];
		const Data &d = data[k];
		const std::optional<double> mn = size_of(*p.it, p.it->cs().min_height, ch_def, d.e, false);
		const double least = mn ? *mn + d.e.mt.value_or(0) + d.e.mb.value_or(0)
			: p.it->cs().clips() && !replaced(*p.it) ? d.e.vx() : d.h;
		min_r[p.r] = std::max(min_r[p.r], least);
	}
	std::vector<double> flex_r(nr), base_r(nr), limit_r(nr);
	for (size_t i = 0; i < nr; i++)
	{
		const TrackSize &t = g.rows[i];
		flex_r[i] = t.max.kind == Track::Kind::Fr ? t.max.v : 0;
		base_r[i] = t.min.kind == Track::Kind::Fixed ? t.min.v : t.min.kind == Track::Kind::MaxContent ? row_h[i] : min_r[i];
		limit_r[i] = flex_r[i] ? INF : t.max.kind == Track::Kind::Fixed ? t.max.v : row_h[i];
		if (limit_r[i] < base_r[i])
			limit_r[i] = base_r[i];
	}
	const double rgaps = rgap * double(nr > 0 ? nr - 1 : 0);
	if (!ch_def)
	{
		for (size_t i = 0; i < nr; i++)
			row_h[i] = flex_r[i] ? std::max(base_r[i], row_h[i]) : limit_r[i];
	}
	else
	{
		std::vector<double> size = base_r;
		double free = *ch_def - rgaps;
		for (double s : size)
			free -= s;
		for (int k = 0; k < 20 && free > 0.001; k++)
		{
			std::vector<size_t> open;
			for (size_t i = 0; i < nr; i++)
				if (!flex_r[i] && size[i] < limit_r[i])
					open.push_back(i);
			if (open.empty())
				break;
			const double each = free / double(open.size());
			for (size_t i : open)
			{
				const double add = std::min(each, limit_r[i] - size[i]);
				size[i] += add;
				free -= add;
			}
		}
		double sum_f = 0;
		for (double f : flex_r)
			sum_f += f;
		if (sum_f)
		{
			std::vector<bool> fixed(nr, false);
			double fr = 0;
			for (size_t k = 0; k <= nr; k++)
			{
				double left = *ch_def - rgaps, sf = 0;
				for (size_t i = 0; i < nr; i++)
				{
					if (!(flex_r[i] && !fixed[i]))
						left -= size[i];
					if (!fixed[i])
						sf += flex_r[i];
				}
				fr = sf ? std::max(0.0, left) / std::max(sf, 1.0) : 0;
				bool again = false;
				for (size_t i = 0; i < nr; i++)
					if (flex_r[i] && !fixed[i] && fr * flex_r[i] < base_r[i])
					{
						fixed[i] = true;
						again = true;
					}
				if (!again)
					break;
			}
			for (size_t i = 0; i < nr; i++)
				if (flex_r[i] && !fixed[i])
					size[i] = fr * flex_r[i];
		}
		else
		{
			const Align ac = cs.align_content;
			double used = rgaps;
			for (double s : size)
				used += s;
			if (used < *ch_def && (ac == Align::Normal || ac == Align::Stretch))
			{
				std::vector<size_t> autos;
				for (size_t i = 0; i < nr; i++)
					if (g.rows[i].max.kind == Track::Kind::Auto)
						autos.push_back(i);
				for (size_t i : autos)
					size[i] += (*ch_def - used) / double(autos.size());
			}
		}
		row_h = size;
	}
	double used_h = rgaps;
	for (double h : row_h)
		used_h += h;
	double y0 = 0;
	if (ch_def && used_h < *ch_def)
	{
		const Align ac = cs.align_content;
		if (ac == Align::Center)
			y0 = (*ch_def - used_h) / 2;
		else if (ac == Align::End || ac == Align::FlexEnd)
			y0 = *ch_def - used_h;
	}
	std::vector<double> row_y(nr);
	double yy = y0;
	for (size_t i = 0; i < nr; i++)
	{
		row_y[i] = yy;
		yy += row_h[i] + rgap;
	}
	std::optional<double> base;
	for (size_t k = 0; k < g.place.size(); k++)
	{
		const Placed &p = g.place[k];
		LayoutNode &it = *p.it;
		const Data &d = data[k];
		const Align as = it.cs().align_self != Align::Auto ? it.cs().align_self : ai;
		const std::optional<double> spec_h = size_of(it, it.cs().height, row_h[p.r], d.e, false);
		// laid out at the row's height when stretched to it, else at its own
		double h = d.content;
		std::optional<double> want;
		if ((as == Align::Normal || as == Align::Stretch) && !spec_h && !replaced(it))
			want = clamp_h(it, row_h[p.r] - d.e.mt.value_or(0) - d.e.mb.value_or(0), row_h[p.r], d.e);
		if (want && std::fabs(*want - h) > 0.001)
		{
			layout_box(it, d.w, *want, col_w[p.c], row_h[p.r]);
			h = *want;
		}
		else
			layout_box(it, d.w, std::nullopt, col_w[p.c], d.fixed_h);
		double off = 0;
		const double free = row_h[p.r] - h - d.e.mt.value_or(0) - d.e.mb.value_or(0);
		if (as == Align::Center)
			off = free / 2;
		else if (as == Align::End || as == Align::FlexEnd)
			off = free;
		double xoff = 0;
		const double free_x = col_w[p.c] - d.w - d.e.ml.value_or(0) - d.e.mr.value_or(0);
		if (ji == Align::Center)
			xoff = free_x / 2;
		else if (ji == Align::End)
			xoff = free_x;
		it.x = col_x[p.c] + d.e.ml.value_or(0) + xoff;
		it.y = row_y[p.r] + d.e.mt.value_or(0) + off;
		if (!base && p.r == 0)
			base = it.y + (it.base ? *it.base : it.h);
		if (it.kind == LayoutNode::Kind::Anonymous)
			adopt_anon(n, it);
	}
	return { ch_def ? *ch_def : used_h, base };
}

} // namespace qdom
