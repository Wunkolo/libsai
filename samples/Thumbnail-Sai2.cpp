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

bool ExtractFile(const std::span<const std::byte> FileData);
bool ExtractThumbnail(
	const sai2::CanvasHeader& Header, const sai2::CanvasEntry& TableEntry,
	std::span<const std::byte> Bytes
);

int main(int argc, char* argv[])
{
	std::vector<std::string_view> Args;

	for( std::size_t i = 1; i < argc; ++i )
	{
		Args.emplace_back(argv[i]);
	}

	for( const std::string_view& Arg : Args )
	{
		std::puts(Arg.data());
		const std::filesystem::path FilePath(Arg);
		if( !(std::filesystem::exists(FilePath) && std::filesystem::is_regular_file(FilePath)) )
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

			ExtractFile(FileData);
		}
		else
		{
			std::printf("Error reading file contents %s\n", Arg.data());
			continue;
		}
	}

	return EXIT_SUCCESS;
}

bool ExtractFile(const std::span<const std::byte> FileData)
{
	std::span<const std::byte> Bytes  = FileData;
	const sai2::CanvasHeader&  Header = ReadType<sai2::CanvasHeader>(Bytes);

	std::printf("%.*s\n", Header.Identifier.size(), Header.Identifier.data());

	const std::span<const sai2::CanvasEntry> TableEntries(
		reinterpret_cast<const sai2::CanvasEntry*>(Bytes.data()), Header.TableCount
	);

	Bytes = Bytes.subspan(Header.TableCount * sizeof(sai2::CanvasEntry));

	for( std::size_t TableEntryIndex = 0; TableEntryIndex < Header.TableCount; ++TableEntryIndex )
	{
		const sai2::CanvasEntry& TableEntry = TableEntries[TableEntryIndex];

		const std::size_t DataSize
			= ((TableEntryIndex + 1) == Header.TableCount)
				? std::dynamic_extent
				: (TableEntries[TableEntryIndex + 1].BlobsOffset - TableEntry.BlobsOffset);

		std::printf(
			"%.4s:%08X @ %016lX\n", &TableEntry.Type, TableEntry.LayerID, TableEntry.BlobsOffset
		);

		switch( TableEntry.Type )
		{
		case sai2::CanvasDataType::Thumbnail:
		{
			ExtractThumbnail(
				Header, TableEntry, FileData.subspan(TableEntry.BlobsOffset, DataSize)
			);
			break;
		}
		}
	}

	return true;
}

std::size_t DecompressRasterData(
	std::span<const std::byte> Compressed, std::span<std::int16_t> Decompressed,
	std::uint32_t PixelCount, std::uint8_t OutputChannels, std::uint8_t InputChannels
)
{
	const std::size_t CompressedSize = Compressed.size_bytes();

	std::uint32_t RemainingBits    = 0;
	std::uint64_t CurControlMask64 = 0;

	for( std::uint8_t CurrentChannel = 0; CurrentChannel < InputChannels; ++CurrentChannel )
	{

		std::uint8_t CurChannelPixelCount = 0;

		auto CurChannelWrite = Decompressed.subspan(CurrentChannel);

		// Decode channel
		while( true )
		{
			// Shift in more bits
			if( RemainingBits < 32 )
			{
				const std::uint32_t ShiftAmount = RemainingBits;
				RemainingBits += 32;
				CurControlMask64
					|= (static_cast<std::uint64_t>(ReadType<std::uint32_t>(Compressed))
						<< ShiftAmount);
			}

			if( CurControlMask64 == 0u )
			{
				return 0;
			}

			const std::uint8_t FirstSetBitIndex = std::countr_zero(CurControlMask64);

			const std::uint64_t NextSetBitMask = CurControlMask64 >> (FirstSetBitIndex + 1);
			CurControlMask64                   = NextSetBitMask >> 1;

			const std::uint32_t CurOpCode = (2 * FirstSetBitIndex) | (NextSetBitMask & 1);

			RemainingBits += -2 - FirstSetBitIndex;

			// Ends the current channel
			if( CurOpCode == 0u )
			{
				break;
			}
			// Write Value
			else if( CurOpCode <= 0xE )
			{
				const std::int32_t BitActiveMask
					= -(((std::uint32_t)CurControlMask64 & (1 << CurOpCode)) != 0);

				const std::int16_t ChannelValue
					= (BitActiveMask & 1)
					+ (BitActiveMask
					   ^ (((1 << CurOpCode) | (CurControlMask64 & ((1 << CurOpCode) - 1))) - 1));

				CurChannelWrite[0] = ChannelValue;

				// Next Pixel
				assert(CurChannelWrite.size() >= OutputChannels);
				++CurChannelPixelCount;
				CurChannelWrite = CurChannelWrite.subspan(OutputChannels);

				// Next bits
				RemainingBits += -1 - CurOpCode;
				CurControlMask64 >>= CurOpCode + 1;
			}
			// Fill pixel channels with zeroes
			else if( CurOpCode == 0xF )
			{
				const std::uint64_t ZeroFillCount = (CurControlMask64 & 0x7F) + 8;
				for( std::size_t i = 0; i < ZeroFillCount; ++i )
				{
					CurChannelWrite[i * OutputChannels] = 0;
				}

				// Next Pixel
				assert(CurChannelWrite.size() >= (OutputChannels * ZeroFillCount));
				CurChannelPixelCount += ZeroFillCount;
				CurChannelWrite = CurChannelWrite.subspan(OutputChannels * ZeroFillCount);

				// Next bits
				RemainingBits -= 7;
				CurControlMask64 >>= 7;
			}
			else
			{
				// Invalid opcode
				return 0;
			}

			if( CurChannelPixelCount >= PixelCount )
			{
				// Move onto next channel
				break;
			}
		}
	}

	// Pad unused channels with zero
	if( InputChannels < OutputChannels )
	{
		for( std::size_t CurChannel = InputChannels; CurChannel < OutputChannels; ++CurChannel )
		{
			auto CurChannelWrite = Decompressed.subspan(CurChannel);

			for( std::size_t i = 0; i < PixelCount; ++i )
			{
				CurChannelWrite[i * OutputChannels] = 0;
			}
		}
	}

	// Return number of bytes read, including any unprocessed bits
	return (CompressedSize - Compressed.size_bytes()) - (RemainingBits / 8);
}

