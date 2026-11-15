// Copyright 2026 Ian Karlsson
#include "css/stylesheet.h"

#include "css/properties.h"
#include "css/values.h"

namespace qdom
{

// Firefox's defaults for the tags supported (its html.css and forms.css, the parts that matter).
// Form controls keep the sizes Firefox gives them with its non-native theme.
const char *const USER_AGENT_SHEET = R"css(
html, body, div, header, footer, nav, main, section, article, aside, h1, h2, h3, h4, h5, h6, p, pre, dialog, ul, ol,
li, form, fieldset, figure, blockquote, address, hr, legend, details, summary { display: block; }
li { display: list-item; }
body { margin: 8px; }
h1 { font-size: 2em; font-weight: bold; margin: 0.67em 0; }
h2 { font-size: 1.5em; font-weight: bold; margin: 0.83em 0; }
h3 { font-size: 1.17em; font-weight: bold; margin: 1em 0; }
h4 { font-size: 1em; font-weight: bold; margin: 1.33em 0; }
h5 { font-size: 0.83em; font-weight: bold; margin: 1.67em 0; }
h6 { font-size: 0.67em; font-weight: bold; margin: 2.33em 0; }
p, ul, ol, blockquote, figure { margin: 1em 0; }
ul, ol { padding-left: 40px; }
pre { font-family: monospace; white-space: pre; margin: 1em 0; }
b, strong, th { font-weight: bold; }
i, em { font-style: italic; }
small { font-size: smaller; }
code, kbd, samp, tt { font-family: monospace; }
table { display: table; border-spacing: 2px; }
tr { display: table-row; }
tbody { display: table-row-group; }
thead { display: table-header-group; }
tfoot { display: table-footer-group; }
td, th { display: table-cell; padding: 1px; vertical-align: middle; }
th { text-align: center; }
button, select, input, textarea { display: inline-block; font: 13.3333px sans-serif; letter-spacing: normal;
	word-spacing: normal; text-transform: none; text-indent: 0; text-shadow: none; text-align: initial; }
button, select, input:is([type=button], [type=submit], [type=reset], [type=color], [type=checkbox], [type=radio]) {
	box-sizing: border-box; }
button { padding: 1px 4px; border: 2px outset #777; text-align: center; white-space: nowrap; }
select { padding: 1px 4px; border: 2px inset #777; white-space: nowrap; line-height: normal !important; }
input { padding: 1px 2px; border: 2px inset #777; white-space: nowrap; overflow: clip; }
input[type=range] { border: none; padding: 0; margin: 2px; background: none; }
input:is([type=checkbox], [type=radio]) { width: 14px; height: 14px; padding: 0; border: none; margin: 3px 3px 3px 4px; }
textarea { padding: 2px; border: 2px inset #777; white-space: pre-wrap; overflow: auto; font: 12px monospace;
	vertical-align: text-bottom; }
style, script, head, title, template, option, optgroup, datalist { display: none; }
dialog { position: absolute; left: 0; right: 0; margin: auto; border: 1px solid; padding: 1em; background: #fff;
	color: #000; width: fit-content; height: fit-content; }
dialog:modal { position: fixed; top: 0; bottom: 0; max-width: calc(100% - 6px - 2em);
	max-height: calc(100% - 6px - 2em); overflow: auto; }
dialog:not([open]) { display: none; }
dialog::backdrop { background: rgba(0, 0, 0, 0.1); }
[hidden] { display: none; }
::placeholder { opacity: 0.54; }
svg:not(:root) { overflow: hidden; }
)css";

namespace
{

bool space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

//! The text with its comments taken out (strings kept as they are).
std::string strip_comments(std::string_view text)
{
	std::string out;
	out.reserve(text.size());
	char quote = 0;
	for (size_t i = 0; i < text.size(); i++)
	{
		const char c = text[i];
		if (quote)
		{
			out += c;
			if (c == '\\' && i + 1 < text.size())
				out += text[++i];
			else if (c == quote)
				quote = 0;
			continue;
		}
		if (c == '"' || c == '\'')
		{
			quote = c;
			out += c;
			continue;
		}
		if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
		{
			const size_t end = text.find("*/", i + 2);
			if (end == std::string_view::npos)
				break;
			i = end + 1;
			out += ' ';
			continue;
		}
		out += c;
	}
	return out;
}

//! The index of the bracket that closes the one at i (text[i] == open), or npos.
size_t closing(std::string_view text, size_t i, char open, char close)
{
	int depth = 0;
	char quote = 0;
	for (; i < text.size(); i++)
	{
		const char c = text[i];
		if (quote)
		{
			if (c == '\\')
				i++;
			else if (c == quote)
				quote = 0;
			continue;
		}
		if (c == '"' || c == '\'')
			quote = c;
		else if (c == open)
			depth++;
		else if (c == close && --depth == 0)
			return i;
	}
	return std::string_view::npos;
}

} // namespace

Declarations parse_declarations(std::string_view block)
{
	Declarations out;
	const std::string text = strip_comments(block);
	size_t from = 0;
	int depth = 0;
	char quote = 0;
	auto take = [&](size_t end) {
		const std::string_view d = trim(std::string_view(text).substr(from, end - from));
		from = end + 1;
		const size_t colon = d.find(':');
		if (colon == std::string_view::npos)
			return;
		Declaration decl;
		const std::string_view name = trim(d.substr(0, colon));
		decl.name = name.starts_with("--") ? std::string(name) : lower(name);
		std::string_view value = trim(d.substr(colon + 1));
		// !important at the end
		const size_t bang = value.rfind('!');
		if (bang != std::string_view::npos && lower(trim(value.substr(bang + 1))) == "important")
		{
			decl.important = true;
			value = trim(value.substr(0, bang));
		}
		decl.value = value;
		if (!decl.name.empty())
			out.push_back(std::move(decl));
	};
	for (size_t i = 0; i < text.size(); i++)
	{
		const char c = text[i];
		if (quote)
		{
			if (c == '\\')
				i++;
			else if (c == quote)
				quote = 0;
			continue;
		}
		if (c == '"' || c == '\'')
			quote = c;
		else if (c == '(' || c == '[' || c == '{')
			depth++;
		else if (c == ')' || c == ']' || c == '}')
			depth--;
		else if (c == ';' && depth <= 0)
			take(i);
	}
	take(text.size());
	return out;
}

std::vector<Rule> parse_stylesheet(std::string_view source, Names &names, Origin origin, uint32_t &order)
{
	std::vector<Rule> rules;
	const std::string text = strip_comments(source);
	const std::string_view t = text;
	size_t i = 0;
	while (i < t.size())
	{
		while (i < t.size() && space(t[i]))
			i++;
		if (i >= t.size())
			break;
		if (t[i] == '@')
		{
			// an at-rule: up to its semicolon, or its block
			size_t j = i;
			while (j < t.size() && t[j] != ';' && t[j] != '{')
				j++;
			if (j >= t.size())
				break;
			if (t[j] == ';')
			{
				i = j + 1;
				continue;
			}
			const size_t end = closing(t, j, '{', '}');
			if (end == std::string_view::npos)
				break;
			i = end + 1;
			continue;
		}
		const size_t open = t.find('{', i);
		if (open == std::string_view::npos)
			break;
		const size_t close = closing(t, open, '{', '}');
		const std::string_view head = trim(t.substr(i, open - i));
		const std::string_view body = t.substr(open + 1, (close == std::string_view::npos ? t.size() : close) - open - 1);
		i = close == std::string_view::npos ? t.size() : close + 1;
		std::optional<std::vector<Selector>> selectors = parse_selectors(head, names);
		if (!selectors)
			continue;
		auto decls = std::make_shared<const Declarations>(parse_declarations(body));
		for (Selector &s : *selectors)
			rules.push_back({ std::move(s), decls, origin, order++ });
	}
	return rules;
}

} // namespace qdom
