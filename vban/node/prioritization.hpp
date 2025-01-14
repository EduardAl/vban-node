#pragma once
#include <vban/lib/numbers.hpp>

#include <cstddef>
#include <set>
#include <vector>

namespace vban
{
class block;
class prioritization final
{
	class value_type
	{
	public:
		uint64_t time;
		std::shared_ptr<vban::block> block;
		bool operator< (value_type const & other_a) const;
		bool operator== (value_type const & other_a) const;
	};
	using priority = std::set<value_type>;
	std::vector<priority> buckets;
	std::vector<vban::uint256_t> minimums;
	void next ();
	void seek ();
	void populate_schedule ();
	std::function<void (std::shared_ptr<vban::block>)> drop;
	// Contains bucket indicies to iterate over when making the next scheduling decision
	std::vector<uint8_t> schedule;
	decltype (schedule)::const_iterator current;

public:
	prioritization (uint64_t maximum = 250000u, std::function<void (std::shared_ptr<vban::block>)> const & drop_a = nullptr);
	void push (uint64_t time, std::shared_ptr<vban::block> block);
	std::shared_ptr<vban::block> top () const;
	void pop ();
	size_t size () const;
	size_t bucket_count () const;
	size_t bucket_size (size_t index) const;
	bool empty () const;
	void dump ();
	uint64_t const maximum;
};
}
