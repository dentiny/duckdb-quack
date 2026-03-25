#include "rpc_server.hpp"
#include "message.hpp"
#include "ssl_key_generator.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/render_tree.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/planner/binder.hpp"

using namespace duckdb;

using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

static std::string GetCertificatePassword() {
	throw InternalException("get_password called without a valid password");
}

context_ptr WebSocketRpcServer::OnTlsInit(WebSocketRpcServer *rpc_server, const websocketpp::connection_hdl &) {
	D_ASSERT(rpc_server);
	return rpc_server->ctx;
}

WebSocketRpcServer::~WebSocketRpcServer() {
	websocket_server.stop();
	try {
		for (auto &thread : listen_threads) {
			thread.join();
		}
	} catch (std::exception &) {
	}
}

void WebSocketRpcServer::WebsocketListenThread(WebSocketRpcServer *rpc_server) {
	D_ASSERT(rpc_server);
	rpc_server->websocket_server.run();
}

void WebSocketRpcServer::Listen(const string &listen_string_p) {
	if (listen_string_p.empty()) {
		throw InvalidInputException("Empty listen string specified");
	}
	{
		namespace asio = websocketpp::lib::asio;

		ctx = websocketpp::lib::make_shared<asio::ssl::context>(asio::ssl::context::sslv23);

		// TLS init gunk...
		try {
			ctx->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
			                 asio::ssl::context::no_sslv3 | asio::ssl::context::single_dh_use);

			// TODO, make this a secret?
			ctx->set_password_callback(bind(&GetCertificatePassword));

			auto &fs = FileSystem::GetFileSystem(*db);
			auto certificate_directory = SslKeyGenerator::GetDefaultCertificateDirectory(fs);
			auto server_key_file = fs.JoinPath(certificate_directory, "server.pem");
			auto private_key_file = fs.JoinPath(certificate_directory, "private_key.pem");
			auto dh_param_file = fs.JoinPath(certificate_directory, "dh.pem");

			if (!fs.FileExists(server_key_file) || !fs.FileExists(private_key_file) || !fs.FileExists(dh_param_file)) {
				throw InvalidInputException(
				    "Certificate files not found in %s - use rpc_generate_keys() to generate them",
				    certificate_directory.c_str());
			}

			ctx->use_certificate_chain_file(server_key_file);
			ctx->use_private_key_file(private_key_file, asio::ssl::context::pem);
			ctx->use_tmp_dh_file(dh_param_file);

			std::string ciphers;

			ciphers = "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-"
			          "ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:DHE-DSS-AES128-GCM-SHA256:kEDH+AESGCM:ECDHE-"
			          "RSA-AES128-SHA256:ECDHE-ECDSA-AES128-SHA256:ECDHE-RSA-AES128-SHA:ECDHE-ECDSA-AES128-SHA:ECDHE-"
			          "RSA-AES256-SHA384:ECDHE-ECDSA-AES256-SHA384:ECDHE-RSA-AES256-SHA:ECDHE-ECDSA-AES256-SHA:DHE-RSA-"
			          "AES128-SHA256:DHE-RSA-AES128-SHA:DHE-DSS-AES128-SHA256:DHE-RSA-AES256-SHA256:DHE-DSS-AES256-SHA:"
			          "DHE-RSA-AES256-SHA:AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA256:AES256-SHA256:AES128-SHA:"
			          "AES256-SHA:AES:CAMELLIA:DES-CBC3-SHA:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!MD5:!PSK:!aECDH:!EDH-DSS-"
			          "DES-CBC3-SHA:!EDH-RSA-DES-CBC3-SHA:!KRB5-DES-CBC3-SHA";

			if (SSL_CTX_set_cipher_list(ctx->native_handle(), ciphers.c_str()) != 1) {
				throw InternalException("Error setting cipher list");
			}
		} catch (std::exception &e) {
			throw InternalException(e.what());
		}
	}
	D_ASSERT(StringUtil::StartsWith(listen_string_p, "wss://"));
	listen_string = listen_string_p;
	{
		websocket_server.set_access_channels(websocketpp::log::alevel::none);
		websocket_server.set_tls_init_handler(bind(&WebSocketRpcServer::OnTlsInit, this, ::_1));
		websocket_server.set_message_handler(bind(&WebSocketRpcServer::OnMessage, this, ::_1, ::_2));
		websocket_server.set_open_handler(bind(&WebSocketRpcServer::OnOpen, this, ::_1));

		websocket_server.init_asio();

		// TODO this is overly simplistic but fine for now
		auto listen_port = atoi(StringUtil::Replace(listen_string, "wss://localhost:", "").c_str());
		if (listen_port < 1 || listen_port > 65535) {
			throw InvalidInputException("Invalid port specified for websocket server (%d)", listen_port);
		}
		websocket_server.listen(listen_port);
		websocket_server.start_accept();

		// TODO this should eventually become its own config flag, but fow now the max threads is an okay proxy
		idx_t thread_pool_size = DBConfig::GetConfig(*db).GetSystemMaxThreads(FileSystem::GetFileSystem(*db));

		// Create a pool of threads to run all of the io_services.
		for (idx_t i = 0; i < thread_pool_size; i++) {
			listen_threads.push_back(std::thread(WebsocketListenThread, this));
		}
	}
}

// https://github.com/zaphoyd/websocketpp/issues/62#issuecomment-3786986
// trick seems to be to have multiple threads run run()
void WebSocketRpcServer::OnMessage(const websocketpp::connection_hdl &hdl, const message_ptr &msg) {
	auto &request_payload = msg.get()->get_payload();
	MemoryStream read_stream((data_ptr_t)request_payload.data(), request_payload.size());
	auto received_message = ProtocolMessage::FromMemoryStream(read_stream);
	auto response_message = HandleMessage(*received_message);

	MemoryStream write_stream;
	response_message->ToMemoryStream(write_stream);
	try {
		websocket_server.send(hdl, write_stream.GetData(), write_stream.GetPosition(),
		                      websocketpp::frame::opcode::binary);
	} catch (websocketpp::exception const &e) {
		// TODO we should not fail here but log something
		std::cout << "sending reply failed because: "
		          << "(" << e.what() << ")" << std::endl;
	}
}

void WebSocketRpcServer::OnOpen(const websocketpp::connection_hdl &hdl) {
	// hdl.
}
