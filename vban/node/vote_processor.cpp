#include <vban/lib/logger_mt.hpp>
#include <vban/lib/stats.hpp>
#include <vban/lib/threading.hpp>
#include <vban/lib/timer.hpp>
#include <vban/node/active_transactions.hpp>
#include <vban/node/node_observers.hpp>
#include <vban/node/nodeconfig.hpp>
#include <vban/node/online_reps.hpp>
#include <vban/node/repcrawler.hpp>
#include <vban/node/signatures.hpp>
#include <vban/node/vote_processor.hpp>
#include <vban/secure/blockstore.hpp>
#include <vban/secure/common.hpp>
#include <vban/secure/ledger.hpp>

#include <boost/format.hpp>

vban::vote_processor::vote_processor (vban::signature_checker & checker_a, vban::active_transactions & active_a, vban::node_observers & observers_a, vban::stat & stats_a, vban::node_config & config_a, vban::node_flags & flags_a, vban::logger_mt & logger_a, vban::online_reps & online_reps_a, vban::rep_crawler & rep_crawler_a, vban::ledger & ledger_a, vban::network_params & network_params_a) :
	checker (checker_a),
	active (active_a),
	observers (observers_a),
	stats (stats_a),
	config (config_a),
	logger (logger_a),
	online_reps (online_reps_a),
	rep_crawler (rep_crawler_a),
	ledger (ledger_a),
	network_params (network_params_a),
	max_votes (flags_a.vote_processor_capacity),
	started (false),
	stopped (false),
	is_active (false),
	thread ([this] () {
		vban::thread_role::set (vban::thread_role::name::vote_processing);
		process_loop ();
	})
{
	vban::unique_lock<vban::mutex> lock (mutex);
	condition.wait (lock, [&started = started] { return started; });
}

void vban::vote_processor::process_loop ()
{
	vban::timer<std::chrono::milliseconds> elapsed;
	bool log_this_iteration;

	vban::unique_lock<vban::mutex> lock (mutex);
	started = true;

	lock.unlock ();
	condition.notify_all ();
	lock.lock ();

	while (!stopped)
	{
		if (!votes.empty ())
		{
			decltype (votes) votes_l;
			votes_l.swap (votes);

			log_this_iteration = false;
			if (config.logging.network_logging () && votes_l.size () > 50)
			{
				/*
				 * Only log the timing information for this iteration if
				 * there are a sufficient number of items for it to be relevant
				 */
				log_this_iteration = true;
				elapsed.restart ();
			}
			is_active = true;
			lock.unlock ();
			verify_votes (votes_l);
			lock.lock ();
			is_active = false;

			lock.unlock ();
			condition.notify_all ();
			total_processed += votes_l.size ();
			lock.lock ();

			if (log_this_iteration && elapsed.stop () > std::chrono::milliseconds (100))
			{
				logger.try_log (boost::str (boost::format ("Processed %1% votes in %2% milliseconds (rate of %3% votes per second)") % votes_l.size () % elapsed.value ().count () % ((votes_l.size () * 1000ULL) / elapsed.value ().count ())));
			}
		}
		else
		{
			condition.wait (lock);
		}
	}
}

bool vban::vote_processor::vote (std::shared_ptr<vban::vote> const & vote_a, std::shared_ptr<vban::transport::channel> const & channel_a)
{
	debug_assert (channel_a != nullptr);
	bool process (false);
	vban::unique_lock<vban::mutex> lock (mutex);
	if (!stopped)
	{
		// Level 0 (< 0.1%)
		if (votes.size () < 6.0 / 9.0 * max_votes)
		{
			process = true;
		}
		// Level 1 (0.1-1%)
		else if (votes.size () < 7.0 / 9.0 * max_votes)
		{
			process = (representatives_1.find (vote_a->account) != representatives_1.end ());
		}
		// Level 2 (1-5%)
		else if (votes.size () < 8.0 / 9.0 * max_votes)
		{
			process = (representatives_2.find (vote_a->account) != representatives_2.end ());
		}
		// Level 3 (> 5%)
		else if (votes.size () < max_votes)
		{
			process = (representatives_3.find (vote_a->account) != representatives_3.end ());
		}
		if (process)
		{
			votes.emplace_back (vote_a, channel_a);
			lock.unlock ();
			condition.notify_all ();
			// Lock no longer required
		}
		else
		{
			stats.inc (vban::stat::type::vote, vban::stat::detail::vote_overflow);
		}
	}
	return !process;
}

