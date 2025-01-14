#pragma once

#include <vban/node/common.hpp>

#include <future>

namespace vban
{
class bootstrap_attempt;
class bootstrap_client;
class bulk_push_client final : public std::enable_shared_from_this<vban::bulk_push_client>
{
public:
	explicit bulk_push_client (std::shared_ptr<vban::bootstrap_client> const &, std::shared_ptr<vban::bootstrap_attempt> const &);
	~bulk_push_client ();
	void start ();
	void push ();
	void push_block (vban::block const &);
	void send_finished ();
	std::shared_ptr<vban::bootstrap_client> connection;
	std::shared_ptr<vban::bootstrap_attempt> attempt;
	std::promise<bool> promise;
	std::pair<vban::block_hash, vban::block_hash> current_target;
};
class bootstrap_server;
class bulk_push_server final : public std::enable_shared_from_this<vban::bulk_push_server>
{
public:
	explicit bulk_push_server (std::shared_ptr<vban::bootstrap_server> const &);
	void throttled_receive ();
	void receive ();
	void received_type ();
	void received_block (boost::system::error_code const &, size_t, vban::block_type);
	std::shared_ptr<std::vector<uint8_t>> receive_buffer;
	std::shared_ptr<vban::bootstrap_server> connection;
};
}
