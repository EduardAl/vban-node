#include <vban/lib/rocksdbconfig.hpp>
#include <vban/lib/tomlconfig.hpp>

vban::error vban::rocksdb_config::serialize_toml (vban::tomlconfig & toml) const
{
	toml.put ("enable", enable, "Whether to use the RocksDB backend for the ledger database.\ntype:bool");
	toml.put ("memory_multiplier", memory_multiplier, "This will modify how much memory is used represented by 1 (low), 2 (medium), 3 (high). Default is 2.\ntype:uint8");
	toml.put ("io_threads", io_threads, "Number of threads to use with the background compaction and flushing. Number of hardware threads is recommended.\ntype:uint32");
	return toml.get_error ();
}

vban::error vban::rocksdb_config::deserialize_toml (vban::tomlconfig & toml)
{
	toml.get_optional<bool> ("enable", enable);
	toml.get_optional<uint8_t> ("memory_multiplier", memory_multiplier);
	toml.get_optional<unsigned> ("io_threads", io_threads);

	// Validate ranges
	if (io_threads == 0)
	{
		toml.get_error ().set ("io_threads must be non-zero");
	}
	if (memory_multiplier < 1 || memory_multiplier > 3)
	{
		toml.get_error ().set ("memory_multiplier must be either 1, 2 or 3");
	}

	return toml.get_error ();
}
