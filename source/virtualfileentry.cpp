// SPDX-FileCopyrightText: Copyright (c) 2017-2023 Wunkolo
// SPDX-License-Identifier: MIT

#include <sai.hpp>

#include <cstring>

namespace sai
{
VirtualFileEntry::VirtualFileEntry(std::weak_ptr<ifstream> Stream, const FATEntry& EntryData)
	: FATData(EntryData), FileStream(Stream)
{
	Offset     = 0;
	PageIndex  = EntryData.PageIndex;
	PageOffset = 0;
}

VirtualFileEntry::~VirtualFileEntry()
{
}

VirtualPage VirtualFileEntry::GetTablePage(std::size_t Index) const
{
	VirtualPage TablePage = {};
	if( std::shared_ptr<ifstream> Stream = FileStream.lock() )
	{
		Stream->seekg(VirtualPage::NearestTableIndex(Index) * VirtualPage::PageSize, std::ios::beg);
		Stream->read(reinterpret_cast<char*>(TablePage.u8.data()), VirtualPage::PageSize);
	}
	return TablePage;
}

const char* VirtualFileEntry::GetName() const
{
	return FATData.Name;
}

FATEntry::EntryType VirtualFileEntry::GetType() const
{
	return FATData.Type;
}

std::time_t VirtualFileEntry::GetTimeStamp() const
{
	return FATData.TimeStamp / 10000000ULL - 11644473600ULL;
}

std::size_t VirtualFileEntry::GetSize() const
{
	return static_cast<std::size_t>(FATData.Size);
}

std::size_t VirtualFileEntry::GetPageIndex() const
{
	return static_cast<std::size_t>(FATData.PageIndex);
}

std::size_t VirtualFileEntry::Tell() const
{
	return Offset;
}

void VirtualFileEntry::Seek(std::size_t NewOffset)
{
	if( std::shared_ptr<ifstream> Stream = FileStream.lock() )
	{
		if( NewOffset >= FATData.Size )
		{
			// Invalid offset
			return;
		}
		Offset     = NewOffset;
		PageOffset = NewOffset % VirtualPage::PageSize;
		PageIndex  = FATData.PageIndex;
		for( std::size_t i = 0; i < NewOffset / VirtualPage::PageSize; ++i )
		{
			// Get the next page index in the page-chain
			const std::uint32_t NextPageIndex = GetTablePage(PageIndex)
													.PageEntries[PageIndex % VirtualPage::TableSpan]
													.NextPageIndex;
			if( NextPageIndex )
			{
				PageIndex = NextPageIndex;
			}
			else
			{
				break;
			}
		}
	}
}

std::size_t VirtualFileEntry::Read(std::span<std::byte> Destination)
{
	std::size_t LeftToRead    = Destination.size();
	bool        NeedsNextPage = false;

	if( std::shared_ptr<ifstream> Stream = FileStream.lock() )
	{
		std::unique_ptr<std::byte[]> ReadBuffer
			= std::make_unique<std::byte[]>(VirtualPage::PageSize);

		while( Stream )
		{
			Stream->seekg(PageIndex * VirtualPage::PageSize + PageOffset, std::ios::beg);

			std::size_t Read;
			if( LeftToRead + PageOffset >= VirtualPage::PageSize )
			{
				Read          = VirtualPage::PageSize - PageOffset;
				PageOffset    = 0;
				NeedsNextPage = true;
			}
			else
			{
				Read = LeftToRead;
				PageOffset += Read;
			}

			Stream->read(reinterpret_cast<char*>(ReadBuffer.get()), Read);
			const std::size_t BytesWritten = Destination.size() - LeftToRead;
			std::memcpy(Destination.data() + BytesWritten, ReadBuffer.get(), Read);

			Offset += Read;
			LeftToRead -= Read;

			if( NeedsNextPage )
			{
				// NOTE: The reason this is here, instead of moving it into the
				// `if (LeftToRead...)` is because `GetTablePage` seeks the
				// stream which mess-ups its position.
				PageIndex = GetTablePage(PageIndex)
								.PageEntries[PageIndex % VirtualPage::TableSpan]
								.NextPageIndex;
				NeedsNextPage = false;
			}
			else if( LeftToRead == 0 )
				break;
		}
	}

	return Destination.size() - LeftToRead;
}
} // namespace sai