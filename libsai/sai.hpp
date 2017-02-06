/*
LibSai - Library for interfacing with SystemMax PaintTool Sai files

LICENSE
	MIT License
	Copyright (c) 2017 Wunkolo
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

#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <array>
#include <memory>

namespace sai
{
#pragma pack(push, 1)
union VirtualPage
{
	static constexpr std::size_t PageSize = 0x1000;
	static constexpr std::size_t TableSpan = PageSize / 8;

	// Data
	std::uint8_t u8[PageSize];
	std::int8_t i8[PageSize];
	std::uint32_t u32[PageSize / sizeof(std::uint32_t)];
	std::int32_t i32[PageSize / sizeof(std::int32_t)];

	// Page Table entries
	struct PageEntry
	{
		std::uint32_t Checksum;
		std::uint32_t Flags;
	} PageEntries[PageSize / sizeof(PageEntry)];

	void DecryptTable(std::uint32_t PageIndex);
	void DecryptData(std::uint32_t PageChecksum);

	/*
	To checksum a table be sure to do "u32[0] = 0" first
	*/
	std::uint32_t Checksum();
};
#pragma pack(pop)

/*
Symmetric keys for decrupting and encrypting the virtual file system
*/
namespace Keys
{
extern const std::uint32_t User[256];
extern const std::uint32_t NotRemoveMe[256];
extern const std::uint32_t LocalState[256];
extern const std::uint32_t System[256];
}

// Streambuf to read from an encrypted file
class ifstreambuf : public std::streambuf
{
public:
	ifstreambuf(
		const std::uint32_t *Key = Keys::User
	);

	// No copy
	ifstreambuf(const ifstreambuf&) = delete;
	ifstreambuf& operator=(const ifstreambuf&) = delete;

	// Adhere similarly to std::basic_filebuf
	ifstreambuf* open(
		const char *Name
	);
	ifstreambuf* close();
	bool is_open() const;

	// std::streambuf overrides
	virtual std::streambuf::int_type underflow() override;
	virtual std::streambuf::pos_type seekoff(
		std::streambuf::off_type Offset,
		std::ios_base::seekdir Direction,
		std::ios_base::openmode Mode = std::ios_base::in
	) override;
	virtual std::streambuf::pos_type seekpos(
		std::streambuf::pos_type Position,
		std::ios_base::openmode Mode = std::ios_base::in
	) override;
private:
	std::ifstream FileIn;

	std::uint32_t PageCount;

	VirtualPage Buffer;
	std::uint32_t CurrentPage;

	// Decryption Key
	const std::uint32_t *Key;

	// Caching

	bool FetchPage(std::uint32_t PageIndex, VirtualPage *Dest);

	std::unique_ptr<VirtualPage> PageCache;
	std::uint32_t PageCacheIndex;

	std::unique_ptr<VirtualPage> TableCache;
	std::uint32_t TableCacheIndex;
};

class ifstream : public std::istream
{
public:
	ifstream(
		const std::string& Path
	);

	ifstream(
		const char *Path
	);

	// Similar to ifstream member functions
	void open(const char* FilePath);
	void open(const std::string &FilePath);
	bool is_open() const;

	virtual ~ifstream();

private:
};
}