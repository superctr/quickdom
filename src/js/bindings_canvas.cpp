// Copyright 2026 Ian Karlsson
// A canvas's 2D context (rectangles, paths, lines with dashes, text, clips, the
// transform, globalAlpha), recorded as drawing commands in the canvas's pixels; and putImageData's pixels.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "js/bindings.h"
#include "paint/canvas.h"

namespace qdom::js
{

namespace
{

constexpr double PI = 3.14159265358979323846;

#define FN [](Dom & dom, JSContext * ctx, JSValueConst self, int argc, JSValueConst *argv) -> JSValue
#define THIS_CANVAS \
	Element *el = canvas_of(dom, self); \
	if (!el || !el->canvas) \
		return illegal(ctx); \
	Canvas &cv = *el->canvas; \
	Canvas2DState &st = cv.state

Element *canvas_of(Dom &dom, JSValueConst self)
{
	const Holder *h = static_cast<const Holder *>(JS_GetOpaque(self, dom.classes().context2d));
	return h ? h->node->as_element() : nullptr;
}

double num(JSContext *ctx, JSValueConst v)
{
	return to_number(ctx, v, std::nan(""));
}

bool finite(std::initializer_list<double> xs)
{
	for (double x : xs)
		if (!std::isfinite(x))
			return false;
	return true;
}

void apply(const Canvas2DState &st, double x, double y, double &ox, double &oy)
{
	ox = st.m[0] * x + st.m[2] * y + st.m[4];
	oy = st.m[1] * x + st.m[3] * y + st.m[5];
}

//! The transform's scale, for line widths and text sizes.
double scale_of(const Canvas2DState &st)
{
	return std::sqrt(std::fabs(st.m[0] * st.m[3] - st.m[1] * st.m[2]));
}

bool axis_aligned(const Canvas2DState &st)
{
	return st.m[1] == 0 && st.m[2] == 0;
}

void multiply(Canvas2DState &st, double a, double b, double c, double d, double e, double f)
{
	const double *m = st.m;
	const double r[6] = { m[0] * a + m[2] * b, m[1] * a + m[3] * b, m[0] * c + m[2] * d, m[1] * c + m[3] * d,
		m[0] * e + m[2] * f + m[4], m[1] * e + m[3] * f + m[5] };
	std::copy(r, r + 6, st.m);
}

void move_to(Canvas &cv, double x, double y)
{
	double X, Y;
	apply(cv.state, x, y, X, Y);
	cv.path.insert(cv.path.end(), { float(op::MOVE), float(X), float(Y) });
	cv.path_x = x;
	cv.path_y = y;
	cv.has_point = true;
}

void line_to(Canvas &cv, double x, double y)
{
	if (!cv.has_point)
	{
		move_to(cv, x, y);
		return;
	}
	double X, Y;
	apply(cv.state, x, y, X, Y);
	cv.path.insert(cv.path.end(), { float(op::LINE), float(X), float(Y) });
	cv.path_x = x;
	cv.path_y = y;
}

void bezier_to(Canvas &cv, double x1, double y1, double x2, double y2, double x, double y)
{
	if (!cv.has_point)
		move_to(cv, x1, y1);
	double a, b, c, d, e, f;
	apply(cv.state, x1, y1, a, b);
	apply(cv.state, x2, y2, c, d);
	apply(cv.state, x, y, e, f);
	cv.path.insert(cv.path.end(), { float(op::BEZIER), float(a), float(b), float(c), float(d), float(e), float(f) });
	cv.path_x = x;
	cv.path_y = y;
}

//! The path as polylines, curves cut in eight, for dashes.
std::vector<std::vector<std::pair<double, double>>> flatten(const std::vector<float> &p)
{
	std::vector<std::vector<std::pair<double, double>>> out;
	double x = 0, y = 0, sx = 0, sy = 0;
	for (size_t i = 0; i < p.size();)
	{
		const int o = int(p[i]);
		if (o == op::MOVE)
		{
			x = sx = p[i + 1];
			y = sy = p[i + 2];
			out.push_back({ { x, y } });
			i += 3;
		}
		else if (o == op::LINE)
		{
			if (out.empty())
				out.push_back({ { x, y } });
			x = p[i + 1];
			y = p[i + 2];
			out.back().emplace_back(x, y);
			i += 3;
		}
		else if (o == op::BEZIER)
		{
			if (out.empty())
				out.push_back({ { x, y } });
			for (int t = 1; t <= 8; t++)
			{
				const double u = t / 8.0, v = 1 - u;
				out.back().emplace_back(v * v * v * x + 3 * v * v * u * p[i + 1] + 3 * v * u * u * p[i + 3] + u * u * u * p[i + 5],
					v * v * v * y + 3 * v * v * u * p[i + 2] + 3 * v * u * u * p[i + 4] + u * u * u * p[i + 6]);
			}
			x = p[i + 5];
			y = p[i + 6];
			i += 7;
		}
		else if (o == op::CLOSE)
		{
			if (!out.empty())
				out.back().emplace_back(sx, sy);
			x = sx;
			y = sy;
			i += 1;
		}
		else
			i += 1;
	}
	return out;
}

//! The path's polylines cut into dashes, the pattern in the canvas's pixels.
std::vector<float> dashed(const std::vector<float> &path, const std::vector<double> &dash, double offset)
{
	std::vector<float> out;
	double total = 0;
	for (double d : dash)
		total += d;
	if (total <= 0)
		return path;
	for (const auto &line : flatten(path))
	{
		size_t k = 0;
		double left = dash[0];
		bool on = true;
		// the offset into the pattern
		double skip = std::fmod(offset, total);
		while (skip > 0)
		{
			if (skip >= left)
			{
				skip -= left;
				k = (k + 1) % dash.size();
				left = dash[k];
				on = !on;
			}
			else
			{
				left -= skip;
				skip = 0;
			}
		}
		bool open = false;
		for (size_t i = 1; i < line.size(); i++)
		{
			double ax = line[i - 1].first, ay = line[i - 1].second;
			const double bx = line[i].first, by = line[i].second;
			double seg = std::hypot(bx - ax, by - ay);
			while (seg > 1e-9)
			{
				const double t = std::min(seg, left);
				const double f = t / seg;
				const double nx = ax + (bx - ax) * f, ny = ay + (by - ay) * f;
				if (on)
				{
					if (!open)
					{
						out.insert(out.end(), { float(op::MOVE), float(ax), float(ay) });
						open = true;
					}
					out.insert(out.end(), { float(op::LINE), float(nx), float(ny) });
				}
				seg -= t;
				left -= t;
				ax = nx;
				ay = ny;
				if (left <= 1e-9)
				{
					on = !on;
					open = false;
					k = (k + 1) % dash.size();
					left = dash[k];
				}
			}
		}
	}
	return out;
}

void emit_path(Commands &cmd, const std::vector<float> &path)
{
	cmd.push({ op::BEGIN });
	cmd.data.insert(cmd.data.end(), path.begin(), path.end());
}

void fill_path(Canvas &cv, const std::vector<float> &path)
{
	const Canvas2DState &st = cv.state;
	if (!st.fill.visible() || st.alpha <= 0 || path.empty())
		return;
	emit_path(cv.commands, path);
	cv.commands.push({ op::FILL });
	cv.commands.color(st.fill, st.alpha);
}

void stroke_path(Canvas &cv, const std::vector<float> &path)
{
	const Canvas2DState &st = cv.state;
	if (!st.stroke.visible() || st.alpha <= 0 || path.empty() || st.line_width <= 0)
		return;
	const double s = scale_of(st);
	std::vector<double> dash;
	for (double d : st.dash)
		dash.push_back(d * s);
	emit_path(cv.commands, dash.empty() ? path : dashed(path, dash, st.dash_offset * s));
	cv.commands.push({ op::STROKE, st.line_width * s, double(st.cap), double(st.join) });
	cv.commands.color(st.stroke, st.alpha);
}

std::vector<float> rect_path(const Canvas2DState &st, double x, double y, double w, double h)
{
	std::vector<float> p;
	const double xs[4] = { x, x + w, x + w, x }, ys[4] = { y, y, y + h, y + h };
	for (int i = 0; i < 4; i++)
	{
		double X, Y;
		apply(st, xs[i], ys[i], X, Y);
		p.insert(p.end(), { float(i ? op::LINE : op::MOVE), float(X), float(Y) });
	}
	p.push_back(float(op::CLOSE));
	return p;
}

//! The font of a CSS font value, resolved to a face of the instance's fonts; false when it is not one.
bool set_font(Dom &dom, Canvas2DState &st, const std::string &value)
{
	std::vector<std::pair<Prop, std::string>> longhands;
	expand("font", value, longhands);
	std::optional<double> size;
	std::string family = "sans-serif";
	int weight = 400;
	FontStyle style = FontStyle::Normal;
	for (const auto &[p, v] : longhands)
	{
		if (p == Prop::FontSize)
		{
			std::optional<Length> l = parse_length(v, { 10, 16, dom.width(), dom.height() });
			if (l && l->is_value())
				size = l->px + l->pct * 10;
		}
		else if (p == Prop::FontFamily)
			family = v;
		else if (p == Prop::FontWeight)
			weight = v == "bold" || v == "bolder" ? 700 : v == "normal" ? 400 : parse_number(v).value_or(400);
		else if (p == Prop::FontStyle)
			style = v == "italic" ? FontStyle::Italic : v == "oblique" ? FontStyle::Oblique : FontStyle::Normal;
	}
	if (!size || family.empty())
		return false;
	st.font = value;
	st.font_size = *size;
	st.family = family;
	st.font_weight = weight;
	st.font_style = style;
	st.face = dom.fonts.face(family, weight, style);
	return true;
}

FaceId face_of(Dom &dom, Canvas &cv)
{
	if (!cv.face_valid)
	{
		cv.state.face = dom.fonts.face(cv.state.family, cv.state.font_weight, cv.state.font_style);
		cv.face_valid = true;
	}
	return cv.state.face;
}

//! A text as the canvas draws and measures it: its ASCII white space (a tab, a line's end) each a space.
std::string canvas_text(JSContext *ctx, JSValueConst v)
{
	std::string s = to_string(ctx, v);
	for (char &c : s)
		if (c == '\t' || c == '\n' || c == '\f' || c == '\r')
			c = ' ';
	return s;
}

void text(Dom &dom, Canvas &cv, const std::string &s, double x, double y, const Color &color)
{
	Canvas2DState &st = cv.state;
	if (!color.visible() || st.alpha <= 0 || s.empty())
		return;
	const FaceId face = face_of(dom, cv);
	const double w = dom.fonts.width(face, st.font_size, 0, s);
	const std::string &a = st.text_align;
	if (a == "center")
		x -= w / 2;
	else if (a == "right" || a == "end")
		x -= w;
	double ascent, descent, gap, average;
	dom.fonts.em_metrics(face, ascent, descent, gap, average);
	const std::string &b = st.text_baseline;
	if (b == "top" || b == "hanging")
		y += ascent * st.font_size;
	else if (b == "middle")
		y += (ascent - descent) / 2 * st.font_size;
	else if (b == "bottom" || b == "ideographic")
		y -= descent * st.font_size;
	double X, Y;
	apply(st, x, y, X, Y);
	cv.commands.push({ op::TEXT, X, Y, double(face), st.font_size * scale_of(st), 0 });
	cv.commands.color(color, st.alpha);
	cv.commands.data.push_back(cv.commands.text(s));
}

std::string color_string(const Color &c)
{
	if (c.a >= 1)
	{
		char buf[8];
		snprintf(buf, sizeof(buf), "#%02x%02x%02x", int(std::lround(c.r)), int(std::lround(c.g)), int(std::lround(c.b)));
		return buf;
	}
	return serialize(c);
}

JSValue image_data(JSContext *ctx, int w, int h, const uint8_t *from)
{
	JSValue o = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, w));
	JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, h));
	const size_t n = size_t(std::max(0, w)) * size_t(std::max(0, h)) * 4;
	JSValue buffer = JS_NewArrayBufferCopy(ctx, from, n);
	if (!from)
	{
		JS_FreeValue(ctx, buffer);
		std::vector<uint8_t> zero(n ? n : 1);
		buffer = JS_NewArrayBufferCopy(ctx, zero.data(), n);
	}
	JSValue clamped = JS_Eval(ctx, "Uint8ClampedArray", 17, "<quickdom>", JS_EVAL_TYPE_GLOBAL);
	JSValue data = JS_CallConstructor(ctx, clamped, 1, &buffer);
	JS_SetPropertyStr(ctx, o, "data", data);
	JS_FreeValue(ctx, clamped);
	JS_FreeValue(ctx, buffer);
	return o;
}

} // namespace

