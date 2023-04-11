// SPDX-FileCopyrightText: Copyright (c) 2017-2023 Wunkolo
// SPDX-License-Identifier: MIT

/*
Sample code for extracting the thumbnail image from a user-created sai file
*/

#include <cstdint>
#include <fstream>
#include <iostream>
#include <chrono>
#include <utility>
#include <sai.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

const char* const Help =
"Extract thumbnail images from user-created .sai documents\n"
"\tThumbnail (filename) (output)\n"
"\tWunkolo - Wunkolo@gmail.com";

int main(int argc, char* argv[])
{
	if( argc < 3 )
	{
		puts(Help);
		return EXIT_FAILURE;
	}

	sai::Document FileIn(argv[1]);

	if( !FileIn.IsOpen() )
	{
		std::cout << "Error opening file for reading: " << argv[1] << std::endl;
		return EXIT_FAILURE;
	}
	
	uint32_t Width, Height;
	Width = Height = 0;
	std::unique_ptr<uint8_t[]> Pixels = {};
	std::tie(Pixels, Width, Height) = FileIn.GetThumbnail();

	stbi_write_png(argv[2], Width, Height, 4, Pixels.get(), 0);

	return EXIT_SUCCESS;
}
