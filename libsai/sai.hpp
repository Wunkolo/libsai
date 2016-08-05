#pragma once

#include <stdint.h>
#include <fstream>

namespace sai
{
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

		bool Mount(const char *FileName);

		size_t GetClusterCount() const;
		size_t GetSize() const;

		// Virtual File System entry
		struct VirtualFileEntry
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
			uint32_t Size;// Max file size 4gb
			// Windows FILETIME
			uint64_t TimeStamp;
			uint64_t UnknownB;
		};

		bool GetEntry(const char *Path, VirtualFileEntry *Entry);

		bool Read(const VirtualFileEntry &Entry, void *Destination, size_t Size);

		class VFSVisitor
		{
		public:
			virtual ~VFSVisitor() {};

			// Visit a Folder
			virtual void VisitFolderBegin(const VirtualFileEntry &Entry) = 0;
			virtual void VisitFolderEnd() = 0;

			// Visit a File
			virtual void VisitFile(const VirtualFileEntry &Entry) = 0;
		};

		void Iterate(VFSVisitor &Visitor);

	private:
		static const size_t ClusterSize = 4096;

		void VisitCluster(size_t ClusterNumber, VFSVisitor &Visitor);

		// Decryption key
		static const uint32_t ClusterKey[1024];

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
			VirtualFileEntry VFSEntries[64];

			void DecryptTable(uint32_t ClusterNumber);
			void DecryptData(uint32_t ClusterKey);

			uint32_t Checksum(bool Table = false);
		};

		bool GetCluster(size_t ClusterNum, VFSCluster *Cluster);

		// Current VFS Variables
		size_t ClusterCount;
		std::ifstream FileStream;
	};

	// Typedefs
	typedef VirtualFileSystem FileSystem;
	typedef VirtualFileSystem::VFSVisitor FileSystemVisitor;
}