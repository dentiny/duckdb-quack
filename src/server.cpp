#include "server.hpp"
#include "message.hpp"
#include "ssl_key_generator.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/render_tree.hpp"

using namespace duckdb;

using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

static std::string get_password() {
	throw InternalException("get_password called without a valid password");
}

// TLS init gunk...
context_ptr RpcServer::OnTlsInit(tls_mode mode, const websocketpp::connection_hdl &hdl) {
	namespace asio = websocketpp::lib::asio;

	context_ptr ctx = websocketpp::lib::make_shared<asio::ssl::context>(asio::ssl::context::sslv23);

	try {
		if (mode == MOZILLA_MODERN) {
			// Modern disables TLSv1
			ctx->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
			                 asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
			                 asio::ssl::context::single_dh_use);
		} else {
			ctx->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
			                 asio::ssl::context::no_sslv3 | asio::ssl::context::single_dh_use);
		}
		ctx->set_password_callback(bind(&get_password));

		// FIXME!!!!
		// auto& fs = FileSystem::GetFileSystem(*db);
		// auto certificate_directory = SslKeyGenerator::GetDefaultCertificateDirectory(fs);

		// auto server_key_file = fs.JoinPath(certificate_directory, "server.pem");
		// auto private_key_file = fs.JoinPath(certificate_directory, "private_key.pem");
		// auto dh_param_file = fs.JoinPath(certificate_directory, "dh.pem");
		//
		// if (!fs.FileExists(server_key_file) || !fs.FileExists(private_key_file) || !fs.FileExists(dh_param_file)) {
		// 	throw InvalidInputException("Certificate files not found in %s - use rpc_generate_keys() to generate them",
		// certificate_directory.c_str());
		// }
		//
		auto server_key_file = "/Users/hannes/.duckdb/extension_data/rpc/server.pem";
		auto private_key_file = "/Users/hannes/.duckdb/extension_data/rpc/private_key.pem";
		auto dh_param_file = "/Users/hannes/.duckdb/extension_data/rpc/dh.pem";

		ctx->use_certificate_chain_file(server_key_file);
		ctx->use_private_key_file(private_key_file, asio::ssl::context::pem);
		ctx->use_tmp_dh_file(dh_param_file);

		std::string ciphers;

		if (mode == MOZILLA_MODERN) {
			ciphers = "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-"
			          "ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:DHE-DSS-AES128-GCM-SHA256:kEDH+AESGCM:ECDHE-"
			          "RSA-AES128-SHA256:ECDHE-ECDSA-AES128-SHA256:ECDHE-RSA-AES128-SHA:ECDHE-ECDSA-AES128-SHA:ECDHE-"
			          "RSA-AES256-SHA384:ECDHE-ECDSA-AES256-SHA384:ECDHE-RSA-AES256-SHA:ECDHE-ECDSA-AES256-SHA:DHE-RSA-"
			          "AES128-SHA256:DHE-RSA-AES128-SHA:DHE-DSS-AES128-SHA256:DHE-RSA-AES256-SHA256:DHE-DSS-AES256-SHA:"
			          "DHE-RSA-AES256-SHA:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!3DES:!MD5:!PSK";
		} else {
			ciphers = "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-"
			          "ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:DHE-DSS-AES128-GCM-SHA256:kEDH+AESGCM:ECDHE-"
			          "RSA-AES128-SHA256:ECDHE-ECDSA-AES128-SHA256:ECDHE-RSA-AES128-SHA:ECDHE-ECDSA-AES128-SHA:ECDHE-"
			          "RSA-AES256-SHA384:ECDHE-ECDSA-AES256-SHA384:ECDHE-RSA-AES256-SHA:ECDHE-ECDSA-AES256-SHA:DHE-RSA-"
			          "AES128-SHA256:DHE-RSA-AES128-SHA:DHE-DSS-AES128-SHA256:DHE-RSA-AES256-SHA256:DHE-DSS-AES256-SHA:"
			          "DHE-RSA-AES256-SHA:AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA256:AES256-SHA256:AES128-SHA:"
			          "AES256-SHA:AES:CAMELLIA:DES-CBC3-SHA:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!MD5:!PSK:!aECDH:!EDH-DSS-"
			          "DES-CBC3-SHA:!EDH-RSA-DES-CBC3-SHA:!KRB5-DES-CBC3-SHA";
		}

		if (SSL_CTX_set_cipher_list(ctx->native_handle(), ciphers.c_str()) != 1) {
			throw InternalException("Error setting cipher list");
		}
	} catch (std::exception &e) {
		throw InternalException(e.what());
	}
	return ctx;
}

