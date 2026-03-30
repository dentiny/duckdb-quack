#include "client.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

using namespace duckdb;

unique_ptr<ProtocolMessage> RpcClient::WaitForMessageInternal(MessageType expected_type) {
	auto result = Receive();
	if (result->Type() != expected_type) {
		if (result->Type() == MessageType::ERROR) {
			throw IOException("Expected %s message, got error message instead: %s", MessageTypeToString(expected_type),
			                  result->Cast<ErrorMessage>().Error().c_str());
		}
		throw IOException("Expected %s message, got %s instead", MessageTypeToString(expected_type),
		                  MessageTypeToString(result->Type()));
	}
	return result;
}

UnixSocketRpcClient::UnixSocketRpcClient(const string &uri_p) : RpcClient(uri_p) {
	D_ASSERT(!StringUtil::StartsWith(uri, "wss://"));

	sockaddr_un remote;
	unix_socket_fd = 0;
	memset(&remote, 0, sizeof(sockaddr_un));

	if ((unix_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		throw IOException("Client: Error on socket() call %s", strerror(errno));
	}
	remote.sun_family = AF_UNIX;
	strcpy(remote.sun_path, uri.c_str());
	auto data_len = SUN_LEN(&remote);

	if (connect(unix_socket_fd, reinterpret_cast<sockaddr *>(&remote), data_len)) {
		throw IOException("Client: Error on connect(%s) call: %s", uri, strerror(errno));
	}
}

UnixSocketRpcClient::~UnixSocketRpcClient() {
	close(unix_socket_fd);
}

unique_ptr<ProtocolMessage> UnixSocketRpcClient::Receive() {
	return ProtocolMessage::FromSocket(unix_socket_fd, read_stream);
}

void UnixSocketRpcClient::Send(unique_ptr<ProtocolMessage> message) {
	D_ASSERT(message);
	message->ToSocket(unix_socket_fd, write_stream);
}

template <class T>
string GetUriPart(T ele) {
	if (ele.afterLast - ele.first < 1) {
		throw InvalidInputException("Invalid URI");
	}
	return string(ele.first, ele.afterLast - ele.first);
}

HttpsRpcClient::HttpsRpcClient(const string &uri_p) : RpcClient(uri_p) {
	D_ASSERT(StringUtil::StartsWith(uri, "https://") || StringUtil::StartsWith(uri, "http://"));

	https_client = make_uniq<duckdb_httplib_openssl::Client>(uri);
	https_client->enable_server_certificate_verification(false);
	https_client->enable_server_hostname_verification(false);
	https_client->set_keep_alive(true);
	https_client->set_follow_location(true);
};

HttpsRpcClient::~HttpsRpcClient() {
	https_client.reset();
}

//  TODO we should probably combine those two into a Request method
void HttpsRpcClient::Send(unique_ptr<ProtocolMessage> message_p) {
	D_ASSERT(message_p);
	message_p->ToMemoryStream(write_stream);
	https_result = https_client->Post("/rpc", (const char *)write_stream.GetData(), write_stream.GetPosition(),
	                                  "application/duckdb");
	if (!https_result) {
		throw IOException("Failed to send message %s ", to_string(https_result.error()));
	}
}
unique_ptr<ProtocolMessage> HttpsRpcClient::Receive() {
	MemoryStream non_owning_read_stream((data_ptr_t)https_result->body.data(), https_result->body.size());
	return ProtocolMessage::FromMemoryStream(non_owning_read_stream);
}

unique_ptr<RpcClient> RpcClient::GetClient(const string &uri) {
	if (StringUtil::StartsWith(uri, "https://") || StringUtil::StartsWith(uri, "http://")) {
		return make_uniq<HttpsRpcClient>(uri);
	} else {
		return make_uniq<UnixSocketRpcClient>(uri);
	}
}
