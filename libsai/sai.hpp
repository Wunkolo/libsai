#pragma once

#include <stdint.h>
#include <fstream>

namespace sai
{
	// Forward prototypes
	class VirtualFileSystem;

	// Typedefs
	typedef VirtualFileSystem Archive;

	class NonCopyable
	{
	public:
		NonCopyable() = default;
		NonCopyable(const NonCopyable&) = delete;
		NonCopyable& operator=(const NonCopyable&) = delete;
	};

	// Prototypes
	class VirtualFileSystem : public NonCopyable
	{
	public:
		VirtualFileSystem();
		~VirtualFileSystem();

		bool Open(const char* FileName);

		size_t GetClusterCount() const;
		size_t GetSize() const;
	private:
		static const size_t ClusterSize = 4096;

		// Decryption key
		static const uint32_t ClusterKey[1024];

		// Virtual File System entry
		struct VFSEntry
		{
			uint32_t Flags;
			char Name[32];
			uint8_t Pad1;
			uint8_t Pad2;
			enum EntryType : uint8_t
			{
				Folder = 0x10,
				File = 0x80
			} Type;
			uint8_t Pad4;
			uint32_t ClusterNumber;
			uint32_t Size; // Max file size 4gb
						   // Windows FILETIME
						   // Contains a 64-bit value representing the number of
						   // 100-nanosecond intervals since January 1, 1601 (UTC).
			uint64_t TimeStamp;
			uint64_t UnknownB;
		};

		// Virtual File System Cluster (4096 bytes)
		union VFSCluster
		{
			// Data
			uint8_t u8[4096];
			uint32_t u32[1024];

			// Table
			struct TableEntry
			{
				uint32_t ClusterChecksum;
				uint32_t ClusterFlags;
			}TableEntries[512];

			// VFS Entries
			VFSEntry VFSEntries[64];

			void DecryptTable(uint32_t ClusterNumber);
			void DecryptData(uint32_t ClusterKey);

			uint32_t Checksum(bool Table = false);
		};

		// Current VFS Variables
		size_t ClusterCount;
		std::ifstream FileStream;
	};
}