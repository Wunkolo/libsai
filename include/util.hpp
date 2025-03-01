// SPDX-FileCopyrightText: Copyright (c) 2025 Wunkolo
// SPDX-License-Identifier: MIT

#include <bit>
#include <cstdint>

namespace sai
{

template<std::endian Endianness = std::endian::big, std::size_t N>
inline constexpr std::uint32_t Tag(const char (&TagString)[N])
{
	static_assert(N == 5, "Tag must be 4 characters");
	if constexpr( Endianness == std::endian::big )
	{
		return (
			(TagString[3] << 0) | (TagString[2] << 8) | (TagString[1] << 16) | (TagString[0] << 24)
		);
	}
	else
	{
		return (
			(TagString[3] << 24) | (TagString[2] << 16) | (TagString[1] << 8) | (TagString[0] << 0)
		);
	}
}
} // namespace sai