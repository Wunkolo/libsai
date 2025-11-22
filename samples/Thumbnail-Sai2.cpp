#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

#include <sai2.hpp>

#include "stb_image.h"
#include "stb_image_write.h"

// Read a type from a span of bytes, and offset the span
// forward by the size of the type
template<typename T>
const T& ReadType(std::span<const std::byte>& Bytes)
{
	assert(Bytes.size_bytes() >= sizeof(T));
	const T& Result = *reinterpret_cast<const T*>(Bytes.data());
	Bytes           = Bytes.subspan(sizeof(T));
	return Result;
}

using ThumbnailT
	= std::tuple<std::unique_ptr<std::byte[]>, std::uint32_t, std::uint32_t>;

bool IterateCanvasItem(
	const std::filesystem::path& FilePath, const sai2::CanvasHeader& Header,
	const sai2::CanvasEntry& TableEntry, std::span<const std::byte> Bytes
)
{
	std::printf(
		"%.4s:%08X @ %016lX\n", reinterpret_cast<const char*>(&TableEntry.Type),
		TableEntry.LayerID, TableEntry.BlobsOffset
	);

	switch( TableEntry.Type )
	{
	case sai2::CanvasDataType::ThumbnailLossy:
	{
		if( const auto JpegStream = sai2::ExtractJssfToJpeg(Bytes);
			!std::get<0>(JpegStream).empty() )
		{
			std::filesystem::path DestPath(FilePath);
			DestPath.replace_filename(FilePath.stem().string() + "-thumbnail");
			DestPath.replace_extension("png");

			// Decode jpeg stream
			const auto JpegData = std::get<0>(JpegStream);

			// Decode jpeg data into RGBA8
			int      JpegWidth       = 0;
			int      JpegHeight      = 0;
			int      JpegChannels    = 0;
			stbi_uc* JpegDecodedData = stbi_load_from_memory(
				reinterpret_cast<const stbi_uc*>(JpegData.data()),
				static_cast<int>(JpegData.size()), &JpegWidth, &JpegHeight,
				&JpegChannels, 4
			);
			if( JpegDecodedData == nullptr )
			{
				return false;
			}

			// Write decoded pixel data into a png
			stbi_write_png(
				DestPath.string().c_str(), JpegWidth, JpegHeight, 4,
				JpegDecodedData, 0
			);

			stbi_image_free(JpegDecodedData);
		}
		// Quit at the first thumbnail
		return false;
		break;
	}
	case sai2::CanvasDataType::ThumbnailLossless:
	{
		if( auto Extracted = sai2::ExtractDpcmToBGRA(Header, Bytes);
			!std::get<0>(Extracted).empty() )
		{
			std::filesystem::path DestPath(FilePath);
			DestPath.replace_filename(FilePath.stem().string() + "-thumbnail");
			DestPath.replace_extension("png");

			std::vector<std::byte>& ThumbnailData = std::get<0>(Extracted);

			const std::span<std::uint32_t> ThumbnailImage(
				reinterpret_cast<std::uint32_t*>(ThumbnailData.data()),
				std::get<1>(Extracted) * std::get<2>(Extracted)
			);

			//// BGRA to RGBA
			for( std::uint32_t& Pixel : ThumbnailImage )
			{
				// G and R are already where they need to be.
				// Just swap B and R channels
				const std::uint8_t R = ((Pixel >> 16));
				const std::uint8_t B = ((Pixel >> 0));
				Pixel = (Pixel & 0xFF'00'FF'00) | R | (std::uint32_t(B) << 16);
			}

			stbi_write_png(
				DestPath.string().c_str(), std::get<1>(Extracted),
				std::get<2>(Extracted), 4, ThumbnailImage.data(), 0
			);
		}
		// Quit at the first thumbnail
		return false;
		break;
	}
	default:
	{
		break;
	}
	}

	return true;
};

int main(int argc, char* argv[])
{
	std::vector<std::string_view> Args;

	for( std::size_t i = 1; i < std::size_t(argc); ++i )
	{
		Args.emplace_back(argv[i]);
	}

	for( const std::string_view& Arg : Args )
	{
		std::puts(Arg.data());
		const std::filesystem::path FilePath(Arg);
		if( !(std::filesystem::exists(FilePath)
			  && std::filesystem::is_regular_file(FilePath)) )
		{
			// Not a file
			std::printf("Invalid path %s\n", Arg.data());
			continue;
		}
		const std::uintmax_t FileSize = std::filesystem::file_size(FilePath);

		std::ifstream          File(FilePath);
		std::vector<std::byte> FileData(FileSize);

		if( File.is_open() )
		{
			File.read(reinterpret_cast<char*>(FileData.data()), FileSize);
			File.close();

			const auto CanvasDataProc = [&FilePath](
											const sai2::CanvasHeader& Header,
											const sai2::CanvasEntry& TableEntry,
											std::span<const std::byte> Bytes
										) {
				return IterateCanvasItem(FilePath, Header, TableEntry, Bytes);
			};

			sai2::IterateCanvasData(FileData, CanvasDataProc);
		}
		else
		{
			std::printf("Error reading file contents %s\n", Arg.data());
			continue;
		}
	}

	return EXIT_SUCCESS;
}