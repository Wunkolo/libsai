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

#include <immintrin.h>

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

bool ExtractFile(
	const std::filesystem::path&     FilePath,
	const std::span<const std::byte> FileData
);
bool ExtractThumbnail(
	const std::filesystem::path& FilePath, const sai2::CanvasHeader& Header,
	const sai2::CanvasEntry& TableEntry, std::span<const std::byte> Bytes
);

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

			ExtractFile(FilePath, FileData);
		}
		else
		{
			std::printf("Error reading file contents %s\n", Arg.data());
			continue;
		}
	}

	return EXIT_SUCCESS;
}

bool ExtractFile(
	const std::filesystem::path&     FilePath,
	const std::span<const std::byte> FileData
)
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
				FilePath, Header, TableEntry,
				FileData.subspan(TableEntry.BlobsOffset, DataSize)
			);
			break;
		}
		}
	}

	return true;
}

std::size_t UnpackDeltaRLE16(
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

uint32_t DeltaUnpack16(
	uint32_t* Dest32, const std::uint32_t* CompositeImage,
	const std::uint64_t* DeltaEncoded16bpc, std::uint32_t CurTileWidth
)
{
	uint32_t result = 65280ULL;

	// 0x0000, 0x0000, 0x0000, 0x0000, 0xff00, 0xff00, 0xff00, 0xff00
	const __m128i VecFF00 = _mm_shufflelo_epi16(_mm_cvtsi32_si128(0xFF00u), 0);

	__m128i Previous32 = _mm_setzero_si128();
	__m128i Sum16      = _mm_setzero_si128();
	do
	{
		--CurTileWidth;
		const __m128i UnknownWidened16bpc = _mm_unpacklo_epi8(
			_mm_cvtsi32_si128(*CompositeImage), _mm_setzero_si128()
		);
		const __m128i CurrentDelta = _mm_loadl_epi64(
			reinterpret_cast<const __m128i*>(DeltaEncoded16bpc)
		);

		Sum16 = _mm_add_epi16(
			_mm_subs_epu16(
				_mm_adds_epu16(
					_mm_subs_epu16(
						_mm_add_epi16(Sum16, UnknownWidened16bpc), Previous32
					),
					VecFF00
				),
				VecFF00
			),
			CurrentDelta
		);
		const __m128i CurPixel16bpc = _mm_move_epi64(Sum16);

		// Saturate 16u->8u
		*Dest32
			= _mm_cvtsi128_si32(_mm_packus_epi16(CurPixel16bpc, CurPixel16bpc));

		++DeltaEncoded16bpc;
		++CompositeImage;
		++Dest32;
		Previous32 = _mm_move_epi64(UnknownWidened16bpc);
	} while( CurTileWidth );
	return result;
}

bool ExtractThumbnail(
	const std::filesystem::path& FilePath, const sai2::CanvasHeader& Header,
	const sai2::CanvasEntry& TableEntry, std::span<const std::byte> Bytes
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
		CompositeRow.fill(0xFF000000);

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
				const std::size_t ConsumedBytes = UnpackDeltaRLE16(
					CurTileBytes, RowDelta16, TileSizeX, 4, ThumbnailChannels
				);
				assert(ConsumedBytes > 0);

				// Row to write to
				std::span<std::uint32_t> TileRowData32
					= std::span(TileImage).subspan(256 * CurTileRowIndex);

				DeltaUnpack16(
					TileRowData32.data(), PreviousRow.data(),
					(uint64_t*)RowDelta16.data(), TileSizeX
				);

				PreviousRow = TileRowData32;

				// Offset by the number of fully consumed bytes
				Bytes = Bytes.subspan(ConsumedBytes);
			}
			///

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