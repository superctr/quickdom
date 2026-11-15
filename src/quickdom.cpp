// Copyright 2026 Ian Karlsson
// The C interface.
#include "quickdom/quickdom.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dom/dom.h"
#include "input/input.h"
#include "instance.h"
#include "paint/paint.h"

qdom_t *qdom_create(JSContext *ctx, const qdom_options_t *options)
{
	auto *d = new qdom::Dom(ctx, options);
	qdom::install(*d);
	auto *q = new qdom_instance;
	q->dom = d;
	q->options = options ? *options : qdom_options_t {};
	return q;
}

void qdom_destroy(qdom_t *dom)
{
	if (!dom)
		return;
	if (dom->gone)
		dom->gone(dom->gone_ctx);
	delete dom->dom;
	delete dom;
}

void qdom_set_viewport(qdom_t *dom, double width, double height, double scale)
{
	dom->dom->set_viewport(width, height, scale);
	dom->dom->run_jobs();
}

void qdom_add_stylesheet(qdom_t *dom, const char *css, size_t length)
{
	dom->dom->add_sheet(std::string(css, length));
}

bool qdom_add_font(qdom_t *dom, const char *family, int weight, bool italic, const void *data, size_t size,
	bool ceil_metrics)
{
	const bool ok = dom->dom->fonts.add(family, weight, italic, static_cast<const uint8_t *>(data), size, false,
		ceil_metrics ? qdom::MetricsRounding::Ceil : qdom::MetricsRounding::Round);
	dom->dom->fonts_changed();
	return ok;
}

bool qdom_add_default_fonts(qdom_t *dom)
{
	const bool ok = dom->dom->fonts.add_defaults();
	dom->dom->fonts_changed();
	return ok;
}

void qdom_set_generic_font(qdom_t *dom, const char *generic, const char *family)
{
	dom->dom->fonts.set_generic(generic, family);
	dom->dom->fonts_changed();
}

void qdom_advance(qdom_t *dom, double now)
{
	dom->dom->advance(now);
	dom->dom->run_jobs();
}

double qdom_next_timer(qdom_t *dom)
{
	return dom->dom->next_timer();
}

bool qdom_frame_requested(qdom_t *dom)
{
	return dom->dom->frame_requested();
}

bool qdom_frame(qdom_t *dom, double now)
{
	const bool paint = dom->dom->frame(now);
	dom->dom->run_jobs();
	return paint;
}

void qdom_update(qdom_t *dom)
{
	dom->dom->update();
}

qdom_commands_t qdom_paint(qdom_t *dom, bool caret)
{
	qdom::PaintOptions o;
	o.caret = caret;
	if (dom->options.image || dom->sink)
		o.image = [dom](const qdom::ImageUpload &u) {
			if (dom->options.image)
				dom->options.image(dom->options.user, u.id, u.width, u.height, u.rgba);
			if (dom->sink)
				dom->sink(dom->sink_ctx, u.id, u.width, u.height, u.rgba);
		};
	const auto t0 = std::chrono::steady_clock::now();
	qdom::paint(*dom->dom, dom->commands, o);
	dom->dom->painted();
	qdom::Dom::Stats &st = dom->dom->stats;
	const double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	st.paints++;
	st.paint_ms += t;
	st.paint_max = std::max(st.paint_max, t);
	dom->texts.clear();
	for (size_t i = 0; i < dom->commands.texts.size(); i++)
	{
		if (i)
			dom->texts += '\0';
		dom->texts += dom->commands.texts[i];
	}
	return { dom->commands.data.data(), dom->commands.data.size(), dom->texts.data(), dom->texts.size() };
}

bool qdom_needs_paint(qdom_t *dom)
{
	return dom->dom->needs_paint();
}

void qdom_input_pointer(qdom_t *dom, int kind, double x, double y, int button, int mods)
{
	const qdom::PointerKind k = kind == QDOM_POINTER_DOWN ? qdom::PointerKind::Down
		: kind == QDOM_POINTER_UP ? qdom::PointerKind::Up : qdom::PointerKind::Move;
	dom->dom->input().pointer(k, x, y, button, mods);
}

void qdom_input_wheel(qdom_t *dom, double x, double y, double dx, double dy, int mods)
{
	dom->dom->input().wheel(x, y, dx, dy, mods);
}

bool qdom_input_key(qdom_t *dom, bool down, const char *key, const char *code, bool repeat, int mods)
{
	return dom->dom->input().key(down, key ? key : "", code ? code : "", repeat, mods);
}

void qdom_input_text(qdom_t *dom, const char *text, size_t length)
{
	dom->dom->input().text(std::string_view(text, length));
}

void qdom_input_paste(qdom_t *dom, const char *text, size_t length)
{
	dom->dom->input().paste(std::string_view(text ? text : "", text ? length : 0));
}

void qdom_input_leave(qdom_t *dom)
{
	dom->dom->input().leave();
}

bool qdom_input_drag(qdom_t *dom, int kind, double x, double y, const char *const *paths, size_t count)
{
	std::vector<std::string> list;
	for (size_t i = 0; paths && i < count; i++)
		if (paths[i])
			list.emplace_back(paths[i]);
	return dom->dom->input().drag(kind, x, y, list);
}

void qdom_input_blur(qdom_t *dom)
{
	dom->dom->input().window_blur();
}

void qdom_input_focus(qdom_t *dom)
{
	dom->dom->input().window_focus();
}

const char *qdom_cursor(qdom_t *dom)
{
	dom->dom->update();
	dom->dom->input().refresh_cursor();
	return dom->dom->input().cursor().c_str();
}

qdom_stats_t qdom_stats(qdom_t *dom)
{
	const qdom::Dom::Stats &s = dom->dom->stats;
	return { s.frames, s.restyles, s.styles, s.layouts, s.boxes, s.paints, s.frame_ms, s.style_ms, s.layout_ms,
		s.paint_ms, s.frame_max, s.style_max, s.layout_max, s.paint_max };
}

void qdom_reset_stats(qdom_t *dom)
{
	dom->dom->stats = {};
}

const char *qdom_title(qdom_t *dom)
{
	dom->title = dom->dom->title();
	return dom->title.c_str();
}

void qdom_resource(qdom_t *dom, const char *src, const void *data, size_t size)
{
	if (src)
		dom->dom->picture_bytes(src, data, data ? size : 0);
}

char *qdom_boxes(qdom_t *dom)
{
	const std::string s = dom->dom->boxes_json();
	char *out = static_cast<char *>(malloc(s.size() + 1));
	if (out)
		memcpy(out, s.c_str(), s.size() + 1);
	return out;
}

void qdom_free(void *p)
{
	free(p);
}
