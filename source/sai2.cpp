#include <sai2.hpp>

#include <cassert>

namespace
{
// Read a type from a span of bytes, and offset the span
// forward by the size of the type
template<typename T>
const T& ReadType(std::span<const std::byte>& Bytes)
{
	assert(Bytes.size_bytes() >= sizeof(T));
	const T& Result = *reinterpret_cast<const T*>(Bytes.data());
	Bytes           = Bytes.subspan(sizeof(T));
	return Result;
}
} // namespace

namespace sai2
{

bool IterateCanvasData(
	const std::span<const std::byte>     FileData,
	const std::function<CanvasDataProcT> CanvasDataProc
)
{
	std::span<const std::byte> Bytes  = FileData;
	const CanvasHeader&        Header = ReadType<CanvasHeader>(Bytes);

	const std::span<const sai2::CanvasEntry> TableEntries(
		reinterpret_cast<const sai2::CanvasEntry*>(Bytes.data()),
		Header.TableCount
	);

	Bytes = Bytes.subspan(Header.TableCount * sizeof(sai2::CanvasEntry));

	for( std::size_t TableEntryIndex = 0; TableEntryIndex < Header.TableCount;
		 ++TableEntryIndex )
	{
		const sai2::CanvasEntry& TableEntry = TableEntries[TableEntryIndex];

		const std::size_t DataSize
			= ((TableEntryIndex + 1) == Header.TableCount)
				? std::dynamic_extent
				: (TableEntries[TableEntryIndex + 1].BlobsOffset
				   - TableEntry.BlobsOffset);

		if( !CanvasDataProc(
				Header, TableEntry,
				FileData.subspan(TableEntry.BlobsOffset, DataSize)
			) )
		{
			break;
		}
	}

	return true;
}

} // namespace sai2