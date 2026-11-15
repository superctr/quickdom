# quickdom: design

quickdom is a DOM for QuickJS: the browser's document and window, their styles, layout and drawing, in a
QuickJS-ng context that a host owns, so that pages written for the browser run unchanged in a native window (a
PUGL window, an audio plugin's included), laid out as Firefox lays them out. The C API is documented in its headers
(`include/quickdom/quickdom.h`, `quickdom_gl.h`, `quickdom_pugl.h`).

## Scope

quickdom is for pages that build their elements by script, as a single-page application does: the page's
JavaScript makes elements, sets their attributes, classes and styles, listens to their events and draws on
canvases. It parses CSS but not HTML (`innerHTML` takes plain text). It has no network, storage, workers or
WebAssembly: what a page needs of those is the host's, given through its own globals.

The host keeps its JSRuntime and JSContext, its module loader and its own globals; quickdom is installed into that
context and owns no clock and no thread. The host moves the time on (`qdom_advance`: timers; `qdom_frame`: animation
frames, layout, resize observers), hands over the window's input (`qdom_input_*`) and draws the commands
`qdom_paint` gives, with `quickdom_gl` or its own renderer; or it lets `quickdom_pugl`'s window do the last two.

## What pages get

- **Nodes**: elements (HTML and SVG) and texts, the tree's edits, live child lists, selectors (`querySelector`,
  `matches`, `closest`), attributes, `classList`, inline styles, expando properties. No comments, fragments, shadow
  DOM or mutation observers.
