// Copyright 2026 Ian Karlsson
// The document drawn into commands, and hit testing in the same order.
#include "paint/paint.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>

#include "dom/dom.h"
#include "input/field.h"
#include "input/input.h"
#include "layout/layout.h"
#include "paint/canvas.h"

namespace qdom
{

int op_length(int o)
{
	switch (o)
	{
	case op::SAVE: case op::RESTORE: case op::BEGIN: case op::CLOSE: return 0;
	case op::ALPHA: return 1;
	case op::MOVE: case op::LINE: return 2;
	case op::CLIP: case op::FILL: case op::CLEAR: return 4;
	case op::BEZIER: case op::IMAGE: case op::TRANSFORM: return 6;
	case op::STROKE: return 7;
	case op::TEXT: case op::SHADOW: return 10;
	case op::RECT: return 12;
	case op::STROKE_RECT: return 13;
	case op::GRADIENT: return 25;
	default: return -1;
	}
}

namespace
{

constexpr double PI = 3.14159265358979323846;
const Color SELECTION { 43, 93, 134, 1 };    // the selection's colour
const Color SCROLL_THUMB { 69, 73, 80, 1 };  // the scrollbar thumb's colour

// ---------------------------------------------------------------- paths

using PathCmd = std::vector<double>;  // 'M' x y, 'L' x y, 'C' x1 y1 x2 y2 x y, 'Z'

//! An elliptical arc as cubic curves (SVG's endpoint form to its centre form).
void arc_curves(std::vector<PathCmd> &out, double x1, double y1, double rx, double ry, double rot, bool large,
	bool sweep, double x2, double y2)
{
	if (rx == 0 || ry == 0 || (x1 == x2 && y1 == y2))
	{
		out.push_back({ 'C', x1, y1, x2, y2, x2, y2 });
		return;
	}
	rx = std::fabs(rx);
	ry = std::fabs(ry);
	const double phi = rot * PI / 180, c = std::cos(phi), s = std::sin(phi);
	const double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
	const double x1p = c * dx + s * dy, y1p = -s * dx + c * dy;
	const double lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
	if (lam > 1)
	{
		rx *= std::sqrt(lam);
		ry *= std::sqrt(lam);
	}
	const double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
	const double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
	double co = std::sqrt(std::max(0.0, num / den));
	if (large == sweep)
		co = -co;
	const double cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
	const double cx = c * cxp - s * cyp + (x1 + x2) / 2, cy = s * cxp + c * cyp + (y1 + y2) / 2;
	auto ang = [](double ux, double uy, double vx, double vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
	const double t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
	double dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
	if (!sweep && dt > 0)
		dt -= 2 * PI;
	else if (sweep && dt < 0)
		dt += 2 * PI;
	const int segs = std::max(1, int(std::ceil(std::fabs(dt) / (PI / 2))));
	const double step = dt / segs, k = 4.0 / 3 * std::tan(step / 4);
	auto pt = [&](double t) {
		return std::pair { cx + rx * std::cos(t) * c - ry * std::sin(t) * s, cy + rx * std::cos(t) * s + ry * std::sin(t) * c };
	};
	auto dpt = [&](double t) {
		return std::pair { -rx * std::sin(t) * c - ry * std::cos(t) * s, -rx * std::sin(t) * s + ry * std::cos(t) * c };
	};
	for (int i = 0; i < segs; i++)
	{
		const double a = t1 + i * step, b = a + step;
		const auto p0 = pt(a), p3 = pt(b), d0 = dpt(a), d3 = dpt(b);
		out.push_back({ 'C', p0.first + k * d0.first, p0.second + k * d0.second, p3.first - k * d3.first,
			p3.second - k * d3.second, p3.first, p3.second });
	}
}

//! An SVG path's data as absolute moves, lines, cubic curves and closes.
const std::vector<PathCmd> &parse_path(const std::string &d)
{
	static thread_local std::unordered_map<std::string, std::vector<PathCmd>> cache;
	auto it = cache.find(d);
	if (it != cache.end())
		return it->second;
	if (cache.size() > 4000)
		cache.clear();
	std::vector<PathCmd> p;
	// the tokens: commands and numbers
	std::vector<std::pair<char, double>> tok;
	for (size_t i = 0; i < d.size();)
	{
		const char ch = d[i];
		if (std::string_view("MLHVACQZmlhvacqz").find(ch) != std::string_view::npos)
		{
			tok.emplace_back(ch, 0);
			i++;
			continue;
		}
		if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '+')
		{
			size_t j = i + 1;
			bool dot = ch == '.';
			while (j < d.size() && ((d[j] >= '0' && d[j] <= '9') || (d[j] == '.' && !dot)))
			{
				dot = dot || d[j] == '.';
				j++;
			}
			if (j < d.size() && (d[j] == 'e' || d[j] == 'E'))
			{
				j++;
				if (j < d.size() && (d[j] == '-' || d[j] == '+'))
					j++;
				while (j < d.size() && d[j] >= '0' && d[j] <= '9')
					j++;
			}
			tok.emplace_back(0, parse_number(std::string_view(d).substr(i, j - i)).value_or(0));
			i = j;
			continue;
		}
		i++;
	}
	size_t i = 0;
	char cmd = 0;
	double x = 0, y = 0, sx = 0, sy = 0;
	auto num = [&]() { return i < tok.size() ? tok[i++].second : 0.0; };
	while (i < tok.size())
	{
		if (tok[i].first)
			cmd = tok[i++].first;
		if (!cmd)
			break;
		const bool rel = cmd >= 'a';
		const char C = char(rel ? cmd - 32 : cmd);
		if (C == 'Z')
		{
			p.push_back({ 'Z' });
			x = sx;
			y = sy;
			continue;
		}
		if (i >= tok.size() || tok[i].first)
			break;
		switch (C)
		{
		case 'M':
			x = (rel ? x : 0) + num();
			y = (rel ? y : 0) + num();
			sx = x;
			sy = y;
			p.push_back({ 'M', x, y });
			cmd = rel ? 'l' : 'L';
			break;
		case 'L':
			x = (rel ? x : 0) + num();
			y = (rel ? y : 0) + num();
			p.push_back({ 'L', x, y });
			break;
		case 'H':
			x = (rel ? x : 0) + num();
			p.push_back({ 'L', x, y });
			break;
		case 'V':
			y = (rel ? y : 0) + num();
			p.push_back({ 'L', x, y });
			break;
		case 'C':
		{
			const double ox = rel ? x : 0, oy = rel ? y : 0;
			double a[6];
			for (int k = 0; k < 6; k++)
				a[k] = num() + (k % 2 ? oy : ox);
			p.push_back({ 'C', a[0], a[1], a[2], a[3], a[4], a[5] });
			x = a[4];
			y = a[5];
			break;
		}
		case 'Q':
		{
			const double ox = rel ? x : 0, oy = rel ? y : 0;
			const double qx = num() + ox, qy = num() + oy, ex = num() + ox, ey = num() + oy;
			p.push_back({ 'C', x + 2.0 / 3 * (qx - x), y + 2.0 / 3 * (qy - y), ex + 2.0 / 3 * (qx - ex),
				ey + 2.0 / 3 * (qy - ey), ex, ey });
			x = ex;
			y = ey;
			break;
		}
		case 'A':
		{
			const double rx = num(), ry = num(), rot = num(), large = num(), sweep = num();
			const double ex = (rel ? x : 0) + num(), ey = (rel ? y : 0) + num();
			arc_curves(p, x, y, rx, ry, rot, large != 0, sweep != 0, ex, ey);
			x = ex;
			y = ey;
			break;
		}
		default:
			i++;
		}
	}
	return cache.emplace(d, std::move(p)).first->second;
}

using Polyline = std::vector<std::pair<double, double>>;

//! A path as polylines, curves cut in eight, for dashes and for finding the pointer.
std::vector<Polyline> flatten(const std::vector<PathCmd> &cmds)
{
	std::vector<Polyline> lines;
	double x = 0, y = 0;
	for (const PathCmd &c : cmds)
	{
		switch (char(c[0]))
		{
		case 'M':
			lines.push_back({ { c[1], c[2] } });
			x = c[1];
			y = c[2];
			break;
		case 'L':
			if (lines.empty())
				lines.push_back({ { x, y } });
			lines.back().emplace_back(c[1], c[2]);
			x = c[1];
			y = c[2];
			break;
		case 'C':
			if (lines.empty())
				lines.push_back({ { x, y } });
			for (int t = 1; t <= 8; t++)
			{
				const double u = t / 8.0, v = 1 - u;
				lines.back().emplace_back(v * v * v * x + 3 * v * v * u * c[1] + 3 * v * u * u * c[3] + u * u * u * c[5],
					v * v * v * y + 3 * v * v * u * c[2] + 3 * v * u * u * c[4] + u * u * u * c[6]);
			}
			x = c[5];
			y = c[6];
			break;
		case 'Z':
			if (!lines.empty() && !lines.back().empty())
				lines.back().push_back(lines.back().front());
			break;
		default:
			break;
		}
	}
	return lines;
}

std::vector<Polyline> dashed(const std::vector<Polyline> &lines, const std::vector<double> &dash)
{
	double total = 0;
	for (double d : dash)
		total += d;
	if (dash.empty() || total <= 0)
		return lines;
	std::vector<Polyline> out;
	for (const Polyline &l : lines)
	{
		if (l.empty())
			continue;
		size_t k = 0;
		double left = dash[0];
		bool on = true;
		Polyline cur { l[0] };
		for (size_t i = 1; i < l.size(); i++)
		{
			double ax = l[i - 1].first, ay = l[i - 1].second;
			const double bx = l[i].first, by = l[i].second;
			double seg = std::hypot(bx - ax, by - ay);
			while (seg > 0)
			{
				const double t = std::min(seg, left), f = t / seg;
				const double nx = ax + (bx - ax) * f, ny = ay + (by - ay) * f;
				if (on)
					cur.emplace_back(nx, ny);
				seg -= t;
				left -= t;
				ax = nx;
				ay = ny;
				if (left <= 1e-6)
				{
					if (on && cur.size() > 1)
						out.push_back(cur);
					on = !on;
					k = (k + 1) % dash.size();
					left = dash[k];
					cur = on ? Polyline { { ax, ay } } : Polyline {};
				}
			}
		}
		if (on && cur.size() > 1)
			out.push_back(cur);
	}
	return out;
}

bool in_polygon(const Polyline &l, double x, double y)
{
	bool inside = false;
	for (size_t i = 0, j = l.size() - 1; i < l.size(); j = i++)
	{
		const auto [xi, yi] = l[i];
		const auto [xj, yj] = l[j];
		if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
			inside = !inside;
	}
	return inside;
}

bool near_line(const Polyline &l, double x, double y, double d)
{
	for (size_t i = 1; i < l.size(); i++)
	{
		const auto [ax, ay] = l[i - 1];
		const auto [bx, by] = l[i];
		const double dx = bx - ax, dy = by - ay, len = dx * dx + dy * dy;
		const double t = len ? std::clamp(((x - ax) * dx + (y - ay) * dy) / len, 0.0, 1.0) : 0;
		if (std::hypot(x - ax - t * dx, y - ay - t * dy) <= d)
			return true;
	}
	return false;
}

// ---------------------------------------------------------------- gradients

struct Gradient
{
	bool radial = false;
	double angle = 180;
	double at_x = 0.5, at_y = 0.5;
	std::vector<std::pair<Color, double>> stops;
};

//! A background-image's gradient: linear (to a side, or an angle) or radial (a circle at a place); none for what
//! is not one.
std::optional<Gradient> parse_gradient(const std::string &v)
{
	const std::string l = lower(trim(v));
	const bool radial = l.starts_with("radial-gradient(");
	if (!(radial || l.starts_with("linear-gradient(")) || v.back() != ')')
		return std::nullopt;
	const size_t open = v.find('(');
	std::vector<std::string_view> parts = split_commas(std::string_view(v).substr(open + 1, v.size() - open - 2));
	Gradient g;
	g.radial = radial;
	if (!parts.empty() && !parse_color(split_words(parts[0]).empty() ? "" : split_words(parts[0])[0]))
	{
		const std::string head = lower(parts[0]);
		parts.erase(parts.begin());
		if (head == "to top")
			g.angle = 0;
		else if (head == "to right")
			g.angle = 90;
		else if (head == "to bottom")
			g.angle = 180;
		else if (head == "to left")
			g.angle = 270;
		else if (head.ends_with("deg"))
			g.angle = parse_number(std::string_view(head).substr(0, head.size() - 3)).value_or(180);
		const size_t at = head.find("at ");
		if (at != std::string::npos)
		{
			const auto w = split_words(std::string_view(head).substr(at + 3));
			if (w.size() >= 2 && w[0].ends_with('%') && w[1].ends_with('%'))
			{
				g.at_x = parse_number(w[0].substr(0, w[0].size() - 1)).value_or(50) / 100;
				g.at_y = parse_number(w[1].substr(0, w[1].size() - 1)).value_or(50) / 100;
			}
		}
	}
	std::vector<std::optional<double>> places;
	for (std::string_view part : parts)
	{
		const auto w = split_words(part);
		if (w.empty())
			continue;
		if (std::optional<Color> c = parse_color(w[0]))
		{
			std::optional<double> at;
			if (w.size() > 1 && w[1].ends_with('%'))
				at = parse_number(w[1].substr(0, w[1].size() - 1)).value_or(0) / 100;
			g.stops.emplace_back(*c, 0);
			places.push_back(at);
		}
	}
	const size_t n = g.stops.size();
	if (n < 2)
		return std::nullopt;
	for (size_t i = 0; i < n; i++)
		g.stops[i].second = places[i] ? *places[i] : double(i) / double(n - 1);
	return g;
}

class Painter
{
public:
	Painter(Dom &dom, Commands &out, const PaintOptions &options) : m_dom(dom), m_out(out), m_options(options) {}

