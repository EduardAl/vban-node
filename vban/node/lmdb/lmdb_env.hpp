#pragma once

#include <vban/lib/lmdbconfig.hpp>
#include <vban/node/lmdb/lmdb_txn.hpp>
#include <vban/secure/blockstore.hpp>

namespace vban
{
/**
 * RAII wrapper for MDB_env
 */
class mdb_env final
{
public:
	/** Environment options, most of which originates from the config file. */
	class options final
	{
		friend class mdb_env;

	public:
		static options make ()
		{
			return options ();
		}

		options & set_config (vban::lmdb_config config_a)
		{
			config = config_a;
			return *this;
		}

		options & set_use_no_mem_init (int use_no_mem_init_a)
		{
			use_no_mem_init = use_no_mem_init_a;
			return *this;
		}

		/** Used by the wallet to override the config map size */
		options & override_config_map_size (size_t map_size_a)
		{
			config.map_size = map_size_a;
			return *this;
		}

		/** Used by the wallet to override the sync strategy */
		options & override_config_sync (vban::lmdb_config::sync_strategy sync_a)
		{
			config.sync = sync_a;
			return *this;
		}

	private:
		bool use_no_mem_init{ false };
		vban::lmdb_config config;
	};

	mdb_env (bool &, boost::filesystem::path const &, vban::mdb_env::options options_a = vban::mdb_env::options::make ());
	void init (bool &, boost::filesystem::path const &, vban::mdb_env::options options_a = vban::mdb_env::options::make ());
	~mdb_env ();
	operator MDB_env * () const;
	vban::read_transaction tx_begin_read (mdb_txn_callbacks txn_callbacks = mdb_txn_callbacks{}) const;
	vban::write_transaction tx_begin_write (mdb_txn_callbacks txn_callbacks = mdb_txn_callbacks{}) const;
	MDB_txn * tx (vban::transaction const & transaction_a) const;
	MDB_env * environment;
};
}