RpcServer::RpcServer(ClientContext &context_p) : db(context_p.db) {
}

RpcServer::~RpcServer() {
	websocket_server.stop();
}

void RpcServer::WebsocketListenThread(RpcServer *rpc_server) {
	D_ASSERT(rpc_server);

	rpc_server->websocket_server.start_accept();
	rpc_server->websocket_server.run();
}

void RpcServer::UnixSocketListenThread(RpcServer *rpc_server) {
	D_ASSERT(rpc_server);

	auto server_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (server_socket_fd == -1) {
		throw IOException("Error creating socket");
	}

	sockaddr_un socket_address;
	socket_address.sun_family = AF_UNIX;
	memset(&socket_address, 0, sizeof(sockaddr_un));
	strncpy(socket_address.sun_path, rpc_server->listen_string.c_str(), sizeof(socket_address.sun_path) - 1);

	auto unlink_result = unlink(socket_address.sun_path);
	if (unlink_result && errno != ENOENT) {
		throw IOException("Error cleaning up socket %s: %s", rpc_server->listen_string, strerror(errno));
	}

	if (bind(server_socket_fd, reinterpret_cast<sockaddr *>(&socket_address), SUN_LEN(&socket_address)) ||
	    listen(server_socket_fd, 100 /* TODO: magic constant for connect queue length, should be fine */)) {
		throw IOException("Error listening to socket %s: %s", rpc_server->listen_string, strerror(errno));
	}

	while (true) {
		unsigned int sock_len = 0;
		auto client_socket_fd = accept(server_socket_fd, reinterpret_cast<sockaddr *>(&socket_address), &sock_len);
		if (client_socket_fd == -1) {
			continue;
		}

		std::thread accept_thread([rpc_server, client_socket_fd] {
			bool open = true;
			MemoryStream read_stream, write_stream;

			do {
				try {
					auto received_message = ProtocolMessage::FromSocket(client_socket_fd, read_stream);
					// printf("S RECV %s\n", MessageTypeToString(received_message->Type()).c_str());
					auto response_message = rpc_server->HandleMessage(*received_message);
					// printf("S SEND %s\n", MessageTypeToString(response_message->Type()).c_str());

					response_message->ToSocket(client_socket_fd, write_stream);
				} catch (IOException &) {
					open = false;
				}

			} while (open);
			close(client_socket_fd);
		});
		accept_thread.detach(); // TODO do we need this?
	}
}

void RpcServer::Listen(const string &listen_string_p) {
	if (listen_string_p.empty()) {
		throw InvalidInputException("Empty listen string specified");
	}
	listen_string = listen_string_p;
	if (StringUtil::StartsWith(listen_string, "wss:")) {
		websocket_server.set_access_channels(websocketpp::log::alevel::none);
		websocket_server.set_tls_init_handler(bind(&RpcServer::OnTlsInit, MOZILLA_INTERMEDIATE, ::_1));
		websocket_server.set_message_handler(bind(&RpcServer::OnMessage, this, ::_1, ::_2));
		websocket_server.init_asio();

		// TODO this is overly simplistic but fine for now
		auto listen_port = atoi(StringUtil::Replace(listen_string, "wss://localhost:", "").c_str());
		if (listen_port < 1 || listen_port > 65535) {
			throw InvalidInputException("Invalid port specified for websocket server (%d)", listen_port);
		}
		websocket_server.listen(listen_port);

		listen_thread = std::thread([=]() {
			WebsocketListenThread(this);
			return 1;
		});
	} else {
		listen_thread = std::thread([=]() {
			UnixSocketListenThread(this);
			return 1;
		});
	}
}

