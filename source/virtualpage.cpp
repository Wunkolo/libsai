// SPDX-FileCopyrightText: Copyright (c) 2017-2023 Wunkolo
// SPDX-License-Identifier: MIT

#include <sai.hpp>

#ifdef __x86_64__
#include <immintrin.h>
#endif

namespace sai
{
#if defined(__AVX2__)
inline __m256i KeySum8(__m256i Vector8, std::span<const std::uint32_t, 256> Key)
{
	__m256i Sum = _mm256_i32gather_epi32(
		(const std::int32_t*)Key.data(), _mm256_and_si256(Vector8, _mm256_set1_epi32(0xFF)),
		sizeof(std::uint32_t)
	);

	Sum = _mm256_add_epi32(
		Sum, _mm256_i32gather_epi32(
				 (const std::int32_t*)Key.data(),
				 _mm256_and_si256(_mm256_srli_epi32(Vector8, 8), _mm256_set1_epi32(0xFF)),
				 sizeof(std::uint32_t)
			 )
	);
	Sum = _mm256_add_epi32(
		Sum, _mm256_i32gather_epi32(
				 (const std::int32_t*)Key.data(),
				 _mm256_and_si256(_mm256_srli_epi32(Vector8, 16), _mm256_set1_epi32(0xFF)),
				 sizeof(std::uint32_t)
			 )
	);
	Sum = _mm256_add_epi32(
		Sum,
		_mm256_i32gather_epi32(
			(const std::int32_t*)Key.data(), _mm256_srli_epi32(Vector8, 24), sizeof(std::uint32_t)
		)
	);
	return Sum;
}
#endif

void VirtualPage::DecryptTable(std::uint32_t PageIndex)
{
	std::uint32_t PrevData = PageIndex & (~0x1FF);
#if defined(__AVX2__)
	__m256i PrevData8 = _mm256_set1_epi32(PrevData);
	for( std::size_t i = 0; i < (PageSize / sizeof(std::uint32_t)); i += 8 )
	{
		const __m256i CurData8 = _mm256_loadu_si256((__m256i*)(u32 + i));
		// There is no true _mm_alignr_epi8 for AVX2
		// An extra _mm256_permute2x128_si256 is needed
		PrevData8 = _mm256_alignr_epi8(
			CurData8, _mm256_permute2x128_si256(PrevData8, CurData8, _MM_SHUFFLE(0, 2, 0, 1)),
			sizeof(std::uint32_t) * 3
		);
		__m256i CurPlain8 = _mm256_xor_si256(
			_mm256_xor_si256(CurData8, PrevData8), KeySum8(PrevData8, Keys::User)
		);
		CurPlain8 = _mm256_shuffle_epi8(
			CurPlain8, _mm256_set_epi8(
						   13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2, 13, 12, 15, 14, 9,
						   8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2
					   )
		);
		_mm256_storeu_si256((__m256i*)(u32 + i), CurPlain8);
		PrevData8 = CurData8;
	};
#else
	for( std::size_t i = 0; i < (PageSize / sizeof(std::uint32_t)); i++ )
	{
		const std::uint32_t CurData = u32[i];
		std::uint32_t       X       = PrevData ^ CurData;
		X
			^= (Keys::User[(PrevData >> 24) & 0xFF] + Keys::User[(PrevData >> 16) & 0xFF]
				+ Keys::User[(PrevData >> 8) & 0xFF] + Keys::User[(PrevData >> 0) & 0xFF]);
		u32[i]   = static_cast<std::uint32_t>((X << 16) | (X >> 16));
		PrevData = CurData;
	};
#endif
}

void VirtualPage::DecryptData(std::uint32_t PageChecksum)
{
	std::uint32_t PrevData = PageChecksum;
#if defined(__AVX2__)
	__m256i PrevData8 = _mm256_set1_epi32(PrevData);
	for( std::size_t i = 0; i < (PageSize / sizeof(std::uint32_t)); i += 8 )
	{
		const __m256i CurData8 = _mm256_loadu_si256((__m256i*)(u32 + i));
		// There is no true _mm_alignr_epi8 for AVX2
		// An extra _mm256_permute2x128_si256 is needed
		PrevData8 = _mm256_alignr_epi8(
			CurData8, _mm256_permute2x128_si256(PrevData8, CurData8, _MM_SHUFFLE(0, 2, 0, 1)),
			sizeof(std::uint32_t) * 3
		);
		__m256i CurPlain8 = _mm256_sub_epi32(
			CurData8, _mm256_xor_si256(PrevData8, KeySum8(PrevData8, Keys::User))
		);
		_mm256_storeu_si256((__m256i*)(u32 + i), CurPlain8);
		PrevData8 = CurData8;
	};
#else
	for( std::size_t i = 0; i < (PageSize / sizeof(std::uint32_t)); i++ )
	{
		const std::uint32_t CurData = u32[i];
		u32[i]                      = CurData
			   - (PrevData
				  ^ (Keys::User[(PrevData >> 24) & 0xFF] + Keys::User[(PrevData >> 16) & 0xFF]
					 + Keys::User[(PrevData >> 8) & 0xFF] + Keys::User[(PrevData >> 0) & 0xFF]));
		PrevData = CurData;
	}
#endif
}

std::uint32_t VirtualPage::Checksum()
{
	std::uint32_t Sum = 0;
	for( std::size_t i = 0; i < (PageSize / sizeof(std::uint32_t)); i++ )
	{
		Sum = (2 * Sum | (Sum >> 31)) ^ u32[i];
	}
	return Sum | 1;
}
} // namespace sai