	void run()
	{
		Layout *layout = m_dom.layout_tree();
		LayoutNode *root = layout ? layout->root() : nullptr;
		const double vw = m_dom.width(), vh = m_dom.height();
		// the canvas: the root's background, else the body's
		Color bg { 255, 255, 255, 1 };
		if (root)
		{
			const Color html = root->cs().background_color;
			const Element *body = m_dom.document()->body();
			if (html.visible())
				bg = html;
			else if (body && body->style && body->style->background_color.visible())
				bg = body->style->background_color;
		}
		m_out.push({ op::RECT, 0, 0, vw, vh, 0, 0, 0, 0 });
		m_out.color(bg);
		if (root)
			walk(*root, 0, 0, 1);
		// the dialogs, each over its backdrop
		for (const ElementRef &r : m_dom.document()->top_layer())
		{
			LayoutNode *d = m_dom.layout_of(r.el);
			if (!d)
				continue;
			const StylePtr backdrop = m_dom.styles.compute(*r.el, r.el->style.get(), n::backdrop);
			if (backdrop && backdrop->background_color.visible())
			{
				m_out.push({ op::RECT, 0, 0, vw, vh, 0, 0, 0, 0 });
				m_out.color(backdrop->background_color);
			}
			walk(*d, 0, 0, 1);
		}
		// what the input draws over the page: a list's popup, a tooltip
		m_dom.input().overlay(m_out);
	}

private:
	Dom &m_dom;
	Commands &m_out;
	const PaintOptions &m_options;

	std::array<double, 4> radii(const ComputedStyle &cs, double w, double h)
	{
		std::array<double, 4> r {};
		for (int i = 0; i < 4; i++)
			r[i] = std::clamp(cs.radius[i].resolve(std::min(w, h)).value_or(0), 0.0, std::min(w, h) / 2);
		return r;
	}

