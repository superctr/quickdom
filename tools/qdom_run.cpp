// Copyright 2026 Ian Karlsson
// qdom-run: a page's JavaScript run headless on quickdom, its layout printed or compared.
//
//   qdom-run [options] module.js
//     --size WxH         the viewport (1600x1000)
//     --scale S          device pixels per CSS pixel (1)
//     --css FILE         a stylesheet of the host's, before the page's own (repeatable)
//     --fixture          the module exports css (text) and build(document): the stylesheet put into the head and
//                        the page built, as a test harness does it in a browser
//     --boxes            every element's box under the body printed as JSON once the page has settled
//     --expect FILE      the boxes compared with FILE (JSON, the same format --boxes prints)
//     --tolerance PX     how far a box may be off (0.5)
//     --time MS          how long the page's clock runs at most (5000); it stops when nothing is pending
//     --root DIR         where the files the page names are (an img's src): the module's directory by default
//     --shot FILE        the page's last frame drawn by quickdom_gl offscreen (EGL), written as a PNG
//     --expect-shot FILE the frame compared with a PNG (Firefox's): a pixel differs when a channel is off by more
//                        than --threshold (48 of 255); --max-differ P (0.5) the share in percent that may differ,
//                        --diff FILE where they do. Exit 77 (a test skipped) when no GL context can be made
//     --shot-window      the pictures drawn through quickdom_pugl's window (not shown) instead: exit 77 without a
//                        display
//     --window           the page shown in a window once it has settled, on the real clock, until it is closed (the
//                        clipboard the system's)
//     --paint            each frame painted as a window's host paints it (when something changed)
//     --stats            what the work took printed at the end (qdom_stats: frames, restyles, layouts, paints)
//     --check-layout     each layout and restyle checked against ones made anew (QDOM_CHECK_LAYOUT=1): a
//                        difference is an error
//
// Modules are JavaScript, or JSON (imported `with { type: 'json' }`, or a file named *.json) as browsers load them.
// The clock is virtual: timers and animation frames (every 16 ms) run as fast as they can. The clipboard is the
// runner's own (a field's copy keeps the text, its paste gets it back at once). The exit status is 1 when the
// module throws, a listener or callback throws, or a box differs.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "quickjs.h"

#include "quickdom/quickdom.h"
#ifdef QDOM_RUN_WINDOW
#include <chrono>

#include "quickdom/quickdom_pugl.h"
#endif

#include "shots.h"

