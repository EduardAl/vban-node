#include <vban/crypto_lib/random_pool.hpp>
#include <vban/lib/utility.hpp>
#include <vban/node/common.hpp>
#include <vban/node/lmdb/lmdb.hpp>
#include <vban/node/lmdb/lmdb_iterator.hpp>
#include <vban/node/lmdb/wallet_value.hpp>
#include <vban/secure/buffer.hpp>
#include <vban/secure/versioning.hpp>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/polymorphic_cast.hpp>

#include <queue>

namespace vban
{
template <>
void * mdb_val::data () const
{
	return value.mv_data;
}

template <>
size_t mdb_val::size () const
{
	return value.mv_size;
}

template <>
mdb_val::db_val (size_t size_a, void * data_a) :
	value ({ size_a, data_a })
{
}

template <>
void mdb_val::convert_buffer_to_value ()
{
	value = { buffer->size (), const_cast<uint8_t *> (buffer->data ()) };
}
}

vban::mdb_store::mdb_store (vban::logger_mt & logger_a, boost::filesystem::path const & path_a, vban::txn_tracking_config const & txn_tracking_config_a, std::chrono::milliseconds block_processor_batch_max_time_a, vban::lmdb_config const & lmdb_config_a, bool backup_before_upgrade_a) :
	logger (logger_a),
	env (error, path_a, vban::mdb_env::options::make ().set_config (lmdb_config_a).set_use_no_mem_init (true)),
	mdb_txn_tracker (logger_a, txn_tracking_config_a, block_processor_batch_max_time_a),
	txn_tracking_enabled (txn_tracking_config_a.enable)
{
	if (!error)
	{
		auto is_fully_upgraded (false);
		auto is_fresh_db (false);
		{
			auto transaction (tx_begin_read ());
			auto err = mdb_dbi_open (env.tx (transaction), "meta", 0, &meta);
			is_fresh_db = err != MDB_SUCCESS;
			if (err == MDB_SUCCESS)
			{
				is_fully_upgraded = (version_get (transaction) == version);
				mdb_dbi_close (env, meta);
			}
		}

		// Only open a write lock when upgrades are needed. This is because CLI commands
		// open inactive nodes which can otherwise be locked here if there is a long write
		// (can be a few minutes with the --fast_bootstrap flag for instance)
		if (!is_fully_upgraded)
		{
			vban::network_constants network_constants;
			if (!is_fresh_db)
			{
				if (!network_constants.is_dev_network ())
				{
					std::cout << "Upgrade in progress..." << std::endl;
				}
				if (backup_before_upgrade_a)
				{
					create_backup_file (env, path_a, logger_a);
				}
			}
			auto needs_vacuuming = false;
			{
				auto transaction (tx_begin_write ());
				open_databases (error, transaction, MDB_CREATE);
				if (!error)
				{
					error |= do_upgrades (transaction, needs_vacuuming);
				}
			}

			if (needs_vacuuming && !network_constants.is_dev_network ())
			{
				logger.always_log ("Preparing vacuum...");
				auto vacuum_success = vacuum_after_upgrade (path_a, lmdb_config_a);
				logger.always_log (vacuum_success ? "Vacuum succeeded." : "Failed to vacuum. (Optional) Ensure enough disk space is available for a copy of the database and try to vacuum after shutting down the node");
			}
		}
		else
		{
			auto transaction (tx_begin_read ());
			open_databases (error, transaction, 0);
		}
	}
}

bool vban::mdb_store::vacuum_after_upgrade (boost::filesystem::path const & path_a, vban::lmdb_config const & lmdb_config_a)
{
	// Vacuum the database. This is not a required step and may actually fail if there isn't enough storage space.
	auto vacuum_path = path_a.parent_path () / "vacuumed.ldb";

	auto vacuum_success = copy_db (vacuum_path);
	if (vacuum_success)
	{
		// Need to close the database to release the file handle
		mdb_env_sync (env.environment, true);
		mdb_env_close (env.environment);
		env.environment = nullptr;

		// Replace the ledger file with the vacuumed one
		boost::filesystem::rename (vacuum_path, path_a);

		// Set up the environment again
		auto options = vban::mdb_env::options::make ()
					   .set_config (lmdb_config_a)
					   .set_use_no_mem_init (true);
		env.init (error, path_a, options);
		if (!error)
		{
			auto transaction (tx_begin_read ());
			open_databases (error, transaction, 0);
		}
	}
	else
	{
		// The vacuum file can be in an inconsistent state if there wasn't enough space to create it
		boost::filesystem::remove (vacuum_path);
	}
	return vacuum_success;
}

void vban::mdb_store::serialize_mdb_tracker (boost::property_tree::ptree & json, std::chrono::milliseconds min_read_time, std::chrono::milliseconds min_write_time)
{
	mdb_txn_tracker.serialize_json (json, min_read_time, min_write_time);
}

void vban::mdb_store::serialize_memory_stats (boost::property_tree::ptree & json)
{
	MDB_stat stats;
	auto status (mdb_env_stat (env.environment, &stats));
	release_assert (status == 0);
	json.put ("branch_pages", stats.ms_branch_pages);
	json.put ("depth", stats.ms_depth);
	json.put ("entries", stats.ms_entries);
	json.put ("leaf_pages", stats.ms_leaf_pages);
	json.put ("overflow_pages", stats.ms_overflow_pages);
	json.put ("page_size", stats.ms_psize);
}

vban::write_transaction vban::mdb_store::tx_begin_write (std::vector<vban::tables> const &, std::vector<vban::tables> const &)
{
	return env.tx_begin_write (create_txn_callbacks ());
}

vban::read_transaction vban::mdb_store::tx_begin_read () const
{
	return env.tx_begin_read (create_txn_callbacks ());
}

std::string vban::mdb_store::vendor_get () const
{
	return boost::str (boost::format ("LMDB %1%.%2%.%3%") % MDB_VERSION_MAJOR % MDB_VERSION_MINOR % MDB_VERSION_PATCH);
}

