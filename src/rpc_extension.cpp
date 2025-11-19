#define DUCKDB_EXTENSION_MAIN

#include "rpc_extension.hpp"
#include "duckdb.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#define ASIO_STANDALONE // no boost!

#include "message.hpp"
#include "server.hpp"
#include "client.hpp"

typedef websocketpp::client<websocketpp::config::asio_tls_client> client;
typedef websocketpp::config::asio_client::message_type::ptr message_ptr;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> context_ptr;

using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

namespace duckdb {

inline void RpcScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	RpcServer rcp_server(state.GetContext()); // TODO start this in a background thread and don't block the rest
	rcp_server.Listen(4242);
}

struct RpcTableBindData : FunctionData {
	explicit RpcTableBindData() {
	}

	unique_ptr<RpcClient> client;
	string query;

	bool Equals(const FunctionData &other_p) const override {
		throw NotImplementedException("Equals not implemented");
	}

	unique_ptr<FunctionData> Copy() const override {
		throw NotImplementedException("Copy not implemented");
	}
};

static unique_ptr<FunctionData> RpcTableBindFun(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {

	// Set logging to be pretty verbose (everything except message payloads)
	auto uri = input.inputs[0].GetValue<string>();
	auto query = input.inputs[1].GetValue<string>();
	auto client = make_uniq<RpcClient>(uri);

	auto bind_message = make_uniq<ProtocolMessage>();
	bind_message->type = MessageType::BIND;
	bind_message->query = query;

	client->Schedule(std::move(bind_message));

	auto bind_response = client->WaitForMessage();
	if (bind_response->type != MessageType::BIND_RESULT) {
		throw InvalidInputException("Expected bind result message");
	}
	return_types = bind_response->types;
	names = bind_response->names;

	auto res = make_uniq<RpcTableBindData>();
	res->client = std::move(client);
	res->query = query;

	return res;
}

static void RpcTableFun(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<RpcTableBindData>();
	auto &client = *bind_data.client;

	auto execute_message = make_uniq<ProtocolMessage>();
	execute_message->type = MessageType::EXECUTE;
	execute_message->query = bind_data.query;

	client.Send(std::move(execute_message));
	auto execute_response = client.WaitForMessage();

	output.Reference(*execute_response->data);
	output.SetCardinality(execute_response->data->size());
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto rpc_scalar_function = ScalarFunction("start_rpc_server", {}, LogicalType::VARCHAR, RpcScalarFun);
	loader.RegisterFunction(rpc_scalar_function);

	auto rpc_table_function =
	    TableFunction("call_rpc_server", {LogicalType::VARCHAR, LogicalType::VARCHAR}, RpcTableFun, RpcTableBindFun);
	loader.RegisterFunction(rpc_table_function);
}

void RpcExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string RpcExtension::Name() {
	return "rpc";
}

std::string RpcExtension::Version() const {
#ifdef EXT_VERSION_RPC
	return EXT_VERSION_RPC;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(rpc, loader) {
	LoadInternal(loader);
}
}
