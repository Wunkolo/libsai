/*
Sample code to decrypt any user-created .sai file
*/

#include <stdint.h>
#include <fstream>
#include <iostream>
#include <chrono>
#include <utility>
#include "libsai/sai.hpp"

const char* const Help =
"Decrypt user-created .sai files:\n"
"\tDecrypt.exe (filename) (output)\n"
"\tWunkolo - Wunkolo@gmail.com";

template<typename TickType = std::chrono::milliseconds>
struct Benchmark
{
	template<typename F, typename ...Args>
	static typename TickType::rep Run(F func, Args&&... args)
	{
		auto StartPoint = std::chrono::system_clock::now();

		func(std::forward<Args>(args)...);

		auto Duration = std::chrono::duration_cast<TickType>(std::chrono::system_clock::now() - StartPoint);

		return Duration.count();
	}
};

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
		<< Benchmark<>::Run(
		[&]() -> void
	{
		FileOut << &FileIn;
	}
	) << "ms" << std::endl;

	return EXIT_SUCCESS;
}