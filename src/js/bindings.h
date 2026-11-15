// Copyright 2026 Ian Karlsson
//! \file
//! Helpers of the JavaScript bindings. Every function the library puts into a context is made with
//! JS_NewCFunctionData and keeps the instance's reference object, so a binding finds its Dom without a global
//! table; the binding itself is a BindingFn numbered in the instance.
#pragma once

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include "dom/dom.h"

namespace qdom
{

//! What an element's style, class list, child lists, computed style and 2D context hold: the node, counted.
struct Holder
{
	Node *node;
	JSValue value;
	int kind = 0;  //!< a node list's: 0 child nodes, 1 child elements
};

}

namespace qdom::js
{

struct Method
{
	const char *name;
	int length;
	BindingFn fn;
};

struct Accessor
{
	const char *name;
	BindingFn get;
	BindingFn set;  //!< null for a read-only property
};

void define(Dom &dom, JSValueConst obj, std::initializer_list<Method> methods);
void define(Dom &dom, JSValueConst obj, std::initializer_list<Accessor> accessors);
//! A constructor function (callable with new) whose prototype is proto.
JSValue constructor(Dom &dom, const char *name, int length, BindingFn fn, JSValueConst proto);
//! A global set (the reference is taken).
void set_global(Dom &dom, const char *name, JSValue value);

std::string to_string(JSContext *ctx, JSValueConst v);
//! A string, or none for undefined and null.
std::optional<std::string> to_optional_string(JSContext *ctx, JSValueConst v);
double to_number(JSContext *ctx, JSValueConst v, double fallback = 0);
JSValue str(JSContext *ctx, std::string_view s);
JSValue node_value(JSContext *ctx, const Node *n);

JSValue throw_dom(JSContext *ctx, const char *message);
JSValue illegal(JSContext *ctx);

//! A canvas's 2D context, made on first use; null for another kind or another element.
JSValue get_context(Dom &dom, JSContext *ctx, Element *el, const std::string &type);

void install_node(Dom &dom);
void install_window(Dom &dom);
void install_canvas(Dom &dom);

} // namespace qdom::js
