# quickdom

A DOM for QuickJS: the browser's document and window, their styles and layout, in a
[QuickJS-ng](https://github.com/quickjs-ng/quickjs) context, written in C++20 with a C interface. Pages written
for the browser, building their elements by script, run on it unchanged, laid out as Firefox lays them out, to be
drawn natively (with nanovg on OpenGL, in a PUGL window) where no browser can be had: an audio plugin's window, a
desktop application's.

The design, the API it offers and what differs from Firefox are in [docs/design.md](docs/design.md).

## Status

- The DOM (nodes, attributes, classes, inline styles, events with capture and bubbling, focus, form controls,
  dialogs, live child lists, selectors), the window (timers, animation frames, `ResizeObserver`,
  `getComputedStyle`, `performance`), canvases' 2D contexts recorded as drawing commands.
- The cascade: selectors and properties (docs/design.md), `var()`, `calc()`, shorthands, Firefox's own
  defaults.
- The layout: block flow with collapsing margins, lines of text with Firefox's font metrics, flex, grid, tables,
  absolutely placed and fixed boxes, regions that scroll; text measured with the built-in fonts (Noto Sans, DejaVu,
  and after them, as Firefox's fallback finds them, Noto Sans Symbols 2 and Noto Color Emoji).
- The paint: the document as drawing commands (`qdom_paint`), and hit testing (`document.elementFromPoint`).
- The input (`qdom_input_*`): the pointer, the wheel and the keys made into the DOM's events, with what a browser
  does by itself, as Firefox on Linux does it: hover, the cursor, clicks and double clicks, the pointer's capture, the
  focus by press and by Tab, regions scrolled by the wheel and their scrollbars' thumbs, text fields and textareas
  edited (selections, words, history, the host's clipboard, which `navigator.clipboard` reaches too), a list's popup,
  ranges, checkboxes, labels, a dialog's Escape, tooltips, the caret's blink, files dragged in from other programs
  (their paths in the drop's `dataTransfer`).
- Pictures: `<img>` (the host gives the file's bytes, `qdom_resource`) and canvases' `putImageData`.
- `quickdom_gl`: the commands drawn with nanovg on OpenGL 2.1 in the host's context (Linux and Windows).
- `quickdom_pugl`: a document in a PUGL window of its own or in a host's (a plugin's): the system's pointer, keys,
  focus and clipboard handed to it, its frames drawn, its cursor and title followed, its pictures taken without
  showing it.
- Laying out and styling only what a change reaches: the box tree and its layouts are kept across passes, a colour
  changed needs no layout, a class restyles only as far as the selectors read it; a frame of a page of 1,500 boxes
  with one control changed takes about 0.1 ms. `--check-layout` checks each layout and restyle against ones made anew.
- Colour emoji drawn as Firefox draws them on Linux: Noto Color Emoji's pictures (its CBDT bitmaps), reduced and
  drawn smooth as WebRender does them, by `quickdom_gl` from an atlas of colour glyphs; the pictographs shown as text
  from Noto Sans Symbols 2.
- A collection of the JavaScript heap once the input has been still a while, so that what a burst of input left is
  freed between bursts; `__qdom.retained()` lists what is alive though out of the document, to find what keeps it.

## Building

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug   # configure
cmake --build build                                 # the library and qdom-run
```

For Windows, cross-compiled with mingw-w64 (`qdom-run.exe` runs under Wine):

```sh
cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
cmake --build build-win
```

`qdom-run` runs a page headless and prints or compares its boxes:

```sh
build/qdom-run page.js --size 1600x1000 --boxes                 # a module that builds a page
build/qdom-run --css app.css app.js --expect golden.json         # an application with its stylesheet
build/qdom-run --css app.css app.js --window                     # the page in a window, until it is closed
build/qdom-run page.js --check-layout                            # each layout and restyle checked against ones made anew
```

The test suite is not part of this repository yet.

## Using it

As a submodule, from the host's CMake:

```cmake
add_subdirectory(thirdparty/quickdom)       # uses the host's qjs target when there is one
target_link_libraries(my_host PRIVATE quickdom)       # and quickdom_gl to draw with OpenGL, quickdom_pugl for a window
```

A host with its own `pugl` or `nanovg` targets names them (`-DQUICKDOM_PUGL_TARGET=pugl`, `QUICKDOM_NANOVG_TARGET`),
so that one copy is linked; `QUICKDOM_GL` and `QUICKDOM_PUGL` are on only when quickdom is the top project.

```c
JSRuntime *rt = JS_NewRuntime();
JSContext *ctx = JS_NewContext(rt);
/* the host's own globals and module loader */
qdom_t *dom = qdom_create(ctx, &(qdom_options_t){ .log = my_log });
qdom_add_default_fonts(dom);
qdom_add_stylesheet(dom, css, strlen(css));
qdom_set_viewport(dom, 1600, 1000, 1.0);
/* evaluate the page's modules; once the window's GL context is current: */
qdom_gl_t *gl = qdom_gl_create(dom);
/* each frame: the window's input (qdom_input_*), then */
qdom_advance(dom, now);
if (qdom_frame(dom, now) || qdom_needs_paint(dom))
{
	qdom_commands_t c = qdom_paint(dom, true);
	qdom_gl_draw(gl, &c, width, height, scale);   /* then swap the window's buffers */
}
/* ... */
qdom_gl_destroy(gl);
qdom_destroy(dom);
JS_FreeContext(ctx);
JS_FreeRuntime(rt);
```

Or in quickdom's window, which runs the frames and draws them (`include/quickdom/quickdom_pugl.h`):

```c
qdom_window_t *w = qdom_window_create(&(qdom_window_options_t){ .title = "app", .width = 1600, .height = 1000 },
	err, sizeof(err));
qdom_window_attach(w, dom);
qdom_window_show(w);
while (!qdom_window_closed(w))
{
	qdom_advance(dom, now);
	qdom_window_update(w, 1.0 / 120);
}
qdom_window_destroy(w);
```

## Licences

quickdom is under the BSD 3-Clause License (`LICENSE`). QuickJS-ng (MIT), nanovg (zlib), PUGL (ISC) are submodules;
`thirdparty/stb/stb_truetype.h` and nanovg's copies of stb_image and stb_image_write are public domain; the fonts are
Noto Sans (SIL OFL 1.1, `fonts/OFL.txt`) and DejaVu (`fonts/DEJAVU_LICENSE`), cut to Latin, punctuation, arrows and
common symbols by `fonts/subset.sh`, and Noto Sans Symbols 2 and Noto Color Emoji (SIL OFL 1.1), cut by
`fonts/fallbacks.py`.
