#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

//! Internal table function that streams the DataChunks a client sends via QUACK_SEND_DATA out of a
//! per-stream queue. The server drives `INSERT INTO tbl SELECT * FROM
//! scan_data_from_quack_client('<stream-id>')`, which lets core's PhysicalBatchInsert do the insert
//! (optimistic writes, memory-bounded buffering) inside one transaction.
class QuackScanFromClientFunction {
public:
	static TableFunction GetFunction();
};

} // namespace duckdb