vban::mdb_txn_callbacks vban::mdb_store::create_txn_callbacks () const
{
	vban::mdb_txn_callbacks mdb_txn_callbacks;
	if (txn_tracking_enabled)
	{
		mdb_txn_callbacks.txn_start = ([&mdb_txn_tracker = mdb_txn_tracker] (const vban::transaction_impl * transaction_impl) {
			mdb_txn_tracker.add (transaction_impl);
		});
		mdb_txn_callbacks.txn_end = ([&mdb_txn_tracker = mdb_txn_tracker] (const vban::transaction_impl * transaction_impl) {
			mdb_txn_tracker.erase (transaction_impl);
		});
	}
	return mdb_txn_callbacks;
}

void vban::mdb_store::open_databases (bool & error_a, vban::transaction const & transaction_a, unsigned flags)
{
	error_a |= mdb_dbi_open (env.tx (transaction_a), "frontiers", flags, &frontiers) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "unchecked", flags, &unchecked) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "online_weight", flags, &online_weight) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "meta", flags, &meta) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "peers", flags, &peers) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "pruned", flags, &pruned) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "confirmation_height", flags, &confirmation_height) != 0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "accounts", flags, &accounts_v0) != 0;
	accounts = accounts_v0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "pending", flags, &pending_v0) != 0;
	pending = pending_v0;
	error_a |= mdb_dbi_open (env.tx (transaction_a), "final_votes", flags, &final_votes) != 0;

	auto version_l = version_get (transaction_a);
	if (version_l < 19)
	{
		// These legacy (and state) block databases are no longer used, but need opening so they can be deleted during an upgrade
		error_a |= mdb_dbi_open (env.tx (transaction_a), "send", flags, &send_blocks) != 0;
		error_a |= mdb_dbi_open (env.tx (transaction_a), "receive", flags, &receive_blocks) != 0;
		error_a |= mdb_dbi_open (env.tx (transaction_a), "open", flags, &open_blocks) != 0;
		error_a |= mdb_dbi_open (env.tx (transaction_a), "change", flags, &change_blocks) != 0;
		if (version_l >= 15)
		{
			error_a |= mdb_dbi_open (env.tx (transaction_a), "state_blocks", flags, &state_blocks) != 0;
			state_blocks_v0 = state_blocks;
		}
	}
	else
	{
		error_a |= mdb_dbi_open (env.tx (transaction_a), "blocks", MDB_CREATE, &blocks) != 0;
	}

	if (version_l < 16)
	{
		// The representation database is no longer used, but needs opening so that it can be deleted during an upgrade
		error_a |= mdb_dbi_open (env.tx (transaction_a), "representation", flags, &representation) != 0;
	}

	if (version_l < 15)
	{
		// These databases are no longer used, but need opening so they can be deleted during an upgrade
		error_a |= mdb_dbi_open (env.tx (transaction_a), "state", flags, &state_blocks_v0) != 0;
		state_blocks = state_blocks_v0;
		error_a |= mdb_dbi_open (env.tx (transaction_a), "accounts_v1", flags, &accounts_v1) != 0;
		error_a |= mdb_dbi_open (env.tx (transaction_a), "pending_v1", flags, &pending_v1) != 0;
		error_a |= mdb_dbi_open (env.tx (transaction_a), "state_v1", flags, &state_blocks_v1) != 0;
	}
}

bool vban::mdb_store::do_upgrades (vban::write_transaction & transaction_a, bool & needs_vacuuming)
{
	auto error (false);
	auto version_l = version_get (transaction_a);
	switch (version_l)
	{
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
		case 8:
		case 9:
		case 10:
		case 11:
		case 12:
		case 13:
			logger.always_log (boost::str (boost::format ("The version of the ledger (%1%) is lower than the minimum (%2%) which is supported for upgrades. Either upgrade to a v19, v20 or v21 node first or delete the ledger.") % version_l % minimum_version));
			error = true;
			break;
		case 14:
			upgrade_v14_to_v15 (transaction_a);
			[[fallthrough]];
			// Upgrades to version 16, 17 & 18 are all part of the v21 node release
		case 15:
			upgrade_v15_to_v16 (transaction_a);
			[[fallthrough]];
		case 16:
			upgrade_v16_to_v17 (transaction_a);
			[[fallthrough]];
		case 17:
			upgrade_v17_to_v18 (transaction_a);
			[[fallthrough]];
			// Upgrades to version 19 & 20 are both part of the v22 node release
		case 18:
			upgrade_v18_to_v19 (transaction_a);
			needs_vacuuming = true;
			[[fallthrough]];
		case 19:
			upgrade_v19_to_v20 (transaction_a);
			[[fallthrough]];
		case 20:
			upgrade_v20_to_v21 (transaction_a);
			[[fallthrough]];
		case 21:
			break;
		default:
			logger.always_log (boost::str (boost::format ("The version of the ledger (%1%) is too high for this node") % version_l));
			error = true;
			break;
	}
	return error;
}

