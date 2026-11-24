# quickdom advanced usage

## Build options

- `-DQUICKDOM_FONTS=OFF` leaves the built-in fonts out of the library;
- `-DQUICKDOM_TOOLS`, `-DQUICKDOM_GL` and `-DQUICKDOM_PUGL` build `qdom-run`, `quickdom_gl` and `quickdom_pugl`; they
  are on only when quickdom is the top-level project.

For Windows, cross-compiled with mingw-w64 (`qdom-run.exe` runs under Wine):

```sh
cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
cmake --build build-win
```

## qdom-run

`qdom-run` runs a page headless and prints or compares its boxes:

```sh
build/qdom-run page.js --size 1600x1000 --boxes                 # a module that builds a page
build/qdom-run --css app.css app.js --expect golden.json         # an application with its stylesheet
build/qdom-run --css app.css app.js --window                     # the page in a window, until it is closed
build/qdom-run page.js --check-layout                            # each layout and restyle checked against ones made anew
```

The test suite is not part of this repository yet.

## Using the library

The C interface is in `include/quickdom/quickdom.h`, `quickdom_gl.h` and `quickdom_pugl.h`.

As a submodule, from the host's CMake:

```cmake
add_subdirectory(thirdparty/quickdom)       # uses the host's qjs target when there is one
target_link_libraries(my_host PRIVATE quickdom)       # and quickdom_gl to draw with OpenGL, quickdom_pugl for a window
```

A host with its own `pugl` or `nanovg` targets names them (`-DQUICKDOM_PUGL_TARGET=pugl`, `QUICKDOM_NANOVG_TARGET`),
so that only one copy is linked.

The host keeps its JSRuntime and JSContext, its module loader and its own globals; quickdom is installed into that
context and owns no clock and no thread.  The host moves time on (`qdom_advance`, `qdom_frame`), hands over the
window's input (`qdom_input_*`) and draws the commands `qdom_paint` gives:

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

Or in quickdom's own window, which handles input and drawing (`include/quickdom/quickdom_pugl.h`):

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

Pictures for `<img>` come from the host as the file's bytes (`qdom_resource`).

## Debugging

- `qdom-run --check-layout` checks each incremental layout and restyle against one made from scratch.
- `__qdom.retained()` lists what is alive in the JavaScript heap though out of the document, to find what keeps it.
