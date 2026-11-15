// Copyright 2026 Ian Karlsson
// Tables of rows of cells, without spans: the automatic layout's column widths between
// the cells' least and most, border-spacing (none when the borders collapse), cells aligned on their first
// baseline or by vertical-align, the row groups' boxes about their rows.
#include <algorithm>
#include <cmath>

#include "dom/dom.h"
#include "layout/layout.h"

namespace qdom
{

namespace
{

bool is_row(const LayoutNode *n)
{
	return n->kind == LayoutNode::Kind::Element && n->el->is(n::tr);
}

bool is_group(const LayoutNode *n)
{
	return n->kind == LayoutNode::Kind::Element && (n->el->is(n::tbody) || n->el->is(n::thead) || n->el->is(n::tfoot));
}

bool is_cell(const LayoutNode *n)
{
	return n->kind == LayoutNode::Kind::Element && (n->el->is(n::td) || n->el->is(n::th));
}

} // namespace

std::vector<Layout::TableRow> Layout::table_rows(LayoutNode &n)
{
	std::vector<TableRow> rows;
	auto add = [&](LayoutNode *tr, LayoutNode *group) {
		TableRow r { tr, group, {} };
		for (LayoutNode *c : tr->kids)
			if (is_cell(c))
				r.cells.push_back(c);
		rows.push_back(std::move(r));
	};
	for (LayoutNode *c : n.kids)
	{
		if (is_row(c))
			add(c, nullptr);
		else if (is_group(c))
			for (LayoutNode *r : c->kids)
				if (is_row(r))
					add(r, c);
	}
	return rows;
}

double Layout::spacing_of(const LayoutNode &n) const
{
	return n.cs().border_collapse == BorderCollapse::Collapse ? 0 : n.cs().border_spacing;
}

Layout::Intrinsic Layout::table_intrinsic(LayoutNode &n)
{
	const std::vector<TableRow> rows = table_rows(n);
	size_t cols = 0;
	for (const TableRow &r : rows)
		cols = std::max(cols, r.cells.size());
	std::vector<double> mn(cols, 0), mx(cols, 0);
	for (const TableRow &r : rows)
		for (size_t i = 0; i < r.cells.size(); i++)
		{
			const Intrinsic o = intrinsic(*r.cells[i]);
			mn[i] = std::max(mn[i], o.min);
			mx[i] = std::max(mx[i], o.max);
		}
	const double sp = spacing_of(n) * double(cols + 1);
	double a = sp, b = sp;
	for (size_t i = 0; i < cols; i++)
	{
		a += mn[i];
		b += mx[i];
	}
	return { a, b };
}

Layout::Content Layout::layout_table(LayoutNode &n, double cw)
{
	const std::vector<TableRow> rows = table_rows(n);
	size_t cols = 0;
	for (const TableRow &r : rows)
		cols = std::max(cols, r.cells.size());
	std::vector<double> mn(cols, 0), mx(cols, 0);
	for (const TableRow &r : rows)
		for (size_t i = 0; i < r.cells.size(); i++)
		{
			const Intrinsic o = intrinsic(*r.cells[i]);
			mn[i] = std::max(mn[i], o.min);
			mx[i] = std::max(mx[i], o.max);
		}
	const double sp = spacing_of(n);
	const double avail = cw - sp * double(cols + 1);
	double sum_min = 0, sum_max = 0;
	for (size_t i = 0; i < cols; i++)
	{
		sum_min += mn[i];
		sum_max += mx[i];
	}
	// the columns between their least and most widths; wider than their most, each grows by its share of it
	std::vector<double> widths(cols);
	for (size_t i = 0; i < cols; i++)
		widths[i] = sum_max <= avail ? (sum_max > 0 ? mx[i] * avail / sum_max : avail / double(cols))
			: sum_min >= avail ? mn[i]
			: mn[i] + (mx[i] - mn[i]) * (avail - sum_min) / std::max(1e-9, sum_max - sum_min);
	double y = sp;
	std::optional<double> base;
	LayoutNode *group = nullptr;
	double group_y = 0;
	for (const TableRow &r : rows)
	{
		// the cells on their first baselines (a cell without text on its bottom), the row as tall as they make it
		std::vector<double> nat(r.cells.size());
		std::vector<std::optional<double>> lift(r.cells.size());
		for (size_t i = 0; i < r.cells.size(); i++)
		{
			LayoutNode &c = *r.cells[i];
			layout_box(c, widths[i], std::nullopt, cw, std::nullopt);
			nat[i] = c.h;
			if (c.cs().vertical_align == VerticalAlign::Baseline)
				lift[i] = c.base ? *c.base : c.h - c.bb - c.pb;
		}
		double row_base = 0;
		for (const auto &l : lift)
			if (l)
				row_base = std::max(row_base, *l);
		double h = 0;
		for (size_t i = 0; i < r.cells.size(); i++)
			h = std::max(h, nat[i] + (lift[i] ? row_base - *lift[i] : 0));
		double width = sp * double(cols > 0 ? cols - 1 : 0);
		for (double w : widths)
			width += w;
		// a row's box about its cells, a group's about its rows
		if (r.group != group)
		{
			group = r.group;
			group_y = y;
			if (group)
			{
				group->laid = true;
				group->pass = m_pass;
				group->x = sp;
				group->y = y;
				group->w = group->cw = width;
				group->h = group->ch = 0;
			}
		}
		LayoutNode &tr = *r.row;
		tr.laid = true;
		tr.pass = m_pass;
		tr.x = group ? 0 : sp;
		tr.y = group ? y - group_y : y;
		tr.w = tr.cw = width;
		tr.h = tr.ch = h;
		if (group)
			group->h = group->ch = y + h - group_y;
		double x = sp;
		for (size_t i = 0; i < r.cells.size(); i++)
		{
			LayoutNode &c = *r.cells[i];
			const double content = c.content_h;
			if (std::fabs(c.h - h) > 0.001)
				layout_box(c, widths[i], h, cw, std::nullopt);
			const double extra = h - (content + c.bt + c.bb + c.pt + c.pb);
			const VerticalAlign va = c.cs().vertical_align;
			const double dy = lift[i] ? row_base - *lift[i] : va == VerticalAlign::Middle ? extra / 2
				: va == VerticalAlign::Bottom ? extra : 0;
			// moved by what it was not moved by already (a cell taken as it was laid out keeps its shift)
			if (const double by = dy - c.cell_dy; by != 0)
			{
				for (LayoutNode *k : c.kids)
					if (k->kind != LayoutNode::Kind::Text && k->laid && !out_of_flow(*k))
						k->y += by;
				for (TextPiece &p : c.pieces)
					p.y += by;
				c.cell_dy = dy;
			}
			c.x = x - sp;
			c.y = 0;
			if (!base && c.base)
				base = y + *c.base + dy;
			x += widths[i] + sp;
		}
		y += h + sp;
	}
	if (rows.empty())
		y = 0;
	return { y, base };
}

} // namespace qdom
