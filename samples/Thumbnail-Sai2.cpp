#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

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
	const auto PushJpegBytes = [&](std::span<const std::byte> Data) -> void {
		JpegData.insert(JpegData.end(), Data.begin(), Data.end()); // Data
	};
	const auto PushJpegData8 = [&](const std::uint8_t& Data) -> void {
		JpegData.push_back(std::bit_cast<std::byte>(Data));
	};
	const auto PushJpegData16 = [&](const std::uint16_t& Data) -> void {
		PushJpegData8(std::uint8_t(Data >> 8)); // Hi
		PushJpegData8(std::uint8_t(Data));      // Lo
	};

	// SOI - Start of image
	PushJpegData16(0xFF'D8);

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
	PushJpegData16(0xFF'DB);
	// Length
	// If there is more than 1 channel, then we must store both tables
	PushJpegData16((JssfChannels > 1 ? 65 : 0) + 67);

	// Luma Quantization Table
	PushJpegData8(0x0'0); // 4:Precision 4:Table Id
	JpegData.insert(JpegData.end(), JssfLumaQuant.begin(), JssfLumaQuant.end());

	// Chroma Quantization Table
	if( JssfChannels > 1 )
	{
		PushJpegData8(0x0'1); // 4:Precision 4:Table Id
		JpegData.insert(
			JpegData.end(), JssChromaQuant.begin(), JssChromaQuant.end()
		);
	}

	/// Huffman tables
	// Default huffman tables
	static const std::uint8_t HuffmanLut0[29] = {
		// DC/AC : Id
		0x0'0,
		// 16 code length counters
		0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		// Huffman codes
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B
	};
	static const std::uint8_t HuffmanLut1[179] = {
		// DC/AC : Id
		0x1'0,
		// 16 code length counters
		0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04,
		0x00, 0x00, 0x01, 0x7D,
		// Huffman codes
		0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
		0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
		0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
		0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
		0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45,
		0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
		0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75,
		0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
		0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3,
		0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
		0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
		0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
		0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4,
		0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA
	};

	static const std::uint8_t HuffmanLut2[29] = {
		// DC/AC : Id
		0x0'1,
		// 16 code length counters
		0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
		0x00, 0x00, 0x00, 0x00,
		// Huffman codes
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B
	};

	static const std::uint8_t HuffmanLut3[179] = {
		// DC/AC : Id
		0x1'1,
		// 16 code length counters
		0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04,
		0x00, 0x01, 0x02, 0x77,
		// Huffman codes
		0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
		0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
		0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1,
		0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
		0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44,
		0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
		0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74,
		0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
		0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
		0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
		0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
		0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
		0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4,
		0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA
	};
	// DHT - Define Huffman table
	PushJpegData16(0xFF'C4);
	PushJpegData16(
		2 + sizeof(HuffmanLut0) + sizeof(HuffmanLut1) + sizeof(HuffmanLut2)
		+ sizeof(HuffmanLut3)
	); // Length
	PushJpegBytes(std::as_bytes(std::span(HuffmanLut0)));
	PushJpegBytes(std::as_bytes(std::span(HuffmanLut1)));
	if( JssfChannels > 1 )
	{
		PushJpegBytes(std::as_bytes(std::span(HuffmanLut2)));
		PushJpegBytes(std::as_bytes(std::span(HuffmanLut3)));
	}

	// SOF0 - Start of frame(Baseline DCT)
	PushJpegData16(0xFF'C0);
	PushJpegData16(8 + (JssfChannels * 3));
	PushJpegData8(0x8);          // Precision
	PushJpegData16(JssfHeight);  // Height
	PushJpegData16(JssfWidth);   // Width
	PushJpegData8(JssfChannels); // Channels

	for( std::size_t ChannelIndex = 0; ChannelIndex < JssfChannels;
		 ++ChannelIndex )
	{
		// Component Id
		PushJpegData8(ChannelIndex + 1);

		// Sampling Factor
		// Might need to be Y:0x22 Cb:0x11 Cr:0x11?
		PushJpegData8(0x11);

		// Quantization Table Index
		// Should be Y:0 Cb:1 Cr:1
		PushJpegData8(ChannelIndex != 0);
	}

	// DRI - Define restart interval
	PushJpegData16(0xFF'DD);
	PushJpegData16(0x0004); // Length
	// Number of MCUs between RSTn markers
	// These are 8x8 tiles, so be sure to round up
	PushJpegData16((JssfWidth + 7) / 8);

	// SOS - Start of scan
	PushJpegData16(0xFF'DA);
	PushJpegData16(6 + (JssfChannels * 2)); // Length
	PushJpegData8(JssfChannels);            // Components
	for( std::size_t ChannelIndex = 0; ChannelIndex < JssfChannels;
		 ++ChannelIndex )
	{
		// Component Id
		PushJpegData8(ChannelIndex + 1);

		// Huffman Table index
		// Should be Y:0x00 Cb:0x11 Cr:0x11
		PushJpegData8(ChannelIndex == 0 ? 0x00 : 0x11);
	}

	// Start of spectral/predictor selection
	PushJpegData8(0x00);
	// End of spectral/predictor selection
	PushJpegData8(0x3F);
	// Successive approximation hi:lo
	PushJpegData8(0x0'0);

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
		JpegData.insert(JpegData.end(), McuData.begin(), McuData.end());

		// Insert a restart marker to move on to the next row
		// RSTm - Restart with modulo
		if( McuRowIndex < (McuCount - 1) )
		{
			PushJpegData16(0xFF'D0 | (McuRowIndex & 0b111));
		}
	}

	// EOI - End of image
	PushJpegData16(0xFF'D9);

	// Decode jpeg data into RGB8
	int      x         = 0;
	int      y         = 0;
	int      channels  = 0;
	stbi_uc* ImageData = stbi_load_from_memory(
		reinterpret_cast<stbi_uc*>(JpegData.data()), JpegData.size(), &x, &y,
		&channels, JssfChannels
	);
	if( ImageData == nullptr )
	{
		return false;
	}

	std::filesystem::path DestPath(FilePath);
	DestPath.replace_filename(FilePath.stem().string() + "-thumbnail");
	DestPath.replace_extension("png");
	stbi_write_png(DestPath.string().c_str(), x, y, channels, ImageData, 0);

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