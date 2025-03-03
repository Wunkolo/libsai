// SPDX-FileCopyrightText: Copyright (c) 2025 Wunkolo
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>

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
	Thumbnail = TagLE("intg"),
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
};

} // namespace sai2