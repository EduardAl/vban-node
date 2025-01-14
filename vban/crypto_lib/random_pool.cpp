#include <vban/crypto_lib/random_pool.hpp>

#include <crypto/cryptopp/osrng.h>

std::mutex vban::random_pool::mutex;

void vban::random_pool::generate_block (unsigned char * output, size_t size)
{
	auto & pool = get_pool ();
	std::lock_guard guard (mutex);
	pool.GenerateBlock (output, size);
}

unsigned vban::random_pool::generate_word32 (unsigned min, unsigned max)
{
	auto & pool = get_pool ();
	std::lock_guard guard (mutex);
	return pool.GenerateWord32 (min, max);
}

unsigned char vban::random_pool::generate_byte ()
{
	auto & pool = get_pool ();
	std::lock_guard guard (mutex);
	return pool.GenerateByte ();
}

CryptoPP::AutoSeededRandomPool & vban::random_pool::get_pool ()
{
	static CryptoPP::AutoSeededRandomPool pool;
	return pool;
}