void vban::mdb_store::upgrade_v14_to_v15 (vban::write_transaction & transaction_a)
{
	logger.always_log ("Preparing v14 to v15 database upgrade...");

	std::vector<std::pair<vban::account, vban::account_info>> account_infos;
	upgrade_counters account_counters (count (transaction_a, accounts_v0), count (transaction_a, accounts_v1));
	account_infos.reserve (account_counters.before_v0 + account_counters.before_v1);

	vban::mdb_merge_iterator<vban::account, vban::account_info_v14> i_account (transaction_a, accounts_v0, accounts_v1);
	vban::mdb_merge_iterator<vban::account, vban::account_info_v14> n_account{};
	for (; i_account != n_account; ++i_account)
	{
		vban::account account (i_account->first);
		vban::account_info_v14 account_info_v14 (i_account->second);

		// Upgrade rep block to representative account
		auto rep_block = block_get_v14 (transaction_a, account_info_v14.rep_block);
		release_assert (rep_block != nullptr);
		account_infos.emplace_back (account, vban::account_info{ account_info_v14.head, rep_block->representative (), account_info_v14.open_block, account_info_v14.balance, account_info_v14.modified, account_info_v14.block_count, i_account.from_first_database ? vban::epoch::epoch_0 : vban::epoch::epoch_1 });
		// Move confirmation height from account_info database to its own table
		mdb_put (env.tx (transaction_a), confirmation_height, vban::mdb_val (account), vban::mdb_val (account_info_v14.confirmation_height), MDB_APPEND);
		i_account.from_first_database ? ++account_counters.after_v0 : ++account_counters.after_v1;
	}

	logger.always_log ("Finished extracting confirmation height to its own database");

	debug_assert (account_counters.are_equal ());
	// No longer need accounts_v1, keep v0 but clear it
	mdb_drop (env.tx (transaction_a), accounts_v1, 1);
	mdb_drop (env.tx (transaction_a), accounts_v0, 0);

	for (auto const & account_account_info_pair : account_infos)
	{
		auto const & account_info (account_account_info_pair.second);
		mdb_put (env.tx (transaction_a), accounts, vban::mdb_val (account_account_info_pair.first), vban::mdb_val (account_info), MDB_APPEND);
	}

	logger.always_log ("Epoch merge upgrade: Finished accounts, now doing state blocks");

	account_infos.clear ();

	// Have to create a new database as we are iterating over the existing ones and want to use MDB_APPEND for quick insertion
	MDB_dbi state_blocks_new;
	mdb_dbi_open (env.tx (transaction_a), "state_blocks", MDB_CREATE, &state_blocks_new);

	upgrade_counters state_counters (count (transaction_a, state_blocks_v0), count (transaction_a, state_blocks_v1));

	vban::mdb_merge_iterator<vban::block_hash, vban::state_block_w_sideband_v14> i_state (transaction_a, state_blocks_v0, state_blocks_v1);
	vban::mdb_merge_iterator<vban::block_hash, vban::state_block_w_sideband_v14> n_state{};
	auto num = 0u;
	for (; i_state != n_state; ++i_state, ++num)
	{
		vban::block_hash hash (i_state->first);
		vban::state_block_w_sideband_v14 state_block_w_sideband_v14 (i_state->second);
		auto & sideband_v14 = state_block_w_sideband_v14.sideband;

		vban::block_sideband_v18 sideband (sideband_v14.account, sideband_v14.successor, sideband_v14.balance, sideband_v14.height, sideband_v14.timestamp, i_state.from_first_database ? vban::epoch::epoch_0 : vban::epoch::epoch_1, false, false, false);

		// Write these out
		std::vector<uint8_t> data;
		{
			vban::vectorstream stream (data);
			state_block_w_sideband_v14.state_block->serialize (stream);
			sideband.serialize (stream, sideband_v14.type);
		}

		vban::mdb_val value{ data.size (), (void *)data.data () };
		auto s = mdb_put (env.tx (transaction_a), state_blocks_new, vban::mdb_val (hash), value, MDB_APPEND);
		release_assert_success (s);

		// Every so often output to the log to indicate progress
		constexpr auto output_cutoff = 1000000;
		if (num % output_cutoff == 0 && num != 0)
		{
			logger.always_log (boost::str (boost::format ("Database epoch merge upgrade %1% million state blocks upgraded") % (num / output_cutoff)));
		}
		i_state.from_first_database ? ++state_counters.after_v0 : ++state_counters.after_v1;
	}

	debug_assert (state_counters.are_equal ());
	logger.always_log ("Epoch merge upgrade: Finished state blocks, now doing pending blocks");

	state_blocks = state_blocks_new;

	// No longer need states v0/v1 databases
	mdb_drop (env.tx (transaction_a), state_blocks_v1, 1);
	mdb_drop (env.tx (transaction_a), state_blocks_v0, 1);

	state_blocks_v0 = state_blocks;

	upgrade_counters pending_counters (count (transaction_a, pending_v0), count (transaction_a, pending_v1));
	std::vector<std::pair<vban::pending_key, vban::pending_info>> pending_infos;
	pending_infos.reserve (pending_counters.before_v0 + pending_counters.before_v1);

	vban::mdb_merge_iterator<vban::pending_key, vban::pending_info_v14> i_pending (transaction_a, pending_v0, pending_v1);
	vban::mdb_merge_iterator<vban::pending_key, vban::pending_info_v14> n_pending{};
	for (; i_pending != n_pending; ++i_pending)
	{
		vban::pending_info_v14 info (i_pending->second);
		pending_infos.emplace_back (vban::pending_key (i_pending->first), vban::pending_info{ info.source, info.amount, i_pending.from_first_database ? vban::epoch::epoch_0 : vban::epoch::epoch_1 });
		i_pending.from_first_database ? ++pending_counters.after_v0 : ++pending_counters.after_v1;
	}

	debug_assert (pending_counters.are_equal ());

	// No longer need the pending v1 table
	mdb_drop (env.tx (transaction_a), pending_v1, 1);
	mdb_drop (env.tx (transaction_a), pending_v0, 0);

	for (auto const & pending_key_pending_info_pair : pending_infos)
	{
		mdb_put (env.tx (transaction_a), pending, vban::mdb_val (pending_key_pending_info_pair.first), vban::mdb_val (pending_key_pending_info_pair.second), MDB_APPEND);
	}

	version_put (transaction_a, 15);
	logger.always_log ("Finished epoch merge upgrade");
}

void vban::mdb_store::upgrade_v15_to_v16 (vban::write_transaction const & transaction_a)
{
	// Representation table is no longer used
	debug_assert (representation != 0);
	if (representation != 0)
	{
		auto status (mdb_drop (env.tx (transaction_a), representation, 1));
		release_assert (status == MDB_SUCCESS);
		representation = 0;
	}
	version_put (transaction_a, 16);
}

