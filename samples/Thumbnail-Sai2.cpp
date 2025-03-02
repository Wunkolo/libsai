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
		reinterpret_cast<const sai2::CanvasEntry*>(Bytes.data()),
		Header.TableCount
	);

	Bytes = Bytes.subspan(Header.TableCount * sizeof(sai2::CanvasEntry));

	for( std::size_t TableEntryIndex = 0; TableEntryIndex < Header.TableCount;
		 ++TableEntryIndex )
	{
		const sai2::CanvasEntry& TableEntry = TableEntries[TableEntryIndex];

		const std::size_t DataSize
			= ((TableEntryIndex + 1) == Header.TableCount)
				? std::dynamic_extent
				: (TableEntries[TableEntryIndex + 1].BlobsOffset
				   - TableEntry.BlobsOffset);

		std::printf(
			"%.4s:%08X @ %016lX\n", &TableEntry.Type, TableEntry.LayerID,
			TableEntry.BlobsOffset
		);

		switch( TableEntry.Type )
		{
		case sai2::CanvasDataType::Thumbnail:
		{
			ExtractThumbnail(
				Header, TableEntry,
				FileData.subspan(TableEntry.BlobsOffset, DataSize)
			);
			break;
		}
		}
	}

	return true;
}

std::size_t DecompressRasterData(
	std::span<const std::byte> Compressed, std::span<std::int16_t> Decompressed,
	std::uint32_t PixelCount, std::uint8_t OutputChannels,
	std::uint8_t InputChannels
)
{
	const std::size_t CompressedSize = Compressed.size_bytes();

	std::uint32_t RemainingBits    = 0;
	std::uint64_t CurControlMask64 = 0;

	for( std::uint8_t CurrentChannel = 0; CurrentChannel < InputChannels;
		 ++CurrentChannel )
	{

		std::uint32_t CurChannelPixelCount = 0;

		auto CurPixelWrite = Decompressed;

		// Decode channel
		while( true )
		{
			// Shift in more bits
			if( RemainingBits < 32 )
			{
				const std::uint32_t ShiftAmount = RemainingBits;
				RemainingBits += 32;
				CurControlMask64
					|= (static_cast<std::uint64_t>(
							ReadType<std::uint32_t>(Compressed)
						)
						<< ShiftAmount);
			}

			if( CurControlMask64 == 0u )
			{
				return 0;
			}

			// Find the first set bit, and the bit right after
			const std::uint8_t FirstSetBitIndex
				= std::countr_zero(CurControlMask64);
			const std::uint64_t NextSetBitMask
				= CurControlMask64 >> (FirstSetBitIndex + 1);

			const std::uint32_t CurOpCode
				= (2 * FirstSetBitIndex) | (NextSetBitMask & 1);
			RemainingBits -= (2 + FirstSetBitIndex);

			// Too many zero bits?
			assert(CurOpCode <= 0xF);

			CurControlMask64 = NextSetBitMask >> 1;

			// Write a singular zero
			if( CurOpCode == 0u )
			{
				CurPixelWrite[CurrentChannel] = 0;

				// Next Pixel
				assert(CurPixelWrite.size() >= OutputChannels);
				++CurChannelPixelCount;
				CurPixelWrite = CurPixelWrite.subspan(OutputChannels);
			}
			// Write Value
			else if( CurOpCode <= 0xE )
			{
				// The opcode itself is the number of bits to consume

				// 000... or 111... if last bit is active
				const std::int32_t BitActiveMask
					= -((CurControlMask64 & (1 << CurOpCode)) != 0);

				// Mask of bits to read
				const std::uint64_t BitValueMask = ((1 << CurOpCode) - 1);
				const std::uint64_t BitValue
					= (CurControlMask64 & BitValueMask);

				const std::int16_t ChannelValue
					= (BitActiveMask & 1)
					+ (BitActiveMask ^ (((1 << CurOpCode) | BitValue) - 1));

				RemainingBits -= (CurOpCode + 1);
				CurControlMask64 >>= (CurOpCode + 1);

				// Write the actual pixel-value
				CurPixelWrite[CurrentChannel] = ChannelValue;

				// Next Pixel
				assert(CurPixelWrite.size() >= OutputChannels);
				++CurChannelPixelCount;
				CurPixelWrite = CurPixelWrite.subspan(OutputChannels);
			}
			// Fill pixel channels with zeroes
			else if( CurOpCode == 0xF )
			{
				// Next 7 bits are the pixel-count
				const std::uint64_t ZeroFillCount
					= (CurControlMask64 & 0x7F) + 8;
				RemainingBits -= 7;
				CurControlMask64 >>= 7;

				// Write channel values
				for( std::size_t i = 0; i < ZeroFillCount; ++i )
				{
					CurPixelWrite[i * OutputChannels + CurrentChannel] = 0;
				}

				// Next Pixel
				assert(
					CurPixelWrite.size() >= (OutputChannels * ZeroFillCount)
				);
				CurChannelPixelCount += ZeroFillCount;
				CurPixelWrite
					= CurPixelWrite.subspan(OutputChannels * ZeroFillCount);
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
		for( std::size_t CurChannel = InputChannels;
			 CurChannel < OutputChannels; ++CurChannel )
		{
			auto CurChannelWrite = Decompressed.subspan(CurChannel);

			for( std::size_t i = 0; i < PixelCount; ++i )
			{
				CurChannelWrite[i * OutputChannels] = 0;
			}
		}
	}

	// Return number of bytes read, including any unprocessed bits
	return (CompressedSize - Compressed.size_bytes()) - ((RemainingBits / 8));
}

bool ExtractThumbnail(
	const sai2::CanvasHeader& Header, const sai2::CanvasEntry& TableEntry,
	std::span<const std::byte> Bytes
)
{
	const std::string FileName = std::to_string(TableEntry.BlobsOffset);

	const sai2::BlobDataType Format = ReadType<sai2::BlobDataType>(Bytes);
	assert(Format == sai2::BlobDataType::DeltaPixelsCompressed);

	// Total blob size in bytes
	const std::uint32_t BytesSize = ReadType<std::uint32_t>(Bytes);

	// 3 channels minimum, four if the header seems to indicate that there is
	// transparency
	const std::uint32_t ThumbnailChannels
		= ((((Header.Flags1 & 7) == 0)) != 0) + 3;

	constexpr std::uint32_t TileSize = 256u;

	const std::uint32_t Width  = Header.Width;
	const std::uint32_t Height = Header.Height;

	const std::uint32_t TilesX = (Width + (TileSize - 1)) / TileSize;
	const std::uint32_t TilesY = (Height + (TileSize - 1)) / TileSize;

	std::uint8_t PrevTileXIndex = 0;

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

		for( ; CurTileXIndex < TilesX; ++CurTileXIndex )
		{
			const std::uint32_t TileBegX = CurTileXIndex * TileSize;
			const std::uint32_t TileEndX = std::min(TileBegX + TileSize, Width);
			const std::uint32_t TileSizeX = TileEndX - TileBegX;

			const std::uint32_t RowReadSize = 3 * ThumbnailChannels * TileSizeX;
			assert(Bytes.size_bytes() >= RowReadSize);

			/// Compressed rows
			for( std::uint8_t CurTileRowIndex = 0; CurTileRowIndex < TileSizeY;
				 PrevTileXIndex               = ++CurTileRowIndex )
			{
				// Read compressed tile row data
				const std::span<const std::byte> CurTileBytes
					= Bytes.first(RowReadSize);

				// Decompress row
				std::array<std::int16_t, 256 * 4 + 1> TileRowData16;
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