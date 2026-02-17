#include "rpc_start_function.hpp"
#include "rpc_storage_extension.hpp"
#include "ssl_key_generator.hpp"

#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/database.hpp"

using namespace duckdb;

static void RpcStartFun(const DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.AllConstant());
	auto listen_value = args.GetValue(0, 0);
	auto listen_string = listen_value.GetValue<string>();
	if (listen_value.IsNull() || listen_string.empty()) {
		throw InvalidInputException("Invalid listen string specified");
	}
	auto &rpc_state = RpcStorageExtensionInfo::GetState(*state.GetContext().db);
	rpc_state.FindOrCreateServer(state.GetContext(), listen_string);
	result.SetValue(0, StringUtil::Format("Listening on %s", listen_string));
}

ScalarFunction RpcStartFunction::GetFunction() {
	auto rpc_start_function = ScalarFunction("rpc_start", {LogicalType::VARCHAR}, LogicalType::VARCHAR, RpcStartFun);
	rpc_start_function.stability = FunctionStability::VOLATILE;
	rpc_start_function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return rpc_start_function;
}

static void RpcGenerateKeysFun(const DataChunk &args, ExpressionState &state, Vector &result) {
	auto &fs = FileSystem::GetFileSystem(state.GetContext());

	auto certificate_directory = SslKeyGenerator::GetDefaultCertificateDirectory(fs);

	auto server_key_file = fs.JoinPath(certificate_directory, "server.pem");
	auto private_key_file = fs.JoinPath(certificate_directory, "private_key.pem");
	auto dh_param_file = fs.JoinPath(certificate_directory, "dh.pem");

	if (fs.FileExists(server_key_file) && fs.FileExists(private_key_file) && fs.FileExists(dh_param_file)) {
		result.SetValue(0,
		                StringUtil::Format("Key files exist in %s - remove to recreate them", certificate_directory));
		return;
	}
	SslKeyGenerator::GenerateSslKeys(server_key_file, private_key_file, dh_param_file, 3650);
	result.SetValue(0, StringUtil::Format("Key files generated in %s", certificate_directory));
}

ScalarFunction RpcGenerateKeysFunction::GetFunction() {
	auto rpc_start_function = ScalarFunction("rpc_generate_keys", {}, LogicalType::VARCHAR, RpcGenerateKeysFun);
	rpc_start_function.stability = FunctionStability::VOLATILE;
	rpc_start_function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return rpc_start_function;
}
