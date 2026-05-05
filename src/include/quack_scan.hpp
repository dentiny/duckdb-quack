#pragma once

#include "quack_uri.hpp"
#include "quack_client.hpp"

namespace duckdb {

struct RpcBindData : FunctionData {
	bool Equals(const FunctionData &other_p) const override {
		throw NotImplementedException("Equals not implemented");
	}

	unique_ptr<FunctionData> Copy() const override {
		throw NotImplementedException("Copy not implemented");
	}
	string connection_id;
	RpcUri server_uri;
	string table_name;
	unique_ptr<RpcClient> initial_client;
	vector<string> column_names;
	vector<LogicalType> column_types;
	vector<unique_ptr<DataChunk>> results;
	bool needs_more_fetch;
	mutex lock;
};

class TableFunction;

class RpcScanFunction {
public:
	static TableFunction GetFunction();
};

class RpcScanByNameFunction {
public:
	static TableFunction GetFunction();
};

} // namespace duckdb