void vban::mdb_store::upgrade_v16_to_v17 (vban::write_transaction const & transaction_a)
{
	logger.always_log ("Preparing v16 to v17 database upgrade...");

	auto account_info_i = accounts_begin (transaction_a);
	auto account_info_n = accounts_end ();

	// Set the confirmed frontier for each account in the confirmation height table
	std::vector<std::pair<vban::account, vban::confirmation_height_info>> confirmation_height_infos;
	auto num = 0u;
	for (vban::mdb_iterator<vban::account, uint64_t> i (transaction_a, confirmation_height), n (vban::mdb_iterator<vban::account, uint64_t>{}); i != n; ++i, ++account_info_i, ++num)
	{
		vban::account account (i->first);
		uint64_t confirmation_height (i->second);

		// Check account hashes matches both the accounts table and confirmation height table
		debug_assert (account == account_info_i->first);

		auto const & account_info = account_info_i->second;

		if (confirmation_height == 0)
		{
			confirmation_height_infos.emplace_back (account, confirmation_height_info{ 0, vban::block_hash (0) });
		}
		else
		{
			if (account_info_i->second.block_count / 2 >= confirmation_height)
			{
				// The confirmation height of the account is closer to the bottom of the chain, so start there and work up
				auto block = block_get_v18 (transaction_a, account_info.open_block);
				debug_assert (block);
				auto height = 1;

				while (height != confirmation_height)
				{
					block = block_get_v18 (transaction_a, block->sideband ().successor);
					debug_assert (block);
					++height;
				}

				debug_assert (block->sideband ().height == confirmation_height);
				confirmation_height_infos.emplace_back (account, confirmation_height_info{ confirmation_height, block->hash () });
			}
			else
			{
				// The confirmation height of the account is closer to the top of the chain so start there and work down
				auto block = block_get_v18 (transaction_a, account_info.head);
				auto height = block->sideband ().height;
				while (height != confirmation_height)
				{
					block = block_get_v18 (transaction_a, block->previous ());
					debug_assert (block);
					--height;
				}
				confirmation_height_infos.emplace_back (account, confirmation_height_info{ confirmation_height, block->hash () });
			}
		}

		// Every so often output to the log to indicate progress (every 200k accounts)
		constexpr auto output_cutoff = 200000;
		if (num % output_cutoff == 0 && num != 0)
		{
			logger.always_log (boost::str (boost::format ("Confirmation height frontier set for %1%00k accounts") % ((num / output_cutoff) * 2)));
		}
	}

	// Clear it then append
	auto status (mdb_drop (env.tx (transaction_a), confirmation_height, 0));
	release_assert_success (status);

	for (auto const & confirmation_height_info_pair : confirmation_height_infos)
	{
		mdb_put (env.tx (transaction_a), confirmation_height, vban::mdb_val (confirmation_height_info_pair.first), vban::mdb_val (confirmation_height_info_pair.second), MDB_APPEND);
	}

	version_put (transaction_a, 17);
	logger.always_log ("Finished upgrading confirmation height frontiers");
}

void vban::mdb_store::upgrade_v17_to_v18 (vban::write_transaction const & transaction_a)
{
	logger.always_log ("Preparing v17 to v18 database upgrade...");

	auto count_pre (count (transaction_a, state_blocks));

	auto num = 0u;
	for (vban::mdb_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::state_block>> state_i (transaction_a, state_blocks), state_n{}; state_i != state_n; ++state_i, ++num)
	{
		vban::block_w_sideband_v18<vban::state_block> block_w_sideband (state_i->second);
		auto & block (block_w_sideband.block);
		auto & sideband (block_w_sideband.sideband);

		bool is_send{ false };
		bool is_receive{ false };
		bool is_epoch{ false };

		vban::amount prev_balance (0);
		if (!block->hashables.previous.is_zero ())
		{
			prev_balance = block_balance_v18 (transaction_a, block->hashables.previous);
		}
		if (block->hashables.balance == prev_balance && network_params.ledger.epochs.is_epoch_link (block->hashables.link))
		{
			is_epoch = true;
		}
		else if (block->hashables.balance < prev_balance)
		{
			is_send = true;
		}
		else if (!block->hashables.link.is_zero ())
		{
			is_receive = true;
		}

		vban::block_sideband_v18 new_sideband (sideband.account, sideband.successor, sideband.balance, sideband.height, sideband.timestamp, sideband.details.epoch, is_send, is_receive, is_epoch);
		// Write these out
		std::vector<uint8_t> data;
		{
			vban::vectorstream stream (data);
			block->serialize (stream);
			new_sideband.serialize (stream, block->type ());
		}
		vban::mdb_val value{ data.size (), (void *)data.data () };
		auto s = mdb_cursor_put (state_i.cursor, state_i->first, value, MDB_CURRENT);
		release_assert_success (s);

		// Every so often output to the log to indicate progress
		constexpr auto output_cutoff = 1000000;
		if (num > 0 && num % output_cutoff == 0)
		{
			logger.always_log (boost::str (boost::format ("Database sideband upgrade %1% million state blocks upgraded (out of %2%)") % (num / output_cutoff) % count_pre));
		}
	}

	auto count_post (count (transaction_a, state_blocks));
	release_assert (count_pre == count_post);

	version_put (transaction_a, 18);
	logger.always_log ("Finished upgrading the sideband");
}