JSValue get_context(Dom &dom, JSContext *ctx, Element *el, const std::string &type)
{
	if (!el->is(n::canvas) || type != "2d")
		return JS_NULL;
	if (!el->canvas)
		el->canvas = std::make_unique<Canvas>();
	if (JS_IsUndefined(el->context))
	{
		JSValue obj = JS_NewObjectClass(ctx, int(dom.classes().context2d));
		JS_SetOpaque(obj, new Holder { el, JS_DupValue(ctx, el->wrapper()), 0 });
		el->context = obj;
	}
	return JS_DupValue(ctx, el->context);
}

void install_canvas(Dom &dom)
{
	JSContext *ctx = dom.ctx();
	JSValue proto = JS_NewObject(ctx);
	define(dom, proto, {
		{ "canvas", FN { THIS_CANVAS; (void)st; return JS_DupValue(ctx, el->wrapper()); }, nullptr },
		{ "fillStyle",
			FN { THIS_CANVAS; return str(ctx, color_string(st.fill)); },
			FN {
				THIS_CANVAS;
				if (JS_IsString(argv[0]))
					if (std::optional<Color> c = parse_color(to_string(ctx, argv[0])))
						st.fill = *c;
				return JS_UNDEFINED;
			} },
		{ "strokeStyle",
			FN { THIS_CANVAS; return str(ctx, color_string(st.stroke)); },
			FN {
				THIS_CANVAS;
				if (JS_IsString(argv[0]))
					if (std::optional<Color> c = parse_color(to_string(ctx, argv[0])))
						st.stroke = *c;
				return JS_UNDEFINED;
			} },
		{ "lineWidth",
			FN { THIS_CANVAS; return JS_NewFloat64(ctx, st.line_width); },
			FN {
				THIS_CANVAS;
				const double w = num(ctx, argv[0]);
				if (std::isfinite(w) && w > 0)
					st.line_width = w;
				return JS_UNDEFINED;
			} },
		{ "lineCap",
			FN { THIS_CANVAS; return JS_NewString(ctx, st.cap == 1 ? "round" : st.cap == 2 ? "square" : "butt"); },
			FN {
				THIS_CANVAS;
				const std::string v = to_string(ctx, argv[0]);
				st.cap = v == "round" ? 1 : v == "square" ? 2 : v == "butt" ? 0 : st.cap;
				return JS_UNDEFINED;
			} },
		{ "lineJoin",
			FN { THIS_CANVAS; return JS_NewString(ctx, st.join == 1 ? "round" : st.join == 2 ? "bevel" : "miter"); },
			FN {
				THIS_CANVAS;
				const std::string v = to_string(ctx, argv[0]);
				st.join = v == "round" ? 1 : v == "bevel" ? 2 : v == "miter" ? 0 : st.join;
				return JS_UNDEFINED;
			} },
		{ "miterLimit",
			FN { THIS_CANVAS; return JS_NewFloat64(ctx, st.miter_limit); },
			FN { THIS_CANVAS; st.miter_limit = to_number(ctx, argv[0], st.miter_limit); return JS_UNDEFINED; } },
		{ "lineDashOffset",
			FN { THIS_CANVAS; return JS_NewFloat64(ctx, st.dash_offset); },
			FN { THIS_CANVAS; st.dash_offset = to_number(ctx, argv[0], st.dash_offset); return JS_UNDEFINED; } },
		{ "globalAlpha",
			FN { THIS_CANVAS; return JS_NewFloat64(ctx, st.alpha); },
			FN {
				THIS_CANVAS;
				const double a = num(ctx, argv[0]);
				if (std::isfinite(a) && a >= 0 && a <= 1)
					st.alpha = a;
				return JS_UNDEFINED;
			} },
		{ "globalCompositeOperation",
			FN { return JS_NewString(ctx, "source-over"); },
			FN { return JS_UNDEFINED; } },
		{ "font",
			FN { THIS_CANVAS; return str(ctx, st.font); },
			FN { THIS_CANVAS; set_font(dom, st, to_string(ctx, argv[0])); cv.face_valid = true; return JS_UNDEFINED; } },
		{ "textAlign",
			FN { THIS_CANVAS; return str(ctx, st.text_align); },
			FN { THIS_CANVAS; st.text_align = to_string(ctx, argv[0]); return JS_UNDEFINED; } },
		{ "textBaseline",
			FN { THIS_CANVAS; return str(ctx, st.text_baseline); },
			FN { THIS_CANVAS; st.text_baseline = to_string(ctx, argv[0]); return JS_UNDEFINED; } },
		{ "direction", FN { return JS_NewString(ctx, "ltr"); }, FN { return JS_UNDEFINED; } },
		{ "imageSmoothingEnabled", FN { return JS_TRUE; }, FN { return JS_UNDEFINED; } },
		{ "shadowBlur", FN { return JS_NewInt32(ctx, 0); }, FN { return JS_UNDEFINED; } },
		{ "shadowColor", FN { return JS_NewString(ctx, "rgba(0, 0, 0, 0)"); }, FN { return JS_UNDEFINED; } },
		{ "shadowOffsetX", FN { return JS_NewInt32(ctx, 0); }, FN { return JS_UNDEFINED; } },
		{ "shadowOffsetY", FN { return JS_NewInt32(ctx, 0); }, FN { return JS_UNDEFINED; } },
	});
	define(dom, proto, {
		{ "save", 0, FN {
			THIS_CANVAS;
			cv.stack.push_back(st);
			st.clips = 0;
			return JS_UNDEFINED;
		} },
		{ "restore", 0, FN {
			THIS_CANVAS;
			if (cv.stack.empty())
				return JS_UNDEFINED;
			for (int i = 0; i < st.clips; i++)
				cv.commands.push({ op::RESTORE });
			st = cv.stack.back();
			cv.stack.pop_back();
			return JS_UNDEFINED;
		} },
		{ "setTransform", 6, FN {
			THIS_CANVAS;
			double m[6];
			if (JS_IsObject(argv[0]))
			{
				const char *keys[6] = { "a", "b", "c", "d", "e", "f" };
				for (int i = 0; i < 6; i++)
				{
					JSValue v = JS_GetPropertyStr(ctx, argv[0], keys[i]);
					m[i] = to_number(ctx, v, i == 0 || i == 3 ? 1 : 0);
					JS_FreeValue(ctx, v);
				}
			}
			else if (argc == 0)
			{
				const double id[6] = { 1, 0, 0, 1, 0, 0 };
				std::copy(id, id + 6, m);
			}
			else
				for (int i = 0; i < 6; i++)
					m[i] = num(ctx, argv[i]);
			if (finite({ m[0], m[1], m[2], m[3], m[4], m[5] }))
				std::copy(m, m + 6, st.m);
			return JS_UNDEFINED;
		} },
		{ "resetTransform", 0, FN {
			THIS_CANVAS;
			const double id[6] = { 1, 0, 0, 1, 0, 0 };
			std::copy(id, id + 6, st.m);
			return JS_UNDEFINED;
		} },
		{ "getTransform", 0, FN {
			THIS_CANVAS;
			JSValue o = JS_NewObject(ctx);
			const char *keys[6] = { "a", "b", "c", "d", "e", "f" };
			for (int i = 0; i < 6; i++)
				JS_SetPropertyStr(ctx, o, keys[i], JS_NewFloat64(ctx, st.m[i]));
			return o;
		} },
		{ "transform", 6, FN {
			THIS_CANVAS;
			double m[6];
			for (int i = 0; i < 6; i++)
				m[i] = num(ctx, argv[i]);
			if (finite({ m[0], m[1], m[2], m[3], m[4], m[5] }))
				multiply(st, m[0], m[1], m[2], m[3], m[4], m[5]);
			return JS_UNDEFINED;
		} },
		{ "translate", 2, FN {
			THIS_CANVAS;
			const double x = num(ctx, argv[0]), y = num(ctx, argv[1]);
			if (finite({ x, y }))
				multiply(st, 1, 0, 0, 1, x, y);
			return JS_UNDEFINED;
		} },
		{ "scale", 2, FN {
			THIS_CANVAS;
			const double x = num(ctx, argv[0]), y = num(ctx, argv[1]);
			if (finite({ x, y }))
				multiply(st, x, 0, 0, y, 0, 0);
			return JS_UNDEFINED;
		} },
		{ "rotate", 1, FN {
			THIS_CANVAS;
			const double a = num(ctx, argv[0]);
			if (finite({ a }))
				multiply(st, std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0, 0);
			return JS_UNDEFINED;
		} },
		{ "fillRect", 4, FN {
			THIS_CANVAS;
			double x = num(ctx, argv[0]), y = num(ctx, argv[1]), w = num(ctx, argv[2]), h = num(ctx, argv[3]);
			if (!finite({ x, y, w, h }) || !w || !h || !st.fill.visible() || st.alpha <= 0)
				return JS_UNDEFINED;
			if (axis_aligned(st))
			{
				double x0, y0, x1, y1;
				apply(st, x, y, x0, y0);
				apply(st, x + w, y + h, x1, y1);
				cv.commands.push({ op::RECT, std::min(x0, x1), std::min(y0, y1), std::fabs(x1 - x0), std::fabs(y1 - y0), 0, 0, 0, 0 });
				cv.commands.color(st.fill, st.alpha);
			}
			else
				fill_path(cv, rect_path(st, x, y, w, h));
			dom.invalidate_paint();
			return JS_UNDEFINED;
		} },
		{ "strokeRect", 4, FN {
			THIS_CANVAS;
			const double x = num(ctx, argv[0]), y = num(ctx, argv[1]), w = num(ctx, argv[2]), h = num(ctx, argv[3]);
			if (!finite({ x, y, w, h }))
				return JS_UNDEFINED;
			stroke_path(cv, rect_path(st, x, y, w, h));
			dom.invalidate_paint();
			return JS_UNDEFINED;
		} },
		{ "clearRect", 4, FN {
			THIS_CANVAS;
			const double x = num(ctx, argv[0]), y = num(ctx, argv[1]), w = num(ctx, argv[2]), h = num(ctx, argv[3]);
			if (!finite({ x, y, w, h }))
				return JS_UNDEFINED;
			double x0, y0, x1, y1;
			apply(st, x, y, x0, y0);
			apply(st, x + w, y + h, x1, y1);
			const double cw = el->number_attribute(n::width, 300), ch = el->number_attribute(n::height, 150);
			// a clear of the whole canvas starts a new picture; less than that is drawn as a clear
			if (axis_aligned(st) && std::min(x0, x1) <= 0 && std::min(y0, y1) <= 0 && std::max(x0, x1) >= cw
				&& std::max(y0, y1) >= ch && cv.stack.empty() && st.clips == 0)
				cv.commands.clear();
			else
				cv.commands.push({ op::CLEAR, std::min(x0, x1), std::min(y0, y1), std::fabs(x1 - x0), std::fabs(y1 - y0) });
			dom.invalidate_paint();
			return JS_UNDEFINED;
		} },
		{ "beginPath", 0, FN { THIS_CANVAS; (void)st; cv.path.clear(); cv.has_point = false; return JS_UNDEFINED; } },
		{ "closePath", 0, FN {
			THIS_CANVAS;
			(void)st;
			if (cv.has_point)
				cv.path.push_back(float(op::CLOSE));
			return JS_UNDEFINED;
		} },
		{ "moveTo", 2, FN {
			THIS_CANVAS;
			(void)st;
			const double x = num(ctx, argv[0]), y = num(ctx, argv[1]);
			if (finite({ x, y }))
				move_to(cv, x, y);
			return JS_UNDEFINED;
		} },
		{ "lineTo", 2, FN {
			THIS_CANVAS;
			(void)st;
			const double x = num(ctx, argv[0]), y = num(ctx, argv[1]);
			if (finite({ x, y }))
				line_to(cv, x, y);
			return JS_UNDEFINED;
		} },
		{ "bezierCurveTo", 6, FN {
			THIS_CANVAS;
			(void)st;
			double v[6];
			for (int i = 0; i < 6; i++)
				v[i] = num(ctx, argv[i]);
			if (finite({ v[0], v[1], v[2], v[3], v[4], v[5] }))
				bezier_to(cv, v[0], v[1], v[2], v[3], v[4], v[5]);
			return JS_UNDEFINED;
		} },
		{ "quadraticCurveTo", 4, FN {
			THIS_CANVAS;
			(void)st;
			const double qx = num(ctx, argv[0]), qy = num(ctx, argv[1]), x = num(ctx, argv[2]), y = num(ctx, argv[3]);
			if (!finite({ qx, qy, x, y }))
				return JS_UNDEFINED;
			if (!cv.has_point)
				move_to(cv, qx, qy);
			const double x0 = cv.path_x, y0 = cv.path_y;
			bezier_to(cv, x0 + 2.0 / 3 * (qx - x0), y0 + 2.0 / 3 * (qy - y0), x + 2.0 / 3 * (qx - x), y + 2.0 / 3 * (qy - y), x, y);
			return JS_UNDEFINED;
		} },
		{ "arc", 6, FN {
			THIS_CANVAS;
			(void)st;
			const double cx = num(ctx, argv[0]), cy = num(ctx, argv[1]), r = num(ctx, argv[2]);
			double a0 = num(ctx, argv[3]), a1 = num(ctx, argv[4]);
			const bool ccw = JS_ToBool(ctx, argv[5]);
			if (!finite({ cx, cy, r, a0, a1 }) || r < 0)
				return JS_UNDEFINED;
			const double tau = 2 * PI;
			double sweep = a1 - a0;
			if (!ccw && sweep < 0)
				sweep = std::fmod(sweep, tau) + tau;
			else if (ccw && sweep > 0)
				sweep = std::fmod(sweep, tau) - tau;
			if (std::fabs(a1 - a0) >= tau)
				sweep = ccw ? -tau : tau;
			const int segs = std::max(1, int(std::ceil(std::fabs(sweep) / (PI / 2))));
			const double step = sweep / segs, k = 4.0 / 3 * std::tan(step / 4);
			const double sx = cx + r * std::cos(a0), sy = cy + r * std::sin(a0);
			if (cv.has_point)
				line_to(cv, sx, sy);
			else
				move_to(cv, sx, sy);
			for (int i = 0; i < segs; i++)
			{
				const double a = a0 + i * step, b = a + step;
				const double x0 = cx + r * std::cos(a), y0 = cy + r * std::sin(a);
				const double x3 = cx + r * std::cos(b), y3 = cy + r * std::sin(b);
				bezier_to(cv, x0 - k * r * std::sin(a), y0 + k * r * std::cos(a), x3 + k * r * std::sin(b),
					y3 - k * r * std::cos(b), x3, y3);
			}
			return JS_UNDEFINED;
		} },
		{ "rect", 4, FN {
			THIS_CANVAS;
			(void)st;
			const double x = num(ctx, argv[0]), y = num(ctx, argv[1]), w = num(ctx, argv[2]), h = num(ctx, argv[3]);
			if (!finite({ x, y, w, h }))
				return JS_UNDEFINED;
			move_to(cv, x, y);
			line_to(cv, x + w, y);
			line_to(cv, x + w, y + h);
			line_to(cv, x, y + h);
			cv.path.push_back(float(op::CLOSE));
			move_to(cv, x, y);
			return JS_UNDEFINED;
		} },
		{ "fill", 1, FN { THIS_CANVAS; (void)st; fill_path(cv, cv.path); dom.invalidate_paint(); return JS_UNDEFINED; } },
		{ "stroke", 1, FN { THIS_CANVAS; (void)st; stroke_path(cv, cv.path); dom.invalidate_paint(); return JS_UNDEFINED; } },
		{ "clip", 1, FN {
			THIS_CANVAS;
			// the path's bounds (clips are rectangles)
			double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
			for (const auto &line : flatten(cv.path))
				for (const auto &[x, y] : line)
				{
					x0 = std::min(x0, x);
					y0 = std::min(y0, y);
					x1 = std::max(x1, x);
					y1 = std::max(y1, y);
				}
			if (x1 < x0)
				x0 = y0 = x1 = y1 = 0;
			cv.commands.push({ op::SAVE, op::CLIP, x0, y0, x1 - x0, y1 - y0 });
			st.clips++;
			return JS_UNDEFINED;
		} },
		{ "fillText", 4, FN {
			THIS_CANVAS;
			const double x = num(ctx, argv[1]), y = num(ctx, argv[2]);
			if (finite({ x, y }))
				text(dom, cv, canvas_text(ctx, argv[0]), x, y, st.fill);
			dom.invalidate_paint();
			return JS_UNDEFINED;
		} },
		{ "strokeText", 4, FN {
			THIS_CANVAS;
			const double x = num(ctx, argv[1]), y = num(ctx, argv[2]);
			if (finite({ x, y }))
				text(dom, cv, canvas_text(ctx, argv[0]), x, y, st.stroke);
			dom.invalidate_paint();
			return JS_UNDEFINED;
		} },
		{ "measureText", 1, FN {
			THIS_CANVAS;
			const FaceId face = face_of(dom, cv);
			const std::string s = canvas_text(ctx, argv[0]);
			const double w = dom.fonts.width(face, st.font_size, 0, s);
			double a, d, g, avg;
			dom.fonts.em_metrics(face, a, d, g, avg);
			JSValue o = JS_NewObject(ctx);
			const std::pair<const char *, double> fields[] = { { "width", w }, { "actualBoundingBoxLeft", 0 },
				{ "actualBoundingBoxRight", w }, { "actualBoundingBoxAscent", a * st.font_size },
				{ "actualBoundingBoxDescent", d * st.font_size }, { "fontBoundingBoxAscent", a * st.font_size },
				{ "fontBoundingBoxDescent", d * st.font_size } };
			for (const auto &[k, v] : fields)
				JS_SetPropertyStr(ctx, o, k, JS_NewFloat64(ctx, v));
			return o;
		} },
		{ "setLineDash", 1, FN {
			THIS_CANVAS;
			std::vector<double> dash;
			JSValue len = JS_GetPropertyStr(ctx, argv[0], "length");
			const int n = int(to_number(ctx, len, 0));
			JS_FreeValue(ctx, len);
			for (int i = 0; i < n; i++)
			{
				JSValue v = JS_GetPropertyUint32(ctx, argv[0], uint32_t(i));
				const double d = num(ctx, v);
				JS_FreeValue(ctx, v);
				if (!std::isfinite(d) || d < 0)
					return JS_UNDEFINED;
				dash.push_back(d);
			}
			if (dash.size() % 2)
				dash.insert(dash.end(), dash.begin(), dash.end());
			st.dash = dash;
			return JS_UNDEFINED;
		} },
		{ "getLineDash", 0, FN {
			THIS_CANVAS;
			JSValue a = JS_NewArray(ctx);
			for (size_t i = 0; i < st.dash.size(); i++)
				JS_SetPropertyUint32(ctx, a, uint32_t(i), JS_NewFloat64(ctx, st.dash[i]));
			return a;
		} },
		{ "createImageData", 2, FN {
			int w, h;
			if (JS_IsObject(argv[0]))
			{
				JSValue a = JS_GetPropertyStr(ctx, argv[0], "width"), b = JS_GetPropertyStr(ctx, argv[0], "height");
				w = int(to_number(ctx, a));
				h = int(to_number(ctx, b));
				JS_FreeValue(ctx, a);
				JS_FreeValue(ctx, b);
			}
			else
			{
				w = int(std::fabs(to_number(ctx, argv[0])));
				h = int(std::fabs(to_number(ctx, argv[1])));
			}
			return image_data(ctx, w, h, nullptr);
		} },
		{ "getImageData", 4, FN {
			THIS_CANVAS;
			(void)st;
			const int x = int(to_number(ctx, argv[0])), y = int(to_number(ctx, argv[1]));
			const int w = int(to_number(ctx, argv[2])), h = int(to_number(ctx, argv[3]));
			std::vector<uint8_t> out(size_t(std::max(0, w)) * size_t(std::max(0, h)) * 4);
			for (int r = 0; r < h; r++)
				for (int k = 0; k < w; k++)
				{
					const int sx = x + k, sy = y + r;
					if (sx < 0 || sy < 0 || sx >= cv.pixels_w || sy >= cv.pixels_h)
						continue;
					std::copy_n(&cv.pixels[(size_t(sy) * size_t(cv.pixels_w) + size_t(sx)) * 4], 4, &out[(size_t(r) * size_t(w) + size_t(k)) * 4]);
				}
			return image_data(ctx, w, h, out.data());
		} },
		{ "putImageData", 3, FN {
			THIS_CANVAS;
			(void)st;
			JSValue wv = JS_GetPropertyStr(ctx, argv[0], "width"), hv = JS_GetPropertyStr(ctx, argv[0], "height");
			JSValue data = JS_GetPropertyStr(ctx, argv[0], "data");
			const int w = int(to_number(ctx, wv)), h = int(to_number(ctx, hv));
			JS_FreeValue(ctx, wv);
			JS_FreeValue(ctx, hv);
			size_t offset = 0, length = 0, bytes = 0;
			JSValue buffer = JS_GetTypedArrayBuffer(ctx, data, &offset, &length, &bytes);
			JS_FreeValue(ctx, data);
			if (JS_IsException(buffer))
				return JS_EXCEPTION;
			size_t size;
			const uint8_t *src = JS_GetArrayBuffer(ctx, &size, buffer);
			const int cw = int(el->number_attribute(n::width, 300)), ch = int(el->number_attribute(n::height, 150));
			if (cv.pixels_w != cw || cv.pixels_h != ch)
			{
				cv.pixels.assign(size_t(std::max(0, cw)) * size_t(std::max(0, ch)) * 4, 0);
				cv.pixels_w = cw;
				cv.pixels_h = ch;
			}
			const int dx = int(to_number(ctx, argv[1])), dy = int(to_number(ctx, argv[2]));
			if (src && length >= size_t(w) * size_t(h) * 4)
				for (int r = 0; r < h; r++)
					for (int k = 0; k < w; k++)
					{
						const int tx = dx + k, ty = dy + r;
						if (tx < 0 || ty < 0 || tx >= cw || ty >= ch)
							continue;
						std::copy_n(src + offset + (size_t(r) * size_t(w) + size_t(k)) * 4, 4,
							&cv.pixels[(size_t(ty) * size_t(cw) + size_t(tx)) * 4]);
					}
			JS_FreeValue(ctx, buffer);
			cv.version++;
			dom.invalidate_paint();
			return JS_UNDEFINED;
		} },
	});
	JS_SetClassProto(ctx, dom.classes().context2d, proto);
}

} // namespace qdom::js
