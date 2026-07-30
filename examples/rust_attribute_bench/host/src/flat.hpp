#pragma once
/**
 * The baseline: the tree crosses the boundary as a flat array of self-describing
 * nodes, one per key.
 *
 * Going out, the guest builds the array (one heap allocation for the root and one
 * per nested group and list) and the host reads it node by node. Coming in, the
 * host builds the array in guest memory -- allocating a key per entry and a byte
 * block per string value, plus an array per group and list -- and the guest
 * rebuilds the tree from it and releases the whole scaffolding again.
 *
 * The key and the string values travel as (address, length) rather than as
 * NUL-terminated C strings, because a Rust `String` is not NUL-terminated. It
 * happens to be the cheaper choice for the host too: one `memview` instead of a
 * scan across guest pages looking for a terminator.
 */
#include "attributes.hpp"
#include "script.hpp"

#include <cstddef>

namespace bench {

/// @brief The host's view of the guest's FlatAttr. Delivered byte-for-byte, so
/// the layout must match guest/src/flat.rs exactly.
struct GuestFlatAttr {
	Script::gaddr_t key;
	uint32_t key_len;
	uint32_t tag;
	union {
		int64_t i;
		double  f;
		vec3    v3;
		vec4    v4;
		double  d3[3];
		double  d2[2];
		bool    b;
		/// @brief The bytes of a string, or the nodes of a group or a list
		struct { Script::gaddr_t ptr; Script::gaddr_t len; } span;
	};
};
static_assert(sizeof(GuestFlatAttr) == 40, "flat node layout");
static_assert(offsetof(GuestFlatAttr, span) == 16, "the payload follows the tag");

/// @brief Read a flat array the guest built into the host tree.
void readFlatAttributes(Script& script, Script::gaddr_t nodes, std::size_t count,
	HostAttributes& out, int level = 0);

/// @brief Build a flat array for the guest, on the guest heap.
/// @return The address and the element count of the array. Every allocation it
/// contains is taken over or released by the guest as it consumes the array.
std::pair<Script::gaddr_t, std::size_t> writeFlatAttributes(
	Script& script, const HostAttributes& attrs, int level = 0);

} // namespace bench