void vban::mdb_store::upgrade_v18_to_v19 (vban::write_transaction const & transaction_a)
{
	logger.always_log ("Preparing v18 to v19 database upgrade...");
	auto count_pre (count (transaction_a, state_blocks) + count (transaction_a, send_blocks) + count (transaction_a, receive_blocks) + count (transaction_a, change_blocks) + count (transaction_a, open_blocks));

	// Combine in order of likeliness of counts
	std::map<vban::block_hash, vban::block_w_sideband> legacy_open_receive_change_blocks;

	for (auto i (vban::store_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::change_block>> (std::make_unique<vban::mdb_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::change_block>>> (transaction_a, change_blocks))), n (vban::store_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::change_block>> (nullptr)); i != n; ++i)
	{
		vban::block_sideband_v18 const & old_sideband (i->second.sideband);
		vban::block_sideband new_sideband (old_sideband.account, old_sideband.successor, old_sideband.balance, old_sideband.height, old_sideband.timestamp, vban::epoch::epoch_0, false, false, false, vban::epoch::epoch_0);
		legacy_open_receive_change_blocks[i->first] = { vban::block_w_sideband{ i->second.block, new_sideband } };
	}

	for (auto i (vban::store_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::open_block>> (std::make_unique<vban::mdb_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::open_block>>> (transaction_a, open_blocks))), n (vban::store_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::open_block>> (nullptr)); i != n; ++i)
	{
		vban::block_sideband_v18 const & old_sideband (i->second.sideband);
		vban::block_sideband new_sideband (old_sideband.account, old_sideband.successor, old_sideband.balance, old_sideband.height, old_sideband.timestamp, vban::epoch::epoch_0, false, false, false, vban::epoch::epoch_0);
		legacy_open_receive_change_blocks[i->first] = { vban::block_w_sideband{ i->second.block, new_sideband } };
	}

	for (auto i (vban::store_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::receive_block>> (std::make_unique<vban::mdb_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::receive_block>>> (transaction_a, receive_blocks))), n (vban::store_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::receive_block>> (nullptr)); i != n; ++i)
	{
		vban::block_sideband_v18 const & old_sideband (i->second.sideband);
		vban::block_sideband new_sideband (old_sideband.account, old_sideband.successor, old_sideband.balance, old_sideband.height, old_sideband.timestamp, vban::epoch::epoch_0, false, false, false, vban::epoch::epoch_0);
		legacy_open_receive_change_blocks[i->first] = { vban::block_w_sideband{ i->second.block, new_sideband } };
	}

	release_assert (!mdb_drop (env.tx (transaction_a), receive_blocks, 1));
	receive_blocks = 0;
	release_assert (!mdb_drop (env.tx (transaction_a), open_blocks, 1));
	open_blocks = 0;
	release_assert (!mdb_drop (env.tx (transaction_a), change_blocks, 1));
	change_blocks = 0;

	logger.always_log ("Write legacy open/receive/change to new format");

	MDB_dbi temp_legacy_open_receive_change_blocks;
	{
		mdb_dbi_open (env.tx (transaction_a), "temp_legacy_open_receive_change_blocks", MDB_CREATE, &temp_legacy_open_receive_change_blocks);

		for (auto const & legacy_block : legacy_open_receive_change_blocks)
		{
			std::vector<uint8_t> data;
			{
				vban::vectorstream stream (data);
				vban::serialize_block (stream, *legacy_block.second.block);
				legacy_block.second.sideband.serialize (stream, legacy_block.second.block->type ());
			}

			vban::mdb_val value{ data.size (), (void *)data.data () };
			auto s = mdb_put (env.tx (transaction_a), temp_legacy_open_receive_change_blocks, vban::mdb_val (legacy_block.first), value, MDB_APPEND);
			release_assert_success (s);
		}
	}

	logger.always_log ("Write legacy send to new format");

	// Write send blocks to a new table (this was not done in memory as it would push us above memory requirements)
	MDB_dbi temp_legacy_send_blocks;
	{
		mdb_dbi_open (env.tx (transaction_a), "temp_legacy_send_blocks", MDB_CREATE, &temp_legacy_send_blocks);

		for (auto i (vban::store_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::send_block>> (std::make_unique<vban::mdb_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::send_block>>> (transaction_a, send_blocks))), n (vban::store_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::send_block>> (nullptr)); i != n; ++i)
		{
			auto const & block_w_sideband_v18 (i->second);

			std::vector<uint8_t> data;
			{
				vban::vectorstream stream (data);
				vban::serialize_block (stream, *block_w_sideband_v18.block);
				block_w_sideband_v18.sideband.serialize (stream, vban::block_type::send); // Equal to new version for legacy blocks
			}

			vban::mdb_val value{ data.size (), (void *)data.data () };
			auto s = mdb_put (env.tx (transaction_a), temp_legacy_send_blocks, vban::mdb_val (i->first), value, MDB_APPEND);
			release_assert_success (s);
		}
	}

	release_assert (!mdb_drop (env.tx (transaction_a), send_blocks, 1));
	send_blocks = 0;

	logger.always_log ("Merge legacy open/receive/change with legacy send blocks");

	MDB_dbi temp_legacy_send_open_receive_change_blocks;
	{
		mdb_dbi_open (env.tx (transaction_a), "temp_legacy_send_open_receive_change_blocks", MDB_CREATE, &temp_legacy_send_open_receive_change_blocks);

		vban::mdb_merge_iterator<vban::block_hash, vban::block_w_sideband> i (transaction_a, temp_legacy_open_receive_change_blocks, temp_legacy_send_blocks);
		vban::mdb_merge_iterator<vban::block_hash, vban::block_w_sideband> n{};
		for (; i != n; ++i)
		{
			auto s = mdb_put (env.tx (transaction_a), temp_legacy_send_open_receive_change_blocks, vban::mdb_val (i->first), vban::mdb_val (i->second), MDB_APPEND);
			release_assert_success (s);
		}

		// Delete tables
		mdb_drop (env.tx (transaction_a), temp_legacy_send_blocks, 1);
		mdb_drop (env.tx (transaction_a), temp_legacy_open_receive_change_blocks, 1);
	}

	logger.always_log ("Write state blocks to new format");

	// Write state blocks to a new table (this was not done in memory as it would push us above memory requirements)
	MDB_dbi temp_state_blocks;
	{
		auto type_state (vban::block_type::state);
		mdb_dbi_open (env.tx (transaction_a), "temp_state_blocks", MDB_CREATE, &temp_state_blocks);

		for (auto i (vban::store_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::state_block>> (std::make_unique<vban::mdb_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::state_block>>> (transaction_a, state_blocks))), n (vban::store_iterator<vban::block_hash, vban::block_w_sideband_v18<vban::state_block>> (nullptr)); i != n; ++i)
		{
			auto const & block_w_sideband_v18 (i->second);
			vban::block_sideband_v18 const & old_sideband (block_w_sideband_v18.sideband);
			vban::epoch source_epoch (vban::epoch::epoch_0);
			// Source block v18 epoch
			if (old_sideband.details.is_receive)
			{
				auto db_val (block_raw_get_by_type_v18 (transaction_a, block_w_sideband_v18.block->link ().as_block_hash (), type_state));
				if (db_val.is_initialized ())
				{
					vban::bufferstream stream (reinterpret_cast<uint8_t const *> (db_val.get ().data ()), db_val.get ().size ());
					auto source_block (vban::deserialize_block (stream, type_state));
					release_assert (source_block != nullptr);
					vban::block_sideband_v18 source_sideband;
					auto error (source_sideband.deserialize (stream, type_state));
					release_assert (!error);
					source_epoch = source_sideband.details.epoch;
				}
			}
			vban::block_sideband new_sideband (old_sideband.account, old_sideband.successor, old_sideband.balance, old_sideband.height, old_sideband.timestamp, old_sideband.details.epoch, old_sideband.details.is_send, old_sideband.details.is_receive, old_sideband.details.is_epoch, source_epoch);

			std::vector<uint8_t> data;
			{
				vban::vectorstream stream (data);
				vban::serialize_block (stream, *block_w_sideband_v18.block);
				new_sideband.serialize (stream, vban::block_type::state);
			}

			vban::mdb_val value{ data.size (), (void *)data.data () };
			auto s = mdb_put (env.tx (transaction_a), temp_state_blocks, vban::mdb_val (i->first), value, MDB_APPEND);
			release_assert_success (s);
		}
	}

	release_assert (!mdb_drop (env.tx (transaction_a), state_blocks, 1));
	state_blocks = 0;

	logger.always_log ("Merging all legacy blocks with state blocks");

	// Merge all legacy blocks with state blocks into the final table
	vban::mdb_merge_iterator<vban::block_hash, vban::block_w_sideband> i (transaction_a, temp_legacy_send_open_receive_change_blocks, temp_state_blocks);
	vban::mdb_merge_iterator<vban::block_hash, vban::block_w_sideband> n{};
	mdb_dbi_open (env.tx (transaction_a), "blocks", MDB_CREATE, &blocks);
	for (; i != n; ++i)
	{
		auto s = mdb_put (env.tx (transaction_a), blocks, vban::mdb_val (i->first), vban::mdb_val (i->second), MDB_APPEND);
		release_assert_success (s);
	}

	// Delete tables
	mdb_drop (env.tx (transaction_a), temp_legacy_send_open_receive_change_blocks, 1);
	mdb_drop (env.tx (transaction_a), temp_state_blocks, 1);

	auto count_post (count (transaction_a, blocks));
	release_assert (count_pre == count_post);

	MDB_dbi vote{ 0 };
	release_assert (!mdb_dbi_open (env.tx (transaction_a), "vote", MDB_CREATE, &vote));
	release_assert (!mdb_drop (env.tx (transaction_a), vote, 1));

	version_put (transaction_a, 19);
	logger.always_log ("Finished upgrading all blocks to new blocks database");
}

