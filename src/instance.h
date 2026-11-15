// Copyright 2026 Ian Karlsson
//! \file
//! The C interface's instance, shared with the GL backend (which takes the images as the host's callback does) and
//! the window.
#pragma once

#include <string>

#include "quickdom/quickdom.h"

#include "dom/dom.h"
#include "paint/commands.h"

struct qdom_instance
{
	qdom::Dom *dom = nullptr;
	qdom_options_t options {};
	qdom::Commands commands;
	std::string texts;
	std::string title;
	//! The GL backend's taker of images, beside the host's image callback.
	void (*sink)(void *ctx, int id, int width, int height, const unsigned char *rgba) = nullptr;
	void *sink_ctx = nullptr;
	//! The window showing the document (quickdom_pugl's), told when the document goes first.
	void (*gone)(void *ctx) = nullptr;
	void *gone_ctx = nullptr;
};