bool ExtractThumbnail(
	const sai2::CanvasHeader& Header, const sai2::CanvasEntry& TableEntry,
	std::span<const std::byte> Bytes
)
{
	const std::string FileName = std::to_string(TableEntry.BlobsOffset);

	const sai2::BlobDataType Format = ReadType<sai2::BlobDataType>(Bytes);
	assert(Format == sai2::BlobDataType::DeltaPixelsCompressed);
	const std::uint32_t BytesSize = ReadType<std::uint32_t>(Bytes);

	// 3 channels minimum, four if the header seems to indicate that there is transparency
	const std::uint32_t ThumbnailChannels = ((((Header.Flags1 & 7) == 0)) != 0) + 3;

	constexpr std::uint32_t TileSize = 256u;

	const std::uint32_t Width  = Header.Width;
	const std::uint32_t Height = Header.Height;

	const std::uint32_t TilesX = (Width + (TileSize - 1)) / TileSize;
	const std::uint32_t TilesY = (Height + (TileSize - 1)) / TileSize;

	std::uint8_t PrevTileXIndex = 0;

	for( std::uint32_t CurTileY = 0; CurTileY < TilesY; ++CurTileY )
	{
		const std::uint32_t TileBegY  = CurTileY * TileSize;
		const std::uint32_t TileEndY  = std::min(TileBegY + TileSize, Height);
		const std::uint32_t TileSizeY = TileEndY - TileBegY;

		std::uint32_t CurTileXIndex = 0;

		const std::uint16_t TileBeginChecksum = ReadType<std::uint16_t>(Bytes);
		// High byte should equal Tile Index X
		assert((TileBeginChecksum >> 8) == PrevTileXIndex);

		for( ; CurTileXIndex < TilesX; ++CurTileXIndex )
		{
			const std::uint32_t TileBegX  = CurTileXIndex * TileSize;
			const std::uint32_t TileEndX  = std::min(TileBegX + TileSize, Width);
			const std::uint32_t TileSizeX = TileEndX - TileBegX;

			const std::uint32_t RowReadSize = 3 * ThumbnailChannels * TileSizeX;
			assert(Bytes.size_bytes() >= RowReadSize);

			/// Compressed rows
			for( std::uint8_t CurTileRowIndex = 0; CurTileRowIndex < TileSizeY;
				 PrevTileXIndex               = ++CurTileRowIndex )
			{
				// Read compressed tile row data
				const std::span<const std::byte> CurTileBytes = Bytes.first(RowReadSize);

				// Decompress row
				std::array<std::int16_t, 256 * 4> TileRowData16;
				TileRowData16.fill(-1);
				const std::size_t ConsumedBytes = DecompressRasterData(
					CurTileBytes, TileRowData16, TileSizeX, 4, ThumbnailChannels
				);
				assert(ConsumedBytes > 0);

				// Delta-encoded bytes, expand from 8->16bpc
				// std::array<std::uint32_t, 256 * 4> TileRowData32;
				// TileRowData32.fill(0xFFFF0000);

				// Offset by the number of fully consumed bytes
				Bytes = Bytes.subspan(ConsumedBytes);
			}
			///
		}
	}
	const std::uint16_t TileEndChecksum = ReadType<std::uint16_t>(Bytes);
	// High byte should equal Tile Index X
	// assert((TileEndChecksum >> 8) == PrevTileXIndex);

	return true;
}