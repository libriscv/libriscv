#pragma once
/**
 * The zero-copy path: the guest hands over the address of its own
 * std::unordered_map and the host walks it in place -- and builds the guest's
 * map directly when the traffic goes the other way.
 *
 * Nothing is marshalled: no flat array, no key strings copied onto the guest
 * heap, no re-insertion by the guest. Keys and string values are read as views
 * straight out of guest memory, and copied exactly once into the host tree.
 */
#include "attributes.hpp"
#include "script.hpp"

#include <libriscv/guest_datatypes.hpp>

namespace bench {

using GuestString = riscv::GuestStdString<Script::MARCH>;

/// @brief The guest's std::unique_ptr<Attributes> resp.
/// std::unique_ptr<std::vector<AttributeBaseValue>>. Modelled as plain PODs so
/// libriscv treats them as opaque pointer-sized alternatives: the walk follows
/// them explicitly, which is what keeps the ownership obvious.
struct GuestGroupPtr { Script::gaddr_t addr; };
struct GuestListPtr  { Script::gaddr_t addr; };

// The order of the alternatives IS the ABI, and it must match the guest's
// std::variant and the AttrType enum, which already agree.
using GAttrBase = riscv::GuestStdVariant<Script::MARCH,
	int64_t, double, vec3, vec4, dvec3, dvec2, GuestString, bool>;
using GAttrValue = riscv::GuestStdVariant<Script::MARCH,
	int64_t, double, vec3, vec4, dvec3, dvec2, GuestString, bool,
	GuestGroupPtr, GuestListPtr>;
/// @brief The guest map uses a custom hasher, so libstdc++ does NOT cache the
/// hash code in the nodes: CacheHashCode must be false here.
using GAttrMap  = riscv::GuestStdUnorderedMap<Script::MARCH, GuestString, GAttrValue, false>;
using GAttrList = riscv::GuestStdVector<Script::MARCH, GAttrBase>;

static_assert(sizeof(GAttrMap) == 7 * sizeof(Script::gaddr_t), "guest sizeof(Attributes)");
static_assert(sizeof(GAttrValue) == 40 && alignof(GAttrValue) == 8, "guest sizeof(Attribute)");
static_assert(sizeof(GAttrBase) == 40, "guest sizeof(AttributeBaseValue)");

/// @brief Bounds for a guest-provided tree. Scripts are adversarial input and a
/// hash map is a linked structure, so a lying element count or a cycle in the
/// node list must fail cleanly instead of hanging the thread.
struct GuestAttrLimits {
	static constexpr std::size_t MAX_ENTRIES = 4096;
	static constexpr std::size_t MAX_NODES   = 16384;
	static constexpr std::size_t MAX_BUCKETS = 1u << 20;
	static constexpr std::size_t MAX_DEPTH   = 8;
	static constexpr std::size_t MAX_LIST    = 4096;
	static constexpr std::size_t MAX_KEY     = 1024;
	static constexpr std::size_t MAX_STRING  = 4u << 20;
};

/// @brief View a guest attribute object as a map, validating the parts of its
/// state that the walk and the insert rely on. Throws when it is not usable.
GAttrMap& guestAttributes(Script& script, Script::gaddr_t addr);

/// @brief Convert a guest attribute tree into the host tree.
void readGuestAttributes(Script& script, Script::gaddr_t addr, HostAttributes& out);

/// @brief Insert a host tree into a constructed guest map, in place. Nested
/// groups and lists are allocated on the guest heap -- the same arena the guest's
/// own malloc uses -- so the guest can own and free them.
void writeGuestAttributes(Script& script, Script::gaddr_t self, const HostAttributes& attrs);

/// @brief Allocate a guest attribute object on the guest heap and fill it.
Script::gaddr_t createGuestAttributes(Script& script, const HostAttributes& attrs);

/// @brief Free a tree made by createGuestAttributes, and the object itself. A
/// guest that moved the tree out leaves an empty map behind, and freeing that is
/// a no-op rather than a double free.
void destroyGuestAttributes(Script& script, Script::gaddr_t self) noexcept;

} // namespace bench
