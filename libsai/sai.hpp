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
	class VirtualFileSystem;

	// File Entry
	class VirtualFileEntry
	{
		friend class VirtualFileSystem;
	public:
		VirtualFileEntry();
		~VirtualFileEntry();
		uint32_t GetFlags() const;
		const char* GetName() const;

		enum class EntryType : uint8_t
		{
			Folder = 0x10,
			File = 0x80
		} GetType() const;
		uint32_t GetCluster() const;
		uint32_t GetSize() const;
		time_t GetTimeStamp() const;

	private:
		struct FATEntry
		{
			uint32_t Flags;
			char Name[32];
			uint8_t Pad1;
			uint8_t Pad2;
			EntryType Type;
			uint8_t Pad4;
			uint32_t Cluster;
			uint32_t Size;
			uint64_t TimeStamp; // Windows FILETIME
			uint64_t UnknownB;
		} Data;
	};

	typedef VirtualFileEntry FileEntry;

	// File System Visitor
	class VFSVisitor
	{
	public:
		virtual ~VFSVisitor() {};

		// Visit a Folder
		virtual void VisitFolderBegin(const FileEntry &Entry) = 0;
		virtual void VisitFolderEnd() = 0;

		// Visit a File
		virtual void VisitFile(const FileEntry &Entry) = 0;
	};

	typedef VFSVisitor FileSystemVisitor;

	// File System
	class VirtualFileSystem : public NonCopyable
	{
	public:
		VirtualFileSystem();

		~VirtualFileSystem();

		bool Mount(const char *FileName);

		size_t GetClusterCount() const;

		size_t GetSize() const;

		bool GetEntry(const char *Path, FileEntry &Entry);

		bool Read(const FileEntry &Entry, size_t Offset, size_t Size, void *Destination);

		template< typename T>
		inline bool Read(const FileEntry &Entry, size_t Offset, T &Data)
		{
			return Read(Entry, Offset, sizeof(T), &Data);
		}

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
			VirtualFileEntry::FATEntry VFSEntries[64];

			void DecryptTable(uint32_t ClusterNumber);
			void DecryptData(uint32_t ClusterKey);

			uint32_t Checksum(bool Table = false);
		};

		bool GetCluster(size_t ClusterNum, VFSCluster *Cluster);

		// Current VFS Variables
		size_t ClusterCount;
		std::ifstream FileStream;

		// Cluster Caching
		intmax_t CacheTableNum = -1;
		VFSCluster *CacheTable;
		VFSCluster *CacheBuffer;
	};

	typedef VirtualFileSystem FileSystem;
}