- **Elements**: form controls (inputs of the text kinds, checkboxes, radios, ranges, selects, textareas) with their
  values, selections and focus; dialogs and the top layer; pointer capture; the box measures and scrolling
  (`getBoundingClientRect`, `offset*`, `client*`, `scroll*`, `scrollIntoView`), each read laying the document out
  first; `<img>` (the host gives the file's bytes) and `<canvas>`.
- **Events**: listeners with capture, target and bubble phases; one native event class with the fields of every
  kind; the events a browser makes from the pointer, the wheel and the keys, with what it does by itself (below).
- **The window**: timers, animation frames, `ResizeObserver`, `getComputedStyle`, `performance.now()`, `console`,
  `navigator.clipboard`, `document.title`, `elementFromPoint`, files dragged in from other programs (`drop`'s
  `dataTransfer`, a file's `path` as Electron's `File` has it).
- **Canvas 2D**: rectangles, paths (lines, curves, arcs), clips, alpha, colours, line widths, caps, joins and
  dashes, text and its measures, transforms, image data. Gradients, patterns, `drawImage`, shadows and composite
  operations are accepted and ignored. A canvas records drawing commands; nothing is rasterized on the CPU.
- **CSS**: the common selectors and pseudo-classes (`:hover`, `:focus-visible`, `:checked`, `:nth-child()`, `:is()`,
  `:not()`, `::before`, `::backdrop`…); box, flex, grid, overflow, text, paint and table properties with their
  shorthands; `var()`, `calc()`, `min()`, `max()`, `clamp()`, `!important`; Firefox's defaults for the tags supported,
  form controls at the sizes Firefox gives them. Not supported: transforms, transitions, animations, floats,
  `z-index` (stacking is document order), multi-column, writing modes, `@font-face` (fonts are the host's), at-rules.

## How it is built

- **Nodes and the collector.** A node lives inside its JavaScript object, which owns it. The tree's edges are
  counted references reported to QuickJS's cycle collector, so a detached subtree that nothing reaches is collected
  as one cycle. A collection runs once the input has been still for half a second after some, so that what a burst of
  input left is freed between bursts rather than in the middle of the next. `__qdom.retained()` lists what is alive
  though out of the document, each subtree with the library's references into it (a subtree with none is held by
  script), to find what keeps a page alive.
- **Styles.** The browser's defaults, the host's sheets, then the document's `<style>` elements; rules indexed by
  their rightmost compound; computed styles immutable and shared between alike elements. How far a change of class,
  attribute or state reaches is read from the selectors once, and the restyle walks only what it reaches.
- **Layout**, written to match Firefox's boxes to half a pixel: block flow with collapsing margins, lines of text,
  flex, grid (track lists with `repeat()`, `minmax()`, `fr`; items placed in order), tables (the automatic layout,
  no spans), absolute, fixed, relative and sticky boxes, regions that scroll with their scrollbars' room, replaced
  elements sized as Firefox sizes them. The box tree and its layouts are kept across passes: a change lays out what
  it reaches and the ancestors that place it, and a colour changed needs no layout at all; a frame of a page of 1,500
  boxes with one control changed takes about 0.1 ms. `QDOM_CHECK_LAYOUT=1` (`qdom-run --check-layout`) checks every
  layout and restyle against ones made anew.
- **Text.** TrueType fonts read with stb_truetype, advances and kerning rounded as Firefox's layout keeps them, line
  heights as Firefox makes room for every font a line's glyphs come from. A glyph the face lacks comes from the
  fallbacks in the order Firefox finds them on Linux; colour emoji are Noto Color Emoji's bitmaps, reduced and drawn
  smooth as WebRender does them. Built-in subsets of Noto Sans, DejaVu, Noto Sans Symbols 2 and Noto Color Emoji
  (`fonts/`); hosts add their own (`qdom_add_font`).
- **Paint.** The document becomes a list of drawing commands (`src/paint/commands.h`): backgrounds, gradients,
  borders and radii, shadows, text with ellipses, controls, SVG items, canvases' recordings, scrollbars, the top
  layer, and what the input draws over the page (a list's popup, a tooltip, the caret). Hit testing walks the same
  order. `quickdom_gl` draws the commands with nanovg on OpenGL 2.1 in the host's context, text from glyph atlases at
  quarter-pixel offsets, edges snapped as Firefox snaps them (Linux; on Windows the functions past GL 1.1 are
  fetched at run time).
- **Input**, as Firefox on Linux behaves: hover and the cursor, pointer and mouse events, clicks and their common
  ancestor, double clicks, the focus by press and by Tab, the wheel scrolling regions, scrollbars' thumbs, text
  fields and textareas edited (caret, selections, words, history, the host's clipboard), a list's popup, ranges,
  checkboxes, radios, labels, a dialog's Escape, tooltips, the caret's blink.
- **The window.** `quickdom_pugl` shows a document in a PUGL view, a window of its own or a child of a host's (a
  plugin's): the system's pointer, keys, focus, clipboard and dragged files handed over, frames run at the host's
  time and drawn when they change, the cursor and the title followed, snapshots without showing it. Linux (X11, GLX)
  and Windows (WGL, cross-built with mingw-w64).

## Known differences from Firefox

- `monospace` is DejaVu Sans Mono where fontconfig gives Firefox Noto Sans Mono.
- Glyphs outside the built-in subsets come from no font: the emoji above U+2BFF, symbols shown as text outside
  U+1F300 to U+1F8FF that DejaVu Sans lacks, other scripts. No emoji sequences (skin tones, ZWJ, flags).
- No shaping beyond kerning (no ligatures, no bidirectional text); glyphs rasterized without hinting, so their edges
  differ by a few levels. An upright fallback font's glyphs stay upright in italic text.
- A text decoration is solid and as thick as its font says, in one colour for all its lines.
- `min()`/`max()` with a percentage inside are not kept; length shifts of `vertical-align` are taken as baseline.
- The static position of an absolutely placed block box among lines is the start of its run, not the next line.
- An inset `box-shadow` is drawn unblurred; dashed and dotted borders are solid on a box with radii.
- Scrollbars, a list's popup, tooltips and form controls left to the browser's look are the library's own drawings,
  not GTK's.
- An absolutely placed box is no part of its containing block's scrollable overflow.
- An event object has every kind's fields where a browser's has only its own kind's.

## Not done

- Drag and drop of the page's own elements, touch, input methods (text comes composed from the host), `keypress`,
  gotpointercapture and lostpointercapture, type-ahead and `multiple` in lists.
- `innerHTML` with markup, `fetch`, `localStorage`.
- A canvas's partial `clearRect` is recorded but not drawn.
- GL 3's core profile (macOS).
- Text in scripts that need shaping (HarfBuzz).

## Dependencies

QuickJS-ng (MIT), nanovg (zlib) and PUGL (ISC) as submodules, each replaceable by a host's own target
(`QUICKDOM_QUICKJS_TARGET`, `QUICKDOM_NANOVG_TARGET`, `QUICKDOM_PUGL_TARGET`); stb_truetype, stb_image and
stb_image_write (public domain); the system's OpenGL, EGL and X11 (or Windows' opengl32 and friends); the fonts above
(SIL OFL 1.1, DejaVu's licence). No GPL or LGPL code.
