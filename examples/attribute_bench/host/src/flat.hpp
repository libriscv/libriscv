#pragma once
/**
 * The baseline: an attribute tree crosses the boundary as a flat array of
 * self-describing nodes, one per key.
 *
 * Going out, the guest builds the array (one heap allocation per group and per
 * list) and the host reads it node by node, scanning guest memory for each
 * NUL-terminated key. Coming in, the host builds the array in guest memory --
 * allocating a key string per entry, a value string per string, and an array per
 * group and list -- and the guest re-inserts every key into a fresh map and frees
 * the whole scaffolding again.
 */
#include "attributes.hpp"
#include "script.hpp"

namespace bench {

/// @brief The host's view of the guest HostAttr node. Delivered byte-for-byte,
/// so the layout must match guest/attributes.hpp exactly.
struct GuestAttribute {
	Script::gaddr_t namePtr;
	int32_t type;
	int32_t padding;
	union {
		struct { Script::gaddr_t valuePtr; uint32_t valueSize; };
		int64_t i;
		double  f;
		vec3    v3;
		vec4    v4;
		double  d3[3];
		double  d2[2];
		bool    b;
		struct { Script::gaddr_t groupPtr; uint32_t groupSize; };
		struct { Script::gaddr_t listPtr;  uint32_t listSize; };
	};
};
static_assert(sizeof(GuestAttribute) == 40, "flat node layout");

/// @brief Read a flat array the guest built into a host tree.
void readFlatAttributes(Script& script, Script::gaddr_t nodes, std::size_t count,
	HostAttributes& out, int level = 0);

/// @brief Build a flat array for the guest, on the guest heap.
/// @return The address and the element count of the array. Every allocation it
/// contains is freed by the guest as it consumes the array.
std::pair<Script::gaddr_t, std::size_t> writeFlatAttributes(
	Script& script, const HostAttributes& attrs, int level = 0);

} // namespace bench
