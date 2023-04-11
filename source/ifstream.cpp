// SPDX-FileCopyrightText: Copyright (c) 2017-2023 Wunkolo
// SPDX-License-Identifier: MIT

#include <sai.hpp>

namespace sai
{
/// ifstream
ifstream::ifstream(const std::filesystem::path& Path) : std::istream(new ifstreambuf())
{
	reinterpret_cast<ifstreambuf*>(rdbuf())->open(Path);
}

void ifstream::open(const std::filesystem::path& Path) const
{
	reinterpret_cast<ifstreambuf*>(rdbuf())->close();
	reinterpret_cast<ifstreambuf*>(rdbuf())->open(Path);
}

bool ifstream::is_open() const
{
	return reinterpret_cast<ifstreambuf*>(rdbuf())->is_open();
}

ifstream::~ifstream()
{
	if( rdbuf() )
	{
		delete rdbuf();
	}
}

} // namespace sai