// SPDX-FileCopyrightText: Copyright (c) 2025 Wunkolo
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>

#include "util.hpp"

namespace sai2
{
using namespace sai;

struct CanvasHeader
{
	// "SAI-CANVAS-TYPE0"
	std::array<char, 16> Identifier;

	std::uint8_t Flags0;
	std::uint8_t Flags1; // & 0x7 Indicates if thumbnail has transparency?
	std::uint8_t Flags2;
	std::uint8_t Flags3;

	std::uint32_t Width;
	std::uint32_t Height;
	std::uint32_t PrintingResolution;
	std::uint32_t TableCount;
	std::uint32_t SelectedLayer;
	std::uint64_t UnknownA;
	std::uint64_t UnknownB;
	std::uint32_t UnknownFlags;
	std::uint32_t UnknownBlendingMode;
};
static_assert(sizeof(CanvasHeader) == 64);

enum class CanvasDataType : std::uint32_t
{
	ThumbnailOld = TagLE("thum"),
	Thumbnail    = TagLE("intg"),
};

struct CanvasEntry
{
	CanvasDataType Type;
	std::uint32_t  LayerID;
	std::uint64_t  BlobsOffset; // Absolute file offset
};
static_assert(sizeof(CanvasEntry) == 16);

enum class BlobDataType : std::uint32_t
{
	// Delta-compressed pixel stream with an additional "RLE" compression
	DeltaPixelsCompressed = TagLE("dpcm"),

	// Older image format used for thumbnails?
	Jssf = TagLE("jssf"),
};

// Return false to break iteration
using CanvasDataProcT = bool(
	const sai2::CanvasHeader& Header, const sai2::CanvasEntry& TableEntry,
	std::span<const std::byte> Bytes
);

bool IterateCanvasData(
	const std::span<const std::byte>     FileData,
	const std::function<CanvasDataProcT> CanvasDataProc
);

std::size_t UnpackDeltaRLE16(
	std::span<const std::byte> Compressed, std::span<std::int16_t> Decompressed,
	std::uint32_t PixelCount, std::uint8_t OutputChannels,
	std::uint8_t InputChannels
);

// This looks kind of like the "Up" png-filter seen here:
// http://www.libpng.org/pub/png/spec/1.2/PNG-Filters.html
uint32_t DeltaUnpackRow16Bpc(
	uint32_t* Dest8Bpc, const std::uint32_t* PreviousRow8Bpc,
	const std::uint64_t* DeltaEncoded16Bpc, const std::uint32_t PixelCount
);

} // namespace sai2