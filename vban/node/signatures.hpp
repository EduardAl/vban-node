#pragma once

#include <vban/lib/threading.hpp>
#include <vban/lib/utility.hpp>

#include <atomic>
#include <future>
#include <mutex>

namespace vban
{
class signature_check_set final
{
public:
	signature_check_set (size_t size, unsigned char const ** messages, size_t * message_lengths, unsigned char const ** pub_keys, unsigned char const ** signatures, int * verifications) :
		size (size), messages (messages), message_lengths (message_lengths), pub_keys (pub_keys), signatures (signatures), verifications (verifications)
	{
	}

	size_t size;
	unsigned char const ** messages;
	size_t * message_lengths;
	unsigned char const ** pub_keys;
	unsigned char const ** signatures;
	int * verifications;
};

/** Multi-threaded signature checker */
class signature_checker final
{
public:
	signature_checker (unsigned num_threads);
	~signature_checker ();
	void verify (signature_check_set &);
	void stop ();
	void flush ();

	static size_t constexpr batch_size = 256;

private:
	std::atomic<int> tasks_remaining{ 0 };
	std::atomic<bool> stopped{ false };
	vban::thread_pool thread_pool;

	struct Task final
	{
		Task (vban::signature_check_set & check, size_t pending) :
			check (check), pending (pending)
		{
		}
		~Task ()
		{
			release_assert (pending == 0);
		}
		vban::signature_check_set & check;
		std::atomic<size_t> pending;
	};

	bool verify_batch (const vban::signature_check_set & check_a, size_t index, size_t size);
	void verify_async (vban::signature_check_set & check_a, size_t num_batches, std::promise<void> & promise);
	bool single_threaded () const;
};
}
