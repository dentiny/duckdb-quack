#define DUCKDB_EXTENSION_MAIN

#include "rpc_extension.hpp"
#include "duckdb.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#define ASIO_STANDALONE // no boost!

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "websocketpp/config/asio.hpp"
#include "websocketpp/server.hpp"

#include "websocketpp/config/asio_client.hpp"
#include "websocketpp/client.hpp"

#include <iostream>

namespace duckdb {

// TODO split this up in separate messages
// TODO generate this

enum class MessageType : uint8_t { INVALID = 0, BIND = 1, BIND_RESULT = 2, EXECUTE = 3, EXECUTE_RESULT = 4, ERROR = 5 };

struct ProtocolMessage {
	void Serialize(Serializer &serializer) {
		serializer.WriteProperty<uint8_t>(1, "type", static_cast<uint8_t>(type));
		serializer.WriteProperty<string>(2, "query", query);
		serializer.WriteProperty<string>(3, "error", error);
		serializer.WriteProperty<vector<LogicalType>>(4, "types", types);
		serializer.WriteProperty<vector<string>>(5, "names", names);
		if (type == MessageType::EXECUTE_RESULT) {
			serializer.WriteObject(6, "data", [&](Serializer &serializer2) { data.Serialize(serializer2); });
		}
	}

	static unique_ptr<ProtocolMessage> Deserialize(Deserializer &deserializer) {
		auto result = make_uniq<ProtocolMessage>();
		result->type = static_cast<MessageType>(deserializer.ReadProperty<uint8_t>(1, "type"));
		result->query = deserializer.ReadProperty<string>(2, "query");
		result->error = deserializer.ReadProperty<string>(3, "error");
		result->types = deserializer.ReadProperty<vector<LogicalType>>(4, "types");
		result->names = deserializer.ReadProperty<vector<string>>(5, "names");
		if (result->type == MessageType::EXECUTE_RESULT) {
			deserializer.ReadObject(6, "data",
			                        [&](Deserializer &deserializer2) { result->data.Deserialize(deserializer2); });
		}

		return result;
	}
	MessageType type;
	string query;
	string error;
	vector<string> names;
	vector<LogicalType> types;
	DataChunk data;
};

// TODO move this other stuff into the namespace, too
} // namespace duckdb

typedef websocketpp::server<websocketpp::config::asio_tls> server;

using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

// pull out the type of messages sent by our config
typedef websocketpp::config::asio::message_type::ptr message_ptr;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> context_ptr;

static void on_message_server(server *s, websocketpp::connection_hdl hdl, message_ptr msg) {
	duckdb::MemoryStream read_stream(duckdb::data_ptr_cast((void *)msg->get_payload().data()),
	                                 static_cast<idx_t>(msg->get_payload().size()));
	duckdb::BinaryDeserializer deserializer(read_stream);
	auto received_message = duckdb::ProtocolMessage::Deserialize(deserializer);

	printf("message type %lld\n", (uint8_t)received_message->type);

	switch (received_message->type) {
	case duckdb::MessageType::BIND: {
		D_ASSERT(received_message->query.size() > 0);
		printf("BIND %s\n", received_message->query.c_str());

		duckdb::ProtocolMessage response_message;
		response_message.type = duckdb::MessageType::BIND_RESULT;

		// TODO we need a connection object here for the actual bind
		response_message.types.push_back(duckdb::LogicalType::BOOLEAN);
		response_message.names.push_back("dummy");

		duckdb::MemoryStream write_stream; // TODO pass allocator here
		duckdb::BinarySerializer serializer(write_stream);
		serializer.Begin();
		response_message.Serialize(serializer);
		serializer.End();

		try {

			s->send(hdl, write_stream.GetData(), write_stream.GetPosition(), websocketpp::frame::opcode::binary);
		} catch (websocketpp::exception const &e) {
			// TODO we should not fail here but log something
			std::cout << "bind reply failed because: "
			          << "(" << e.what() << ")" << std::endl;
		}
		break;
	}
	default: {
		printf("eeek!\n");
		// TODO complain, but do not exit
		break;
	}
	}

	// try {
	//     s->send(hdl, msg->get_payload(), msg->get_opcode());
	// } catch (websocketpp::exception const & e) {
	// 	// TODO we should not fail here but log something
	//     std::cout << "Echo failed because: "
	//               << "(" << e.what() << ")" << std::endl;
	// }
}

std::string get_password() {
	throw std::runtime_error("get_password called without a valid password");
}

// See https://wiki.mozilla.org/Security/Server_Side_TLS for more details about
// the TLS modes. The code below demonstrates how to implement both the modern
enum tls_mode { MOZILLA_INTERMEDIATE = 1, MOZILLA_MODERN = 2 };

context_ptr on_tls_init_server(tls_mode mode, websocketpp::connection_hdl hdl) {
	namespace asio = websocketpp::lib::asio;

	// std::cout << "on_tls_init called with hdl: " << hdl.lock().get() << std::endl;
	// std::cout << "using TLS mode: " << (mode == MOZILLA_MODERN ? "Mozilla Modern" : "Mozilla Intermediate") <<
	// std::endl;

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
		ctx->use_certificate_chain_file("server.pem");
		ctx->use_private_key_file("key.pem", asio::ssl::context::pem);

		// Example method of generating this file:
		// `openssl dhparam -out dh.pem 2048`
		// Mozilla Intermediate suggests 1024 as the minimum size to use
		// Mozilla Modern suggests 2048 as the minimum size to use.
		ctx->use_tmp_dh_file("dh.pem");

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
			std::cout << "Error setting cipher list" << std::endl;
		}
	} catch (std::exception &e) {
		std::cout << "Exception: " << e.what() << std::endl;
	}
	return ctx;
}

