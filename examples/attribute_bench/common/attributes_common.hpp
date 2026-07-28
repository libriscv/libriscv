#pragma once
/**
 * Shared between the host and the guest: the leaf types of an attribute tree,
 * the workload shapes the benchmark measures, and an order-independent checksum.
 *
 * The host and the guest each have their own attribute container, but they agree
 * on the leaf types and on the order of the variant alternatives -- that order is
 * the ABI the zero-copy path reads and writes.
 */
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace bench {

// Plain leaf vectors, laid out so the guest and the host agree byte-for-byte
struct vec3  { float x, y, z; };
struct vec4  { float x, y, z, w; };
struct dvec3 { double x, y, z; };
struct dvec2 { double x, y; };

static_assert(sizeof(vec3) == 12 && alignof(vec3) == 4);
static_assert(sizeof(vec4) == 16 && alignof(vec4) == 4);
static_assert(sizeof(dvec3) == 24 && alignof(dvec3) == 8);
static_assert(sizeof(dvec2) == 16 && alignof(dvec2) == 8);

/// @brief The scalar alternatives, in ABI order. Identical on both sides, which
/// is what lets one build_workload() fill either container.
using BaseValue = std::variant<int64_t, double, vec3, vec4, dvec3, dvec2, std::string, bool>;

enum AttrType : int {
	INT64, FLOAT, VEC3, VEC4, DVEC3, DVEC2, STRING, BOOL, GROUP, LIST
};

/// @brief The shape of one attribute tree. The three presets below stand in for
/// the payloads the engine actually ships across the boundary.
struct Workload {
	const char* name;
	int scalars;      // keys cycling through the numeric/bool leaf types
	int strings;      // string-valued keys
	int string_len;   // bytes per string (<= 15 stays in the SSO buffer)
	int groups;       // nested sub-groups
	int group_size;   // keys per sub-group
	int lists;        // list-valued keys
	int list_len;     // elements per list
};

/// @brief An RPC payload: a handful of scalars and one short string.
inline constexpr Workload WORKLOAD_SMALL { "small", 7, 1, 12, 0, 0, 0, 0 };
/// @brief A mail letter: mostly strings, several of them past the SSO limit.
inline constexpr Workload WORKLOAD_STRINGS { "strings", 6, 10, 48, 0, 0, 0, 0 };
/// @brief A quest/shop state blob: groups and lists on top of the leaves.
inline constexpr Workload WORKLOAD_NESTED { "nested", 8, 6, 32, 3, 8, 2, 12 };

/// @brief Fill an attribute container with the given shape. Both containers
/// offer set()/addGroup()/setList(), so this is the single definition of what
/// "the same tree" means on the two sides of the boundary.
template <typename Attrs>
inline void build_workload(Attrs& attrs, const Workload& w)
{
	for (int i = 0; i < w.scalars; i++) {
		const std::string key = "s" + std::to_string(i);
		switch (i % 6) {
		case 0: attrs.set(key, int64_t(i) * 1337); break;
		case 1: attrs.set(key, double(i) * 0.5); break;
		case 2: attrs.set(key, vec3{float(i), 1.0f, 2.0f}); break;
		case 3: attrs.set(key, vec4{float(i), 1.0f, 2.0f, 3.0f}); break;
		case 4: attrs.set(key, dvec3{double(i), 1.5, 2.5}); break;
		case 5: attrs.set(key, (i & 1) != 0); break;
		}
	}
	for (int i = 0; i < w.strings; i++) {
		attrs.set("t" + std::to_string(i),
			std::string(std::size_t(w.string_len), char('a' + i % 26)));
	}
	for (int g = 0; g < w.groups; g++) {
		auto& group = attrs.addGroup("g" + std::to_string(g));
		for (int i = 0; i < w.group_size; i++) {
			if (i % 2 == 0)
				group.set("k" + std::to_string(i), int64_t(g) * 100 + i);
			else
				group.set("k" + std::to_string(i),
					std::string(std::size_t(w.string_len), char('A' + i % 26)));
		}
	}
	for (int l = 0; l < w.lists; l++) {
		std::vector<BaseValue> list;
		list.reserve(std::size_t(w.list_len));
		for (int i = 0; i < w.list_len; i++) {
			switch (i % 3) {
			case 0: list.emplace_back(int64_t(i)); break;
			case 1: list.emplace_back(double(i) * 0.25); break;
			case 2: list.emplace_back(std::string(std::size_t(w.string_len), 'x')); break;
			}
		}
		attrs.setList("l" + std::to_string(l), std::move(list));
	}
}

// --- Checksum ---------------------------------------------------------------
// Order-independent, because both containers are unordered: every entry
// contributes a hash and the hashes are summed.

inline uint64_t mix64(uint64_t v) noexcept
{
	v ^= v >> 33;
	v *= 0xff51afd7ed558ccdULL;
	v ^= v >> 33;
	v *= 0xc4ceb9fe1a85ec53ULL;
	return v ^ (v >> 33);
}

inline uint64_t hash_bytes(const void* data, std::size_t len, uint64_t seed = 0x9e3779b97f4a7c15ULL) noexcept
{
	const auto* p = static_cast<const unsigned char*>(data);
	uint64_t h = seed ^ (len * 0x100000001b3ULL);
	for (std::size_t i = 0; i < len; i++)
		h = mix64(h ^ p[i]);
	return h;
}

template <typename Attrs>
uint64_t checksum(const Attrs& attrs);

/// @brief Hash one scalar leaf. Works on either side's variant, because the
/// alternative types are the same.
template <typename Variant>
uint64_t checksum_base(const Variant& value)
{
	uint64_t h = mix64(uint64_t(value.index()) + 1);
	std::visit([&] (const auto& v) {
		using T = std::decay_t<decltype(v)>;
		if constexpr (std::is_same_v<T, std::string>)
			h ^= hash_bytes(v.data(), v.size());
		else if constexpr (std::is_same_v<T, bool>)
			h ^= mix64(v ? 3 : 5);
		else
			h ^= hash_bytes(&v, sizeof(v));
	}, value);
	return h;
}

/// @brief Hash a whole tree. GROUP and LIST are told apart by what they support
/// rather than by type name, since the two sides spell those alternatives
/// differently while meaning the same thing.
template <typename Attrs>
uint64_t checksum(const Attrs& attrs)
{
	uint64_t total = 0;
	for (const auto& entry : attrs.getAllAttributes()) {
		const auto& key = entry.first;
		uint64_t h = hash_bytes(key.data(), key.size(), 0x517cc1b727220a95ULL)
			^ mix64(uint64_t(entry.second.index()) + 1);
		std::visit([&] (const auto& v) {
			using T = std::decay_t<decltype(v)>;
			if constexpr (std::is_same_v<T, std::string>) {
				h ^= hash_bytes(v.data(), v.size());
			} else if constexpr (std::is_same_v<T, bool>) {
				h ^= mix64(v ? 3 : 5);
			} else if constexpr (requires { v->getAllAttributes(); }) {
				if (v) h ^= checksum(*v);
			} else if constexpr (requires { v->size(); v->begin(); }) {
				if (v) {
					uint64_t lh = 0;
					for (const auto& element : *v)
						lh = mix64(lh) ^ checksum_base(element);
					h ^= lh;
				}
			} else {
				h ^= hash_bytes(&v, sizeof(v));
			}
		}, entry.second);
		total += h;
	}
	return total;
}

} // namespace bench
