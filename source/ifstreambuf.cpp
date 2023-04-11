// SPDX-FileCopyrightText: Copyright (c) 2017-2023 Wunkolo
// SPDX-License-Identifier: MIT

#include <sai.hpp>

#include <cstring>

namespace sai
{
ifstreambuf::ifstreambuf(std::span<const std::uint32_t, 256> DecryptionKey)
	: Key(DecryptionKey), CurrentPage(-1), PageCache(nullptr), PageCacheIndex(-1),
	  TableCache(nullptr), TableCacheIndex(-1), PageCount(0)
{
	setg(nullptr, nullptr, nullptr);
	setp(nullptr, nullptr);

	PageCache  = std::make_unique<VirtualPage>();
	TableCache = std::make_unique<VirtualPage>();
}

ifstreambuf* ifstreambuf::open(const std::filesystem::path& Path)
{
	if( is_open() == true )
	{
		return nullptr;
	}

	FileIn.open(Path, std::ios::binary | std::ios::ate);

	if( FileIn.is_open() == false )
	{
		close();
		return nullptr;
	}

	const std::ifstream::pos_type FileSize = FileIn.tellg();

	if( FileSize % VirtualPage::PageSize != 0 )
	{
		// File size is not pagealigned
		close();
		return nullptr;
	}

	PageCount = static_cast<std::uint32_t>(FileSize) / VirtualPage::PageSize;

	seekpos(0);

	return this;
}

ifstreambuf* ifstreambuf::close()
{
	if( FileIn.is_open() )
	{
		FileIn.close();
		return this;
	}
	return nullptr;
}

bool ifstreambuf::is_open() const
{
	return FileIn.is_open();
}

std::streambuf::int_type ifstreambuf::underflow()
{
	if( FileIn.eof() )
	{
		return traits_type::eof();
	}

	if( gptr() == egptr() )
	{
		// buffer depleated, get next block
		if( seekpos((CurrentPage + 1) * VirtualPage::PageSize)
			== std::streampos(std::streamoff(-1)) )
		{
			// Seek position error
			return traits_type::eof();
		}
	}

	return traits_type::to_int_type(*gptr());
}

std::streambuf::pos_type ifstreambuf::seekoff(
	std::streambuf::off_type Offset, std::ios::seekdir Direction, std::ios::openmode /*Mode*/
)
{
	std::streambuf::pos_type Position;

	if( Direction == std::ios::beg )
	{
		Position = Offset;
	}
	else if( Direction == std::ios::cur )
	{
		Position = (CurrentPage * VirtualPage::PageSize); // Current Page
		Position += (gptr() - egptr());                   // Offset within page
		Position += Offset;
	}
	else if( Direction == std::ios::end )
	{
		Position = (PageCount * VirtualPage::PageSize) + Offset;
	}

	return seekpos(Position);
}

std::streambuf::pos_type
	ifstreambuf::seekpos(std::streambuf::pos_type Position, std::ios::openmode Mode)
{
	if( Mode & std::ios::in )
	{
		CurrentPage = static_cast<std::uint32_t>(Position) / VirtualPage::PageSize;

		if( CurrentPage < PageCount )
		{
			if( FetchPage(CurrentPage, &Buffer) )
			{
				setg(
					reinterpret_cast<char*>(Buffer.u8),
					reinterpret_cast<char*>(Buffer.u8) + (Position % VirtualPage::PageSize),
					reinterpret_cast<char*>(Buffer.u8) + VirtualPage::PageSize
				);
				return true;
			}
		}
	}
	setg(nullptr, nullptr, nullptr);
	return std::streampos(std::streamoff(-1));
}

bool ifstreambuf::FetchPage(std::uint32_t PageIndex, VirtualPage* Dest)
{
	if( FileIn.fail() )
	{
		return false;
	}

	if( PageIndex % VirtualPage::TableSpan == 0 ) // Table Block
	{
		if( PageIndex == TableCacheIndex )
		{
			// Cache Hit
			if( Dest != nullptr )
			{
				std::memcpy(Dest, TableCache.get(), VirtualPage::PageSize);
			}
			return true;
		}
		// Cache Miss
		// Get table cache
		FileIn.seekg(PageIndex * VirtualPage::PageSize, std::ios::beg);
		FileIn.read(reinterpret_cast<char*>(TableCache.get()), VirtualPage::PageSize);
		if( FileIn.fail() )
		{
			return false;
		}
		TableCache.get()->DecryptTable(PageIndex);
		TableCacheIndex = PageIndex;
		if( Dest != nullptr )
		{
			std::memcpy(Dest, TableCache.get(), VirtualPage::PageSize);
		}
	}
	else // Data Block
	{
		if( PageIndex == PageCacheIndex )
		{
			// Cache Hit
			if( Dest != nullptr )
			{
				std::memcpy(Dest, PageCache.get(), VirtualPage::PageSize);
			}
			return true;
		}
		// Prefetch nearest table
		// Ensure it is in the cache
		const std::uint32_t NearestTable
			= (PageIndex / VirtualPage::TableSpan) * VirtualPage::TableSpan;

		if( FetchPage(NearestTable, nullptr) == false )
		{
			// Failed to fetch table
			return false;
		}
		FileIn.seekg(PageIndex * VirtualPage::PageSize, std::ios::beg);
		FileIn.read(reinterpret_cast<char*>(PageCache.get()), VirtualPage::PageSize);
		if( FileIn.fail() )
		{
			return false;
		}
		PageCache.get()->DecryptData(
			TableCache.get()->PageEntries[PageIndex % VirtualPage::TableSpan].Checksum
		);

		if( PageCache.get()->Checksum()
			!= TableCache.get()->PageEntries[PageIndex % VirtualPage::TableSpan].Checksum )
		{
			// Checksum mismatch, file corrupt
			return false;
		}

		PageCacheIndex = PageIndex;
		if( Dest != nullptr )
		{
			std::memcpy(Dest, PageCache.get(), VirtualPage::PageSize);
		}
	}
	return true;
}
} // namespace sai