	void rect(double x, double y, double w, double h, const std::array<double, 4> &r, const Color &c, double alpha = 1)
	{
		m_out.push({ op::RECT, x, y, w, h, r[0], r[1], r[2], r[3] });
		m_out.color(c, alpha);
	}

	//! A box's shadows: those outside it (before its background), or those inside it (after its background, inside
	//! its padding box).
	void shadow(const ComputedStyle &cs, double x, double y, double w, double h, const std::array<double, 4> &r,
		bool inside)
	{
		if (cs.box_shadow.empty())
			return;
		for (std::string_view one : split_commas(cs.box_shadow))
		{
			std::vector<double> nums;
			std::optional<Color> color;
			bool inset = false;
			for (std::string_view word : split_words(one))
			{
				if (lower(word) == "inset")
					inset = true;
				else if (std::optional<Length> l = parse_length(word, {}); l && l->is_value())
					nums.push_back(l->px);
				else if (std::optional<Color> c = parse_color(word))
					color = c;
			}
			if (!color || nums.size() < 2 || inset != inside)
				continue;
			const double ox = nums[0], oy = nums[1], blur = nums.size() > 2 ? nums[2] : 0, spread = nums.size() > 3 ? nums[3] : 0;
			if (inset)
			{
				// unblurred: a line along an edge (the box less itself moved by the offset,
				// following its corners), or a ring of the spread's width
				if (ox || oy)
				{
					if (oy)
						inset_band(x, y, w, h, r, oy, false, *color);
					if (ox)
						inset_band(x, y, w, h, r, ox, true, *color);
				}
				else if (spread > 0)
				{
					auto in = [&](double q) { return std::max(0.0, q - spread / 2); };
					m_out.push({ op::STROKE_RECT, x + spread / 2, y + spread / 2, w - spread, h - spread, in(r[0]), in(r[1]),
						in(r[2]), in(r[3]), spread });
					m_out.color(*color);
				}
				continue;
			}
			m_out.push({ op::SHADOW, x + ox - spread, y + oy - spread, w + 2 * spread, h + 2 * spread, r[0], blur });
			m_out.color(*color);
		}
	}

	//! What of a rounded box its copy moved by an offset along one axis leaves uncovered: a band along the edge it
	//! moved away from, as thick as the offset, its ends following the corners. Across: the offset is along x.
	void inset_band(double x, double y, double w, double h, const std::array<double, 4> &r, double off, bool across,
		const Color &color)
	{
		// in the band's own axes: u along the edge, v across it, the band at v's far side for a negative offset
		const double len = across ? h : w, depth = across ? w : h, t = std::min(std::fabs(off), depth);
		// the corners at the band's start and end, at the near (v = 0) and far (v = depth) sides
		const double near0 = r[0], near1 = across ? r[3] : r[1];
		const double far0 = across ? r[1] : r[3], far1 = r[2];
		auto edge = [&](double u, double r0, double r1, bool far) {
			// how far in from its side the box's edge is at u
			double in = 0;
			if (r0 > 0 && u < r0)
				in = r0 - std::sqrt(std::max(0.0, r0 * r0 - (r0 - u) * (r0 - u)));
			else if (r1 > 0 && u > len - r1)
				in = r1 - std::sqrt(std::max(0.0, r1 * r1 - (u - (len - r1)) * (u - (len - r1))));
			return far ? depth - in : in;
		};
		const bool far = off < 0;
		std::vector<double> us { 0, len };
		// the band's ends sampled through the corners on either side
		const double ca = std::max(near0, far0), cb = std::max(near1, far1);
		for (int k = 1; k < 16; k++)
		{
			us.push_back(ca * k / 16);
			us.push_back(len - cb * k / 16);
		}
		std::sort(us.begin(), us.end());
		auto at = [&](double u, double v) { return across ? std::pair { x + v, y + u } : std::pair { x + u, y + v }; };
		std::vector<std::pair<double, double>> pts;
		for (double u : us)
		{
			const double outer = far ? edge(u, far0, far1, true) : edge(u, near0, near1, false);
			pts.push_back(at(u, outer));
		}
		for (auto it = us.rbegin(); it != us.rend(); ++it)
		{
			const double u = *it;
			const double outer = far ? edge(u, far0, far1, true) : edge(u, near0, near1, false);
			const double other = far ? edge(u, near0, near1, false) : edge(u, far0, far1, true);
			pts.push_back(at(u, far ? std::max(other, outer - t) : std::min(other, outer + t)));
		}
		m_out.push({ op::BEGIN, op::MOVE, pts[0].first, pts[0].second });
		for (size_t i = 1; i < pts.size(); i++)
			m_out.push({ op::LINE, pts[i].first, pts[i].second });
		m_out.push({ op::CLOSE, op::FILL });
		m_out.color(color);
	}

	void gradient(const Gradient &g, double x, double y, double w, double h, const std::array<double, 4> &r)
	{
		auto band = [&](double sx, double sy, double sw, double sh, double a, double b, double c, double d,
			const Color &c0, const Color &c1) {
			m_out.push({ op::GRADIENT, x, y, w, h, r[0], r[1], r[2], r[3], sx, sy, sw, sh, g.radial ? 1.0 : 0.0, a, b, c, d });
			m_out.color(c0);
			m_out.color(c1);
		};
		const auto &st = g.stops;
		const auto &first = st.front(), &last = st.back();
		if (g.radial)
		{
			const double cx = x + g.at_x * w, cy = y + g.at_y * h;
			const double R = std::max({ std::hypot(cx - x, cy - y), std::hypot(x + w - cx, cy - y),
				std::hypot(cx - x, y + h - cy), std::hypot(x + w - cx, y + h - cy) });
			band(x, y, w, h, cx, cy, first.second * R, last.second * R, first.first, last.first);
			return;
		}
		const double a = g.angle * PI / 180;
		const double dx = std::round(std::sin(a) * 1e6) / 1e6, dy = std::round(-std::cos(a) * 1e6) / 1e6;
		const double len = std::fabs(w * dx) + std::fabs(h * dy), mx = x + w / 2, my = y + h / 2;
		auto px = [&](double t) { return mx + dx * len * (t - 0.5); };
		auto py = [&](double t) { return my + dy * len * (t - 0.5); };
		if (std::fmod(g.angle, 90) != 0)
		{
			band(x, y, w, h, px(first.second), py(first.second), px(last.second), py(last.second), first.first, last.first);
			return;
		}
		for (size_t i = 0; i + 1 < st.size(); i++)
		{
			const auto &[c0, t0] = st[i];
			const auto &[c1, t1] = st[i + 1];
			const double xa = px(t0), ya = py(t0), xb = px(t1), yb = py(t1);
			// the band between the two places, and what lies before the first stop or after the last
			const double lo = i == 0 ? -1e4 : 0, hi = i + 2 == st.size() ? 1e4 : 0;
			const double sx = dx ? std::min(xa, xb) - (dx > 0 ? -lo : hi) : x;
			const double ex = dx ? std::max(xa, xb) + (dx > 0 ? hi : -lo) : x + w;
			const double sy = dy ? std::min(ya, yb) - (dy > 0 ? -lo : hi) : y;
			const double ey = dy ? std::max(ya, yb) + (dy > 0 ? hi : -lo) : y + h;
			band(sx, sy, ex - sx, ey - sy, xa, ya, xb, yb, c0, c1);
		}
	}