typedef websocketpp::client<websocketpp::config::asio_tls_client> client;

using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

namespace duckdb {

inline void RpcScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	// Create a server endpoint
	server echo_server;
	echo_server.set_access_channels(websocketpp::log::alevel::none);
	// Initialize ASIO
	echo_server.init_asio();

	// Register our message handler
	echo_server.set_message_handler(bind(&on_message_server, &echo_server, ::_1, ::_2));
	// echo_server.set_http_handler(bind(&on_http,&echo_server,::_1));
	echo_server.set_tls_init_handler(bind(&on_tls_init_server, MOZILLA_INTERMEDIATE, ::_1));
	int port = 4242;
	echo_server.listen(port);
	echo_server.start_accept();
	printf("Listening on port %d\n", port);
	echo_server.run();
}

static void RpcTableFun(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	throw NotImplementedException("oof");
}

typedef websocketpp::config::asio_client::message_type::ptr message_ptr;
static void on_message_client(client *c, websocketpp::connection_hdl hdl, message_ptr msg) {

	websocketpp::lib::error_code ec;

	if (ec) {
		std::cout << "Bind failed because: " << ec.message() << std::endl;
	}

	duckdb::MemoryStream read_stream(duckdb::data_ptr_cast((void *)msg->get_payload().data()),
	                                 static_cast<idx_t>(msg->get_payload().size()));
	duckdb::BinaryDeserializer deserializer(read_stream);
	auto received_message = duckdb::ProtocolMessage::Deserialize(deserializer);

	D_ASSERT(received_message->type == MessageType::BIND_RESULT);

	// TODO fill names and types

	c->close(hdl, websocketpp::close::status::normal, "");
}

// static bool verify_certificate(const char * hostname, bool preverified, asio::ssl::verify_context& ctx) {
// 	return true; // cough
// }

static context_ptr on_tls_init_client(const char *hostname, websocketpp::connection_hdl) {
	context_ptr ctx = websocketpp::lib::make_shared<asio::ssl::context>(asio::ssl::context::sslv23);

	try {

		// TODO ???
		ctx->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
		                 asio::ssl::context::no_sslv3 | asio::ssl::context::single_dh_use);

		ctx->set_verify_mode(asio::ssl::verify_none);
		// ctx->set_verify_callback(bind(&verify_certificate, hostname, ::_1, ::_2));

		// // Here we load the CA certificates of all CA's that this client trusts.
		// ctx->load_verify_file("ca-chain.cert.pem");
	} catch (std::exception &e) {
		throw InvalidInputException(e.what());
	}
	return ctx;
}

static void on_open(client *c, websocketpp::connection_hdl hdl) {
	std::string msg = "Hello";

	duckdb::ProtocolMessage bind_message;
	bind_message.type = duckdb::MessageType::BIND;

	// TODO we need a connection object here for the actual bind
	bind_message.query = "SELECT true AS dummy";

	duckdb::MemoryStream write_stream; // TODO pass allocator here
	duckdb::BinarySerializer serializer(write_stream);
	serializer.Begin();
	bind_message.Serialize(serializer);
	serializer.End();
	c->send(hdl, write_stream.GetData(), write_stream.GetPosition(), websocketpp::frame::opcode::binary);
}

void on_fail(client *c, websocketpp::connection_hdl hdl) {
	client::connection_ptr con = c->get_con_from_hdl(hdl);
	// TODO there is more error stuff to expose here
	throw InvalidInputException("RPC request failed: %s", con->get_ec().message().c_str());
}

static unique_ptr<FunctionData> RpcTableBindFun(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	// TODO this should come from input obviously
	std::string uri = "wss://localhost:4242";
	client c;

	try {
		// Set logging to be pretty verbose (everything except message payloads)
		c.set_access_channels(websocketpp::log::alevel::none);
		c.set_error_channels(websocketpp::log::alevel::none);
		// c.clear_access_channels(websocketpp::log::alevel::frame_payload);

		// Initialize ASIO
		c.init_asio();
		c.set_tls_init_handler(bind(&on_tls_init_client, "localhost", ::_1));
		c.set_user_agent("DuckDB");

		// Register our message handler
		c.set_message_handler(bind(&on_message_client, &c, ::_1, ::_2));
		c.set_open_handler(bind(&on_open, &c, ::_1));
		c.set_fail_handler(bind(&on_fail, &c, ::_1));

		websocketpp::lib::error_code ec;
		client::connection_ptr con = c.get_connection(uri, ec);
		if (ec) {
			throw duckdb::InternalException(ec.message());
		}
		c.connect(con);
		c.run();
	} catch (websocketpp::exception const &e) {
		throw duckdb::InvalidInputException(e.what());
	}

	printf("got here\n");

	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("dummy");
	return nullptr;
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
	duckdb::LoadInternal(loader);
}
}
