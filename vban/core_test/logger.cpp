#include <vban/lib/jsonconfig.hpp>
#include <vban/lib/logger_mt.hpp>
#include <vban/node/logging.hpp>
#include <vban/secure/utility.hpp>
#include <vban/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <regex>
#include <thread>

using namespace std::chrono_literals;

TEST (logging, serialization)
{
	auto path (vban::unique_path ());
	vban::logging logging1;
	logging1.init (path);
	logging1.ledger_logging_value = !logging1.ledger_logging_value;
	logging1.ledger_duplicate_logging_value = !logging1.ledger_duplicate_logging_value;
	logging1.network_logging_value = !logging1.network_logging_value;
	logging1.network_timeout_logging_value = !logging1.network_timeout_logging_value;
	logging1.network_message_logging_value = !logging1.network_message_logging_value;
	logging1.network_publish_logging_value = !logging1.network_publish_logging_value;
	logging1.network_packet_logging_value = !logging1.network_packet_logging_value;
	logging1.network_keepalive_logging_value = !logging1.network_keepalive_logging_value;
	logging1.network_node_id_handshake_logging_value = !logging1.network_node_id_handshake_logging_value;
	logging1.node_lifetime_tracing_value = !logging1.node_lifetime_tracing_value;
	logging1.insufficient_work_logging_value = !logging1.insufficient_work_logging_value;
	logging1.bulk_pull_logging_value = !logging1.bulk_pull_logging_value;
	logging1.work_generation_time_value = !logging1.work_generation_time_value;
	logging1.log_to_cerr_value = !logging1.log_to_cerr_value;
	logging1.max_size = 10;
	logging1.min_time_between_log_output = 100ms;
	vban::jsonconfig tree;
	logging1.serialize_json (tree);
	vban::logging logging2;
	logging2.init (path);
	bool upgraded (false);
	ASSERT_FALSE (logging2.deserialize_json (upgraded, tree));
	ASSERT_FALSE (upgraded);
	ASSERT_EQ (logging1.ledger_logging_value, logging2.ledger_logging_value);
	ASSERT_EQ (logging1.ledger_duplicate_logging_value, logging2.ledger_duplicate_logging_value);
	ASSERT_EQ (logging1.network_logging_value, logging2.network_logging_value);
	ASSERT_EQ (logging1.network_timeout_logging_value, logging2.network_timeout_logging_value);
	ASSERT_EQ (logging1.network_message_logging_value, logging2.network_message_logging_value);
	ASSERT_EQ (logging1.network_publish_logging_value, logging2.network_publish_logging_value);
	ASSERT_EQ (logging1.network_packet_logging_value, logging2.network_packet_logging_value);
	ASSERT_EQ (logging1.network_keepalive_logging_value, logging2.network_keepalive_logging_value);
	ASSERT_EQ (logging1.network_node_id_handshake_logging_value, logging2.network_node_id_handshake_logging_value);
	ASSERT_EQ (logging1.node_lifetime_tracing_value, logging2.node_lifetime_tracing_value);
	ASSERT_EQ (logging1.insufficient_work_logging_value, logging2.insufficient_work_logging_value);
	ASSERT_EQ (logging1.bulk_pull_logging_value, logging2.bulk_pull_logging_value);
	ASSERT_EQ (logging1.work_generation_time_value, logging2.work_generation_time_value);
	ASSERT_EQ (logging1.log_to_cerr_value, logging2.log_to_cerr_value);
	ASSERT_EQ (logging1.max_size, logging2.max_size);
	ASSERT_EQ (logging1.min_time_between_log_output, logging2.min_time_between_log_output);
}

TEST (logger, changing_time_interval)
{
	auto path1 (vban::unique_path ());
	vban::logging logging;
	logging.init (path1);
	logging.min_time_between_log_output = 0ms;
	vban::logger_mt my_logger (logging.min_time_between_log_output);
	auto error (my_logger.try_log ("logger.changing_time_interval1"));
	ASSERT_FALSE (error);
	my_logger.min_log_delta = 20s;
	error = my_logger.try_log ("logger, changing_time_interval2");
	ASSERT_TRUE (error);
}

TEST (logger, try_log)
{
	auto path1 (vban::unique_path ());
	std::stringstream ss;
	vban::boost_log_cerr_redirect redirect_cerr (ss.rdbuf ());
	vban::logger_mt my_logger (100ms);
	auto output1 = "logger.try_log1";
	auto error (my_logger.try_log (output1));
	ASSERT_FALSE (error);
	auto output2 = "logger.try_log2";
	error = my_logger.try_log (output2);
	ASSERT_TRUE (error); // Fails as it is occuring too soon

	// Sleep for a bit and then confirm
	std::this_thread::sleep_for (100ms);
	error = my_logger.try_log (output2);
	ASSERT_FALSE (error);

	std::string str;
	std::getline (ss, str, '\n');
	ASSERT_STREQ (str.c_str (), output1);
	std::getline (ss, str, '\n');
	ASSERT_STREQ (str.c_str (), output2);
}

TEST (logger, always_log)
{
	auto path1 (vban::unique_path ());
	std::stringstream ss;
	vban::boost_log_cerr_redirect redirect_cerr (ss.rdbuf ());
	vban::logger_mt my_logger (20s); // Make time interval effectively unreachable
	auto output1 = "logger.always_log1";
	auto error (my_logger.try_log (output1));
	ASSERT_FALSE (error);

	// Time is too soon after, so it won't be logged
	auto output2 = "logger.always_log2";
	error = my_logger.try_log (output2);
	ASSERT_TRUE (error);

	// Force it to be logged
	my_logger.always_log (output2);

	std::string str;
	std::getline (ss, str, '\n');
	ASSERT_STREQ (str.c_str (), output1);
	std::getline (ss, str, '\n');
	ASSERT_STREQ (str.c_str (), output2);
}

TEST (logger, stable_filename)
{
	auto path (vban::unique_path ());
	vban::logging logging;

	// Releasing allows setting up logging again
	logging.release_file_sink ();

	logging.stable_log_filename = true;
	logging.init (path);

	vban::logger_mt logger (logging.min_time_between_log_output);
	logger.always_log ("stable1");

	auto log_file = path / "log" / "node.log";

#if BOOST_VERSION >= 107000
	EXPECT_TRUE (boost::filesystem::exists (log_file));
	// Try opening it again
	logging.release_file_sink ();
	logging.init (path);
	logger.always_log ("stable2");
#else
	// When using Boost < 1.70 , behavior is reverted to not using the stable filename
	EXPECT_FALSE (boost::filesystem::exists (log_file));
#endif

	// Reset the logger
	logging.release_file_sink ();
	vban::logging ().init (path);
}