	void background(const LayoutNode &n, double x, double y)
	{
		const ComputedStyle &cs = n.cs();
		const double w = n.w, h = n.h;
		const std::array<double, 4> r = radii(cs, w, h);
		shadow(cs, x, y, w, h, r, false);
		if (cs.background_color.visible())
			rect(x, y, w, h, r, cs.background_color);
		if (!cs.background_image.empty())
			if (std::optional<Gradient> g = parse_gradient(cs.background_image))
				gradient(*g, x, y, w, h, r);
		const double bt = n.bt, br = n.br, bb = n.bb, bl = n.bl;
		{
			// the shadows inside: over the background, in the padding box
			auto in = [](double q, double a, double b) { return std::max(0.0, q - std::max(a, b)); };
			const std::array<double, 4> ri { in(r[0], bt, bl), in(r[1], bt, br), in(r[2], bb, br), in(r[3], bb, bl) };
			shadow(cs, x + bl, y + bt, w - bl - br, h - bt - bb, ri, true);
		}
		if (!(bt || br || bb || bl))
			return;
		const auto &bc = cs.border_color;
		const auto &bs = cs.border_style;
		auto broken = [](BorderStyle b) { return b == BorderStyle::Dashed || b == BorderStyle::Dotted; };
		if ((broken(bs[0]) || broken(bs[1]) || broken(bs[2]) || broken(bs[3])) && !(r[0] || r[1] || r[2] || r[3]))
		{
			// dashes (three times the width) or dots (the width) along each side, a whole one at each end and the
			// rest spread evenly between, as Firefox lays them out
			auto side = [&](BorderStyle b, const Color &c, double sx, double sy, double len, double width, bool across) {
				if (!c.visible() || len <= 0 || width <= 0)
					return;
				if (!broken(b))
				{
					rect(sx, sy, across ? width : len, across ? len : width, {}, c);
					return;
				}
				const double d = b == BorderStyle::Dashed ? 3 * width : width;
				const int count = std::max(1, int(std::round((len + d) / (2 * d))));
				const double gap = count > 1 ? (len - count * d) / (count - 1) : 0;
				for (int k = 0; k < count; k++)
				{
					const double at = count > 1 ? k * (d + gap) : (len - d) / 2;
					const double l = std::min(d, len);
					rect(across ? sx : sx + at, across ? sy + at : sy, across ? width : l, across ? l : width, {}, c);
				}
			};
			side(bs[TOP], bc[TOP], x, y, w, bt, false);
			side(bs[BOTTOM], bc[BOTTOM], x, y + h - bb, w, bb, false);
			side(bs[LEFT], bc[LEFT], x, y, h, bl, true);
			side(bs[RIGHT], bc[RIGHT], x + w - br, y, h, br, true);
			return;
		}
		if (bt == br && bt == bb && bt == bl && bc[0] == bc[1] && bc[0] == bc[2] && bc[0] == bc[3])
		{
			if (bc[0].visible())
			{
				m_out.push({ op::STROKE_RECT, x + bt / 2, y + bt / 2, w - bt, h - bt, std::max(0.0, r[0] - bt / 2),
					std::max(0.0, r[1] - bt / 2), std::max(0.0, r[2] - bt / 2), std::max(0.0, r[3] - bt / 2), bt });
				m_out.color(bc[0]);
			}
			return;
		}
		if (r[0] || r[1] || r[2] || r[3])
		{
			rounded_sides(x, y, w, h, r, { bt, br, bb, bl }, bc);
			return;
		}
		const std::array<double, 4> none {};
		auto side = [&](const Color &c, double sx, double sy, double sw, double sh) {
			if (c.visible() && sw > 0 && sh > 0)
				rect(sx, sy, sw, sh, none, c);
		};
		side(bc[TOP], x, y, w, bt);
		side(bc[BOTTOM], x, y + h - bb, w, bb);
		side(bc[LEFT], x, y + bt, bl, h - bt - bb);
		side(bc[RIGHT], x + w - br, y + bt, br, h - bt - bb);
	}

	//! A rounded box's border whose sides differ in width or colour (a split button's half without its left side):
	//! each side a shape of its own between the outer edge and the inner one (the padding box's, its corners' radii
	//! less the sides' widths), the corners shared by their two sides in the ratio of their widths, so that a side
	//! of no width leaves its corners to the others. Widths and colours by side: top, right, bottom, left; radii
	//! top left, top right, bottom right, bottom left.
	void rounded_sides(double x, double y, double w, double h, const std::array<double, 4> &r,
		const std::array<double, 4> &bw, const std::array<Color, 4> &bc)
	{
		const double PI_2 = PI / 2;
		// the corners: their outer centres and radius, inner centres and radii, the angle they start at, and the
		// sides before and after them (y grows down: 0 points right, PI / 2 down)
		struct Corner
		{
			double ox, oy, r, ix, iy, rx, ry, start;
			int before, after;
		};
		const double bt = bw[0], br = bw[1], bb = bw[2], bl = bw[3];
		auto inner = [](double q, double b) { return std::max(0.0, q - b); };
		const Corner corners[4] = {
			// top left: the left side, then the top
			{ x + r[0], y + r[0], r[0], x + bl + inner(r[0], bl), y + bt + inner(r[0], bt), inner(r[0], bl),
				inner(r[0], bt), PI, 3, 0 },
			// top right: the top, then the right
			{ x + w - r[1], y + r[1], r[1], x + w - br - inner(r[1], br), y + bt + inner(r[1], bt), inner(r[1], br),
				inner(r[1], bt), 3 * PI_2, 0, 1 },
			// bottom right: the right, then the bottom
			{ x + w - r[2], y + h - r[2], r[2], x + w - br - inner(r[2], br), y + h - bb - inner(r[2], bb),
				inner(r[2], br), inner(r[2], bb), 0, 1, 2 },
			// bottom left: the bottom, then the left
			{ x + r[3], y + h - r[3], r[3], x + bl + inner(r[3], bl), y + h - bb - inner(r[3], bb), inner(r[3], bl),
				inner(r[3], bb), PI_2, 2, 3 },
		};
		// where a corner passes from the side before it to the one after
		auto split = [&](const Corner &c) {
			const double a = bw[size_t(c.before)], b = bw[size_t(c.after)];
			return c.start + (a + b > 0 ? PI_2 * a / (a + b) : PI_2 / 2);
		};
		using Points = std::vector<std::pair<double, double>>;
		// a corner's outer and inner edges from angle a0 to a1
		auto arc = [](const Corner &c, double a0, double a1, Points &outer, Points &in) {
			const int n = std::max(1, int(std::ceil(8 * std::fabs(a1 - a0) / (PI / 2))));
			for (int k = 0; k <= n; k++)
			{
				const double a = a0 + (a1 - a0) * k / n;
				outer.emplace_back(c.ox + c.r * std::cos(a), c.oy + c.r * std::sin(a));
				in.emplace_back(c.ix + c.rx * std::cos(a), c.iy + c.ry * std::sin(a));
			}
		};
		for (int s = 0; s < 4; s++)
		{
			const Color &c = bc[size_t(s)];
			if (!c.visible() || bw[size_t(s)] <= 0)
				continue;
			// the side runs from the corner before it (whose second side it is) to the one after
			const Corner &from = corners[size_t(s)], &to = corners[size_t((s + 1) % 4)];
			Points outer, in;
			arc(from, split(from), from.start + PI_2, outer, in);
			arc(to, to.start, split(to), outer, in);
			m_out.push({ op::BEGIN, op::MOVE, outer[0].first, outer[0].second });
			for (size_t i = 1; i < outer.size(); i++)
				m_out.push({ op::LINE, outer[i].first, outer[i].second });
			for (auto it = in.rbegin(); it != in.rend(); ++it)
				m_out.push({ op::LINE, it->first, it->second });
			m_out.push({ op::CLOSE, op::FILL });
			m_out.color(c);
		}
	}

