#pragma once

#include <vban/lib/config.hpp>
#include <vban/lib/errors.hpp>

#include <string>

namespace vban
{
class jsonconfig;
class tomlconfig;
namespace ipc
{
	/** Base class for transport configurations */
	class ipc_config_transport
	{
	public:
		virtual ~ipc_config_transport () = default;
		bool enabled{ false };
		bool allow_unsafe{ false };
		size_t io_timeout{ 15 };
		long io_threads{ -1 };
	};

	/**
	 * Flatbuffers encoding config. See TOML serialization calls for details about each field.
	 */
	class ipc_config_flatbuffers final
	{
	public:
		bool skip_unexpected_fields_in_json{ true };
		bool verify_buffers{ true };
	};

	/** Domain socket specific transport config */
	class ipc_config_domain_socket : public ipc_config_transport
	{
	public:
		/**
		 * Default domain socket path for Unix systems. Once Boost supports Windows 10 usocks,
		 * this value will be conditional on OS.
		 */
		std::string path{ "/tmp/vban" };

		unsigned json_version () const
		{
			return 1;
		}
	};

	/** TCP specific transport config */
	class ipc_config_tcp_socket : public ipc_config_transport
	{
	public:
		ipc_config_tcp_socket () :
			port (network_constants.default_ipc_port)
		{
		}
		vban::network_constants network_constants;
		/** Listening port */
		uint16_t port;
	};

	/** IPC configuration */
	class ipc_config
	{
	public:
		vban::error deserialize_json (bool & upgraded_a, vban::jsonconfig & json_a);
		vban::error serialize_json (vban::jsonconfig & json) const;
		vban::error deserialize_toml (vban::tomlconfig & toml_a);
		vban::error serialize_toml (vban::tomlconfig & toml) const;
		ipc_config_domain_socket transport_domain;
		ipc_config_tcp_socket transport_tcp;
		ipc_config_flatbuffers flatbuffers;
	};
}
}