void vban::mdb_store::upgrade_v19_to_v20 (vban::write_transaction const & transaction_a)
{
	logger.always_log ("Preparing v19 to v20 database upgrade...");
	mdb_dbi_open (env.tx (transaction_a), "pruned", MDB_CREATE, &pruned);
	version_put (transaction_a, 20);
	logger.always_log ("Finished creating new pruned table");
}

void vban::mdb_store::upgrade_v20_to_v21 (vban::write_transaction const & transaction_a)
{
	logger.always_log ("Preparing v20 to v21 database upgrade...");
	mdb_dbi_open (env.tx (transaction_a), "final_votes", MDB_CREATE, &final_votes);
	version_put (transaction_a, 21);
	logger.always_log ("Finished creating new final_vote table");
}

/** Takes a filepath, appends '_backup_<timestamp>' to the end (but before any extension) and saves that file in the same directory */
void vban::mdb_store::create_backup_file (vban::mdb_env & env_a, boost::filesystem::path const & filepath_a, vban::logger_mt & logger_a)
{
	auto extension = filepath_a.extension ();
	auto filename_without_extension = filepath_a.filename ().replace_extension ("");
	auto orig_filepath = filepath_a;
	auto & backup_path = orig_filepath.remove_filename ();
	auto backup_filename = filename_without_extension;
	backup_filename += "_backup_";
	backup_filename += std::to_string (std::chrono::system_clock::now ().time_since_epoch ().count ());
	backup_filename += extension;
	auto backup_filepath = backup_path / backup_filename;
	auto start_message (boost::str (boost::format ("Performing %1% backup before database upgrade...") % filepath_a.filename ()));
	logger_a.always_log (start_message);
	std::cout << start_message << std::endl;
	auto error (mdb_env_copy (env_a, backup_filepath.string ().c_str ()));
	if (error)
	{
		auto error_message (boost::str (boost::format ("%1% backup failed") % filepath_a.filename ()));
		logger_a.always_log (error_message);
		std::cerr << error_message << std::endl;
		std::exit (1);
	}
	else
	{
		auto success_message (boost::str (boost::format ("Backup created: %1%") % backup_filename));
		logger_a.always_log (success_message);
		std::cout << success_message << std::endl;
	}
}

std::vector<vban::unchecked_info> vban::mdb_store::unchecked_get (vban::transaction const & transaction_a, vban::block_hash const & hash_a)
{
	std::vector<vban::unchecked_info> result;
	for (auto i (unchecked_begin (transaction_a, vban::unchecked_key (hash_a, 0))), n (unchecked_end ()); i != n && i->first.key () == hash_a; ++i)
	{
		vban::unchecked_info const & unchecked_info (i->second);
		result.push_back (unchecked_info);
	}
	return result;
}

void vban::mdb_store::version_put (vban::write_transaction const & transaction_a, int version_a)
{
	vban::uint256_union version_key (1);
	vban::uint256_union version_value (version_a);
	auto status (mdb_put (env.tx (transaction_a), meta, vban::mdb_val (version_key), vban::mdb_val (version_value), 0));
	release_assert_success (status);
}

bool vban::mdb_store::exists (vban::transaction const & transaction_a, tables table_a, vban::mdb_val const & key_a) const
{
	vban::mdb_val junk;
	auto status = get (transaction_a, table_a, key_a, junk);
	release_assert (status == MDB_SUCCESS || status == MDB_NOTFOUND);
	return (status == MDB_SUCCESS);
}

int vban::mdb_store::get (vban::transaction const & transaction_a, tables table_a, vban::mdb_val const & key_a, vban::mdb_val & value_a) const
{
	return mdb_get (env.tx (transaction_a), table_to_dbi (table_a), key_a, value_a);
}

int vban::mdb_store::put (vban::write_transaction const & transaction_a, tables table_a, vban::mdb_val const & key_a, const vban::mdb_val & value_a) const
{
	return (mdb_put (env.tx (transaction_a), table_to_dbi (table_a), key_a, value_a, 0));
}

int vban::mdb_store::del (vban::write_transaction const & transaction_a, tables table_a, vban::mdb_val const & key_a) const
{
	return (mdb_del (env.tx (transaction_a), table_to_dbi (table_a), key_a, nullptr));
}

int vban::mdb_store::drop (vban::write_transaction const & transaction_a, tables table_a)
{
	return clear (transaction_a, table_to_dbi (table_a));
}

int vban::mdb_store::clear (vban::write_transaction const & transaction_a, MDB_dbi handle_a)
{
	return mdb_drop (env.tx (transaction_a), handle_a, 0);
}

