/*
libsai - Library for interfacing with SystemMax PaintTool Sai files

LICENSE
	MIT License
	Copyright (c) 2017-2019 Wunkolo
	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:
	The above copyright notice and this permission notice shall be included in all
	copies or substantial portions of the Software.
	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
	SOFTWARE.
*/

/*
Sample code to decrypt any user-created .sai file
*/

#include <cstdint>
#include <fstream>
#include <iostream>
#include <chrono>
#include <utility>
#include <sai.hpp>

#include "Benchmark.hpp"

const char* const Help =
"Decrypt user-created .sai files:\n"
"\tDecrypt.exe (filename) (output)\n"
"\tWunkolo - Wunkolo@gmail.com";

int main(int argc, char* argv[])
{
	if( argc < 3 )
	{
		puts(Help);
		return EXIT_FAILURE;
	}

	sai::ifstreambuf FileIn;
	FileIn.open(argv[1]);

	if( !FileIn.is_open() )
	{
		std::cout << "Error opening file for reading: " << argv[1] << std::endl;
		return EXIT_FAILURE;
	}

	std::ofstream FileOut;
	FileOut.open(argv[2], std::ios::binary);

	if( !FileOut.is_open() )
	{
		std::cout << "Error opening file for writing: " << argv[2] << std::endl;
		return EXIT_FAILURE;
	}

	std::cout << "File decrypted in:"
		<< Benchmark<std::chrono::nanoseconds>::Run(
		[&]() -> void
	{
		FileOut << &FileIn;
	}
	).count() << "ns" << std::endl;

	return EXIT_SUCCESS;
}
