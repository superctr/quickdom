// Copyright 2026 Ian Karlsson
//! \file
//! Selectors: parsed into compounds right to left, matched against elements, with their specificity.
//! What the stylesheets use: type, id, class and attribute selectors, :hover :focus :disabled :checked :modal
//! :root :scope :first-child :last-child :only-child :nth-child() :nth-last-child() :empty :not() :is() :where(),
//! ::before ::after ::backdrop ::placeholder, and the descendant, child and sibling combinators.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/names.h"

namespace qdom
{

class Element;
struct Selector;

struct AttributeSelector
{
	enum class Op : uint8_t
	{
		Exists, Equals, Includes, DashMatch, Prefix, Suffix, Substring
	};
	Name name = 0;
	Op op = Op::Exists;
	std::string value;
	bool icase = false;
};

enum class PseudoClass : uint8_t
{
	Hover, Active, Focus, FocusVisible, FocusWithin, Disabled, Enabled, Checked, Modal, Open, Root, Scope, FirstChild,
	LastChild, OnlyChild, NthChild, NthLastChild, FirstOfType, LastOfType, Empty, PlaceholderShown,
};

struct PseudoSelector
{
	PseudoClass kind;
	int a = 0, b = 0;  //!< an+b of :nth-child()
};

//! One compound (div.a#b:hover) and how it joins the compound to its left.
struct Compound
{
	Name tag = 0;  //!< 0 for any
	Name id = 0;
	std::vector<Name> classes;
	std::vector<AttributeSelector> attrs;
	std::vector<PseudoSelector> pseudos;
	std::vector<std::vector<Selector>> nots;
	std::vector<std::vector<Selector>> ises;  //!< :is() and :where()
	//! ' ' descendant, '>' child, '+' next sibling, '~' any sibling before; 0 for the leftmost compound
	char combinator = 0;
};

struct Selector
{
	std::vector<Compound> parts;  //!< right to left
	uint32_t specificity = 0;     //!< a << 20 | b << 10 | c
	Name pseudo_element = 0;      //!< after, before, backdrop, placeholder; 0 for the element itself
	//! Whether it reads a sibling before the subject (+ or ~), which a change to a sibling may change.
	bool reads_siblings = false;
	//! Whether it reads whether the pointer is over an element.
	bool reads_hover = false;
};

//! A selector list; none when any selector in it is not understood (a browser drops the rule then).
std::optional<std::vector<Selector>> parse_selectors(std::string_view text, Names &names);

//! Whether a selector matches an element; scope is :scope's element.
bool matches(const Selector &s, const Element &el, const Element *scope = nullptr);
bool matches_any(const std::vector<Selector> &list, const Element &el, const Element *scope = nullptr);

} // namespace qdom
