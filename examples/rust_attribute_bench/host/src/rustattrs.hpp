#pragma once
/**
 * The zero-copy path: the host mirror of the guest's Rust attribute tree.
 *
 * Nothing is marshalled in either direction. Going out, the guest hands over the
 * address of its own three-word `Attributes` and the host walks it in place,
 * reading keys and string values as views straight out of guest memory. Coming
 * in, the host builds the tree on the guest heap in its final shape -- one
 * allocation for all the entries of a map rather than a node per key -- and the
 * guest takes it with a single `Box::from_raw`.
 *
 * The mirror is four declarations and one registration line. Because the value
 * type has to name itself -- a group is a `Box<Attributes>`, and `Attributes`
 * holds entries of values -- it is a struct deriving from the enum rather than an
 * alias, which is what lets it be forward declared.
 *
 * The variant order and the tag width ARE the ABI. They must match the guest's
 * `#[repr(C, u64)] enum Value`, and `probe_guest_layout()` checks that they do.
 */
#include "attributes.hpp"
#include "script.hpp"

#include <libriscv/guest/guest_rust_attributes.hpp>

namespace bench {

using GString = riscv::GuestRustString<Script::MARCH>;

struct GValue;
using GAttrs = riscv::GuestRustAttributes<Script::MARCH, GValue>;
/// @brief Group(Box<Attributes>)
using GGroup = riscv::GuestRustBox<Script::MARCH, GAttrs>;
/// @brief List(Box<Vec<Value>>)
using GList  = riscv::GuestRustBox<Script::MARCH, riscv::GuestRustVec<Script::MARCH, GValue>>;

/// @brief The mirror of the guest's `enum Value`, in declaration order.
struct GValue : riscv::GuestRustEnum<Script::MARCH, uint64_t,
	int64_t,   // Int(i64)
	double,    // Float(f64)
	vec3,      // Vec3([f32; 3])
	vec4,      // Vec4([f32; 4])
	dvec3,     // DVec3([f64; 3])
	dvec2,     // DVec2([f64; 2])
	GString,   // Str(String)
	bool,      // Bool(bool)
	GGroup,    // Group(Box<Attributes>)
	GList>     // List(Box<Vec<Value>>)
{
	using GuestRustEnum::GuestRustEnum;
};

} // namespace bench

RISCV_REGISTER_GUEST_RUST_ENUM(8, bench::GValue);

namespace bench {

// The host mirror is the same size as what the guest declared: a u64 tag ahead
// of the widest variant, which is the String -- and the [f64; 3], also 24 bytes
static_assert(sizeof(GValue) == 8 + 24, "tag + the widest variant");
static_assert(sizeof(riscv::GuestRustAttr<Script::MARCH, GValue>)
	== sizeof(GString) + sizeof(GValue), "Attr is a String and a Value");
static_assert(sizeof(GAttrs) == 3 * sizeof(Script::gaddr_t),
	"Attributes is a newtype over a Vec");

/// @brief Bounds for a guest-provided tree. A script is adversarial input, and
/// while the mirrors bound every individual access, a walk should be bounded too.
struct GuestAttrLimits {
	static constexpr std::size_t MAX_ENTRIES = 4096;
	static constexpr std::size_t MAX_NODES   = 16384;
	static constexpr std::size_t MAX_DEPTH   = 8;
	static constexpr std::size_t MAX_LIST    = 4096;
	static constexpr std::size_t MAX_KEY     = 1024;
	static constexpr std::size_t MAX_STRING  = 4u << 20;
};

/// @brief Convert a guest attribute tree into the host tree.
void readGuestAttributes(Script& script, Script::gaddr_t addr, HostAttributes& out);

/// @brief Build a host tree on the guest heap and return the `Box<Attributes>`
/// the guest will call `from_raw()` on. From there the tree is the guest's: its
/// drop glue walks it, so there is no host-side teardown function.
Script::gaddr_t createGuestAttributes(Script& script, const HostAttributes& attrs);

/// @brief Free a tree that no guest took ownership of, recursively. Only the
/// error paths need it.
void destroyGuestAttributes(Script& script, Script::gaddr_t addr) noexcept;

} // namespace bench
