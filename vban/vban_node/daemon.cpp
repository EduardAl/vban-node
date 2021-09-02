#include <vban/boost/process/child.hpp>
#include <vban/lib/signal_manager.hpp>
#include <vban/lib/threading.hpp>
#include <vban/lib/utility.hpp>
#include <vban/vban_node/daemon.hpp>
#include <vban/node/cli.hpp>
#include <vban/node/daemonconfig.hpp>
#include <vban/node/ipc/ipc_server.hpp>
#include <vban/node/json_handler.hpp>
#include <vban/node/node.hpp>
#include <vban/node/openclwork.hpp>
#include <vban/rpc/rpc.hpp>

#include <boost/format.hpp>

#include <iostream>

namespace
{
volatile sig_atomic_t sig_int_or_term = 0;

constexpr std::size_t OPEN_FILE_DESCRIPTORS_LIMIT = 16384;
}

static void load_and_set_bandwidth_params (std::shared_ptr<vban::node> const & node, boost::filesystem::path const & data_path, vban::node_flags const & flags)
{
	vban::daemon_config config (data_path);

	auto error = vban::read_node_config_toml (data_path, config, flags.config_overrides);
	if (!error)
	{
		error = vban::flags_config_conflicts (flags, config.node);
		if (!error)
		{
			node->set_bandwidth_params (config.node.bandwidth_limit, config.node.bandwidth_limit_burst_ratio);
		}
	}
}

