#include "memory_file_handle.hpp"

namespace duckdb {

MemoryFileHandle::MemoryFileHandle(FileSystem &fs, string path, string data_p)
    : FileHandle(fs, std::move(path), FileOpenFlags::FILE_FLAGS_READ), data(std::move(data_p)), position(0) {
}

void MemoryFileHandle::Close() {
	// Nothing to clean up for read-only memory handles
}

} // namespace duckdb
