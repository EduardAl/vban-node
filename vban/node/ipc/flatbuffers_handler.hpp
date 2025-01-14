#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace flatbuffers
{
class FlatBufferBuilder;
class Parser;
}
namespace vban
{
class node;
namespace ipc
{
	class subscriber;
	class ipc_config;
	class ipc_server;
	/**
	 * This handler sits between the IPC server and the action handler. Its job is to deserialize
	 * Flatbuffers in binary and json formats into high level message objects. These messages are
	 * then used to dispatch the correct action handler.
	 * @throws Methods of this class throw vban::error on failure.
	 * @note This class is not thread safe; use one instance per session/thread.
	 */
	class flatbuffers_handler final : public std::enable_shared_from_this<flatbuffers_handler>
	{
	public:
		/**
		 * Constructs the handler.
		 * @param node_a Node
		 * @param subscriber Subscriber instance
		 * @param ipc_server_a Optional IPC server (may be nullptr, i.e when calling through the RPC gateway)
		 */
		flatbuffers_handler (vban::node & node_a, vban::ipc::ipc_server & ipc_server_a, std::shared_ptr<vban::ipc::subscriber> const & subscriber_a, vban::ipc::ipc_config const & ipc_config_a);

		/**
		 * Deserialize flatbuffer message, look up and call the action handler, then call the response handler with a
		 * FlatBufferBuilder to allow for zero-copy transfers of data.
		 * @param response_handler Receives a shared pointer to the flatbuffer builder, from which the buffer and size can be queried
		 * @throw Throws std:runtime_error on deserialization or processing errors
		 */
		void process (const uint8_t * message_buffer_a, size_t buffer_size_a, std::function<void (std::shared_ptr<flatbuffers::FlatBufferBuilder> const &)> const & response_handler);

		/**
		 * Parses a JSON encoded requests into Flatbuffer format, calls process(), yields the result as a JSON string
		 */
		void process_json (const uint8_t * message_buffer_a, size_t buffer_size_a, std::function<void (std::shared_ptr<std::string> const &)> const & response_handler);

		/**
		 * Creates a Flatbuffers parser with the schema preparsed. This can then be used to parse and produce JSON.
		 */
		static std::shared_ptr<flatbuffers::Parser> make_flatbuffers_parser (vban::ipc::ipc_config const & ipc_config_a);

	private:
		std::shared_ptr<flatbuffers::Parser> parser;
		vban::node & node;
		vban::ipc::ipc_server & ipc_server;
		std::weak_ptr<vban::ipc::subscriber> subscriber;
		vban::ipc::ipc_config const & ipc_config;
	};
}
}
