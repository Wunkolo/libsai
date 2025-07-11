#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <algorithm>
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

bool ExtractThumbnailJssf(
	const std::filesystem::path& FilePath, const sai2::CanvasHeader& Header,
	const sai2::CanvasEntry& TableEntry, std::span<const std::byte> Bytes
);
bool ExtractThumbnailDeltaCompressed(
	const std::filesystem::path& FilePath, const sai2::CanvasHeader& Header,
	const sai2::CanvasEntry& TableEntry, std::span<const std::byte> Bytes
);

bool IterateCanvasItem(
	const std::filesystem::path& FilePath, const sai2::CanvasHeader& Header,
	const sai2::CanvasEntry& TableEntry, std::span<const std::byte> Bytes
)
{
	std::printf(
		"%.4s:%08X @ %016lX\n", &TableEntry.Type, TableEntry.LayerID,
		TableEntry.BlobsOffset
	);

	switch( TableEntry.Type )
	{
	case sai2::CanvasDataType::ThumbnailOld:
	{
		return ExtractThumbnailJssf(FilePath, Header, TableEntry, Bytes);
		break;
	}
	case sai2::CanvasDataType::Thumbnail:
	{
		return ExtractThumbnailDeltaCompressed(
			FilePath, Header, TableEntry, Bytes
		);
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

bool ExtractThumbnailJssf(
	const std::filesystem::path& FilePath, const sai2::CanvasHeader& Header,
	const sai2::CanvasEntry& TableEntry, std::span<const std::byte> Bytes
)
{
	const std::uint32_t Width  = ReadType<std::uint32_t>(Bytes);
	const std::uint32_t Height = ReadType<std::uint32_t>(Bytes);

	const sai2::BlobDataType Format = ReadType<sai2::BlobDataType>(Bytes);
	assert(Format == sai2::BlobDataType::Jssf);

	const std::size_t JssfDataSize = Bytes.size_bytes();

	const std::uint16_t JssfWidth    = ReadType<std::uint16_t>(Bytes);
	const std::uint16_t JssfHeight   = ReadType<std::uint16_t>(Bytes);
	const std::uint16_t JssfChannels = ReadType<std::uint16_t>(Bytes);

	// Extract the JPEG data into a standard JPEG strea while decoding
	std::vector<std::byte> JpegData;
	const auto PushJpegData = [&JpegData]<typename T>(const T& Data) -> void {
		const auto Swapped = std::byteswap(Data);

		const std::span<const std::byte> SwappedBytes(
			reinterpret_cast<const std::byte*>(&Swapped), sizeof(Swapped)
		);

		JpegData.insert(
			JpegData.end(), SwappedBytes.cbegin(), SwappedBytes.cend()
		);
	};

	// SOI - Start of image
	PushJpegData(std::uint16_t(0xFF'D8));

	/// Quantization Tables
	const std::span<const std::byte, 64> JssfLumaQuant = Bytes.first<64>();
	Bytes                                              = Bytes.subspan(64);

	// Preemptively read the next quant table, but don't move the stream forward
	// if we're not an actually colored image
	const std::span<const std::byte, 64> JssChromaQuant = Bytes.first<64>();
	if( JssfChannels > 1 )
	{
		Bytes = Bytes.subspan(64);
	}

	// DQT - Define quantization table
	PushJpegData(std::uint16_t(0xFF'DB));
	// Length
	// If there is more than 1 channel, then we must store both tables
	PushJpegData(std::uint16_t((JssfChannels > 1 ? 65 : 0) + 67));

	// Luma Quantization Table
	PushJpegData(std::uint8_t(0x0'0)); // 4:Precision 4:Table Id
	JpegData.insert(
		JpegData.end(), JssfLumaQuant.cbegin(), JssfLumaQuant.cend()
	);

	// Chroma Quantization Table
	if( JssfChannels > 1 )
	{
		PushJpegData(std::uint8_t(0x0'1)); // 4:Precision 4:Table Id
		JpegData.insert(
			JpegData.end(), JssChromaQuant.cbegin(), JssChromaQuant.cend()
		);
	}

	/// Huffman tables
	/// We use the default huffman tables

	// SOF0 - Start of frame(Baseline DCT)
	PushJpegData(std::uint16_t(0xFF'C0));
	PushJpegData(std::uint16_t(8 + (JssfChannels * 3)));
	PushJpegData(std::uint8_t(0x8));          // Precision
	PushJpegData(std::uint16_t(JssfHeight));  // Height
	PushJpegData(std::uint16_t(JssfWidth));   // Width
	PushJpegData(std::uint8_t(JssfChannels)); // Channels

	for( std::size_t ChannelIndex = 0; ChannelIndex < JssfChannels;
		 ++ChannelIndex )
	{
		// Component Id
		PushJpegData(std::uint8_t(ChannelIndex + 1));

		// Sampling Factor
		// Might need to be Y:0x22 Cb:0x11 Cr:0x11?
		PushJpegData(std::uint8_t(0x11));

		// Quantization Table Index
		// Should be Y:0 Cb:1 Cr:1
		PushJpegData(std::uint8_t(ChannelIndex != 0));
	}

	// DRI - Define restart interval
	PushJpegData(std::uint16_t(0xFF'DD));
	PushJpegData(std::uint16_t(0x0004)); // Length
	// Number of MCUs between RSTn markers
	// These are 8x8 tiles, so be sure to round up
	PushJpegData(std::uint16_t((JssfWidth + 7) / 8));

	// SOS - Start of scan
	PushJpegData(std::uint16_t(0xFF'DA));
	PushJpegData(std::uint16_t(6 + (JssfChannels * 2))); // Length
	PushJpegData(std::uint8_t(JssfChannels));            // Components
	for( std::size_t ChannelIndex = 0; ChannelIndex < JssfChannels;
		 ++ChannelIndex )
	{
		// Component Id
		PushJpegData(std::uint8_t(ChannelIndex + 1));

		// Huffman Table index
		// Should be Y:0x00 Cb:0x11 Cr:0x11
		PushJpegData(std::uint8_t(ChannelIndex == 0 ? 0x00 : 0x11));
	}

	// Start of spectral/predictor selection
	PushJpegData(std::uint8_t(0x00));
	// End of spectral/predictor selection
	PushJpegData(std::uint8_t(0x3F));
	// Successive approximation hi:lo
	PushJpegData(std::uint8_t(0x0'0));

	// Each row of MCU is made of 8x8 tiles, rounded up
	const std::size_t McuCount = (JssfHeight + 7) / 8;
	for( std::size_t McuRowIndex = 0; McuRowIndex < McuCount; ++McuRowIndex )
	{
		// Length of the MCU bit stream
		const std::uint16_t McuRowSize = ReadType<std::uint16_t>(Bytes);
		assert(Bytes.size_bytes() >= McuRowSize);

		// Entropy encoded data
		const std::span<const std::byte> McuData = Bytes.first(McuRowSize);
		Bytes                                    = Bytes.subspan(McuRowSize);

		// Insert a row of entropy data
		JpegData.insert(JpegData.end(), McuData.cbegin(), McuData.cend());

		// Insert a restart marker to move on to the next row
		// RSTm - Restart with modulo
		PushJpegData(std::uint16_t(0xFF'D0 | (McuRowIndex & 0b111)));
	}

	// EOI - End of image
	PushJpegData(std::uint16_t(0xFF'D9));

	// Decode jpeg data into RGB8
	// int      x         = 0;
	// int      y         = 0;
	// int      channels  = 0;
	// stbi_uc* ImageData = stbi_load_from_memory(
	// 	reinterpret_cast<stbi_uc*>(JpegData.data()), JpegData.size(), &x, &y,
	// 	&channels, JssfChannels
	// );
	// if( ImageData == nullptr )
	// {
	// 	std::puts(stbi_failure_reason());
	// }
	// stbi_write_png(DestPath.string().c_str(), x, y, channels, ImageData, 0);

	// Write to file
	std::filesystem::path DestPath(FilePath);
	DestPath.replace_filename(FilePath.stem().string() + "-thumbnail");
	DestPath.replace_extension("jpeg");
	// DestPath.replace_extension("png");

	{
		std::ofstream OutFile(DestPath);
		OutFile.write(
			reinterpret_cast<const char*>(JpegData.data()), JpegData.size()
		);
	}

	// Stream ends with a singular `0x00` byte
	return true;
}

bool ExtractThumbnailDeltaCompressed(
	const std::filesystem::path& FilePath, const sai2::CanvasHeader& Header,
	const sai2::CanvasEntry& TableEntry, std::span<const std::byte> Bytes
)
{
	const sai2::BlobDataType Format = ReadType<sai2::BlobDataType>(Bytes);
	assert(Format == sai2::BlobDataType::DeltaPixelsCompressed);

	// Total blob size in bytes
	const std::uint32_t BytesSize = ReadType<std::uint32_t>(Bytes);

	// 3 channels minimum, four if the header indicates that the canvas
	// uses a transparent background
	const std::uint32_t ThumbnailChannels
		= ((((Header.Flags1 & 7) == 0)) != 0) + 3;

	constexpr std::uint32_t TileSize = 256u;

	const std::uint32_t Width  = Header.Width;
	const std::uint32_t Height = Header.Height;

	const std::uint32_t TilesX = (Width + (TileSize - 1)) / TileSize;
	const std::uint32_t TilesY = (Height + (TileSize - 1)) / TileSize;

	std::uint32_t PrevTileXIndex = 0;

	for( std::uint32_t CurTileYIndex = 0; CurTileYIndex < TilesY;
		 ++CurTileYIndex )
	{
		const std::uint32_t TileBegY  = CurTileYIndex * TileSize;
		const std::uint32_t TileEndY  = std::min(TileBegY + TileSize, Height);
		const std::uint32_t TileSizeY = TileEndY - TileBegY;

		std::uint32_t CurTileXIndex = 0;

		const std::uint16_t TileBeginChecksum = ReadType<std::uint16_t>(Bytes);
		// High byte should equal Tile Index X
		assert((TileBeginChecksum >> 8) == PrevTileXIndex);

		std::array<std::uint32_t, 256> CompositeRow = {};
		CompositeRow.fill(0);

		for( ; CurTileXIndex < TilesX; ++CurTileXIndex )
		{
			const std::uint32_t TileBegX = CurTileXIndex * TileSize;
			const std::uint32_t TileEndX = std::min(TileBegX + TileSize, Width);
			const std::uint32_t TileSizeX = TileEndX - TileBegX;

			const std::uint32_t RowReadSize = 3 * ThumbnailChannels * TileSizeX;
			assert(Bytes.size_bytes() >= RowReadSize);

			// AA|RR|GG|BB / BB|GG|RR|AA
			std::array<std::uint32_t, 256 * 256> TileImage;

			std::span<const std::uint32_t> PreviousRow(CompositeRow);

			/// Compressed rows
			for( std::uint32_t CurTileRowIndex = 0; CurTileRowIndex < TileSizeY;
				 PrevTileXIndex                = ++CurTileRowIndex )
			{
				// Read compressed tile row data
				const std::span<const std::byte> CurTileBytes
					= Bytes.first(RowReadSize);

				// Decompress row
				std::array<std::int16_t, 256 * 4> RowDelta16;
				RowDelta16.fill(-1);
				const std::size_t ConsumedBytes = sai2::UnpackDeltaRLE16(
					CurTileBytes, RowDelta16, TileSizeX, 4, ThumbnailChannels
				);
				assert(ConsumedBytes > 0);

				// Row to write to
				std::span<std::uint32_t> TileRowData32
					= std::span(TileImage).subspan(256 * CurTileRowIndex);

				sai2::DeltaUnpackRow16Bpc(
					TileRowData32.data(), PreviousRow.data(),
					(uint64_t*)RowDelta16.data(), TileSizeX
				);

				PreviousRow = TileRowData32;

				// Offset by the number of fully consumed bytes
				Bytes = Bytes.subspan(ConsumedBytes);
			}
			///

			// Fill alpha(for now)
			for( auto& Pixel : TileImage )
			{
				Pixel |= 0xFF'00'00'00;
			}

			//// BGRA to RGBA
			for( std::uint32_t& Pixel : TileImage )
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
				DestPath.string().c_str(), TileSizeX, TileSizeY, 4,
				TileImage.data(), 256 * sizeof(std::uint32_t)
			);
		}
	}
	const std::uint16_t TileEndChecksum = ReadType<std::uint16_t>(Bytes);
	// High byte should equal Tile Index X
	// assert((TileEndChecksum >> 8) == PrevTileXIndex);

	return true;
}