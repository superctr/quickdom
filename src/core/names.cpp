// Copyright 2026 Ian Karlsson
#include "core/names.h"

namespace qdom
{

Names::Names()
{
	m_strings.emplace_back();
	static const char *const predefined[] = {
#define QDOM_NAME_STRING(x) #x,
		QDOM_NAMES(QDOM_NAME_STRING)
#undef QDOM_NAME_STRING
	};
	for (const char *s : predefined)
	{
		// a trailing underscore keeps a C++ keyword apart: class_ is "class"
		std::string_view v = s;
		if (v.ends_with('_'))
			v.remove_suffix(1);
		m_strings.emplace_back(v);
		m_map.emplace(m_strings.back(), Name(m_strings.size() - 1));
	}
}

Name Names::get(std::string_view s)
{
	auto it = m_map.find(s);
	if (it != m_map.end())
		return it->second;
	m_strings.emplace_back(s);
	const Name n = Name(m_strings.size() - 1);
	m_map.emplace(m_strings.back(), n);
	return n;
}

Name Names::find(std::string_view s) const
{
	auto it = m_map.find(s);
	return it == m_map.end() ? 0 : it->second;
}

} // namespace qdom
