// Copyright 2026 Ian Karlsson
#include "css/selectors.h"

#include <algorithm>
#include <cstring>

#include "css/values.h"
#include "dom/node.h"

namespace qdom
{

namespace
{

bool ident_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
		|| (unsigned char)c >= 0x80;
}

bool space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

struct Specificity
{
	uint32_t a = 0, b = 0, c = 0;

	uint32_t packed() const { return std::min(a, 1023u) << 20 | std::min(b, 1023u) << 10 | std::min(c, 1023u); }
	static Specificity of(uint32_t p) { return { p >> 20, (p >> 10) & 1023, p & 1023 }; }
};

//! an+b from :nth-child()'s argument; false when it is not one
bool parse_nth(std::string_view s, int &a, int &b)
{
	std::string t;
	for (char c : s)
		if (!space(c))
			t += c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
	if (t == "odd")
	{
		a = 2;
		b = 1;
		return true;
	}
	if (t == "even")
	{
		a = 2;
		b = 0;
		return true;
	}
	const size_t npos = t.find('n');
	auto number = [](std::string_view v, int &out) {
		if (v.empty())
			return false;
		size_t i = 0;
		int sign = 1;
		if (v[0] == '+' || v[0] == '-')
		{
			sign = v[0] == '-' ? -1 : 1;
			i = 1;
		}
		if (i >= v.size())
			return false;
		int n = 0;
		for (; i < v.size(); i++)
		{
			if (v[i] < '0' || v[i] > '9')
				return false;
			n = n * 10 + (v[i] - '0');
		}
		out = sign * n;
		return true;
	};
	if (npos == std::string::npos)
	{
		a = 0;
		return number(t, b);
	}
	const std::string_view head = std::string_view(t).substr(0, npos), tail = std::string_view(t).substr(npos + 1);
	if (head.empty() || head == "+")
		a = 1;
	else if (head == "-")
		a = -1;
	else if (!number(head, a))
		return false;
	b = 0;
	return tail.empty() || number(tail, b);
}

class Parser
{
public:
	Parser(Names &names) : m_names(names) {}

	std::optional<std::vector<Selector>> list(std::string_view text)
	{
		std::vector<Selector> out;
		for (std::string_view part : split_commas(text))
		{
			if (part.empty())
				return std::nullopt;
			std::optional<Selector> s = selector(part);
			if (!s)
				return std::nullopt;
			out.push_back(std::move(*s));
		}
		return out;
	}

private:
	Names &m_names;

	std::optional<Selector> selector(std::string_view text)
	{
		// the compounds left to right, each with the combinator before it
		std::vector<std::pair<std::string_view, char>> parts;
		int depth = 0;
		char quote = 0, comb = 0;
		size_t from = std::string_view::npos;
		auto push = [&](size_t end) {
			if (from != std::string_view::npos)
			{
				parts.emplace_back(text.substr(from, end - from), parts.empty() ? 0 : (comb ? comb : ' '));
				comb = 0;
			}
			from = std::string_view::npos;
		};
		for (size_t i = 0; i < text.size(); i++)
		{
			const char c = text[i];
			if (quote)
			{
				if (c == quote)
					quote = 0;
				continue;
			}
			if (c == '"' || c == '\'')
				quote = c;
			else if (c == '(' || c == '[')
				depth++;
			else if (c == ')' || c == ']')
				depth--;
			if (!depth && !quote && (c == '>' || c == '+' || c == '~'))
			{
				push(i);
				if (parts.empty())
					return std::nullopt;
				comb = c;
				continue;
			}
			if (!depth && !quote && space(c))
			{
				push(i);
				continue;
			}
			if (from == std::string_view::npos)
				from = i;
		}
		push(text.size());
		if (parts.empty() || comb)
			return std::nullopt;
		Selector s;
		Specificity spec;
		for (size_t i = 0; i < parts.size(); i++)
		{
			Compound c;
			Name element = 0;
			if (!compound(parts[i].first, c, spec, element, s))
				return std::nullopt;
			// a pseudo-element ends the selector
			if (element && i + 1 != parts.size())
				return std::nullopt;
			if (element)
				s.pseudo_element = element;
			c.combinator = parts[i].second;
			if (c.combinator == '+' || c.combinator == '~')
				s.reads_siblings = true;
			s.parts.push_back(std::move(c));
		}
		// right to left, each part keeping how it joins the part to its left
		std::reverse(s.parts.begin(), s.parts.end());
		s.specificity = spec.packed();
		return s;
	}

