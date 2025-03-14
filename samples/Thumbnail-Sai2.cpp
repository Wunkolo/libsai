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

bool GetThumbnail(
	const std::filesystem::path&     FilePath,
	const std::span<const std::byte> FileData
);
bool ExtractThumbnailOld(
	const std::filesystem::path& FilePath, const sai2::CanvasHeader& Header,
	const sai2::CanvasEntry& TableEntry, std::span<const std::byte> Bytes
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

			GetThumbnail(FilePath, FileData);
		}
		else
		{
			std::printf("Error reading file contents %s\n", Arg.data());
			continue;
		}
	}

	return EXIT_SUCCESS;
}

bool GetThumbnail(
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
		case sai2::CanvasDataType::ThumbnailOld:
		{
			return ExtractThumbnailOld(
				FilePath, Header, TableEntry,
				FileData.subspan(TableEntry.BlobsOffset, DataSize)
			);
			break;
		}
		case sai2::CanvasDataType::Thumbnail:
		{
			return ExtractThumbnail(
				FilePath, Header, TableEntry,
				FileData.subspan(TableEntry.BlobsOffset, DataSize)
			);
			break;
		}
		default:
		{
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

union Pixel16Bpc
{
	std::array<std::int16_t, 4>  i16;
	std::array<std::uint16_t, 4> u16;
	std::uint64_t                u64;

	// Saturated 16bpc->8bpc
	std::uint32_t To8BpcSaturated() const
	{
		std::uint32_t Result = 0u;
		for( std::uint8_t CurChannel = 0u; CurChannel < 4u; ++CurChannel )
		{
			const std::uint8_t ChannelValue = std::uint8_t(
				u16[CurChannel] > 0xFFu ? 0xFFu : u16[CurChannel]
			);

			Result |= (std::uint32_t(ChannelValue) << (CurChannel * 8u));
		}
		return Result;
	}

	static Pixel16Bpc From8Bpc(std::uint32_t Pixel)
	{
		Pixel16Bpc Result;
		for( std::uint8_t CurChannel = 0u; CurChannel < 4u; ++CurChannel )
		{
			Result.u16[CurChannel] = ((Pixel >> (8u * CurChannel)) & 0xFFu);
		}
		return Result;
	}
};
static_assert(sizeof(Pixel16Bpc) == sizeof(std::uint64_t));

inline Pixel16Bpc Add16Bpc(const Pixel16Bpc A, const Pixel16Bpc B)
{
	Pixel16Bpc Result;
	std::transform(
		A.u16.begin(), A.u16.end(), B.u16.begin(), Result.u16.begin(),
		std::plus<std::uint16_t>()
	);
	return Result;
}

template<typename T>
	requires std::integral<T>
T AddSaturated(T A, T B)
{
	if constexpr( std::is_signed<T>::value )
	{
		if( A > 0 && B > std::numeric_limits<T>::max() - A )
		{
			return std::numeric_limits<T>::max();
		}
		else if( A < 0 && B < std::numeric_limits<T>::min() - A )
		{
			return std::numeric_limits<T>::min();
		}
	}
	else if( B > std::numeric_limits<T>::max() - A )
	{
		return std::numeric_limits<T>::max();
	}
	return A + B;
}

template<typename T>
	requires std::integral<T>
T SubSaturated(T A, T B)
{
	if( B > 0 && A < std::numeric_limits<T>::min() + B )
	{
		return std::numeric_limits<T>::min();
	}
	else if( B < 0 && A > std::numeric_limits<T>::max() + B )
	{
		return std::numeric_limits<T>::max();
	}
	else
	{
		return A - B;
	}
}

inline Pixel16Bpc AddSaturated16Bpc(const Pixel16Bpc A, const Pixel16Bpc B)
{
	Pixel16Bpc Result;
	std::
		transform(A.u16.begin(), A.u16.end(), B.u16.begin(), Result.u16.begin(), AddSaturated<std::uint16_t>);
	return Result;
}

inline Pixel16Bpc SubSaturated16Bpc(const Pixel16Bpc A, const Pixel16Bpc B)
{
	Pixel16Bpc Result;
	std::
		transform(A.u16.begin(), A.u16.end(), B.u16.begin(), Result.u16.begin(), SubSaturated<std::uint16_t>);
	return Result;
}

uint32_t DeltaUnpackRow16Bpc(
	uint32_t* Dest8Bpc, const std::uint32_t* PreviousRow8Bpc,
	const std::uint64_t* DeltaEncoded16Bpc, const std::uint32_t PixelCount
)
{
	uint32_t result = 65280ULL;

	// 0x0000, 0x0000, 0x0000, 0x0000, 0xff00, 0xff00, 0xff00, 0xff00
	Pixel16Bpc PixelFF00;
	PixelFF00.u16.fill(0xFF00u);

	Pixel16Bpc PreviousRowPixel16Bpc = {};
	Pixel16Bpc Sum16Bpc              = {};
	for( std::size_t i = 0; i < PixelCount; ++i )
	{
		// 8->16bpc
		const Pixel16Bpc CurPreviousRowPixel16Bpc
			= Pixel16Bpc::From8Bpc(*PreviousRow8Bpc);

		const Pixel16Bpc& CurrentPixelDelta
			= *reinterpret_cast<const Pixel16Bpc*>(DeltaEncoded16Bpc);

		Sum16Bpc = Add16Bpc(
			SubSaturated16Bpc(
				AddSaturated16Bpc(
					SubSaturated16Bpc(
						Add16Bpc(Sum16Bpc, CurPreviousRowPixel16Bpc),
						PreviousRowPixel16Bpc
					),
					PixelFF00
				),
				PixelFF00
			),
			CurrentPixelDelta
		);

		// Saturate 16u->8u
		*Dest8Bpc = Sum16Bpc.To8BpcSaturated();

		++DeltaEncoded16Bpc;
		++PreviousRow8Bpc;
		++Dest8Bpc;

		PreviousRowPixel16Bpc = CurPreviousRowPixel16Bpc;
	}
	return result;
}

// uint32_t DeltaUnpackRow16BpcSSE(
// 	uint32_t* Dest8Bpc, const std::uint32_t* PreviousRow8Bpc,
// 	const std::uint64_t* DeltaEncoded16Bpc, const std::uint32_t PixelCount
// )
// {
// 	uint32_t result = 65280ULL;

// 	// 0x0000, 0x0000, 0x0000, 0x0000, 0xff00, 0xff00, 0xff00, 0xff00
// 	const __m128i VecFF00 = _mm_shufflelo_epi16(_mm_cvtsi32_si128(0xFF00u), 0);

// 	__m128i PreviousRowPixel16Bpc = _mm_setzero_si128();
// 	__m128i Sum16Bpc              = _mm_setzero_si128();
// 	for( std::size_t i = 0; i < PixelCount; ++i )
// 	{
// 		// 8->16bpc
// 		const __m128i CurPreviousRowPixel16Bpc = _mm_unpacklo_epi8(
// 			_mm_cvtsi32_si128(*PreviousRow8Bpc), _mm_setzero_si128()
// 		);
// 		const __m128i CurrentDelta = _mm_loadl_epi64(
// 			reinterpret_cast<const __m128i*>(DeltaEncoded16Bpc)
// 		);

// 		Sum16Bpc = _mm_add_epi16(
// 			_mm_subs_epu16(
// 				_mm_adds_epu16(
// 					_mm_subs_epu16(
// 						_mm_add_epi16(Sum16Bpc, CurPreviousRowPixel16Bpc),
// 						PreviousRowPixel16Bpc
// 					),
// 					VecFF00
// 				),
// 				VecFF00
// 			),
// 			CurrentDelta
// 		);
// 		const __m128i CurPixel16bpc = _mm_move_epi64(Sum16Bpc);

// 		// Saturate 16u->8u
// 		*Dest8Bpc
// 			= _mm_cvtsi128_si32(_mm_packus_epi16(CurPixel16bpc, CurPixel16bpc));

// 		++DeltaEncoded16Bpc;
// 		++PreviousRow8Bpc;
// 		++Dest8Bpc;
// 		PreviousRowPixel16Bpc = _mm_move_epi64(CurPreviousRowPixel16Bpc);
// 	}
// 	return result;
// }

bool ExtractThumbnailOld(
	const std::filesystem::path& FilePath, const sai2::CanvasHeader& Header,
	const sai2::CanvasEntry& TableEntry, std::span<const std::byte> Bytes
)
{
	const std::uint32_t Width  = ReadType<std::uint32_t>(Bytes);
	const std::uint32_t Height = ReadType<std::uint32_t>(Bytes);

	const sai2::BlobDataType Format = ReadType<sai2::BlobDataType>(Bytes);
	assert(Format == sai2::BlobDataType::Fssj);

	std::array<std::byte, 4096> BlockBuffer;

	const std::size_t FssjDataSize = Bytes.size_bytes();

	std::size_t BlockCount = 0u;
	for( std::size_t FssjDataOffset = 0; FssjDataOffset < FssjDataSize;
		 ++BlockCount )
	{
		std::size_t CurBlockSize = 4096u;
		if( FssjDataSize - FssjDataOffset < 4096u )
		{
			CurBlockSize = FssjDataSize - FssjDataOffset;
		}
		if( !CurBlockSize )
		{
			break;
		}

		const std::span<const std::byte> CurFssjData
			= Bytes.subspan(FssjDataOffset, CurBlockSize);

		FssjDataOffset += CurBlockSize;
	}

	return true;
}

bool ExtractThumbnail(
	const std::filesystem::path& FilePath, const sai2::CanvasHeader& Header,
	const sai2::CanvasEntry& TableEntry, std::span<const std::byte> Bytes
)
{
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
				const std::size_t ConsumedBytes = UnpackDeltaRLE16(
					CurTileBytes, RowDelta16, TileSizeX, 4, ThumbnailChannels
				);
				assert(ConsumedBytes > 0);

				// Row to write to
				std::span<std::uint32_t> TileRowData32
					= std::span(TileImage).subspan(256 * CurTileRowIndex);

				DeltaUnpackRow16Bpc(
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