namespace
{

int g_errors = 0;

//! The host's side: its clipboard, the files the page names.
struct Host
{
	qdom_t *dom = nullptr;
	std::string clipboard;
	std::string root;
};

std::string read_file(const std::string &path, bool &ok)
{
	std::ifstream f(path, std::ios::binary);
	ok = bool(f);
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

void log_line(void *, const char *text)
{
	fprintf(stderr, "%s\n", text);
	// what the library logs on its own is an exception thrown by a listener or a callback
	if (!strncmp(text, "Error", 5) || strstr(text, "Error:") || strstr(text, "    at "))
		g_errors++;
}

// a JSON module's one export, its value
int json_module_init(JSContext *ctx, JSModuleDef *m)
{
	return JS_SetModuleExport(ctx, m, "default", JS_GetModulePrivateValue(ctx, m));
}

// a module: JavaScript, or JSON (imported `with { type: 'json' }` or named *.json) as browsers have it
JSModuleDef *load_module(JSContext *ctx, const char *name, void *, JSValueConst attributes)
{
	bool ok;
	const std::string src = read_file(name, ok);
	if (!ok)
	{
		JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
		return nullptr;
	}
	bool json = std::string_view(name).ends_with(".json");
	if (JS_IsObject(attributes))
	{
		JSValue type = JS_GetPropertyStr(ctx, attributes, "type");
		if (const char *t = JS_IsString(type) ? JS_ToCString(ctx, type) : nullptr)
		{
			json = !strcmp(t, "json");
			JS_FreeCString(ctx, t);
		}
		JS_FreeValue(ctx, type);
	}
	if (json)
	{
		JSValue value = JS_ParseJSON(ctx, src.c_str(), src.size(), name);
		if (JS_IsException(value))
			return nullptr;
		JSModuleDef *m = JS_NewCModule(ctx, name, json_module_init);
		if (!m)
		{
			JS_FreeValue(ctx, value);
			return nullptr;
		}
		JS_AddModuleExport(ctx, m, "default");
		JS_SetModulePrivateValue(ctx, m, value);
		return m;
	}
	JSValue fn = JS_Eval(ctx, src.c_str(), src.size(), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
	if (JS_IsException(fn))
		return nullptr;
	auto *m = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(fn));
	JS_FreeValue(ctx, fn);
	return m;
}

void report(JSContext *ctx, JSValueConst e)
{
	const char *text = JS_ToCString(ctx, e);
	JSValue stack = JS_IsError(e) ? JS_GetPropertyStr(ctx, e, "stack") : JS_UNDEFINED;
	const char *where = JS_IsUndefined(stack) ? nullptr : JS_ToCString(ctx, stack);
	fprintf(stderr, "%s\n%s", text ? text : "(exception)", where ? where : "");
	JS_FreeCString(ctx, where);
	JS_FreeCString(ctx, text);
	JS_FreeValue(ctx, stack);
}

void run_jobs(JSRuntime *rt)
{
	JSContext *c;
	int r;
	while ((r = JS_ExecutePendingJob(rt, &c)) != 0)
		if (r < 0)
		{
			JSValue e = JS_GetException(c);
			report(c, e);
			JS_FreeValue(c, e);
			g_errors++;
		}
}

std::string js_string(const std::string &s)
{
	std::string out = "\"";
	for (char c : s)
	{
		if (c == '"' || c == '\\')
			out += '\\';
		out += c;
	}
	return out + "\"";
}

// the comparison of two box lists, in the context's JavaScript: a box by its path and its place among the boxes
// with that path
const char *const COMPARE = R"js(
(ours, theirs, tolerance) => {
	const key = (list) => {
		const seen = new Map(), out = new Map();
		for (const b of list) {
			const n = seen.get(b[0]) || 0;
			seen.set(b[0], n + 1);
			out.set(`${b[0]}[${n}]`, b);
		}
		return out;
	};
	const a = key(JSON.parse(ours)), b = key(JSON.parse(theirs));
	const lines = [];
	let differ = 0;
	for (const [k, x] of b) {
		const y = a.get(k);
		if (!y) {
			differ++;
			lines.push(`missing  ${k}  firefox ${x.slice(1).join(' ')}`);
			continue;
		}
		if (x.slice(1).some((v, i) => Math.abs(v - y[i + 1]) > tolerance + 1e-9)) {
			differ++;
			lines.push(`differs  ${k}\n         firefox  ${x.slice(1).join(' ')}\n         quickdom ${y.slice(1).join(' ')}`);
		}
	}
	for (const k of a.keys())
		if (!b.has(k)) {
			differ++;
			lines.push(`extra    ${k}  quickdom ${a.get(k).slice(1).join(' ')}`);
		}
	return JSON.stringify({ total: b.size, differ, lines: lines.slice(0, 40) });
}
)js";

// the page drawn through the window's snapshot (a window made, not shown)
bool draw_in_window(qdom_t *dom, int width, int height, double scale, Image &out, std::string &error)
{
#ifdef QDOM_RUN_WINDOW
	char why[200];
	qdom_window_options_t o {};
	o.width = width;
	o.height = height;
	o.class_name = "qdom-run";
	qdom_window_t *w = qdom_window_create(&o, why, sizeof(why));
	if (!w)
	{
		error = why;
		return false;
	}
	qdom_window_attach(w, dom);
	out.width = int(std::lrint(width * scale));
	out.height = int(std::lrint(height * scale));
	out.rgba.assign(size_t(out.width) * size_t(out.height) * 4, 0);
	const bool ok = qdom_window_snapshot(w, out.rgba.data(), width, height, scale);
	qdom_window_destroy(w);
	if (!ok)
		error = "the window's context could not draw it";
	return ok;
#else
	error = "built without quickdom_pugl";
	return false;
#endif
}

} // namespace

