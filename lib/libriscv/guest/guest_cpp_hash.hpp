#pragma once
#include "guest_cpp_string.hpp"

namespace riscv {

// Replica of libstdc++'s std::_Hash_bytes (a MurmurHash2 variant), which is
// what std::hash uses for strings and floating-point values. The width of the
// guests std::size_t decides which variant is used, so both are implemented.
template <int W>
struct GuestStdHashBytes
{
	using ghash_t = riscv::address_type<W>; // The guests std::size_t
	static_assert(W == 4 || W == 8, "Guest std::hash requires a 32- or 64-bit guest");

	/// @brief The seed that libstdc++ uses for all of its byte hashing
	static constexpr ghash_t SEED = ghash_t(0xc70f6907UL);

	static ghash_t hash(const void* data, std::size_t len, ghash_t seed = SEED) noexcept
	{
		const char* buf = (const char *)data;
		if constexpr (W == 8) {
			static constexpr uint64_t mul =
				(uint64_t(0xc6a4a793) << 32) + uint64_t(0x5bd1e995);
			// The main loop consumes the input 8 bytes at a time
			const std::size_t aligned_len = len & ~std::size_t(0x7);
			const char* const end = buf + aligned_len;
			uint64_t hash = seed ^ (len * mul);
			for (const char* p = buf; p != end; p += 8)
				hash = (hash ^ (shift_mix(load_word<uint64_t>(p) * mul) * mul)) * mul;
			if ((len & 0x7) != 0)
				hash = (hash ^ load_bytes(end, len - aligned_len)) * mul;
			hash = shift_mix(hash) * mul;
			return shift_mix(hash);
		} else {
			static constexpr uint32_t m = 0x5bd1e995;
			uint32_t hash = uint32_t(seed) ^ uint32_t(len);
			// Mix 4 bytes at a time into the hash
			while (len >= 4) {
				uint32_t k = load_word<uint32_t>(buf);
				k *= m;
				k ^= k >> 24;
				k *= m;
				hash *= m;
				hash ^= k;
				buf += 4;
				len -= 4;
			}
			// Handle the last few bytes of the input
			switch (len) {
			case 3: hash ^= uint32_t((unsigned char)buf[2]) << 16; [[fallthrough]];
			case 2: hash ^= uint32_t((unsigned char)buf[1]) << 8;  [[fallthrough]];
			case 1: hash ^= uint32_t((unsigned char)buf[0]);
				hash *= m;
			}
			// Murmur's mix function
			hash ^= hash >> 13;
			hash *= m;
			hash ^= hash >> 15;
			return hash;
		}
	}

private:
	static uint64_t shift_mix(uint64_t value) noexcept { return value ^ (value >> 47); }

	template <typename T>
	static T load_word(const char* p) noexcept {
		T value;
		std::memcpy(&value, p, sizeof(value));
		return value;
	}
	static uint64_t load_bytes(const char* p, std::size_t n) noexcept {
		uint64_t result = 0;
		while (n > 0)
			result = (result << 8) + (unsigned char)p[--n];
		return result;
	}
};

/// @brief Replica of the guests std::hash<K> and std::equal_to<K>, used to
/// find the bucket of a key. Specialize this in order to support other key
/// types than integers, enums, floats and std::string.
template <int W, typename K>
struct GuestStdHash
{
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;

	/// @brief libstdc++ stores the hash code in every node when the hash
	/// function is not considered fast (see __is_fast_hash), which changes
	/// the node layout. This must match the guest exactly. It is only the
	/// default for GuestStdUnorderedMap: a map that uses a hash function
	/// other than std::hash<K> overrides it with the CacheHashCode argument.
	static constexpr bool cache_hash_code = false;

	static gaddr_t hash(const machine_t&, const K& key)
	{
		if constexpr (std::is_integral_v<K> || std::is_enum_v<K>) {
			// std::hash of an integral type is the identity function,
			// truncated to the width of the guests std::size_t
			return static_cast<gaddr_t>(key);
		} else if constexpr (std::is_same_v<K, float> || std::is_same_v<K, double>) {
			// 0.0 and -0.0 both hash to zero
			return key != K(0.0) ? GuestStdHashBytes<W>::hash(&key, sizeof(key)) : 0;
		} else {
			static_assert(std::is_integral_v<K>,
				"GuestStdHash: Unsupported key type. Please specialize riscv::GuestStdHash<W, K>");
			return 0;
		}
	}
	static bool equals(const machine_t&, const K& a, const K& b) {
		return a == b;
	}
};

// std::hash<std::string> is not a "fast" hash, and so libstdc++ stores the
// hash code of every key in the node it belongs to.
template <int W>
struct GuestStdHash<W, GuestStdString<W>>
{
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	static constexpr bool cache_hash_code = true;

	static gaddr_t hash(const machine_t& machine, const GuestStdString<W>& key) {
		const auto view = key.to_view(machine);
		return GuestStdHashBytes<W>::hash(view.data(), view.size());
	}
	static gaddr_t hash(const machine_t&, std::string_view key) {
		return GuestStdHashBytes<W>::hash(key.data(), key.size());
	}
	static bool equals(const machine_t& machine, const GuestStdString<W>& a, const GuestStdString<W>& b) {
		return a.to_view(machine) == b.to_view(machine);
	}
	static bool equals(const machine_t& machine, const GuestStdString<W>& a, std::string_view b) {
		return a.to_view(machine) == b;
	}
};

// The bucket counts used by libstdc++'s _Prime_rehash_policy. The list is
// truncated: beyond the last entry we simply stop growing the bucket array.
struct GuestStdHashPolicy {
	static constexpr unsigned char fast_bkt[] =
		{ 2, 2, 2, 3, 5, 5, 7, 7, 11, 11, 11, 11, 13, 13 };
	static constexpr uint32_t primes[] = {
		2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67,
		71, 73, 79, 83, 89, 97, 103, 109, 113, 127, 137, 139, 149, 157, 167, 179,
		193, 199, 211, 227, 241, 257, 277, 293, 313, 337, 359, 383, 409, 439, 467,
		503, 541, 577, 619, 661, 709, 761, 823, 887, 953, 1031, 1109, 1193, 1289,
		1381, 1493, 1613, 1741, 1879, 2029, 2179, 2357, 2549, 2753, 2971, 3209,
		3469, 3739, 4027, 4349, 4703, 5087, 5503, 5953, 6427, 6949, 7517, 8123,
		8783, 9497, 10273, 11113, 12011, 12983, 14033, 15173, 16411, 17749, 19183,
		20753, 22447, 24281, 26267, 28411, 30727, 33223, 35933, 38873, 42043,
		45481, 49201, 53201, 57557, 62233, 67307, 72817, 78779, 85229, 92203,
		99733, 107897, 116731, 126271, 136607, 147793, 159871, 172933, 187091,
		202409, 218971, 236897, 256279, 277261, 299951, 324503, 351061, 379787,
		410857, 444487, 480881, 520241, 562841, 608903, 658753, 712697, 771049,
		834181, 902483, 976369, 1056323, 1142821, 1236397, 1337629, 1447153,
		1565659, 1693859, 1832561, 1982627, 2144977, 2320627, 2510653, 2716249,
		2938679, 3179303, 3439651, 3721303, 4026031, 4355707, 4712381, 5098259,
		5515729, 5967347, 6456007, 6984629, 7556579, 8175383, 8844859, 9569143,
		10352717, 11200489, 12117689, 13109983, 14183539, 15345007, 16601593,
		17961079
	};
	static constexpr std::size_t prime_count = sizeof(primes) / sizeof(primes[0]);
};

} // riscv