// main switcheroo happens here
unique_ptr<ProtocolMessage> RpcServer::HandleMessage(ProtocolMessage &received_message) {
	switch (received_message.Type()) {
	case MessageType::CONNECTION_REQUEST: {
		auto connection_request_message = received_message.Cast<ConnectionRequestMessage>();
		// TODO handle auth here! Only return a connection ID if auth succeeds.
		return make_uniq<ConnectionResponseMessage>(CreateNewConnection());
	}
	case MessageType::PREPARE_REQUEST: {
		auto prepare_request_message = received_message.Cast<PrepareRequestMessage>();
		optional_ptr<RpcConnection> rpc_connection = GetConnection(prepare_request_message.ConnectionId());
		std::unique_lock<std::mutex> lock(rpc_connection->lock);
		rpc_connection->duckdb_query_result.reset();

		auto &client_config = ClientConfig::GetConfig(*rpc_connection->duckdb_connection->context);
		client_config.enable_profiler = true;
		client_config.profiling_coverage = ProfilingCoverage::ALL;
		client_config.profiler_settings = {
		    MetricType::EXTRA_INFO}; // 'EXTRA_INFO' means return estimated cardinality (among other things)
		client_config.emit_profiler_output = false;
		// TODO do we need to restore the previous config after this?

		auto statement = rpc_connection->duckdb_connection->Prepare(prepare_request_message.Query());
		if (statement->HasError()) {
			return make_uniq<ErrorMessage>(statement->GetError());
		}

		vector<Value> params; // TODO allow parameters here?
		auto query_result = statement->PendingQuery(params, true)->Execute();

		if (query_result->HasError()) {
			return make_uniq<ErrorMessage>(query_result->GetError());
		}

		// for some reason the profiler is only alive here
		auto &profiler = QueryProfiler::Get(*rpc_connection->duckdb_connection->context);
		D_ASSERT(profiler.GetRoot() && profiler.GetRoot()->children.size() == 1);
		auto &profiler_info = profiler.GetRoot()->children[0]->GetProfilingInfo();
		auto estimated_cardinality = optional_idx::Invalid();

		// this should always be true, see above
		D_ASSERT(profiler_info.Enabled(profiler_info.settings, MetricType::EXTRA_INFO));
		auto extra_info_map = profiler_info.GetMetricValue<InsertionOrderPreservingMap<string>>(MetricType::EXTRA_INFO);
		if (extra_info_map.find(RenderTreeNode::ESTIMATED_CARDINALITY) != extra_info_map.end()) {
			estimated_cardinality = atoll(extra_info_map[RenderTreeNode::ESTIMATED_CARDINALITY].c_str());
		}

		rpc_connection->duckdb_query_result = std::move(query_result);
		return make_uniq<PrepareResponseMessage>(statement->GetTypes(), statement->GetNames(), estimated_cardinality);
	}

	case MessageType::FETCH_REQUEST: {
		auto fetch_request_message = received_message.Cast<FetchRequestMessage>();
		optional_ptr<RpcConnection> rpc_connection = GetConnection(fetch_request_message.ConnectionId());
		std::unique_lock<std::mutex> lock(rpc_connection->lock);

		if (!rpc_connection->duckdb_query_result) {
			return make_uniq<FetchResponseMessage>(nullptr);
		}
		if (rpc_connection->duckdb_query_result->HasError()) {
			return make_uniq<ErrorMessage>(rpc_connection->duckdb_query_result->GetError());
		}
		auto result_chunk = rpc_connection->duckdb_query_result->Fetch();

		if (!result_chunk && rpc_connection->duckdb_query_result->HasError()) {
			auto error = make_uniq<ErrorMessage>(rpc_connection->duckdb_query_result->GetError());
			rpc_connection->duckdb_query_result.reset();
			return error;
		}
		if (!result_chunk || result_chunk->size() == 0) {
			rpc_connection->duckdb_query_result.reset();
			return make_uniq<FetchResponseMessage>(nullptr);
		}
		return make_uniq<FetchResponseMessage>(std::move(result_chunk));
	}
	default: {
		throw IOException("Unimplemented message type %s", MessageTypeToString(received_message.Type()));
	}
	}
}

void RpcServer::OnMessage(const websocketpp::connection_hdl &hdl, const message_ptr &msg) {
	MemoryStream read_stream((data_ptr_t)msg->get_payload().data(), msg.get()->get_payload().size());
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
