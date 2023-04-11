// SPDX-FileCopyrightText: Copyright (c) 2017-2023 Wunkolo
// SPDX-License-Identifier: MIT

#include <sai.hpp>

#ifdef __x86_64__
#include <immintrin.h>
#endif
namespace sai
{

Document::Document(const std::filesystem::path& Path) : VirtualFileSystem(Path)
{
}

Document::~Document()
{
}

std::tuple<std::uint32_t, std::uint32_t> Document::GetCanvasSize()
{
	if( std::optional<VirtualFileEntry> Canvas = GetEntry("canvas"); Canvas )
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
	if( std::optional<VirtualFileEntry> Thumbnail = GetEntry("thumbnail"); Thumbnail )
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

		Thumbnail->Read({Pixels.get(), PixelCount * sizeof(std::uint32_t)});

#if 0
		//// BGRA to RGBA
		std::size_t i = 0;

		//// Simd speedup, four pixels at a time
		while( i < ((PixelCount * sizeof(std::uint32_t)) & ~0xF) )
		{
			const __m128i Swizzle
				= _mm_set_epi8(15, 12, 13, 14, 11, 8, 9, 10, 7, 4, 5, 6, 3, 0, 1, 2);

			__m128i QuadPixel = _mm_loadu_si128(reinterpret_cast<__m128i*>(&Pixels[i]));

			QuadPixel = _mm_shuffle_epi8(QuadPixel, Swizzle);

			_mm_store_si128(reinterpret_cast<__m128i*>(&Pixels[i]), QuadPixel);

			i += (sizeof(std::uint32_t) * 4);
		}

		for( ; i < PixelCount * sizeof(std::uint32_t); i += sizeof(std::uint32_t) )
		{
			std::swap<std::byte>(Pixels[i], Pixels[i + 2]);
		}
#endif

		return std::make_tuple(std::move(Pixels), Header.Width, Header.Height);
	}
	return std::make_tuple(nullptr, 0, 0);
}

void Document::IterateLayerFiles(const std::function<bool(VirtualFileEntry&)>& LayerProc)
{
	if( std::optional<VirtualFileEntry> LayerTableFile = GetEntry("laytbl"); LayerTableFile )
	{
		std::uint32_t LayerCount = LayerTableFile->Read<std::uint32_t>();
		while( LayerCount-- ) // Read each layer entry
		{
			const LayerTableEntry CurLayerEntry = LayerTableFile->Read<LayerTableEntry>();
			char                  LayerPath[32] = {};
			std::snprintf(LayerPath, 32u, "/layers/%08x", CurLayerEntry.Identifier);
			if( std::optional<VirtualFileEntry> LayerFile = GetEntry(LayerPath); LayerFile )
			{
				if( !LayerProc(*LayerFile) )
					break;
			}
		}
	}
}

void Document::IterateSubLayerFiles(const std::function<bool(VirtualFileEntry&)>& SubLayerProc)
{
	if( std::optional<VirtualFileEntry> SubLayerTableFile = GetEntry("subtbl"); SubLayerTableFile )
	{
		std::uint32_t SubLayerCount = SubLayerTableFile->Read<std::uint32_t>();
		while( SubLayerCount-- ) // Read each layer entry
		{
			const LayerTableEntry CurSubLayerEntry = SubLayerTableFile->Read<LayerTableEntry>();
			char                  SubLayerPath[32] = {};
			std::snprintf(SubLayerPath, 32u, "/sublayers/%08x", CurSubLayerEntry.Identifier);
			if( std::optional<VirtualFileEntry> SubLayerFile = GetEntry(SubLayerPath);
				SubLayerFile )
			{
				if( !SubLayerProc(*SubLayerFile) )
					break;
			}
		}
	}
}
} // namespace sai