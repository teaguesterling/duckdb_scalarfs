#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"

namespace duckdb {

class MemoryFileHandle : public FileHandle {
public:
	MemoryFileHandle(FileSystem &fs, string path, string data);

	void Close() override;

	// Data accessors for the owning filesystem
	const string &GetData() const {
		return data;
	}
	idx_t GetPosition() const {
		return position;
	}
	void SetPosition(idx_t pos) {
		position = pos;
	}

private:
	string data;
	idx_t position = 0;
};

} // namespace duckdb
