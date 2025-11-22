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

// Returns (RGBA Pixel Data, Width, Height).
// Returns (null,0,0) if an error has occurred.
ThumbnailT ExtractThumbnailDeltaCompressed(
	const std::filesystem::path& FilePath, const sai2::CanvasHeader& Header,
	const sai2::CanvasEntry& TableEntry, std::span<const std::byte> Bytes
);

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
	case sai2::CanvasDataType::ThumbnailOld:
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
	case sai2::CanvasDataType::Thumbnail:
	{
		if( const auto Image = ExtractThumbnailDeltaCompressed(
				FilePath, Header, TableEntry, Bytes
			);
			std::get<0>(Image) )
		{
			std::filesystem::path DestPath(FilePath);
			DestPath.replace_filename(FilePath.stem().string() + "-thumbnail");
			DestPath.replace_extension("png");
			stbi_write_png(
				DestPath.string().c_str(), std::get<1>(Image),
				std::get<2>(Image), 4, std::get<0>(Image).get(), 0
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

ThumbnailT ExtractThumbnailDeltaCompressed(
	const std::filesystem::path& FilePath, const sai2::CanvasHeader& Header,
	const sai2::CanvasEntry& TableEntry, std::span<const std::byte> Bytes
)
{
	const sai2::BlobDataType Format = ReadType<sai2::BlobDataType>(Bytes);
	assert(Format == sai2::BlobDataType::DeltaPixelsCompressed);

	// Total blob size in bytes
	// const std::uint32_t BytesSize = ReadType<std::uint32_t>(Bytes);

	// 3 channels minimum, four if the header indicates that the canvas
	// uses a transparent background
	const std::uint32_t ThumbnailChannels
		= ((((Header.CanvasBackgroundFlags & 7) == 0)) != 0) + 3;

	constexpr std::uint32_t TileSize = 256u;

	const std::uint32_t Width  = Header.Width;
	const std::uint32_t Height = Header.Height;

	const std::uint32_t TilesX     = (Width + (TileSize - 1)) / TileSize;
	const std::uint32_t TilesY     = (Height + (TileSize - 1)) / TileSize;
	const std::uint32_t TilesCount = TilesX * TilesY;

	// Read tile byte sizes
	std::vector<std::uint32_t> TileSizes;

	TileSizes.reserve(TilesCount);
	for( std::size_t TileIndex2D = 0; TileIndex2D < TilesCount; ++TileIndex2D )
	{
		TileSizes.emplace_back(ReadType<std::uint32_t>(Bytes));
	}

	std::uint32_t PrevTileXIndex = 0;
	std::uint16_t TileChecksum   = 0;

	// const std::uint16_t TileBeginChecksum = ReadType<std::uint16_t>(Bytes);
	// // High byte should equal Tile Index X
	// assert((TileBeginChecksum >> 8) == PrevTileXIndex);

	std::vector<std::uint32_t> ThumbnailImage(Width * Height, 0xFFFF0000);

	for( std::uint32_t CurTileYIndex = 0; CurTileYIndex < TilesY;
		 ++CurTileYIndex )
	{
		const std::uint32_t TileBegY  = CurTileYIndex * TileSize;
		const std::uint32_t TileEndY  = std::min(TileBegY + TileSize, Height);
		const std::uint32_t TileSizeY = TileEndY - TileBegY;

		std::array<std::uint32_t, 256> CompositeRow = {};
		CompositeRow.fill(0);

		for( std::uint32_t CurTileXIndex = 0; CurTileXIndex < TilesX;
			 PrevTileXIndex              = ++CurTileXIndex )
		{
			TileChecksum = ReadType<std::uint16_t>(Bytes);
			// High byte should equal Tile Index X
			assert((TileChecksum >> 8) == PrevTileXIndex);

			const std::uint32_t TileBegX = CurTileXIndex * TileSize;
			const std::uint32_t TileEndX = std::min(TileBegX + TileSize, Width);
			const std::uint32_t TileSizeX = TileEndX - TileBegX;

			const std::uint32_t RowReadSize = 3 * ThumbnailChannels * TileSizeX;
			assert(Bytes.size_bytes() >= RowReadSize);

			const std::uint32_t TileDataSize
				= TileSizes[(CurTileYIndex * TilesX) + CurTileXIndex];

			// Read compressed tile row data
			std::span<const std::byte> CurTileBytes = Bytes.first(TileDataSize);

			// AA|RR|GG|BB / BB|GG|RR|AA
			std::span<std::uint32_t> TileImage(ThumbnailImage);
			TileImage = TileImage.subspan(TileBegX + (TileBegY * TileSize));
			std::span<const std::uint32_t> PreviousRow(CompositeRow);

			/// Compressed rows
			for( std::uint32_t CurTileRowIndex = 0; CurTileRowIndex < TileSizeY;
				 ++CurTileRowIndex )
			{
				// Decompress row
				std::array<std::int16_t, 256 * 4> RowDelta16;
				RowDelta16.fill(-1);
				const std::size_t ConsumedBytes = sai2::UnpackDeltaRLE16(
					CurTileBytes, RowDelta16, TileSizeX, 4, ThumbnailChannels
				);
				assert(ConsumedBytes > 0);

				// Row to write to
				std::span<std::uint32_t> TileRowData32
					= TileImage.subspan(CurTileRowIndex * Width);

				sai2::DeltaUnpackRow16Bpc(
					TileRowData32.data(), PreviousRow.data(),
					(uint64_t*)RowDelta16.data(), TileSizeX
				);

				PreviousRow = TileRowData32;

				// Offset by the number of fully consumed bytes
				CurTileBytes = CurTileBytes.subspan(ConsumedBytes);
			}
			///

			Bytes = Bytes.subspan(TileDataSize);
		}
	}
	// TileChecksum = ReadType<std::uint16_t>(Bytes);
	// High byte should equal Tile Index X
	// assert((TileChecksum >> 8) == PrevTileXIndex);

	// Fill alpha(for now)
	for( std::uint32_t& Pixel : ThumbnailImage )
	{
		Pixel |= 0xFF'00'00'00;
	}

	//// BGRA to RGBA
	for( std::uint32_t& Pixel : ThumbnailImage )
	{
		// Swap B and R channels
		const std::uint8_t R = ((Pixel >> 16));
		const std::uint8_t B = ((Pixel >> 0));
		Pixel = (Pixel & 0xFF'00'FF'00) | R | (std::uint32_t(B) << 16);
	}

	// Write to file
	std::filesystem::path DestPath(FilePath);
	DestPath.replace_filename(FilePath.stem().string() + "-thumbnail");
	DestPath.replace_extension("png");

	stbi_write_png(
		DestPath.string().c_str(), Width, Height, 4, ThumbnailImage.data(),
		Width * sizeof(std::uint32_t)
	);

	return std::make_tuple(nullptr, 0, 0);
}