	std::string_view ident(std::string_view s, size_t &i)
	{
		const size_t from = i;
		while (i < s.size() && ident_char(s[i]))
			i++;
		return s.substr(from, i - from);
	}

	//! the text inside the brackets starting at i (s[i] the opening one); i after the closing one
	std::string_view inside(std::string_view s, size_t &i, char open, char close)
	{
		int depth = 1;
		size_t j = i + 1;
		char quote = 0;
		while (j < s.size() && depth)
		{
			const char c = s[j];
			if (quote)
			{
				if (c == quote)
					quote = 0;
			}
			else if (c == '"' || c == '\'')
				quote = c;
			else if (c == open)
				depth++;
			else if (c == close)
				depth--;
			j++;
		}
		const std::string_view in = s.substr(i + 1, (depth ? j : j - 1) - i - 1);
		i = j;
		return in;
	}

	bool compound(std::string_view s, Compound &c, Specificity &spec, Name &element, Selector &sel)
	{
		size_t i = 0;
		if (s.empty())
			return false;
		while (i < s.size())
		{
			const char ch = s[i];
			if (ch == '*')
			{
				i++;
				continue;
			}
			if (ch == '#')
			{
				i++;
				const std::string_view v = ident(s, i);
				if (v.empty())
					return false;
				c.id = m_names.get(v);
				spec.a++;
				continue;
			}
			if (ch == '.')
			{
				i++;
				const std::string_view v = ident(s, i);
				if (v.empty())
					return false;
				c.classes.push_back(m_names.get(v));
				spec.b++;
				continue;
			}
			if (ch == '[')
			{
				if (!attribute(inside(s, i, '[', ']'), c))
					return false;
				spec.b++;
				continue;
			}
			if (ch == ':')
			{
				const bool two = i + 1 < s.size() && s[i + 1] == ':';
				i += two ? 2 : 1;
				const std::string name = lower(ident(s, i));
				std::string_view arg;
				bool has_arg = false;
				if (i < s.size() && s[i] == '(')
				{
					arg = inside(s, i, '(', ')');
					has_arg = true;
				}
				if (two || name == "before" || name == "after")
				{
					if (element || has_arg)
						return false;
					if (name == "after" || name == "before" || name == "backdrop" || name == "placeholder")
						element = m_names.get(name);
					else
						return false;
					spec.c++;
					continue;
				}
				if (!pseudo(name, has_arg, arg, c, spec, sel))
					return false;
				continue;
			}
			if (ident_char(ch))
			{
				if (i != 0)
					return false;
				c.tag = m_names.get(lower(ident(s, i)));
				spec.c++;
				continue;
			}
			return false;
		}
		return true;
	}

	bool attribute(std::string_view body, Compound &c)
	{
		body = trim(body);
		size_t i = 0;
		while (i < body.size() && ident_char(body[i]))
			i++;
		AttributeSelector a;
		if (!i)
			return false;
		a.name = m_names.get(lower(body.substr(0, i)));
		std::string_view rest = trim(body.substr(i));
		if (rest.empty())
		{
			c.attrs.push_back(std::move(a));
			return true;
		}
		static const struct { const char *text; AttributeSelector::Op op; } ops[] = {
			{ "~=", AttributeSelector::Op::Includes }, { "|=", AttributeSelector::Op::DashMatch },
			{ "^=", AttributeSelector::Op::Prefix }, { "$=", AttributeSelector::Op::Suffix },
			{ "*=", AttributeSelector::Op::Substring }, { "=", AttributeSelector::Op::Equals },
		};
		bool found = false;
		for (const auto &o : ops)
			if (rest.starts_with(o.text))
			{
				a.op = o.op;
				rest = trim(rest.substr(strlen(o.text)));
				found = true;
				break;
			}
		if (!found || rest.empty())
			return false;
		if (rest[0] == '"' || rest[0] == '\'')
		{
			const size_t end = rest.find(rest[0], 1);
			if (end == std::string_view::npos)
				return false;
			a.value = rest.substr(1, end - 1);
			rest = trim(rest.substr(end + 1));
		}
		else
		{
			size_t j = 0;
			while (j < rest.size() && !space(rest[j]))
				j++;
			a.value = rest.substr(0, j);
			rest = trim(rest.substr(j));
		}
		if (rest == "i" || rest == "I")
			a.icase = true;
		else if (!rest.empty() && rest != "s" && rest != "S")
			return false;
		c.attrs.push_back(std::move(a));
		return true;
	}

