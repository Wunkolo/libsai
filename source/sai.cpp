// SPDX-FileCopyrightText: Copyright (c) 2017-2023 Wunkolo
// SPDX-License-Identifier: MIT

#include <sai.hpp>

#include <algorithm>
#include <codecvt>
#include <cstring>
#include <fstream>
#include <locale>

#ifdef __x86_64__
#include <immintrin.h>
#endif

namespace sai
{
/// VirtualPage
#if defined(__AVX2__)
inline __m256i KeySum8(__m256i Vector8, const std::uint32_t Key[256])
{
	__m256i Sum = _mm256_i32gather_epi32(
		(const std::int32_t*)Key, _mm256_and_si256(Vector8, _mm256_set1_epi32(0xFF)),
		sizeof(std::uint32_t)
	);

	Sum = _mm256_add_epi32(
		Sum, _mm256_i32gather_epi32(
				 (const std::int32_t*)Key,
				 _mm256_and_si256(_mm256_srli_epi32(Vector8, 8), _mm256_set1_epi32(0xFF)),
				 sizeof(std::uint32_t)
			 )
	);
	Sum = _mm256_add_epi32(
		Sum, _mm256_i32gather_epi32(
				 (const std::int32_t*)Key,
				 _mm256_and_si256(_mm256_srli_epi32(Vector8, 16), _mm256_set1_epi32(0xFF)),
				 sizeof(std::uint32_t)
			 )
	);
	Sum = _mm256_add_epi32(
		Sum, _mm256_i32gather_epi32(
				 (const std::int32_t*)Key, _mm256_srli_epi32(Vector8, 24), sizeof(std::uint32_t)
			 )
	);
	return Sum;
}
#endif

void VirtualPage::DecryptTable(std::uint32_t PageIndex)
{
	std::uint32_t PrevData = PageIndex & (~0x1FF);
#if defined(__AVX2__)
	__m256i PrevData8 = _mm256_set1_epi32(PrevData);
	for( std::size_t i = 0; i < (PageSize / sizeof(std::uint32_t)); i += 8 )
	{
		const __m256i CurData8 = _mm256_loadu_si256((__m256i*)(u32 + i));
		// There is no true _mm_alignr_epi8 for AVX2
		// An extra _mm256_permute2x128_si256 is needed
		PrevData8 = _mm256_alignr_epi8(
			CurData8, _mm256_permute2x128_si256(PrevData8, CurData8, _MM_SHUFFLE(0, 2, 0, 1)),
			sizeof(std::uint32_t) * 3
		);
		__m256i CurPlain8 = _mm256_xor_si256(
			_mm256_xor_si256(CurData8, PrevData8), KeySum8(PrevData8, Keys::User)
		);
		CurPlain8 = _mm256_shuffle_epi8(
			CurPlain8, _mm256_set_epi8(
						   13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2, 13, 12, 15, 14, 9,
						   8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2
					   )
		);
		_mm256_storeu_si256((__m256i*)(u32 + i), CurPlain8);
		PrevData8 = CurData8;
	};
#else
	for( std::size_t i = 0; i < (PageSize / sizeof(std::uint32_t)); i++ )
	{
		const std::uint32_t CurData = u32[i];
		std::uint32_t       X       = PrevData ^ CurData;
		X
			^= (Keys::User[(PrevData >> 24) & 0xFF] + Keys::User[(PrevData >> 16) & 0xFF]
				+ Keys::User[(PrevData >> 8) & 0xFF] + Keys::User[(PrevData >> 0) & 0xFF]);
		u32[i]   = static_cast<std::uint32_t>((X << 16) | (X >> 16));
		PrevData = CurData;
	};
#endif
}

void VirtualPage::DecryptData(std::uint32_t PageChecksum)
{
	std::uint32_t PrevData = PageChecksum;
#if defined(__AVX2__)
	__m256i PrevData8 = _mm256_set1_epi32(PrevData);
	for( std::size_t i = 0; i < (PageSize / sizeof(std::uint32_t)); i += 8 )
	{
		const __m256i CurData8 = _mm256_loadu_si256((__m256i*)(u32 + i));
		// There is no true _mm_alignr_epi8 for AVX2
		// An extra _mm256_permute2x128_si256 is needed
		PrevData8 = _mm256_alignr_epi8(
			CurData8, _mm256_permute2x128_si256(PrevData8, CurData8, _MM_SHUFFLE(0, 2, 0, 1)),
			sizeof(std::uint32_t) * 3
		);
		__m256i CurPlain8 = _mm256_sub_epi32(
			CurData8, _mm256_xor_si256(PrevData8, KeySum8(PrevData8, Keys::User))
		);
		_mm256_storeu_si256((__m256i*)(u32 + i), CurPlain8);
		PrevData8 = CurData8;
	};
#else
	for( std::size_t i = 0; i < (PageSize / sizeof(std::uint32_t)); i++ )
	{
		const std::uint32_t CurData = u32[i];
		u32[i]                      = CurData
			   - (PrevData
				  ^ (Keys::User[(PrevData >> 24) & 0xFF] + Keys::User[(PrevData >> 16) & 0xFF]
					 + Keys::User[(PrevData >> 8) & 0xFF] + Keys::User[(PrevData >> 0) & 0xFF]));
		PrevData = CurData;
	}
#endif
}

std::uint32_t VirtualPage::Checksum()
{
	std::uint32_t Sum = 0;
	for( std::size_t i = 0; i < (PageSize / sizeof(std::uint32_t)); i++ )
	{
		Sum = (2 * Sum | (Sum >> 31)) ^ u32[i];
	}
	return Sum | 1;
}

/// ifstreambuf
ifstreambuf::ifstreambuf(std::span<const std::uint32_t, 256> DecryptionKey)
	: Key(DecryptionKey), CurrentPage(-1), PageCache(nullptr), PageCacheIndex(-1),
	  TableCache(nullptr), TableCacheIndex(-1), PageCount(0)
{
	setg(nullptr, nullptr, nullptr);
	setp(nullptr, nullptr);

	PageCache  = std::make_unique<VirtualPage>();
	TableCache = std::make_unique<VirtualPage>();
}

ifstreambuf* ifstreambuf::open(const char* Name)
{
	if( is_open() == true )
	{
		return nullptr;
	}

	FileIn.open(Name, std::ios::binary | std::ios::ate);

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

ifstreambuf* ifstreambuf::open(const wchar_t* Name)
{
	if( is_open() == true )
	{
		return nullptr;
	}

#if defined(_MSC_VER)
	FileIn.open(Name, std::ios::binary | std::ios::ate);
#else
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> Converter;
	std::string Name8 = Converter.to_bytes(std::wstring(Name));
	FileIn.open(Name8, std::ios::binary | std::ios::ate);
#endif

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

VirtualFileVisitor::~VirtualFileVisitor()
{
}

bool VirtualFileVisitor::VisitFolderBegin(VirtualFileEntry& /*Entry*/)
{
	return true;
}

bool VirtualFileVisitor::VisitFolderEnd(VirtualFileEntry& /*Entry*/)
{
	return true;
}

bool VirtualFileVisitor::VisitFile(VirtualFileEntry& /*Entry*/)
{
	return true;
}

/// Virtual File System

VirtualFileSystem::VirtualFileSystem(const char* FileName)
	: FileStream(std::make_shared<ifstream>(FileName))
{
}

VirtualFileSystem::VirtualFileSystem(const wchar_t* FileName)
	: FileStream(std::make_shared<ifstream>(FileName))
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

std::size_t
	VirtualFileSystem::Read(std::size_t Offset, std::byte* Destination, std::size_t Size) const
{
	FileStream->seekg(Offset, std::ios::beg);
	FileStream->read(reinterpret_cast<char*>(Destination), Size);
	return Size;
}

void VirtualFileSystem::IterateFileSystem(VirtualFileVisitor& Visitor)
{
	IterateFATBlock(2, Visitor);
}

void VirtualFileSystem::IterateFATBlock(std::size_t PageIndex, VirtualFileVisitor& Visitor)
{
	VirtualPage CurPage = {};
	Read(PageIndex * VirtualPage::PageSize, CurPage);

	for( std::size_t i = 0;
		 i < std::extent<decltype(VirtualPage::FATEntries)>::value && CurPage.FATEntries[i].Flags;
		 i++ )
	{
		VirtualFileEntry CurEntry(FileStream, CurPage.FATEntries[i]);
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

/// VirtualFileEntry
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
		Stream->read(reinterpret_cast<char*>(TablePage.u8), VirtualPage::PageSize);
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

std::size_t VirtualFileEntry::Read(std::byte* Destination, std::size_t Size)
{
	std::size_t LeftToRead    = Size;
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
			std::size_t BytesWrote = Size - LeftToRead;
			std::memcpy(Destination + BytesWrote, ReadBuffer.get(), Read);

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

	return Size - LeftToRead;
}

/// SaiDocument
Document::Document(const char* FileName) : VirtualFileSystem(FileName)
{
}

Document::Document(const wchar_t* FileName) : VirtualFileSystem(FileName)
{
}

Document::~Document()
{
}

std::tuple<std::uint32_t, std::uint32_t> Document::GetCanvasSize()
{
	if( std::unique_ptr<VirtualFileEntry> Canvas = GetEntry("canvas") )
	{
		std::uint32_t Alignment; // Always seems to be 0x10, bpc? Alignment?
		std::uint32_t Width, Height;

		Canvas->Read(Alignment);
		Canvas->Read(Width);
		Canvas->Read(Height);
		return std::make_tuple(Width, Height);
	}
	return std::make_tuple(0, 0);
}

std::tuple<std::unique_ptr<std::byte[]>, std::uint32_t, std::uint32_t> Document::GetThumbnail()
{
	if( std::unique_ptr<VirtualFileEntry> Thumbnail = GetEntry("thumbnail") )
	{
		ThumbnailHeader Header;
		Thumbnail->Read(Header.Width);
		Thumbnail->Read(Header.Height);
		Thumbnail->Read(Header.Magic);

		if( Header.Magic != sai::Tag("BM32") )
		{
			return std::make_tuple(nullptr, 0, 0);
		}

		const std::size_t            PixelCount = Header.Height * Header.Width;
		std::unique_ptr<std::byte[]> Pixels
			= std::make_unique<std::byte[]>(PixelCount * sizeof(std::uint32_t));

		Thumbnail->Read(Pixels.get(), PixelCount * sizeof(std::uint32_t));

		//// BGRA to RGBA
		// std::size_t i = 0;

		//// Simd speedup, four pixels at a time
		// while( i < ((PixelCount * sizeof(std::uint32_t)) & ~0xF) )
		//{
		//	const __m128i Swizzle =
		//		_mm_set_epi8(
		//			15, 12, 13, 14,
		//			11, 8, 9, 10,
		//			7, 4, 5, 6,
		//			3, 0, 1, 2
		//		);

		//	__m128i QuadPixel = _mm_loadu_si128(
		//		reinterpret_cast<__m128i*>(&Pixels[i])
		//	);

		//	QuadPixel = _mm_shuffle_epi8(QuadPixel, Swizzle);

		//	_mm_store_si128(
		//		reinterpret_cast<__m128i*>(&Pixels[i]),
		//		QuadPixel
		//	);

		//	i += (sizeof(std::uint32_t) * 4);
		//}

		// for( ; i < PixelCount * sizeof(std::uint32_t); i +=
		// sizeof(std::uint32_t) )
		//{
		//	std::swap<std::byte>(Pixels[i], Pixels[i + 2]);
		// }

		return std::make_tuple(std::move(Pixels), Header.Width, Header.Height);
	}
	return std::make_tuple(nullptr, 0, 0);
}

void Document::IterateLayerFiles(const std::function<bool(VirtualFileEntry&)>& LayerProc)
{
	if( auto LayerTableFile = GetEntry("laytbl") )
	{
		std::uint32_t LayerCount = LayerTableFile->Read<std::uint32_t>();
		while( LayerCount-- ) // Read each layer entry
		{
			const LayerTableEntry CurLayerEntry = LayerTableFile->Read<LayerTableEntry>();
			char                  LayerPath[32] = {};
			std::snprintf(LayerPath, 32u, "/layers/%08x", CurLayerEntry.Identifier);
			if( auto LayerFile = GetEntry(LayerPath) )
			{
				if( !LayerProc(*LayerFile) )
					break;
			}
		}
	}
}

void Document::IterateSubLayerFiles(const std::function<bool(VirtualFileEntry&)>& SubLayerProc)
{
	if( auto SubLayerTableFile = GetEntry("subtbl") )
	{
		std::uint32_t SubLayerCount = SubLayerTableFile->Read<std::uint32_t>();
		while( SubLayerCount-- ) // Read each layer entry
		{
			const LayerTableEntry CurSubLayerEntry = SubLayerTableFile->Read<LayerTableEntry>();
			char                  SubLayerPath[32] = {};
			std::snprintf(SubLayerPath, 32u, "/sublayers/%08x", CurSubLayerEntry.Identifier);
			if( auto SubLayerFile = GetEntry(SubLayerPath) )
			{
				if( !SubLayerProc(*SubLayerFile) )
					break;
			}
		}
	}
}

} // namespace sai