	//! A line of text, cut with an ellipsis where its box clips it and asks for one.
	void text(const TextPiece &p, double x, double y, double alpha, std::optional<double> clip_right)
	{
		const ComputedStyle &cs = p.owner->cs();
		if (!cs.color.visible() || cs.visibility != Visibility::Visible)
			return;
		std::string s = p.text;
		// a word past the edge is cut away; the one across it ends in the ellipsis
		if (clip_right && x + p.x >= *clip_right)
			return;
		if (clip_right && x + p.x + p.w > *clip_right + 0.5)
		{
			const std::string ell = "…";
			const double room = *clip_right - (x + p.x) - m_dom.fonts.width(p.face, p.size, p.spacing, ell);
			while (!s.empty() && m_dom.fonts.width(p.face, p.size, p.spacing, s) > room)
			{
				// one character off, a whole UTF-8 sequence
				size_t k = s.size() - 1;
				while (k > 0 && (static_cast<unsigned char>(s[k]) & 0xc0) == 0x80)
					k--;
				s.resize(k);
			}
			s += ell;
		}
		const double w = s.size() == p.text.size() ? p.w + p.space : m_dom.fonts.width(p.face, p.size, p.spacing, s);
		if (!s.empty())
		{
			m_out.push({ op::TEXT, x + p.x, y + p.y, double(p.face), p.size, p.spacing });
			m_out.color(cs.color, alpha);
			m_out.data.push_back(m_out.text(std::move(s)));
		}
		// its lines: under it, over it and through it (on the pixel grid, as Firefox draws them)
		if (cs.decorations && cs.decoration_color.visible())
		{
			const FontSet::Decorations d = m_dom.fonts.decorations(p.face, p.size);
			const double base = std::floor(y + p.y + 0.5);
			auto band = [&](double top, double size) {
				rect(x + p.x, std::floor(base - top + 0.5), w, size, {}, cs.decoration_color, alpha);
			};
			if (cs.decorations & DECORATION_UNDERLINE)
				band(d.underline, d.underline_size);
			if (cs.decorations & DECORATION_OVERLINE)
				band(m_dom.fonts.metrics(p.face, p.size).ascent, d.underline_size);
			if (cs.decorations & DECORATION_LINE_THROUGH)
				band(d.strikeout, d.strikeout_size);
		}
	}

	void svg_items(const LayoutNode &n, double x, double y)
	{
		const Element &svg = *n.el;
		std::vector<double> vb;
		if (const std::string *v = svg.attribute(n::viewBox))
		{
			std::string t = *v;
			std::replace(t.begin(), t.end(), ',', ' ');
			for (std::string_view w : split_words(t))
				vb.push_back(parse_number(w).value_or(0));
		}
		double s = 1, ox = x + n.bl + n.pl, oy = y + n.bt + n.pt;
		const double cw = n.w - n.bl - n.br - n.pl - n.pr, ch = n.h - n.bt - n.bb - n.pt - n.pb;
		if (vb.size() == 4 && vb[2] != 0 && vb[3] != 0)
		{
			s = std::min(cw / vb[2], ch / vb[3]);
			ox += (cw - vb[2] * s) / 2 - vb[0] * s;
			oy += (ch - vb[3] * s) / 2 - vb[1] * s;
		}
		// its drawing clipped to its content box, as Firefox's own svg:not(:root) { overflow: hidden } does
		const bool clips = n.cs().clips();
		if (clips)
			m_out.push({ op::SAVE, op::CLIP, x + n.bl + n.pl, y + n.bt + n.pt, cw, ch });
		for (const Link &l : svg.links())
			if (const Element *it = l.node->as_element())
				svg_item(*it, ox, oy, s);
		if (clips)
			m_out.push({ op::RESTORE });
	}

	struct ItemStyle
	{
		std::optional<Color> fill, stroke;
		double width = 1, opacity = 1;
		std::vector<double> dash;
		int cap = 0, join = 0;
	};

	ItemStyle item_style(const Element &it)
	{
		ItemStyle st;
		if (it.style)
		{
			if (!it.style->fill_none)
				st.fill = it.style->fill;
			if (!it.style->stroke_none)
				st.stroke = it.style->stroke;
		}
		auto attr = [&](const char *name) { return it.attribute(m_dom.names.get(name)); };
		if (const std::string *v = attr("stroke-width"))
			st.width = parse_number(*v).value_or(1);
		if (const std::string *v = attr("stroke-dasharray"))
		{
			std::string t = *v;
			std::replace(t.begin(), t.end(), ',', ' ');
			for (std::string_view w : split_words(t))
				st.dash.push_back(parse_number(w).value_or(0));
		}
		if (const std::string *v = attr("stroke-linecap"))
			st.cap = *v == "round" ? 1 : *v == "square" ? 2 : 0;
		if (const std::string *v = attr("stroke-linejoin"))
			st.join = *v == "round" ? 1 : *v == "bevel" ? 2 : 0;
		if (const std::string *v = attr("opacity"))
			st.opacity = parse_number(*v).value_or(1);
		return st;
	}

	void svg_item(const Element &it, double ox, double oy, double s)
	{
		const ItemStyle st = item_style(it);
		auto num = [&](const char *k, double d = 0) { return it.number_attribute(m_dom.names.get(k), d); };
		auto T = [&](double px, double py) { return std::pair { ox + px * s, oy + py * s }; };
		const std::string_view tag = m_dom.names.str(it.tag());
		std::vector<PathCmd> cmds;
		if (tag == "path")
		{
			if (const std::string *d = it.attribute(m_dom.names.get("d")))
				cmds = parse_path(*d);
		}
		else if (tag == "line")
			cmds = { { 'M', num("x1"), num("y1") }, { 'L', num("x2"), num("y2") } };
		else if (tag == "rect" || tag == "circle")
		{
			double X, Y, w, h, r;
			if (tag == "rect")
			{
				w = num("width");
				h = num("height");
				if (w <= 0 || h <= 0)
					return;
				std::tie(X, Y) = T(num("x"), num("y"));
				r = std::min({ num("rx"), w / 2, h / 2 });
			}
			else
			{
				r = num("r");
				std::tie(X, Y) = T(num("cx") - r, num("cy") - r);
				w = h = 2 * r;
			}
			const std::array<double, 4> radius { r * s, r * s, r * s, r * s };
			if (st.fill && st.fill->visible())
				rect(X, Y, w * s, h * s, radius, *st.fill, st.opacity);
			if (st.stroke && st.stroke->visible() && st.width > 0)
			{
				m_out.push({ op::STROKE_RECT, X, Y, w * s, h * s, r * s, r * s, r * s, r * s, st.width * s });
				m_out.color(*st.stroke, st.opacity);
			}
			return;
		}
		else if (tag == "text")
		{
			const double size = num("font-size", 16) * s;
			const std::string *fam = it.attribute(m_dom.names.get("font-family"));
			const std::string *weight = it.attribute(m_dom.names.get("font-weight"));
			const bool bold = weight && (*weight == "bold" || parse_number(*weight).value_or(400) >= 600);
			const FaceId face = m_dom.fonts.face(fam ? *fam : std::string("sans-serif"), bold ? 700 : 400, FontStyle::Normal);
			const std::string t = it.text_content();
			const double w = m_dom.fonts.width(face, size, 0, t);
			auto [X, Y] = T(num("x"), num("y"));
			const std::string *anchor = it.attribute(m_dom.names.get("text-anchor"));
			if (anchor && *anchor == "middle")
				X -= w / 2;
			else if (anchor && *anchor == "end")
				X -= w;
			const Color fill = st.fill ? *st.fill : Color { 0, 0, 0, 1 };
			if (fill.visible())
			{
				// transform="rotate(a [cx cy])": the text turned about that point, the one transform supported
				const std::string *tf = it.attribute(m_dom.names.get("transform"));
				double turn[3] = { 0, 0, 0 };
				int got = 0;
				if (tf && tf->rfind("rotate(", 0) == 0)
					got = std::sscanf(tf->c_str() + 7, "%lf%*[ ,]%lf%*[ ,]%lf", &turn[0], &turn[1], &turn[2]);
				const bool turned = got >= 1 && turn[0] != 0;
				if (turned)
				{
					const double a = turn[0] * 3.14159265358979323846 / 180, co = std::cos(a), si = std::sin(a);
					auto [px, py] = T(turn[1], turn[2]);
					m_out.push({ op::SAVE, op::TRANSFORM, co, si, -si, co, px - co * px + si * py, py - si * px - co * py });
				}
				m_out.push({ op::TEXT, X, Y, double(face), size, 0 });
				m_out.color(fill, st.opacity);
				m_out.data.push_back(m_out.text(t));
				if (turned)
					m_out.push({ op::RESTORE });
			}
			return;
		}
		else
			return;
		if (cmds.empty())
			return;
		auto emit = [&](const std::vector<PathCmd> &cs) {
			m_out.push({ op::BEGIN });
			for (const PathCmd &c : cs)
				switch (char(c[0]))
				{
				case 'M':
				{
					auto [X, Y] = T(c[1], c[2]);
					m_out.push({ op::MOVE, X, Y });
					break;
				}
				case 'L':
				{
					auto [X, Y] = T(c[1], c[2]);
					m_out.push({ op::LINE, X, Y });
					break;
				}
				case 'C':
				{
					auto [a, b] = T(c[1], c[2]);
					auto [cc, d] = T(c[3], c[4]);
					auto [e, f] = T(c[5], c[6]);
					m_out.push({ op::BEZIER, a, b, cc, d, e, f });
					break;
				}
				case 'Z':
					m_out.push({ op::CLOSE });
					break;
				default:
					break;
				}
		};
		if (st.fill && st.fill->visible() && tag == "path")
		{
			emit(cmds);
			m_out.push({ op::FILL });
			m_out.color(*st.fill, st.opacity);
		}
		if (st.stroke && st.stroke->visible() && st.width > 0)
		{
			if (!st.dash.empty())
			{
				m_out.push({ op::BEGIN });
				for (const Polyline &l : dashed(flatten(cmds), st.dash))
					for (size_t i = 0; i < l.size(); i++)
					{
						auto [X, Y] = T(l[i].first, l[i].second);
						m_out.push({ double(i ? op::LINE : op::MOVE), X, Y });
					}
			}
			else
				emit(cmds);
			m_out.push({ op::STROKE, st.width * s, double(st.cap), double(st.join) });
			m_out.color(*st.stroke, st.opacity);
		}
	}