	bool pseudo(const std::string &name, bool has_arg, std::string_view arg, Compound &c, Specificity &spec, Selector &sel)
	{
		if (name == "not" || name == "is" || name == "where" || name == "matches" || name == "-moz-any")
		{
			if (!has_arg)
				return false;
			std::optional<std::vector<Selector>> inner = list(arg);
			if (!inner)
			{
				// :is() and :where() forgive what they do not understand
				if (name == "not")
					return false;
				inner = std::vector<Selector>();
			}
			uint32_t best = 0;
			for (const Selector &s : *inner)
			{
				best = std::max(best, s.specificity);
				sel.reads_siblings = sel.reads_siblings || s.reads_siblings;
				sel.reads_hover = sel.reads_hover || s.reads_hover;
			}
			if (name != "where")
			{
				const Specificity b = Specificity::of(best);
				spec.a += b.a;
				spec.b += b.b;
				spec.c += b.c;
			}
			(name == "not" ? c.nots : c.ises).push_back(std::move(*inner));
			return true;
		}
		static const struct { const char *name; PseudoClass kind; } simple[] = {
			{ "hover", PseudoClass::Hover }, { "active", PseudoClass::Active }, { "focus", PseudoClass::Focus },
			{ "focus-visible", PseudoClass::FocusVisible }, { "focus-within", PseudoClass::FocusWithin },
			{ "disabled", PseudoClass::Disabled }, { "enabled", PseudoClass::Enabled },
			{ "checked", PseudoClass::Checked }, { "modal", PseudoClass::Modal }, { "open", PseudoClass::Open },
			{ "root", PseudoClass::Root }, { "scope", PseudoClass::Scope }, { "first-child", PseudoClass::FirstChild },
			{ "last-child", PseudoClass::LastChild }, { "only-child", PseudoClass::OnlyChild },
			{ "first-of-type", PseudoClass::FirstOfType }, { "last-of-type", PseudoClass::LastOfType },
			{ "empty", PseudoClass::Empty }, { "placeholder-shown", PseudoClass::PlaceholderShown },
		};
		PseudoSelector p { PseudoClass::Hover };
		if (name == "nth-child" || name == "nth-last-child")
		{
			if (!has_arg || !parse_nth(arg, p.a, p.b))
				return false;
			p.kind = name == "nth-child" ? PseudoClass::NthChild : PseudoClass::NthLastChild;
		}
		else
		{
			bool found = false;
			for (const auto &s : simple)
				if (name == s.name)
				{
					p.kind = s.kind;
					found = true;
					break;
				}
			if (!found || has_arg)
				return false;
		}
		if (p.kind == PseudoClass::Hover)
			sel.reads_hover = true;
		c.pseudos.push_back(p);
		spec.b++;
		return true;
	}
};

bool form_control(const Element &el)
{
	return el.ns() == Namespace::Html && (el.is(n::button) || el.is(n::input) || el.is(n::select) || el.is(n::textarea)
		|| el.is(n::option));
}

bool nth(int index, int a, int b)
{
	if (a == 0)
		return index == b;
	const int k = index - b;
	return k % a == 0 && k / a >= 0;
}

bool attribute_matches(const AttributeSelector &a, const Element &el)
{
	const std::string *v = el.attribute(a.name);
	if (!v)
		return false;
	if (a.op == AttributeSelector::Op::Exists)
		return true;
	std::string have = *v, want = a.value;
	if (a.icase)
	{
		have = lower(have);
		want = lower(want);
	}
	switch (a.op)
	{
	case AttributeSelector::Op::Equals:
		return have == want;
	case AttributeSelector::Op::Includes:
		for (std::string_view w : split_words(have))
			if (w == want)
				return true;
		return false;
	case AttributeSelector::Op::DashMatch:
		return have == want || (have.size() > want.size() && have.starts_with(want) && have[want.size()] == '-');
	case AttributeSelector::Op::Prefix:
		return !want.empty() && have.starts_with(want);
	case AttributeSelector::Op::Suffix:
		return !want.empty() && have.ends_with(want);
	case AttributeSelector::Op::Substring:
		return !want.empty() && have.find(want) != std::string::npos;
	default:
		return false;
	}
}

bool pseudo_matches(const PseudoSelector &p, const Element &el, const Element *scope)
{
	switch (p.kind)
	{
	case PseudoClass::Hover:
		return el.hovered;
	case PseudoClass::Active:
		return false;
	case PseudoClass::Focus:
		return el.focused;
	case PseudoClass::FocusVisible:
		return el.focused && el.focus_visible;
	case PseudoClass::FocusWithin:
	{
		const Element *a = el.document()->active();
		return a && a->focused && el.contains(a);
	}
	case PseudoClass::Disabled:
		return form_control(el) && el.disabled();
	case PseudoClass::Enabled:
		return form_control(el) && !el.disabled();
	case PseudoClass::Checked:
		if (el.is(n::option))
			return el.has_attribute(n::selected);
		return el.is(n::input) && el.form.checked;
	case PseudoClass::Modal:
		return el.modal;
	case PseudoClass::Open:
		return el.has_attribute(n::open);
	case PseudoClass::Root:
		return el.parent() && el.parent()->is_document();
	case PseudoClass::Scope:
		return scope ? &el == scope : el.parent() && el.parent()->is_document();
	case PseudoClass::FirstChild:
		return !el.previous_element_sibling();
	case PseudoClass::LastChild:
		return !el.next_element_sibling();
	case PseudoClass::OnlyChild:
		return !el.previous_element_sibling() && !el.next_element_sibling();
	case PseudoClass::FirstOfType:
		for (const Element *s = el.previous_element_sibling(); s; s = s->previous_element_sibling())
			if (s->tag() == el.tag())
				return false;
		return true;
	case PseudoClass::LastOfType:
		for (const Element *s = el.next_element_sibling(); s; s = s->next_element_sibling())
			if (s->tag() == el.tag())
				return false;
		return true;
	case PseudoClass::NthChild:
	case PseudoClass::NthLastChild:
	{
		if (!el.parent() || el.parent()->is_document())
			return false;
		int index = 1;
		if (p.kind == PseudoClass::NthChild)
			for (const Element *s = el.previous_element_sibling(); s; s = s->previous_element_sibling())
				index++;
		else
			for (const Element *s = el.next_element_sibling(); s; s = s->next_element_sibling())
				index++;
		return nth(index, p.a, p.b);
	}
	case PseudoClass::Empty:
		for (const Link &l : el.links())
			if (l.node->is_element() || (l.node->is_text() && !l.node->as_text()->data().empty()))
				return false;
		return true;
	case PseudoClass::PlaceholderShown:
		return (el.is(n::input) || el.is(n::textarea)) && el.has_attribute(n::placeholder) && el.form.value.empty();
	}
	return false;
}

bool compound_matches(const Compound &c, const Element &el, const Element *scope)
{
	if (c.tag && c.tag != el.tag())
		return false;
	if (c.id && c.id != el.id())
		return false;
	for (Name cl : c.classes)
		if (!el.has_class(cl))
			return false;
	for (const AttributeSelector &a : c.attrs)
		if (!attribute_matches(a, el))
			return false;
	for (const PseudoSelector &p : c.pseudos)
		if (!pseudo_matches(p, el, scope))
			return false;
	for (const auto &list : c.nots)
		if (matches_any(list, el, scope))
			return false;
	for (const auto &list : c.ises)
		if (!matches_any(list, el, scope))
			return false;
	return true;
}

bool match_from(const Selector &s, size_t i, const Element &el, const Element *scope)
{
	const Compound &c = s.parts[i];
	if (!compound_matches(c, el, scope))
		return false;
	if (i + 1 == s.parts.size())
		return true;
	switch (c.combinator)
	{
	case '>':
	{
		const Element *p = el.parent_element();
		return p && match_from(s, i + 1, *p, scope);
	}
	case '+':
	{
		const Element *p = el.previous_element_sibling();
		return p && match_from(s, i + 1, *p, scope);
	}
	case '~':
		for (const Element *p = el.previous_element_sibling(); p; p = p->previous_element_sibling())
			if (match_from(s, i + 1, *p, scope))
				return true;
		return false;
	default:
		for (const Element *p = el.parent_element(); p; p = p->parent_element())
			if (match_from(s, i + 1, *p, scope))
				return true;
		return false;
	}
}

} // namespace

std::optional<std::vector<Selector>> parse_selectors(std::string_view text, Names &names)
{
	Parser p(names);
	return p.list(trim(text));
}

bool matches(const Selector &s, const Element &el, const Element *scope)
{
	return !s.parts.empty() && match_from(s, 0, el, scope);
}

bool matches_any(const std::vector<Selector> &list, const Element &el, const Element *scope)
{
	for (const Selector &s : list)
		if (matches(s, el, scope))
			return true;
	return false;
}

} // namespace qdom
