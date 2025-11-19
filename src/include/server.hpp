#pragma once

#define ASIO_STANDALONE // no boost!

#include "websocketpp/config/asio.hpp"
#include "websocketpp/config/asio_client.hpp"
#include "websocketpp/server.hpp"

namespace duckdb {

typedef websocketpp::server<websocketpp::config::asio_tls> server;
typedef websocketpp::config::asio_client::message_type::ptr message_ptr;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> context_ptr;

enum tls_mode { MOZILLA_INTERMEDIATE = 1, MOZILLA_MODERN = 2 };

class ClientContext;

struct RpcServer {
	RpcServer(ClientContext &context_p);

	void Listen(int port);

	void OnMessage(websocketpp::connection_hdl hdl, message_ptr msg);

	ClientContext &context;
	server s;
};
} // namespace duckdb
