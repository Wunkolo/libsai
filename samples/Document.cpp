#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>
#include <chrono>
#include <utility>
#include <sai.hpp>

#include "Benchmark.hpp"

const char* const Help =
"Show .sai document information:\n"
"\tDocument (filenames)\n"
"\tWunkolo - Wunkolo@gmail.com";

void ProcessLayerFile(
	sai::VirtualFileEntry& LayerFile
)
{
	using namespace sai::Literals;
	const sai::LayerHeader LayerHeader
		= LayerFile.Read<sai::LayerHeader>();
	std::printf(
		"\t- \"%08x\"\n",
		LayerHeader.Identifier
	);

	std::uint32_t CurTag;
	std::uint32_t CurTagSize;
	while( LayerFile.Read<std::uint32_t>(CurTag) && CurTag )
	{
		LayerFile.Read<std::uint32_t>(CurTagSize);
		switch( CurTag )
		{
			case "name"_Tag:
			{
				std::uint8_t LayerName[256] = {};
				LayerFile.Read(LayerName, 256);
				std::printf("\t\tName: %.256s\n", LayerName);
				break;
			}
			default:
			{
				// for any streams that we do not handle,
				// we just skip forward in the stream
				LayerFile.Seek(LayerFile.Tell() + CurTagSize);
				break;
			}
		}
	}
}

int main(int argc, char* argv[])
{
	if( argc < 2 )
	{
		std::puts(Help);
		return EXIT_FAILURE;
	}

	for( std::size_t i = 1; i < std::size_t(argc); ++i)
	{
		sai::Document CurDocument(argv[i]);

		if( !CurDocument.IsOpen() )
		{
			std::cout << "Error opening file for reading: " << argv[i] << std::endl;
			return EXIT_FAILURE;
		}

		const std::tuple<std::uint32_t,std::uint32_t> CanvasSize
			= CurDocument.GetCanvasSize();
		std::printf(
			"Width: %u Height: %u\n",
			std::get<0>(CanvasSize), std::get<1>(CanvasSize)
		);

		const auto Bench = Benchmark<std::chrono::nanoseconds>::Run(
			[&CurDocument]() -> void
			{
				CurDocument.IterateLayerFiles(
					[](sai::VirtualFileEntry& LayerFile)
					{
						ProcessLayerFile(LayerFile);
						return true;
					}
				);
				CurDocument.IterateSubLayerFiles(
					[](sai::VirtualFileEntry& SubLayerFile)
					{
						ProcessLayerFile(SubLayerFile);
						return true;
					}
				);
			}
		);
		std::printf(
			"Iterated Document of %s in %zu ns\n",
			argv[i],
			Bench.count()
		);
	}

	return EXIT_SUCCESS;
}
