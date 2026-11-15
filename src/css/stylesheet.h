// Copyright 2026 Ian Karlsson
//! \file
//! Stylesheets parsed into rules: a selector each (a rule with a list is one rule per selector) and the
//! declarations of its block. At-rules are skipped with their blocks.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "css/selectors.h"

namespace qdom
{

struct Declaration
{
	std::string name;   //!< dashed and lower case; a custom property as written
	std::string value;
	bool important = false;
};

using Declarations = std::vector<Declaration>;

//! The origin of a rule: the browser's defaults under the author's.
enum class Origin : uint8_t
{
	UserAgent,
	Author,
};

struct Rule
{
	Selector selector;
	std::shared_ptr<const Declarations> declarations;
	Origin origin = Origin::Author;
	uint32_t order = 0;  //!< its place among every rule of every sheet
};

//! A block's declarations ("a: b; c: d !important").
Declarations parse_declarations(std::string_view block);

//! A stylesheet's rules, numbered on from `order`.
std::vector<Rule> parse_stylesheet(std::string_view text, Names &names, Origin origin, uint32_t &order);

//! The defaults the library gives every page, as Firefox gives them for the tags supported.
extern const char *const USER_AGENT_SHEET;

} // namespace qdom
