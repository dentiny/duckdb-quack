#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace duckdb {

struct QuackStreamChunk {
	unique_ptr<DataChunk> chunk;
	//! Wire batch index (set by the future async/ordered client). Invalid for the current client,
	//! in which case the scan assigns a constant batch index.
	optional_idx batch_index;
};

//! Bounded, thread-safe queue feeding one server-side
//! `INSERT ... SELECT * FROM scan_data_from_quack_client('<id>')` statement. SEND_DATA handlers push
//! chunks; the table function pops them. The bound provides backpressure to the (synchronous) client.
class QuackDataStream {
public:
	enum class PopStatus : uint8_t {
		CHUNK,    //! a chunk was dequeued into `out`
		EMPTY,    //! queue empty, more chunks may still arrive
		FINISHED, //! queue empty and the stream is finished
		ERRORED   //! the stream recorded a fatal error
	};

	explicit QuackDataStream(vector<LogicalType> types_p, idx_t capacity_p = 64)
	    : types(std::move(types_p)), capacity(capacity_p) {
	}

	const vector<LogicalType> &Types() const {
		return types;
	}

	//! Producer: enqueue a chunk, blocking while the queue is full (until the consumer drains or the
	//! stream is finished/errored).
	void Push(unique_ptr<DataChunk> chunk, optional_idx batch_index);
	//! Producer: signal that no more chunks will arrive.
	void Finish();
	//! Record a fatal error (also unblocks producer and consumer).
	void SetError(ErrorData error_p);
	bool HasError();
	ErrorData GetError();

	//! Consumer: non-blocking dequeue. Fills `out` and returns CHUNK if one is ready; otherwise returns
	//! EMPTY / FINISHED / ERRORED. Never blocks.
	PopStatus TryPop(QuackStreamChunk &out);
	//! Consumer: bounded wait used by the async wait-task. Blocks until a chunk is available, the
	//! stream is finished/errored, or a short timeout elapses (so the caller can re-check cancellation).
	void WaitForData();

	void SetInsertCount(idx_t count) {
		insert_count = count;
	}
	idx_t InsertCount() const {
		return insert_count;
	}

private:
	std::mutex lock;
	std::condition_variable cv_nonempty;
	std::condition_variable cv_nonfull;
	std::deque<QuackStreamChunk> queue;
	vector<LogicalType> types;
	idx_t capacity;
	bool finished = false;
	bool errored = false;
	ErrorData error;
	idx_t insert_count = 0;
};

//! Process-global registry mapping a stream id to its QuackDataStream, letting the
//! scan_data_from_quack_client table function find the stream the request handlers created.
class QuackStreamRegistry {
public:
	static QuackStreamRegistry &Get();

	static string MakeId(const string &connection_id, hugeint_t query_uuid);

	shared_ptr<QuackDataStream> Create(const string &id, vector<LogicalType> types);
	shared_ptr<QuackDataStream> Find(const string &id);
	void Erase(const string &id);

private:
	std::mutex lock;
	unordered_map<string, shared_ptr<QuackDataStream>> streams;
};

} // namespace duckdb
