#include <sai2.hpp>

#include <algorithm>
#include <cassert>
#include <limits>

namespace
{
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

// Internal utility-type for handling 16bpc RGBA pixels
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
	std::transform(
		A.u16.begin(), A.u16.end(), B.u16.begin(), Result.u16.begin(),
		AddSaturated<std::uint16_t>
	);
	return Result;
}

inline Pixel16Bpc SubSaturated16Bpc(const Pixel16Bpc A, const Pixel16Bpc B)
{
	Pixel16Bpc Result;
	std::transform(
		A.u16.begin(), A.u16.end(), B.u16.begin(), Result.u16.begin(),
		SubSaturated<std::uint16_t>
	);
	return Result;
}

} // namespace

namespace sai2
{

bool IterateCanvasData(
	const std::span<const std::byte>     FileData,
	const std::function<CanvasDataProcT> CanvasDataProc
)
{
	std::span<const std::byte> Bytes  = FileData;
	const CanvasHeader&        Header = ReadType<CanvasHeader>(Bytes);

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

		if( !CanvasDataProc(
				Header, TableEntry,
				FileData.subspan(TableEntry.BlobsOffset, DataSize)
			) )
		{
			// Stop iterating if the user function returns false
			break;
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

std::vector<std::byte> ConvertJssfToJpeg(
	std::span<const std::byte> JssfBytes, std::uint16_t JssfWidth,
	std::uint16_t JssfHeight, std::uint16_t JssfChannels
)
{

	// Extract the JPEG data into a standard JPEG stream while decoding
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
	const std::span<const std::byte, 64> JssfLumaQuant = JssfBytes.first<64>();
	JssfBytes                                          = JssfBytes.subspan(64);

	// Preemptively read the next quant table, but don't move the stream forward
	// if we're not an actually colored image
	const std::span<const std::byte, 64> JssChromaQuant = JssfBytes.first<64>();
	if( JssfChannels > 1 )
	{
		JssfBytes = JssfBytes.subspan(64);
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
		const std::uint16_t McuRowSize = ReadType<std::uint16_t>(JssfBytes);
		assert(JssfBytes.size_bytes() >= McuRowSize);

		// Entropy encoded data
		const std::span<const std::byte> McuData = JssfBytes.first(McuRowSize);
		JssfBytes = JssfBytes.subspan(McuRowSize);

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

	return JpegData;
}

} // namespace sai2