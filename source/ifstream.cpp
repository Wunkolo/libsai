// SPDX-FileCopyrightText: Copyright (c) 2017-2023 Wunkolo
// SPDX-License-Identifier: MIT

#include <sai.hpp>

namespace sai
{
/// ifstream
ifstream::ifstream(const std::string& Path) : std::istream(new ifstreambuf())
{
	reinterpret_cast<ifstreambuf*>(rdbuf())->open(Path.c_str());
}

ifstream::ifstream(const char* Path) : std::istream(new ifstreambuf())
{
	reinterpret_cast<ifstreambuf*>(rdbuf())->open(Path);
}

ifstream::ifstream(const std::wstring& Path) : std::istream(new ifstreambuf())
{
	reinterpret_cast<ifstreambuf*>(rdbuf())->open(Path.c_str());
}

ifstream::ifstream(const wchar_t* Path) : std::istream(new ifstreambuf())
{
	reinterpret_cast<ifstreambuf*>(rdbuf())->open(Path);
}

void ifstream::open(const char* FilePath) const
{
	reinterpret_cast<ifstreambuf*>(rdbuf())->close();
	reinterpret_cast<ifstreambuf*>(rdbuf())->open(FilePath);
}

void ifstream::open(const std::string& FilePath) const
{
	open(FilePath.c_str());
}

void ifstream::open(const wchar_t* FilePath) const
{
	reinterpret_cast<ifstreambuf*>(rdbuf())->close();
	reinterpret_cast<ifstreambuf*>(rdbuf())->open(FilePath);
}

void ifstream::open(const std::wstring& FilePath) const
{
	open(FilePath.c_str());
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