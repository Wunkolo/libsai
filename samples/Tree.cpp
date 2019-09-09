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

#include <cstdint>
#include <ctime>
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
	FolderDepth(0)
	{
	}
	~SaiTreeView()
	{
	}
	bool VisitFolderBegin(sai::VirtualFileEntry& Entry) override
	{
		PrintVirtualFileEntry(Entry);
		++FolderDepth;
		return true;
	}
	bool VisitFolderEnd(sai::VirtualFileEntry& /*Entry*/) override
	{
		--FolderDepth;
		return true;
	}
	bool VisitFile(sai::VirtualFileEntry& Entry) override
	{
		PrintVirtualFileEntry(Entry);
		return true;
	}
private:
	void PrintVirtualFileEntry(const sai::VirtualFileEntry& Entry) const
	{
		const std::time_t TimeStamp = Entry.GetTimeStamp();
		char TimeString[32];
		std::strftime(
			TimeString,
			32,
			"%D %R",
			std::localtime(&TimeStamp)
		);
		PrintNestedFolder();
		std::printf(
			"\u251C\u2500\u2500 [%12zu %s] %s\n",
			Entry.GetSize(),
			TimeString,
			Entry.GetName()
		);
	}
	void PrintNestedFolder() const
	{
		for( std::size_t i = 0; i < FolderDepth; ++i )
		{
			std::fputs(
				"\u2502   ",
				stdout
			);
		}
	}
	std::uint32_t FolderDepth;
};

const char* const Help =
"Show virtual file system tree of a user-created .sai files:\n"
"\t./Tree (filenames)\n"
"\tWunkolo - Wunkolo@gmail.com";

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

		const auto Bench = Benchmark<std::chrono::nanoseconds>::Run(
			[&CurDocument]() -> void
			{
				SaiTreeView TreeVisitor;
				CurDocument.IterateFileSystem(TreeVisitor);
			}
		);
		std::printf(
			"Iterated VFS of %s in %zu ns\n",
			argv[i],
			Bench.count()
		);
	}

	return EXIT_SUCCESS;
}
