#include <cstdint>
#include <fstream>
#include <iostream>
#include <chrono>
#include <utility>
#include <sai.hpp>

#include "Benchmark.hpp"

class SaiTreeView : public sai::VirtualFileVisitor
{
public:
	SaiTreeView()
	:
	IndentLevel(0)
	{
	}
	bool VisitFolderBegin(sai::VirtualFileEntry& Entry) override
	{
		std::printf(
			"D: %*s%s\n",
			IndentLevel * IndentWidth,
			"",
			Entry.GetName()
		);
		++IndentLevel;
		return true;
	}
	bool VisitFolderEnd(sai::VirtualFileEntry& Entry) override
	{
		--IndentLevel;
		return true;
	}
	bool VisitFile(sai::VirtualFileEntry& Entry) override
	{
		std::printf(
			"F: %*s%s\n",
			IndentLevel * IndentWidth,
			"",
			Entry.GetName()
		);
		return true;
	}
private:
	std::uint32_t IndentLevel;
	static constexpr std::uint32_t IndentWidth = 4;
};

const char* const Help =
"Show virtual file system tree of a user-created .sai files:\n"
"\tDecrypt.exe (filename)\n"
"\tWunkolo - Wunkolo@gmail.com";

int main(int argc, char* argv[])
{
	if( argc < 2 )
	{
		std::puts(Help);
		return EXIT_FAILURE;
	}
	sai::Document CurDocument(argv[1]);

	if( !CurDocument.IsOpen() )
	{
		std::cout << "Error opening file for reading: " << argv[1] << std::endl;
		return EXIT_FAILURE;
	}

	SaiTreeView TreeVisitor;
	const auto Bench = Benchmark<std::chrono::nanoseconds>::Run(
		[&]() -> void
		{
		CurDocument.IterateFileSystem(TreeVisitor);
		}
	);
	std::printf(
		"Iterated VFS of %s in %zu ns\n",
		argv[1],
		Bench.count()
	);

	return EXIT_SUCCESS;
}