void vban_daemon::daemon::run (boost::filesystem::path const & data_path, vban::node_flags const & flags)
{
	// Override segmentation fault and aborting.
	vban::signal_manager sigman;
	sigman.register_signal_handler (SIGSEGV, sigman.get_debug_files_handler (), false);
	sigman.register_signal_handler (SIGABRT, sigman.get_debug_files_handler (), false);

	boost::filesystem::create_directories (data_path);
	boost::system::error_code error_chmod;
	vban::set_secure_perm_directory (data_path, error_chmod);
	std::unique_ptr<vban::thread_runner> runner;
	vban::daemon_config config (data_path);
	auto error = vban::read_node_config_toml (data_path, config, flags.config_overrides);
	vban::set_use_memory_pools (config.node.use_memory_pools);
	if (!error)
	{
		error = vban::flags_config_conflicts (flags, config.node);
	}
	if (!error)
	{
		config.node.logging.init (data_path);
		vban::logger_mt logger{ config.node.logging.min_time_between_log_output };
		boost::asio::io_context io_ctx;
		auto opencl (vban::opencl_work::create (config.opencl_enable, config.opencl, logger));
		vban::work_pool opencl_work (config.node.work_threads, config.node.pow_sleep_interval, opencl ? [&opencl] (vban::work_version const version_a, vban::root const & root_a, uint64_t difficulty_a, std::atomic<int> & ticket_a) {
			return opencl->generate_work (version_a, root_a, difficulty_a, ticket_a);
		}
																									  : std::function<boost::optional<uint64_t> (vban::work_version const, vban::root const &, uint64_t, std::atomic<int> &)> (nullptr));
		try
		{
			// This avoid a blank prompt during any node initialization delays
			auto initialization_text = "Starting up Vban node...";
			std::cout << initialization_text << std::endl;
			logger.always_log (initialization_text);

			vban::set_file_descriptor_limit (OPEN_FILE_DESCRIPTORS_LIMIT);
			logger.always_log (boost::format ("Open file descriptors limit is %1%") % vban::get_file_descriptor_limit ());

			auto node (std::make_shared<vban::node> (io_ctx, data_path, config.node, opencl_work, flags));
			if (!node->init_error ())
			{
				auto network_label = node->network_params.network.get_current_network_as_string ();
				std::cout << "Network: " << network_label << ", version: " << VBAN_VERSION_STRING << "\n"
						  << "Path: " << node->application_path.string () << "\n"
						  << "Build Info: " << BUILD_INFO << "\n"
						  << "Database backend: " << node->store.vendor_get () << std::endl;
				auto voting (node->wallets.reps ().voting);
				if (voting > 1)
				{
					std::cout << "Voting with more than one representative can limit performance: " << voting << " representatives are configured" << std::endl;
				}
				node->start ();
				vban::ipc::ipc_server ipc_server (*node, config.rpc);
				std::unique_ptr<boost::process::child> rpc_process;
				std::unique_ptr<boost::process::child> vban_pow_server_process;

				/*if (config.pow_server.enable)
				{
					if (!boost::filesystem::exists (config.pow_server.pow_server_path))
					{
						std::cerr << std::string ("vban_pow_server is configured to start as a child process, however the file cannot be found at: ") + config.pow_server.pow_server_path << std::endl;
						std::exit (1);
					}

					vban_pow_server_process = std::make_unique<boost::process::child> (config.pow_server.pow_server_path, "--config_path", data_path / "config-nano-pow-server.toml");
				}*/

				std::unique_ptr<vban::rpc> rpc;
				std::unique_ptr<vban::rpc_handler_interface> rpc_handler;
				if (config.rpc_enable)
				{
					if (!config.rpc.child_process.enable)
					{
						// Launch rpc in-process
						vban::rpc_config rpc_config;
						auto error = vban::read_rpc_config_toml (data_path, rpc_config, flags.rpc_config_overrides);
						if (error)
						{
							std::cout << error.get_message () << std::endl;
							std::exit (1);
						}
						rpc_handler = std::make_unique<vban::inprocess_rpc_handler> (*node, ipc_server, config.rpc, [&ipc_server, &workers = node->workers, &io_ctx] () {
							ipc_server.stop ();
							workers.add_timed_task (std::chrono::steady_clock::now () + std::chrono::seconds (3), [&io_ctx] () {
								io_ctx.stop ();
							});
						});
						rpc = vban::get_rpc (io_ctx, rpc_config, *rpc_handler);
						rpc->start ();
					}
					else
					{
						// Spawn a child rpc process
						if (!boost::filesystem::exists (config.rpc.child_process.rpc_path))
						{
							throw std::runtime_error (std::string ("RPC is configured to spawn a new process however the file cannot be found at: ") + config.rpc.child_process.rpc_path);
						}

						auto network = node->network_params.network.get_current_network_as_string ();
						rpc_process = std::make_unique<boost::process::child> (config.rpc.child_process.rpc_path, "--daemon", "--data_path", data_path, "--network", network);
					}
				}

				debug_assert (!vban::signal_handler_impl);
				vban::signal_handler_impl = [&io_ctx] () {
					io_ctx.stop ();
					sig_int_or_term = 1;
				};

				// keep trapping Ctrl-C to avoid a second Ctrl-C interrupting tasks started by the first
				sigman.register_signal_handler (SIGINT, &vban::signal_handler, true);

				// sigterm is less likely to come in bunches so only trap it once
				sigman.register_signal_handler (SIGTERM, &vban::signal_handler, false);

#ifndef _WIN32
				// on sighup we should reload the bandwidth parameters
				std::function<void (int)> sighup_signal_handler ([&node, &data_path, &flags] (int signum) {
					debug_assert (signum == SIGHUP);
					load_and_set_bandwidth_params (node, data_path, flags);
				});
				sigman.register_signal_handler (SIGHUP, sighup_signal_handler, true);
#endif

				runner = std::make_unique<vban::thread_runner> (io_ctx, node->config.io_threads);
				runner->join ();

				if (sig_int_or_term == 1)
				{
					ipc_server.stop ();
					node->stop ();
					if (rpc)
					{
						rpc->stop ();
					}
				}
				if (rpc_process)
				{
					rpc_process->wait ();
				}
			}
			else
			{
				std::cerr << "Error initializing node\n";
			}
		}
		catch (const std::runtime_error & e)
		{
			std::cerr << "Error while running node (" << e.what () << ")\n";
		}
	}
	else
	{
		std::cerr << "Error deserializing config: " << error.get_message () << std::endl;
	}
}