	//! A canvas: its pixels as an image, its 2D context's recording replayed in its box.
	void canvas(const LayoutNode &n, double x, double y)
	{
		Element &el = *n.el;
		if (!el.canvas)
			return;
		Canvas &cv = *el.canvas;
		const double cx = x + n.bl + n.pl, cy = y + n.bt + n.pt;
		const double cw = n.w - n.bl - n.br - n.pl - n.pr, ch = n.h - n.bt - n.bb - n.pt - n.pb;
		const double bw = el.number_attribute(n::width, 300), bh = el.number_attribute(n::height, 150);
		if (!cv.pixels.empty())
		{
			if (!cv.image_id)
				cv.image_id = m_dom.next_image_id();
			if ((cv.uploaded != cv.version || cv.uploaded_generation != m_dom.image_generation()) && m_options.image)
			{
				m_options.image({ cv.image_id, cv.pixels_w, cv.pixels_h, cv.pixels.data() });
				cv.uploaded = cv.version;
				cv.uploaded_generation = m_dom.image_generation();
			}
			m_out.push({ op::IMAGE, double(cv.image_id), cx, cy, cw, ch, n.cs().pixelated ? 0.0 : 1.0 });
		}
		if (cv.commands.empty() || bw <= 0 || bh <= 0)
			return;
		m_out.push({ op::SAVE, op::CLIP, cx, cy, cw, ch, op::TRANSFORM, cw / bw, 0, 0, ch / bh, cx, cy });
		// the recording, its texts numbered on from the frame's
		const float base = float(m_out.texts.size());
		const std::vector<float> &d = cv.commands.data;
		size_t i = 0;
		int depth = 0;
		while (i < d.size())
		{
			const int o = int(d[i]);
			const int len = op_length(o);
			if (len < 0 || i + 1 + size_t(len) > d.size())
				break;
			const size_t from = m_out.data.size();
			m_out.data.insert(m_out.data.end(), d.begin() + long(i), d.begin() + long(i + 1 + size_t(len)));
			if (o == op::TEXT)
				m_out.data[from + 10] += base;
			depth += o == op::SAVE ? 1 : o == op::RESTORE ? -1 : 0;
			i += 1 + size_t(len);
		}
		for (const std::string &t : cv.commands.texts)
			m_out.texts.push_back(t);
		// a recording left with its saves open is closed here
		for (; depth > 0; depth--)
			m_out.push({ op::RESTORE });
		m_out.push({ op::RESTORE });
	}

	//! An img's picture, filling its content box.
	void picture(const LayoutNode &n, double x, double y)
	{
		Picture *p = m_dom.picture_of(n.el);
		if (!p || p->state != Picture::State::Ready)
			return;
		if (p->uploaded != m_dom.image_generation() && m_options.image)
		{
			m_options.image({ p->id, p->width, p->height, p->rgba.data() });
			p->uploaded = m_dom.image_generation();
		}
		const double cx = x + n.bl + n.pl, cy = y + n.bt + n.pt;
		const double cw = n.w - n.bl - n.br - n.pl - n.pr, ch = n.h - n.bt - n.bb - n.pt - n.pb;
		if (cw > 0 && ch > 0)
			m_out.push({ op::IMAGE, double(p->id), cx, cy, cw, ch, n.cs().pixelated ? 0.0 : 1.0 });
	}

