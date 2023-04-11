// SPDX-FileCopyrightText: Copyright (c) 2017-2023 Wunkolo
// SPDX-License-Identifier: MIT

#include <sai.hpp>

#include <cstring>

namespace sai
{
VirtualFileSystem::VirtualFileSystem(const std::filesystem::path& Path)
	: FileStream(std::make_shared<ifstream>(Path))
{
}

VirtualFileSystem::~VirtualFileSystem()
{
}

bool VirtualFileSystem::IsOpen() const
{
	return FileStream->is_open();
}

bool VirtualFileSystem::Exists(const char* Path)
{
	return static_cast<bool>(GetEntry(Path));
}

std::unique_ptr<VirtualFileEntry> VirtualFileSystem::GetEntry(const char* Path)
{
	VirtualPage CurPage = {};
	Read(2 * VirtualPage::PageSize, CurPage);

	std::string CurPath(Path);
	const char* PathDelim    = "./";
	const char* CurToken     = std::strtok(&CurPath[0], PathDelim);
	std::size_t CurEntry     = 0;
	std::size_t CurPageIndex = 0;

	while( CurEntry < 64 && CurPage.FATEntries[CurEntry].Flags && CurToken )
	{
		if( std::strcmp(CurToken, CurPage.FATEntries[CurEntry].Name) == 0 )
		{
			// Match
			if( (CurToken = std::strtok(nullptr, PathDelim)) == nullptr )
			{
				// No more tokens, done
				std::unique_ptr<VirtualFileEntry> Entry
					= std::make_unique<VirtualFileEntry>(FileStream, CurPage.FATEntries[CurEntry]);
				return Entry;
			}
			// Try to go further
			if( CurPage.FATEntries[CurEntry].Type != FATEntry::EntryType::Folder )
			{
				// Part of the path was not a folder, cant go further
				return nullptr;
			}

			const std::uint32_t PageIndex = CurPage.FATEntries[CurEntry].PageIndex;
			Read(PageIndex * VirtualPage::PageSize, CurPage);
			CurEntry     = 0;
			CurPageIndex = PageIndex;
			continue;
		}

		// Last entry ( of this Page ), check if there are more after this.
		if( CurEntry == 63 && CurPageIndex )
		{
			// If a folder has more than 64 `FATEntries`, the `NextPageIndex`
			// field on the `PageEntry` will indicate on what `Page` the extra
			// entries are located.
			VirtualPage TablePage = {};
			Read(VirtualPage::NearestTableIndex(CurPageIndex) * VirtualPage::PageSize, TablePage);

			const std::uint32_t NextPageIndex = TablePage.PageEntries[CurPageIndex].NextPageIndex;
			if( NextPageIndex )
			{
				Read(NextPageIndex * VirtualPage::PageSize, CurPage);
				CurEntry     = 0;
				CurPageIndex = NextPageIndex;
				continue;
			}
		}

		CurEntry++;
	}

	return nullptr;
}

std::size_t VirtualFileSystem::Read(std::size_t Offset, std::span<std::byte> Destination) const
{
	FileStream->seekg(Offset, std::ios::beg);
	FileStream->read(reinterpret_cast<char*>(Destination.data()), Destination.size());
	return Destination.size();
}

void VirtualFileSystem::IterateFileSystem(VirtualFileVisitor& Visitor)
{
	IterateFATBlock(2, Visitor);
}

void VirtualFileSystem::IterateFATBlock(std::size_t PageIndex, VirtualFileVisitor& Visitor)
{
	VirtualPage CurPage = {};
	Read(PageIndex * VirtualPage::PageSize, CurPage);

	for( const FATEntry& CurFATEntry : CurPage.FATEntries )
	{
		if( !CurFATEntry.Flags )
		{
			break;
		}

		VirtualFileEntry CurEntry(FileStream, CurFATEntry);
		switch( CurEntry.GetType() )
		{
		case FATEntry::EntryType::File:
		{
			Visitor.VisitFile(CurEntry);
			break;
		}
		case FATEntry::EntryType::Folder:
		{
			Visitor.VisitFolderBegin(CurEntry);
			IterateFATBlock(CurEntry.GetPageIndex(), Visitor);
			Visitor.VisitFolderEnd(CurEntry);
			break;
		}
		}
	}

	VirtualPage TablePage = {};
	Read(VirtualPage::NearestTableIndex(PageIndex) * VirtualPage::PageSize, TablePage);

	if( TablePage.PageEntries[PageIndex % VirtualPage::TableSpan].NextPageIndex )
	{
		IterateFATBlock(
			TablePage.PageEntries[PageIndex % VirtualPage::TableSpan].NextPageIndex, Visitor
		);
	}
}
} // namespace sai