int main(int argc, char **argv)
{
	double width = 1600, height = 1000, scale = 1, limit = 5000, tolerance = 0.5;
	std::vector<std::string> css_files;
	std::string module, expect, root, shot, expect_shot, diff;
	double max_differ = 0.5;
	int threshold = 48;
	bool fixture = false, boxes = false, window = false, shot_window = false, paint = false, stats = false;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc)
			{
				fprintf(stderr, "qdom-run: %s needs a value\n", a.c_str());
				exit(2);
			}
			return argv[++i];
		};
		if (a == "--size")
		{
			const std::string v = next();
			if (sscanf(v.c_str(), "%lfx%lf", &width, &height) != 2)
			{
				fprintf(stderr, "qdom-run: --size WxH\n");
				return 2;
			}
		}
		else if (a == "--scale")
			scale = atof(next().c_str());
		else if (a == "--css")
			css_files.push_back(next());
		else if (a == "--fixture")
			fixture = true;
		else if (a == "--boxes")
			boxes = true;
		else if (a == "--expect")
			expect = next();
		else if (a == "--tolerance")
			tolerance = atof(next().c_str());
		else if (a == "--time")
			limit = atof(next().c_str());
		else if (a == "--root")
			root = next();
		else if (a == "--shot")
			shot = next();
		else if (a == "--expect-shot")
			expect_shot = next();
		else if (a == "--max-differ")
			max_differ = atof(next().c_str());
		else if (a == "--threshold")
			threshold = atoi(next().c_str());
		else if (a == "--diff")
			diff = next();
		else if (a == "--window")
			window = true;
		else if (a == "--shot-window")
			shot_window = true;
		else if (a == "--paint")
			paint = true;
		else if (a == "--stats")
			stats = true;
		else if (a == "--check-layout")
#if defined(_WIN32)
			_putenv("QDOM_CHECK_LAYOUT=1");
#else
			setenv("QDOM_CHECK_LAYOUT", "1", 1);