	void control(const LayoutNode &n, double x, double y, double alpha)
	{
		const Element &el = *n.el;
		const ComputedStyle &cs = n.cs();
		const double cx = x + n.bl + n.pl, cy = y + n.bt + n.pt;
		const double ch = n.h - n.bt - n.bb - n.pt - n.pb;
		const FontMetrics fm = m_dom.fonts.metrics(cs.face, cs.font_size);
		const double base = (ch - fm.ascent - fm.descent) / 2 + fm.ascent;
		if (el.is(n::select))
		{
			const std::vector<Element *> os = m_dom.options(&el);
			const int i = m_dom.selected_index(&el);
			const std::string t = i >= 0 && i < int(os.size()) ? os[size_t(i)]->text_content() : std::string();
			// the text up to the dropdown's button, which stands inside the border over the padding (Firefox's)
			m_out.push({ op::SAVE, op::CLIP, cx, y, std::max(0.0, n.w - n.br - SELECT_ARROW - (cx - x)), n.h });
			m_out.push({ op::TEXT, cx, cy + base, double(cs.face), cs.font_size, cs.letter_spacing });
			m_out.color(cs.color, alpha);
			m_out.data.push_back(m_out.text(t));
			m_out.push({ op::RESTORE });
			// the arrow
			const double ax = x + n.w - n.br - SELECT_ARROW / 2, ay = y + n.h / 2;
			m_out.push({ op::BEGIN, op::MOVE, ax - 3.5, ay - 1.5, op::LINE, ax, ay + 2, op::LINE, ax + 3.5, ay - 1.5,
				op::STROKE, 1.5, 1, 1 });
			m_out.color(cs.color, alpha * 0.8);
			return;
		}
		const std::string *type = el.attribute(n::type);
		if (el.is(n::input) && type && lower(*type) == "range")
		{
			const double lo = el.number_attribute(n::min, 0), hi = el.number_attribute(n::max, 100);
			const double v = parse_number(el.form.dirty_value ? el.form.value : el.attribute(n::value) ? *el.attribute(n::value)
				: format_number((lo + hi) / 2)).value_or(lo);
			const double f = hi > lo ? std::clamp((v - lo) / (hi - lo), 0.0, 1.0) : 0;
			const double ty = y + n.h / 2;
			const std::array<double, 4> r2 { 2, 2, 2, 2 }, r6 { 6, 6, 6, 6 };
			rect(x + 2, ty - 2, n.w - 4, 4, r2, cs.color, alpha * 0.3);
			rect(x + 2, ty - 2, (n.w - 4) * f, 4, r2, cs.color, alpha);
			rect(x + 2 + (n.w - 14) * f, ty - 6, 12, 12, r6, cs.color, alpha);
			return;
		}
		// a text field, or an input that shows its value as a button does; not a checkbox's or a radio's value
		const bool editable = is_text_field(el);
		if (editable || is_input_type(el, "button") || is_input_type(el, "submit") || is_input_type(el, "reset"))
		{
			const std::string v = field_value(el);
			const std::string *ph = el.attribute(n::placeholder);
			const bool placeholder = v.empty() && ph && !ph->empty();
			const std::string &shown = placeholder ? *ph : v;
			const FieldText ft(m_dom, el, n, shown);
			const double ox = x + ft.cx, oy = y + ft.cy;
			// the text scrolled to keep the caret in view (input/input.cpp), within what it can scroll
			const double sx = placeholder ? 0 : std::clamp(el.form.scroll_x, 0.0, std::max(0.0, ft.width() + 1 - ft.cw));
			const double sy = placeholder ? 0 : std::clamp(el.form.scroll_y, 0.0, std::max(0.0, ft.height() - ft.ch));
			auto shows = [&](const FieldLine &l) {
				return !ft.multiline || (l.top + ft.line_h > sy && l.top < sy + ft.ch + n.pb);
			};
			m_out.push({ op::SAVE, op::CLIP, ox, y + n.bt, ft.cw, n.h - n.bt - n.bb });
			if (editable && el.focused && !placeholder && el.form.sel_start != el.form.sel_end)
			{
				const size_t lo = std::min({ el.form.sel_start, el.form.sel_end, v.size() });
				const size_t hi = std::min(std::max(el.form.sel_start, el.form.sel_end), v.size());
				const double space = m_dom.fonts.width(cs.face, cs.font_size, cs.letter_spacing, " ");
				for (size_t k = 0; k < ft.lines.size(); k++)
				{
					const FieldLine &l = ft.lines[k];
					if (!shows(l) || hi < l.from || lo > l.to)
						continue;
					// a selected new line shows as a space's width
					const bool newline = l.to < v.size() && v[l.to] == '\n' && hi > l.to;
					const size_t a = std::max(lo, l.from), z = std::min(hi, l.to);
					if (a >= z && !newline)
						continue;
					const double xa = l.x + ft.measure(l.from, a), xz = l.x + ft.measure(l.from, z) + (newline ? space : 0);
					if (ft.multiline)
						rect(ox + xa - sx, oy + l.top - sy, xz - xa, ft.line_h, {}, SELECTION, alpha);
					else
						rect(ox + xa - sx, oy, xz - xa, ft.ch, {}, SELECTION, alpha);
				}
			}
			for (const FieldLine &l : ft.lines)
			{
				if (!shows(l) || l.to <= l.from)
					continue;
				m_out.push({ op::TEXT, ox + l.x - sx, oy + l.base - sy, double(cs.face), cs.font_size, cs.letter_spacing });
				m_out.color(cs.color, alpha * (placeholder ? 0.54 : 1));
				m_out.data.push_back(m_out.text(shown.substr(l.from, l.to - l.from)));
			}
			if (editable && el.focused && m_options.caret && m_dom.input().caret_on())
			{
				const size_t at = placeholder ? 0 : std::min(el.form.sel_end, v.size());
				const FieldLine &l = ft.lines[placeholder ? 0 : ft.line_of(at)];
				static const std::string none;
				const double xc = placeholder ? FieldText(m_dom, el, n, none).x_of(0) : ft.x_of(at);
				if (ft.multiline)
					rect(ox + xc - sx, oy + l.base - ft.ascent - sy, 1, ft.ascent + ft.descent, {}, cs.color, alpha);
				else
					rect(ox + xc - sx, oy + 1, 1, std::max(0.0, ft.ch - 2), {}, cs.color, alpha);
			}
			m_out.push({ op::RESTORE });
			// a textarea's text taller than its box: a thumb, as a region's scrollbar has
			if (ft.multiline && ft.height() > ft.ch + 0.5)
			{
				const double track = n.h - n.bt - n.bb, th = std::max(24.0, track * ft.ch / ft.height());
				const double ty = y + n.bt + (track - th) * (sy / std::max(1.0, ft.height() - ft.ch));
				rect(x + n.w - n.br - 8, ty + 2, 4, th - 4, { 2, 2, 2, 2 }, SCROLL_THUMB, alpha);
			}
		}
	}

	void box(const LayoutNode &n, double x, double y, double a)
	{
		if (a < 1)
			m_out.push({ op::SAVE, op::ALPHA, a });
		if (!n.el || !n.el->is(n::html))
			background(n, x, y);
		if (n.el)
		{
			const Element &el = *n.el;
			if (el.ns() == Namespace::Svg && el.tag() == n::svg)
				svg_items(n, x, y);
			else if (el.is(n::canvas))
				canvas(n, x, y);
			else if (el.is(n::img))
				picture(n, x, y);
			else if (el.is(n::select) || el.is(n::input) || el.is(n::textarea))
				control(n, x, y, 1);
		}
		if (a < 1)
			m_out.push({ op::RESTORE });
		const ComputedStyle &cs = n.cs();
		if (cs.outline_width > 0 && cs.outline_color.visible())
		{
			const double ow = cs.outline_width, off = cs.outline_offset;
			std::array<double, 4> r = radii(cs, n.w, n.h);
			for (double &q : r)
				q = q ? q + off + ow / 2 : 0;
			m_out.push({ op::STROKE_RECT, x - off - ow / 2, y - off - ow / 2, n.w + 2 * off + ow, n.h + 2 * off + ow,
				r[0], r[1], r[2], r[3], ow });
			m_out.color(cs.outline_color, a);
		}
	}

	void scrollbars(const LayoutNode &n, double x, double y, double a)
	{
		if (!n.scrolls || !n.el)
			return;
		const ComputedStyle &cs = n.cs();
		Color thumb = SCROLL_THUMB;
		if (cs.scrollbar_color != "auto")
		{
			const auto w = split_words(cs.scrollbar_color);
			if (!w.empty())
				if (std::optional<Color> c = parse_color(w[0]))
					thumb = *c;
		}
		const std::array<double, 4> r2 { 2, 2, 2, 2 };
		if (n.sh > n.ch + 0.5 && n.bar_y > 0)
		{
			const double track = n.ch, th = std::max(24.0, track * n.ch / n.sh);
			const double ty = y + n.bt + (track - th) * (n.el->scroll_y / std::max(1.0, n.sh - n.ch));
			const double bw = n.bar_y;
			rect(x + n.w - n.br - bw / 2 - 2, ty + 2, std::min(4.0, bw - 2), th - 4, r2, thumb, a);
		}
		if (n.sw > n.cw + 0.5 && n.bar_x > 0)
		{
			const double track = n.cw, tw = std::max(24.0, track * n.cw / n.sw);
			const double tx = x + n.bl + (track - tw) * (n.el->scroll_x / std::max(1.0, n.sw - n.cw));
			const double bh = n.bar_x;
			rect(tx + 2, y + n.h - n.bb - bh / 2 - 2, tw - 4, std::min(4.0, bh - 2), r2, thumb, a);
		}
	}