void vban::vote_processor::verify_votes (decltype (votes) const & votes_a)
{
	auto size (votes_a.size ());
	std::vector<unsigned char const *> messages;
	messages.reserve (size);
	std::vector<vban::block_hash> hashes;
	hashes.reserve (size);
	std::vector<size_t> lengths (size, sizeof (vban::block_hash));
	std::vector<unsigned char const *> pub_keys;
	pub_keys.reserve (size);
	std::vector<unsigned char const *> signatures;
	signatures.reserve (size);
	std::vector<int> verifications;
	verifications.resize (size);
	for (auto const & vote : votes_a)
	{
		hashes.push_back (vote.first->hash ());
		messages.push_back (hashes.back ().bytes.data ());
		pub_keys.push_back (vote.first->account.bytes.data ());
		signatures.push_back (vote.first->signature.bytes.data ());
	}
	vban::signature_check_set check = { size, messages.data (), lengths.data (), pub_keys.data (), signatures.data (), verifications.data () };
	checker.verify (check);
	auto i (0);
	for (auto const & vote : votes_a)
	{
		debug_assert (verifications[i] == 1 || verifications[i] == 0);
		if (verifications[i] == 1)
		{
			vote_blocking (vote.first, vote.second, true);
		}
		++i;
	}
}

vban::vote_code vban::vote_processor::vote_blocking (std::shared_ptr<vban::vote> const & vote_a, std::shared_ptr<vban::transport::channel> const & channel_a, bool validated)
{
	auto result (vban::vote_code::invalid);
	if (validated || !vote_a->validate ())
	{
		result = active.vote (vote_a);
		observers.vote.notify (vote_a, channel_a, result);
	}
	std::string status;
	switch (result)
	{
		case vban::vote_code::invalid:
			status = "Invalid";
			stats.inc (vban::stat::type::vote, vban::stat::detail::vote_invalid);
			break;
		case vban::vote_code::replay:
			status = "Replay";
			stats.inc (vban::stat::type::vote, vban::stat::detail::vote_replay);
			break;
		case vban::vote_code::vote:
			status = "Vote";
			stats.inc (vban::stat::type::vote, vban::stat::detail::vote_valid);
			break;
		case vban::vote_code::indeterminate:
			status = "Indeterminate";
			stats.inc (vban::stat::type::vote, vban::stat::detail::vote_indeterminate);
			break;
	}
	if (config.logging.vote_logging ())
	{
		logger.try_log (boost::str (boost::format ("Vote from: %1% timestamp: %2% block(s): %3%status: %4%") % vote_a->account.to_account () % std::to_string (vote_a->timestamp) % vote_a->hashes_string () % status));
	}
	return result;
}

void vban::vote_processor::stop ()
{
	{
		vban::lock_guard<vban::mutex> lock (mutex);
		stopped = true;
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

void vban::vote_processor::flush ()
{
	vban::unique_lock<vban::mutex> lock (mutex);
	while (is_active || !votes.empty ())
	{
		condition.wait (lock);
	}
}

void vban::vote_processor::flush_active ()
{
	vban::unique_lock<vban::mutex> lock (mutex);
	while (is_active)
	{
		condition.wait (lock);
	}
}

size_t vban::vote_processor::size ()
{
	vban::lock_guard<vban::mutex> guard (mutex);
	return votes.size ();
}

bool vban::vote_processor::empty ()
{
	vban::lock_guard<vban::mutex> guard (mutex);
	return votes.empty ();
}

bool vban::vote_processor::half_full ()
{
	return size () >= max_votes / 2;
}

void vban::vote_processor::calculate_weights ()
{
	vban::unique_lock<vban::mutex> lock (mutex);
	if (!stopped)
	{
		representatives_1.clear ();
		representatives_2.clear ();
		representatives_3.clear ();
		auto supply (online_reps.trended ());
		auto rep_amounts = ledger.cache.rep_weights.get_rep_amounts ();
		for (auto const & rep_amount : rep_amounts)
		{
			vban::account const & representative (rep_amount.first);
			auto weight (ledger.weight (representative));
			if (weight > supply / 1000) // 0.1% or above (level 1)
			{
				representatives_1.insert (representative);
				if (weight > supply / 100) // 1% or above (level 2)
				{
					representatives_2.insert (representative);
					if (weight > supply / 20) // 5% or above (level 3)
					{
						representatives_3.insert (representative);
					}
				}
			}
		}
	}
}

std::unique_ptr<vban::container_info_component> vban::collect_container_info (vote_processor & vote_processor, std::string const & name)
{
	size_t votes_count;
	size_t representatives_1_count;
	size_t representatives_2_count;
	size_t representatives_3_count;

	{
		vban::lock_guard<vban::mutex> guard (vote_processor.mutex);
		votes_count = vote_processor.votes.size ();
		representatives_1_count = vote_processor.representatives_1.size ();
		representatives_2_count = vote_processor.representatives_2.size ();
		representatives_3_count = vote_processor.representatives_3.size ();
	}

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "votes", votes_count, sizeof (decltype (vote_processor.votes)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "representatives_1", representatives_1_count, sizeof (decltype (vote_processor.representatives_1)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "representatives_2", representatives_2_count, sizeof (decltype (vote_processor.representatives_2)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "representatives_3", representatives_3_count, sizeof (decltype (vote_processor.representatives_3)::value_type) }));
	return composite;
}