#endif
		else if (a == "-h" || a == "--help")
		{
			printf("qdom-run [--size WxH] [--scale S] [--css FILE]... [--fixture] [--boxes] [--expect FILE "
			       "[--tolerance PX]] [--time MS] [--root DIR] [--shot FILE] [--expect-shot FILE [--max-differ P] "
			       "[--threshold T] [--diff FILE]] [--shot-window] [--window] [--paint] [--stats] [--check-layout] module.js\n");
			return 0;
		}
		else if (!a.empty() && a[0] == '-')
		{
			fprintf(stderr, "qdom-run: unknown option %s\n", a.c_str());
			return 2;
		}
		else
			module = a;
	}
	if (module.empty())
	{
		fprintf(stderr, "qdom-run: a module to run is needed (--help)\n");
		return 2;
	}
	std::error_code ec;
	const std::string path = std::filesystem::absolute(module, ec).lexically_normal().generic_string();

	JSRuntime *rt = JS_NewRuntime();
	JSContext *ctx = JS_NewContext(rt);
	JS_SetModuleLoaderFunc2(rt, nullptr, load_module, nullptr, nullptr);
	Host host;
	qdom_options_t options {};
	options.log = log_line;
	options.user = &host;
	// the runner's clipboard, but in a window, whose is the system's
	if (!window)
	{
		options.copy = [](void *user, const char *text, size_t length) {
			static_cast<Host *>(user)->clipboard.assign(text, length);
		};
		options.paste = [](void *user) {
			Host *h = static_cast<Host *>(user);
			qdom_input_paste(h->dom, h->clipboard.data(), h->clipboard.size());
		};
	}
	host.root = root.empty() ? std::filesystem::path(path).parent_path().generic_string() : root;
	options.resource = [](void *user, const char *src) {
		Host *h = static_cast<Host *>(user);
		bool ok;
		const std::string bytes = read_file((std::filesystem::path(h->root) / src).generic_string(), ok);
		qdom_resource(h->dom, src, ok ? bytes.data() : nullptr, bytes.size());
	};
	qdom_t *dom = qdom_create(ctx, &options);
	host.dom = dom;
	if (!qdom_add_default_fonts(dom))
		fprintf(stderr, "qdom-run: no fonts built in; text measures nothing\n");
	for (const std::string &f : css_files)
	{
		bool ok;
		const std::string css = read_file(f, ok);
		if (!ok)
		{
			fprintf(stderr, "qdom-run: cannot read %s\n", f.c_str());
			return 2;
		}
		qdom_add_stylesheet(dom, css.c_str(), css.size());
	}
	qdom_set_viewport(dom, width, height, scale);

	// the module, or a fixture's stylesheet and page
	std::string source;
	if (fixture)
		source = "import * as f from " + js_string(path) + ";\n"
			"const s = document.createElement('style');\n"
			"s.textContent = f.css || '';\n"
			"document.head.append(s);\n"
			"await f.build(document);\n";
	else
		source = "import " + js_string(path) + ";\n";
	JSValue promise = JS_Eval(ctx, source.c_str(), source.size(), "<qdom-run>", JS_EVAL_TYPE_MODULE);
	int status = 0;
	if (JS_IsException(promise))
	{
		JSValue e = JS_GetException(ctx);
		report(ctx, e);
		JS_FreeValue(ctx, e);
		status = 1;
	}
	run_jobs(rt);

	// the clock: timers as they fall due, a frame every 16 ms, until nothing is pending
	double now = 0;
	while (status == 0 && now <= limit)
	{
		qdom_advance(dom, now);
		if (qdom_frame(dom, now) && paint)
			qdom_paint(dom, true);
		run_jobs(rt);
		const bool settled = JS_PromiseState(ctx, promise) != JS_PROMISE_PENDING;
		if (settled && !qdom_frame_requested(dom) && qdom_next_timer(dom) < 0)
			break;
		double next = now + 16;
		const double timer = qdom_next_timer(dom);
		if (!qdom_frame_requested(dom) && timer >= 0 && timer > next)
			next = timer;
		now = next;
	}
	// a last frame, so that what the last callbacks changed is laid out
	qdom_frame(dom, now);
	run_jobs(rt);
	if (status == 0 && JS_PromiseState(ctx, promise) == JS_PROMISE_REJECTED)
	{
		JSValue e = JS_PromiseResult(ctx, promise);
		report(ctx, e);
		JS_FreeValue(ctx, e);
		status = 1;
	}
	else if (status == 0 && JS_PromiseState(ctx, promise) == JS_PROMISE_PENDING)
	{
		fprintf(stderr, "qdom-run: the module had not finished after %g ms\n", limit);
		status = 1;
	}
	if (g_errors)
		status = 1;
	if (stats)
	{
		const qdom_stats_t st = qdom_stats(dom);
		auto each = [](double ms, unsigned long long n) { return n ? ms / double(n) : 0.0; };
		fprintf(stderr,
			"qdom-run: %llu frames %.1f ms (%.3f each, %.3f at most); %llu restyles %.1f ms (%llu styles, %.3f at most); "
			"%llu layouts %.1f ms (%llu boxes, %.3f each, %.3f at most); %llu paints %.1f ms (%.3f each, %.3f at most)\n",
			st.frames, st.frame_ms, each(st.frame_ms, st.frames), st.frame_max_ms, st.restyles, st.style_ms, st.styles,
			st.style_max_ms, st.layouts, st.layout_ms, st.boxes, each(st.layout_ms, st.layouts), st.layout_max_ms,
			st.paints, st.paint_ms, each(st.paint_ms, st.paints), st.paint_max_ms);
	}

	if (status == 0 && (boxes || !expect.empty()))
	{
		char *json = qdom_boxes(dom);
		if (boxes)
			printf("%s\n", json);
		if (!expect.empty())
		{
			bool ok;
			const std::string theirs = read_file(expect, ok);
			if (!ok)
			{
				fprintf(stderr, "qdom-run: cannot read %s\n", expect.c_str());
				status = 2;
			}
			else
			{
				JSValue fn = JS_Eval(ctx, COMPARE, strlen(COMPARE), "<compare>", JS_EVAL_TYPE_GLOBAL);
				JSValue args[3] = { JS_NewString(ctx, json), JS_NewStringLen(ctx, theirs.data(), theirs.size()),
					JS_NewFloat64(ctx, tolerance) };
				JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 3, args);
				JSValue get = JS_Eval(ctx, "(r) => JSON.parse(r)", 20, "<compare>", JS_EVAL_TYPE_GLOBAL);
				JSValue obj = JS_Call(ctx, get, JS_UNDEFINED, 1, &r);
				JSValue total = JS_GetPropertyStr(ctx, obj, "total"), differ = JS_GetPropertyStr(ctx, obj, "differ");
				JSValue lines = JS_GetPropertyStr(ctx, obj, "lines");
				int32_t t = 0, d = 0;
				JS_ToInt32(ctx, &t, total);
				JS_ToInt32(ctx, &d, differ);
				const int32_t n = [&] {
					JSValue len = JS_GetPropertyStr(ctx, lines, "length");
					int32_t v = 0;
					JS_ToInt32(ctx, &v, len);
					JS_FreeValue(ctx, len);
					return v;
				}();
				for (int32_t i = 0; i < n; i++)
				{
					JSValue l = JS_GetPropertyUint32(ctx, lines, uint32_t(i));
					const char *s = JS_ToCString(ctx, l);
					printf("%s\n", s ? s : "");
					JS_FreeCString(ctx, s);
					JS_FreeValue(ctx, l);
				}
				printf("%s: %d boxes, %d differ by more than %g px\n", module.c_str(), t, d, tolerance);
				if (d)
					status = 1;
				for (JSValue v : { fn, r, get, obj, total, differ, lines, args[0], args[1], args[2] })
					JS_FreeValue(ctx, v);
			}
		}
		qdom_free(json);
	}

	if (status == 0 && (!shot.empty() || !expect_shot.empty()))
	{
		Image frame;
		std::string why;
		if (!(shot_window ? draw_in_window(dom, int(width), int(height), scale, frame, why)
		                  : draw_offscreen(dom, int(width), int(height), scale, frame, why)))
		{
			fprintf(stderr, "qdom-run: no picture: %s\n", why.c_str());
			status = 77;
		}
		else
		{
			if (!shot.empty() && !write_png(shot, frame))
			{
				fprintf(stderr, "qdom-run: cannot write %s\n", shot.c_str());
				status = 2;
			}
			if (!expect_shot.empty())
			{
				Image want;
				if (!read_png(expect_shot, want))
				{
					fprintf(stderr, "qdom-run: cannot read %s\n", expect_shot.c_str());
					status = 2;
				}
				else
				{
					Image where;
					const size_t n = compare_images(frame, want, threshold, diff.empty() ? nullptr : &where);
					const double share = 100.0 * double(n) / std::max(1.0, double(want.width) * want.height);
					printf("%s: %zu of %d pixels differ (%.2f%%, %.2f%% allowed)\n", module.c_str(), n,
						want.width * want.height, share, max_differ);
					if (!diff.empty() && n && !write_png(diff, where))
						fprintf(stderr, "qdom-run: cannot write %s\n", diff.c_str());
					if (share > max_differ)
						status = 1;
				}
			}
		}
	}

#ifdef QDOM_RUN_WINDOW
	if (status == 0 && window)
	{
		char why[200];
		qdom_window_options_t o {};
		o.width = int(width);
		o.height = int(height);
		o.class_name = "qdom-run";
		qdom_window_t *w = qdom_window_create(&o, why, sizeof(why));
		if (!w || (qdom_window_attach(w, dom), !qdom_window_show(w)))
		{
			fprintf(stderr, "qdom-run: no window: %s\n", w ? "it would not open" : why);
			status = 77;
		}
		// the page's clock goes on from its virtual time, at the real rate
		const auto t0 = std::chrono::steady_clock::now();
		while (w && status == 0 && !qdom_window_closed(w))
		{
			const double t = now + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			qdom_advance(dom, t);
			run_jobs(rt);
			qdom_window_update(w, 1.0 / 120);
			run_jobs(rt);
		}
		qdom_window_destroy(w);
		if (g_errors)
			status = 1;
	}
#else
	if (window)
	{
		fprintf(stderr, "qdom-run: built without quickdom_pugl, no window\n");
		status = 77;
	}
#endif

	JS_FreeValue(ctx, promise);
	qdom_destroy(dom);
	JS_FreeContext(ctx);
	JS_FreeRuntime(rt);
	return status;
}