uint64_t vban::mdb_store::count (vban::transaction const & transaction_a, tables table_a) const
{
	return count (transaction_a, table_to_dbi (table_a));
}

uint64_t vban::mdb_store::count (vban::transaction const & transaction_a, MDB_dbi db_a) const
{
	MDB_stat stats;
	auto status (mdb_stat (env.tx (transaction_a), db_a, &stats));
	release_assert_success (status);
	return (stats.ms_entries);
}

MDB_dbi vban::mdb_store::table_to_dbi (tables table_a) const
{
	switch (table_a)
	{
		case tables::frontiers:
			return frontiers;
		case tables::accounts:
			return accounts;
		case tables::blocks:
			return blocks;
		case tables::pending:
			return pending;
		case tables::unchecked:
			return unchecked;
		case tables::online_weight:
			return online_weight;
		case tables::meta:
			return meta;
		case tables::peers:
			return peers;
		case tables::pruned:
			return pruned;
		case tables::confirmation_height:
			return confirmation_height;
		case tables::final_votes:
			return final_votes;
		default:
			release_assert (false);
			return peers;
	}
}

bool vban::mdb_store::not_found (int status) const
{
	return (status_code_not_found () == status);
}

bool vban::mdb_store::success (int status) const
{
	return (MDB_SUCCESS == status);
}

int vban::mdb_store::status_code_not_found () const
{
	return MDB_NOTFOUND;
}

std::string vban::mdb_store::error_string (int status) const
{
	return mdb_strerror (status);
}

bool vban::mdb_store::copy_db (boost::filesystem::path const & destination_file)
{
	return !mdb_env_copy2 (env.environment, destination_file.string ().c_str (), MDB_CP_COMPACT);
}

void vban::mdb_store::rebuild_db (vban::write_transaction const & transaction_a)
{
	// Tables with uint256_union key
	std::vector<MDB_dbi> tables = { accounts, blocks, pruned, confirmation_height };
	for (auto const & table : tables)
	{
		MDB_dbi temp;
		mdb_dbi_open (env.tx (transaction_a), "temp_table", MDB_CREATE, &temp);
		// Copy all values to temporary table
		for (auto i (vban::store_iterator<vban::uint256_union, vban::mdb_val> (std::make_unique<vban::mdb_iterator<vban::uint256_union, vban::mdb_val>> (transaction_a, table))), n (vban::store_iterator<vban::uint256_union, vban::mdb_val> (nullptr)); i != n; ++i)
		{
			auto s = mdb_put (env.tx (transaction_a), temp, vban::mdb_val (i->first), i->second, MDB_APPEND);
			release_assert_success (s);
		}
		release_assert (count (transaction_a, table) == count (transaction_a, temp));
		// Clear existing table
		mdb_drop (env.tx (transaction_a), table, 0);
		// Put values from copy
		for (auto i (vban::store_iterator<vban::uint256_union, vban::mdb_val> (std::make_unique<vban::mdb_iterator<vban::uint256_union, vban::mdb_val>> (transaction_a, temp))), n (vban::store_iterator<vban::uint256_union, vban::mdb_val> (nullptr)); i != n; ++i)
		{
			auto s = mdb_put (env.tx (transaction_a), table, vban::mdb_val (i->first), i->second, MDB_APPEND);
			release_assert_success (s);
		}
		release_assert (count (transaction_a, table) == count (transaction_a, temp));
		// Remove temporary table
		mdb_drop (env.tx (transaction_a), temp, 1);
	}
	// Pending table
	{
		MDB_dbi temp;
		mdb_dbi_open (env.tx (transaction_a), "temp_table", MDB_CREATE, &temp);
		// Copy all values to temporary table
		for (auto i (vban::store_iterator<vban::pending_key, vban::pending_info> (std::make_unique<vban::mdb_iterator<vban::pending_key, vban::pending_info>> (transaction_a, pending))), n (vban::store_iterator<vban::pending_key, vban::pending_info> (nullptr)); i != n; ++i)
		{
			auto s = mdb_put (env.tx (transaction_a), temp, vban::mdb_val (i->first), vban::mdb_val (i->second), MDB_APPEND);
			release_assert_success (s);
		}
		release_assert (count (transaction_a, pending) == count (transaction_a, temp));
		mdb_drop (env.tx (transaction_a), pending, 0);
		// Put values from copy
		for (auto i (vban::store_iterator<vban::pending_key, vban::pending_info> (std::make_unique<vban::mdb_iterator<vban::pending_key, vban::pending_info>> (transaction_a, temp))), n (vban::store_iterator<vban::pending_key, vban::pending_info> (nullptr)); i != n; ++i)
		{
			auto s = mdb_put (env.tx (transaction_a), pending, vban::mdb_val (i->first), vban::mdb_val (i->second), MDB_APPEND);
			release_assert_success (s);
		}
		release_assert (count (transaction_a, pending) == count (transaction_a, temp));
		mdb_drop (env.tx (transaction_a), temp, 1);
	}
}

bool vban::mdb_store::init_error () const
{
	return error;
}

std::shared_ptr<vban::block> vban::mdb_store::block_get_v18 (vban::transaction const & transaction_a, vban::block_hash const & hash_a) const
{
	vban::block_type type;
	auto value (block_raw_get_v18 (transaction_a, hash_a, type));
	std::shared_ptr<vban::block> result;
	if (value.size () != 0)
	{
		vban::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
		result = vban::deserialize_block (stream, type);
		release_assert (result != nullptr);
		vban::block_sideband_v18 sideband;
		auto error = (sideband.deserialize (stream, type));
		release_assert (!error);
		result->sideband_set (vban::block_sideband (sideband.account, sideband.successor, sideband.balance, sideband.height, sideband.timestamp, sideband.details.epoch, sideband.details.is_send, sideband.details.is_receive, sideband.details.is_epoch, vban::epoch::epoch_0));
	}
	return result;
}

vban::mdb_val vban::mdb_store::block_raw_get_v18 (vban::transaction const & transaction_a, vban::block_hash const & hash_a, vban::block_type & type_a) const
{
	vban::mdb_val result;
	// Table lookups are ordered by match probability
	vban::block_type block_types[]{ vban::block_type::state, vban::block_type::send, vban::block_type::receive, vban::block_type::open, vban::block_type::change };
	for (auto current_type : block_types)
	{
		auto db_val (block_raw_get_by_type_v18 (transaction_a, hash_a, current_type));
		if (db_val.is_initialized ())
		{
			type_a = current_type;
			result = db_val.get ();
			break;
		}
	}

	return result;
}

