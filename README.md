# quickdom

A DOM for [QuickJS-ng](https://github.com/quickjs-ng/quickjs): the browser's document and window, with styles and
layout, for running web pages natively where no browser can be had, such as an audio plugin's window or a desktop
application's.

Pages that build their elements by script run unchanged, laid out as Firefox lays them out, and drawn with nanovg on
OpenGL in a PUGL window.

Written in C++20 with a C interface.  BSD-3.

## Features

- **DOM**: nodes, attributes, classes, inline styles, selectors, live child lists, events with capture and bubbling,
  focus, form controls and dialogs.
- **Window**: timers, animation frames, `ResizeObserver`, `getComputedStyle`, `performance`, the clipboard.
- **CSS**: the common selectors and properties, `var()`, `calc()`, shorthands and Firefox's own defaults.
- **Layout**: block flow, text with Firefox's font metrics, flex, grid, tables, absolute and fixed positioning,
  scrolling regions.
- **Input**: pointer, wheel and keys turned into DOM events, with what Firefox on Linux does by itself:
	- hover, cursors, clicks, double clicks and pointer capture
	- focus by click and by Tab
	- scrolling by wheel and by scrollbar
	- text fields and textareas with selection, undo and the system clipboard
	- dropdown lists, ranges, checkboxes, labels, tooltips
	- files dragged in from other programs
- **Drawing**:
	- `<img>` and 2D canvases
	- the page as a list of drawing commands, with hit testing
	- `quickdom_gl`: the commands drawn with nanovg on OpenGL 2.1 (Linux and Windows)
	- `quickdom_pugl`: a page in a window of its own, or in a host's (such as a plugin's)
- **Fonts**: Noto Sans and DejaVu built in, with Noto Sans Symbols 2 and Noto Color Emoji as fallbacks; colour emoji
  drawn as Firefox draws them.
- **Fast updates**: only what a change reaches is restyled and laid out again; a frame of a 1,500-box page with one
  control changed takes about 0.1 ms.
- **Memory**: the JavaScript heap is collected when the input goes quiet; `__qdom.retained()` finds what keeps
  detached elements alive.

What is supported and how it differs from Firefox is in [docs/design.md](docs/design.md).

## Building

```sh
git submodule update --init --recursive
cmake -B build -G Ninja
cmake --build build
```

This builds the library and `qdom-run`, which runs a page headless, or in a window with `--window`:

```sh
build/qdom-run --css app.css app.js --window
```

Needs CMake 3.23 or newer and a C++20 compiler.  The window needs OpenGL, and X11 on Linux.

For Windows cross-compiling, embedding quickdom in your own program and the rest of `qdom-run`'s options, see
[docs/advanced.md](docs/advanced.md).

## Licences

quickdom is under the BSD 3-Clause License (`LICENSE`).

- QuickJS-ng (MIT), nanovg (zlib) and PUGL (ISC) are submodules.
- `thirdparty/stb/stb_truetype.h` and nanovg's copies of stb_image and stb_image_write are public domain.
- The fonts are Noto Sans (SIL OFL 1.1, `fonts/OFL.txt`) and DejaVu (`fonts/DEJAVU_LICENSE`), cut to Latin,
  punctuation, arrows and common symbols by `fonts/subset.sh`, and Noto Sans Symbols 2 and Noto Color Emoji (SIL OFL
  1.1), cut by `fonts/fallbacks.py`.

## Disclaimers

This project is not affiliated with Mozilla or the QuickJS-ng project.

AI disclosure: Anthropic's Claude models were used in this project.
