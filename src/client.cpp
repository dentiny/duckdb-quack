#include "client.hpp"
#include "rpc_uri.hpp"

#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

using namespace duckdb;

template <class T>
string GetUriPart(T ele) {
	if (ele.afterLast - ele.first < 1) {
		throw InvalidInputException("Invalid URI");
	}
	return string(ele.first, ele.afterLast - ele.first);
}

HttpsRpcClient::HttpsRpcClient(const RpcUri &uri_p) : RpcClient(uri_p) {};

HttpsRpcClient::~HttpsRpcClient() {
	http_client.reset();
}

unique_ptr<ProtocolMessage> HttpsRpcClient::RequestInternal(unique_ptr<ProtocolMessage> request_message) {
	D_ASSERT(request_message);
	if (!context) {
		throw InvalidInputException("RpcClient requires a ClientContext to make requests");
	}
	request_message->ToMemoryStream(write_stream);

	auto &db = *context->db;
	ExtensionHelper::AutoLoadExtension(db, "httpfs");
	if (!db.ExtensionIsLoaded("httpfs")) {
		throw MissingExtensionException("The rpc extension requires the httpfs extension to be loaded!");
	}

	auto &http_util = HTTPUtil::Get(db);
	auto request_url = uri.Http() + "/rpc";
	if (!http_params) {
		http_params = http_util.InitializeParameters(*context, request_url);
	}
	if (http_client) {
		http_client->Initialize(*http_params);
	}

	HTTPHeaders headers;
	headers.Insert("Content-Type", "application/duckdb");

	PostRequestInfo post_request(request_url, headers, *http_params,
	                             reinterpret_cast<const_data_ptr_t>(write_stream.GetData()),
	                             write_stream.GetPosition());
	unique_ptr<HTTPResponse> response;
	try {
		// funny side-effect: Request will create (and populate) http_client if nullptr is passed
		response = http_util.Request(post_request, http_client);
	} catch (std::exception &e) {
		throw IOException("Failed to send message: %s", e.what());
	}

	if (!response || !response->Success()) {
		string error = response ? response->GetError() : "no response";
		throw IOException("Failed to send message: %s", error);
	}

	MemoryStream non_owning_read_stream((data_ptr_t)post_request.buffer_out.data(), post_request.buffer_out.size());
	return ProtocolMessage::FromMemoryStream(non_owning_read_stream);
}

unique_ptr<RpcClient> RpcClient::GetClient(const RpcUri &uri) {
	return make_uniq<HttpsRpcClient>(uri);
}