	void walk(const LayoutNode &n, double ox, double oy, double alpha)
	{
		if (n.kind == LayoutNode::Kind::Text || n.kind == LayoutNode::Kind::Anonymous || !n.laid)
			return;
		const ComputedStyle &cs = n.cs();
		if (cs.display == Display::None)
			return;
		double x = n.fixed ? n.fx : ox + n.x, y = n.fixed ? n.fy : oy + n.y;
		if (cs.position == Position::Relative)
		{
			x += cs.inset[LEFT].resolve(std::nullopt).value_or(-cs.inset[RIGHT].resolve(std::nullopt).value_or(0));
			y += cs.inset[TOP].resolve(std::nullopt).value_or(-cs.inset[BOTTOM].resolve(std::nullopt).value_or(0));
		}
		else if (cs.position == Position::Sticky)
			y += m_dom.layout_tree()->stuck(n);
		const double a = alpha * cs.opacity;
		if (a <= 0.001)
			return;
		if (cs.visibility == Visibility::Visible)
			box(n, x, y, a);
		if (n.el && n.el->ns() == Namespace::Svg)
			return;
		const bool clips = cs.clips();
		// what it holds is clipped to its padding box less its scrollbars' room (the scrollport)
		if (clips)
			m_out.push({ op::SAVE, op::CLIP, x + n.bl, y + n.bt, std::max(0.0, n.w - n.bl - n.br - n.bar_y),
				std::max(0.0, n.h - n.bt - n.bb - n.bar_x) });
		const double sx = x - (n.el ? n.el->scroll_x : 0), sy = y - (n.el ? n.el->scroll_y : 0);
		std::optional<double> clip_right;
		if (clips && cs.text_overflow == TextOverflow::Ellipsis)
			clip_right = x + n.w - n.br - n.pr;
		for (const TextPiece &p : n.pieces)
			text(p, sx, sy, a, clip_right);
		std::vector<const LayoutNode *> later;
		for (const LayoutNode *c : n.kids)
		{
			if (c->kind == LayoutNode::Kind::Text)
				continue;
			if (c->cs().position != Position::Static)
				later.push_back(c);
			else
				walk(*c, sx, sy, a);
		}
		for (const LayoutNode *c : later)
			walk(*c, sx, sy, a);
		if (clips)
		{
			m_out.push({ op::RESTORE });
			scrollbars(n, x, y, a);
		}
	}
};

//! What is under a point, walked in the paint's order.
class Hitter
{
public:
	Hitter(Dom &dom, double x, double y) : m_dom(dom), m_x(x), m_y(y) {}

	Element *run()
	{
		const auto &top = m_dom.document()->top_layer();
		for (auto it = top.rbegin(); it != top.rend(); ++it)
			if (LayoutNode *d = m_dom.layout_of(it->el))
			{
				// a modal dialog keeps the pointer to itself
				walk(*d, 0, 0);
				return m_found ? m_found : it->el;
			}
		if (Layout *l = m_dom.layout_tree(); l && l->root())
			walk(*l->root(), 0, 0);
		// the viewport's canvas is the root's
		if (!m_found && inside(0, 0, m_dom.width(), m_dom.height()))
			return m_dom.document()->document_element();
		return m_found;
	}

private:
	Dom &m_dom;
	double m_x, m_y;
	Element *m_found = nullptr;
	struct Clip
	{
		double x, y, w, h;
	};
	std::vector<Clip> m_clips;

	bool inside(double x, double y, double w, double h) const
	{
		return m_x >= x && m_x < x + w && m_y >= y && m_y < y + h;
	}

	Element *svg_hit(const LayoutNode &n, double x, double y)
	{
		const Element &svg = *n.el;
		std::vector<double> vb;
		if (const std::string *v = svg.attribute(n::viewBox))
		{
			std::string t = *v;
			std::replace(t.begin(), t.end(), ',', ' ');
			for (std::string_view w : split_words(t))
				vb.push_back(parse_number(w).value_or(0));
		}
		double s = 1, ox = x + n.bl + n.pl, oy = y + n.bt + n.pt;
		const double cw = n.w - n.bl - n.br - n.pl - n.pr, ch = n.h - n.bt - n.bb - n.pt - n.pb;
		if (vb.size() == 4 && vb[2] != 0 && vb[3] != 0)
		{
			s = std::min(cw / vb[2], ch / vb[3]);
			ox += (cw - vb[2] * s) / 2 - vb[0] * s;
			oy += (ch - vb[3] * s) / 2 - vb[1] * s;
		}
		const double px = (m_x - ox) / s, py = (m_y - oy) / s;
		Element *found = nullptr;
		for (const Link &l : svg.links())
		{
			Element *it = l.node->as_element();
			if (!it)
				continue;
			auto num = [&](const char *k) { return it->number_attribute(m_dom.names.get(k), 0); };
			const std::string_view tag = m_dom.names.str(it->tag());
			const bool fill = it->style && !it->style->fill_none;
			const bool stroke = it->style && !it->style->stroke_none;
			const double sw = it->number_attribute(m_dom.names.get("stroke-width"), 1);
			if (tag == "rect")
			{
				const double rx = num("x"), ry = num("y");
				if (px >= rx && px < rx + num("width") && py >= ry && py < ry + num("height"))
					found = it;
			}
			else if (tag == "circle")
			{
				if (std::hypot(px - num("cx"), py - num("cy")) <= num("r") + (stroke ? sw / 2 : 0))
					found = it;
			}
			else if (tag == "line" || tag == "path")
			{
				std::vector<PathCmd> cmds;
				if (tag == "line")
					cmds = { { 'M', num("x1"), num("y1") }, { 'L', num("x2"), num("y2") } };
				else if (const std::string *d = it->attribute(m_dom.names.get("d")))
					cmds = parse_path(*d);
				const std::vector<Polyline> polys = flatten(cmds);
				const double hw = std::max(2.0, sw / 2 + 1);
				bool hit = false;
				for (const Polyline &p : polys)
					hit = hit || (fill && tag == "path" && in_polygon(p, px, py)) || (stroke && near_line(p, px, py, hw));
				if (hit)
					found = it;
			}
		}
		return found;
	}

	void walk(const LayoutNode &n, double ox, double oy)
	{
		if (n.kind == LayoutNode::Kind::Text || n.kind == LayoutNode::Kind::Anonymous || !n.laid)
			return;
		const ComputedStyle &cs = n.cs();
		double x = n.fixed ? n.fx : ox + n.x, y = n.fixed ? n.fy : oy + n.y;
		if (cs.position == Position::Relative)
		{
			x += cs.inset[LEFT].resolve(std::nullopt).value_or(-cs.inset[RIGHT].resolve(std::nullopt).value_or(0));
			y += cs.inset[TOP].resolve(std::nullopt).value_or(-cs.inset[BOTTOM].resolve(std::nullopt).value_or(0));
		}
		else if (cs.position == Position::Sticky)
			y += m_dom.layout_tree()->stuck(n);
		if (cs.opacity <= 0.001)
			return;
		const bool clipped = std::any_of(m_clips.begin(), m_clips.end(), [&](const Clip &c) { return !inside(c.x, c.y, c.w, c.h); });
		if (!clipped && cs.pointer_events != PointerEvents::None && cs.visibility == Visibility::Visible
			&& inside(x, y, n.w, n.h))
		{
			// a pseudo-element's hit is its element's
			Element *el = n.el;
			for (const LayoutNode *p = &n; !el && p; p = p->parent)
				el = p->el;
			m_found = el;
			if (n.el && n.el->ns() == Namespace::Svg && n.el->tag() == n::svg)
				if (Element *item = svg_hit(n, x, y))
					m_found = item;
		}
		if (n.el && n.el->ns() == Namespace::Svg)
			return;
		const bool clips = cs.clips();
		if (clips)
			m_clips.push_back({ x + n.bl, y + n.bt, n.w - n.bl - n.br - n.bar_y, n.h - n.bt - n.bb - n.bar_x });
		const double sx = x - (n.el ? n.el->scroll_x : 0), sy = y - (n.el ? n.el->scroll_y : 0);
		std::vector<const LayoutNode *> later;
		for (const LayoutNode *c : n.kids)
		{
			if (c->kind == LayoutNode::Kind::Text)
				continue;
			if (c->cs().position != Position::Static)
				later.push_back(c);
			else
				walk(*c, sx, sy);
		}
		for (const LayoutNode *c : later)
			walk(*c, sx, sy);
		if (clips)
			m_clips.pop_back();
	}
};

} // namespace

void paint(Dom &dom, Commands &out, const PaintOptions &options)
{
	dom.update();
	out.clear();
	Painter(dom, out, options).run();
}

Element *hit(Dom &dom, double x, double y)
{
	dom.update();
	return Hitter(dom, x, y).run();
}

} // namespace qdom
