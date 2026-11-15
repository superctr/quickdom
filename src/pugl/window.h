// Copyright 2026 Ian Karlsson
//! \file
//! The window's event function, for the tests: PUGL's events handed to it as the system would.
#pragma once

#include <pugl/pugl.h>

#include "quickdom/quickdom_pugl.h"

namespace qdom::pugl
{

PuglStatus handle_event(qdom_window_t *w, const PuglEvent &e);
//! The PUGL view, for the tests.
PuglView *view_of(qdom_window_t *w);

} // namespace qdom::pugl