boost::optional<vban::mdb_val> vban::mdb_store::block_raw_get_by_type_v18 (vban::transaction const & transaction_a, vban::block_hash const & hash_a, vban::block_type & type_a) const
{
	vban::mdb_val value;
	vban::mdb_val hash (hash_a);
	int status = status_code_not_found ();
	switch (type_a)
	{
		case vban::block_type::send:
		{
			status = mdb_get (env.tx (transaction_a), send_blocks, hash, value);
			break;
		}
		case vban::block_type::receive:
		{
			status = mdb_get (env.tx (transaction_a), receive_blocks, hash, value);
			break;
		}
		case vban::block_type::open:
		{
			status = mdb_get (env.tx (transaction_a), open_blocks, hash, value);
			break;
		}
		case vban::block_type::change:
		{
			status = mdb_get (env.tx (transaction_a), change_blocks, hash, value);
			break;
		}
		case vban::block_type::state:
		{
			status = mdb_get (env.tx (transaction_a), state_blocks, hash, value);
			break;
		}
		case vban::block_type::invalid:
		case vban::block_type::not_a_block:
		{
			break;
		}
	}

	release_assert (success (status) || not_found (status));
	boost::optional<vban::mdb_val> result;
	if (success (status))
	{
		result = value;
	}
	return result;
}

vban::uint256_t vban::mdb_store::block_balance_v18 (vban::transaction const & transaction_a, vban::block_hash const & hash_a) const
{
	auto block (block_get_v18 (transaction_a, hash_a));
	release_assert (block);
	vban::uint256_t result (block_balance_calculated (block));
	return result;
}

// All the v14 functions below are only needed during upgrades
size_t vban::mdb_store::block_successor_offset_v14 (vban::transaction const & transaction_a, size_t entry_size_a, vban::block_type type_a) const
{
	return entry_size_a - vban::block_sideband_v14::size (type_a);
}

vban::block_hash vban::mdb_store::block_successor_v14 (vban::transaction const & transaction_a, vban::block_hash const & hash_a) const
{
	vban::block_type type;
	auto value (block_raw_get_v14 (transaction_a, hash_a, type));
	vban::block_hash result;
	if (value.size () != 0)
	{
		debug_assert (value.size () >= result.bytes.size ());
		vban::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()) + block_successor_offset_v14 (transaction_a, value.size (), type), result.bytes.size ());
		auto error (vban::try_read (stream, result.bytes));
		(void)error;
		debug_assert (!error);
	}
	else
	{
		result.clear ();
	}
	return result;
}

vban::mdb_val vban::mdb_store::block_raw_get_v14 (vban::transaction const & transaction_a, vban::block_hash const & hash_a, vban::block_type & type_a, bool * is_state_v1) const
{
	vban::mdb_val result;
	// Table lookups are ordered by match probability
	vban::block_type block_types[]{ vban::block_type::state, vban::block_type::send, vban::block_type::receive, vban::block_type::open, vban::block_type::change };
	for (auto current_type : block_types)
	{
		auto db_val (block_raw_get_by_type_v14 (transaction_a, hash_a, current_type, is_state_v1));
		if (db_val.is_initialized ())
		{
			type_a = current_type;
			result = db_val.get ();
			break;
		}
	}

	return result;
}

boost::optional<vban::mdb_val> vban::mdb_store::block_raw_get_by_type_v14 (vban::transaction const & transaction_a, vban::block_hash const & hash_a, vban::block_type & type_a, bool * is_state_v1) const
{
	vban::mdb_val value;
	vban::mdb_val hash (hash_a);
	int status = status_code_not_found ();
	switch (type_a)
	{
		case vban::block_type::send:
		{
			status = mdb_get (env.tx (transaction_a), send_blocks, hash, value);
			break;
		}
		case vban::block_type::receive:
		{
			status = mdb_get (env.tx (transaction_a), receive_blocks, hash, value);
			break;
		}
		case vban::block_type::open:
		{
			status = mdb_get (env.tx (transaction_a), open_blocks, hash, value);
			break;
		}
		case vban::block_type::change:
		{
			status = mdb_get (env.tx (transaction_a), change_blocks, hash, value);
			break;
		}
		case vban::block_type::state:
		{
			status = mdb_get (env.tx (transaction_a), state_blocks_v1, hash, value);
			if (is_state_v1 != nullptr)
			{
				*is_state_v1 = success (status);
			}
			if (not_found (status))
			{
				status = mdb_get (env.tx (transaction_a), state_blocks_v0, hash, value);
			}
			break;
		}
		case vban::block_type::invalid:
		case vban::block_type::not_a_block:
		{
			break;
		}
	}

	release_assert (success (status) || not_found (status));
	boost::optional<vban::mdb_val> result;
	if (success (status))
	{
		result = value;
	}
	return result;
}

std::shared_ptr<vban::block> vban::mdb_store::block_get_v14 (vban::transaction const & transaction_a, vban::block_hash const & hash_a, vban::block_sideband_v14 * sideband_a, bool * is_state_v1) const
{
	vban::block_type type;
	auto value (block_raw_get_v14 (transaction_a, hash_a, type, is_state_v1));
	std::shared_ptr<vban::block> result;
	if (value.size () != 0)
	{
		vban::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
		result = vban::deserialize_block (stream, type);
		debug_assert (result != nullptr);
		if (sideband_a)
		{
			sideband_a->type = type;
			bool error = sideband_a->deserialize (stream);
			(void)error;
			debug_assert (!error);
		}
	}
	return result;
}

vban::mdb_store::upgrade_counters::upgrade_counters (uint64_t count_before_v0, uint64_t count_before_v1) :
	before_v0 (count_before_v0),
	before_v1 (count_before_v1)
{
}

bool vban::mdb_store::upgrade_counters::are_equal () const
{
	return (before_v0 == after_v0) && (before_v1 == after_v1);
}

unsigned vban::mdb_store::max_block_write_batch_num () const
{
	return std::numeric_limits<unsigned>::max ();
}

// Explicitly instantiate
template class vban::block_store_partial<MDB_val, vban::mdb_store>;
