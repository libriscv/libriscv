#include "probe.hpp"

#include "flat.hpp"
#include "rustattrs.hpp"

#include <cstddef>
#include <vector>

namespace bench {

using GAttr = riscv::GuestRustAttr<Script::MARCH, GValue>;
/// @brief The offset of the payload inside the enum, ie. sizeof(the tag)
static constexpr std::size_t PAYLOAD_OFFSET = offsetof(GValue::this_enum_type, payload);

/// @brief One word of the guest's report, and what this host expects it to be.
/// The order must match the array in bench_probe_layout().
struct Expectation {
	const char* what;
	std::size_t expected;
};

static const std::vector<Expectation> EXPECTATIONS {
	{ "sizeof(Value)",             sizeof(GValue) },
	{ "alignof(Value)",            alignof(GValue) },
	{ "discriminant of Int",       INT64 },
	{ "discriminant of Float",     FLOAT },
	{ "discriminant of Vec3",      VEC3 },
	{ "discriminant of Vec4",      VEC4 },
	{ "discriminant of DVec3",     DVEC3 },
	{ "discriminant of DVec2",     DVEC2 },
	{ "discriminant of Str",       STRING },
	{ "discriminant of Bool",      BOOL },
	{ "discriminant of Group",     GROUP },
	{ "discriminant of List",      LIST },
	{ "payload offset of Int",     PAYLOAD_OFFSET },
	{ "payload offset of DVec3",   PAYLOAD_OFFSET },
	{ "payload offset of Str",     PAYLOAD_OFFSET },
	{ "payload offset of Group",   PAYLOAD_OFFSET },
	{ "sizeof(Attr)",              sizeof(GAttr) },
	{ "offsetof(Attr, key)",       offsetof(GAttr, key) },
	{ "offsetof(Attr, value)",     offsetof(GAttr, value) },
	{ "sizeof(Attributes)",        sizeof(GAttrs) },
	{ "sizeof(Box<Attributes>)",   sizeof(GGroup) },
	{ "sizeof(FlatAttr)",          sizeof(GuestFlatAttr) },
	{ "offsetof(FlatAttr, span)",  offsetof(GuestFlatAttr, span) },
};

std::string probe_guest_layout(Script& script)
{
	const std::size_t count = EXPECTATIONS.size();
	const Script::gaddr_t addr = script.guest_alloc(count * sizeof(Script::gaddr_t));
	if (addr == 0)
		return "could not allocate the probe buffer on the guest heap";

	script.call("bench_probe_layout", addr);
	const auto* words = script.machine().memory.memarray<const Script::gaddr_t>(addr, count);

	std::string report;
	for (std::size_t i = 0; i < count; i++) {
		if (words[i] != EXPECTATIONS[i].expected) {
			report += "  " + std::string(EXPECTATIONS[i].what) + ": the guest says "
				+ std::to_string(words[i]) + ", the host mirror says "
				+ std::to_string(EXPECTATIONS[i].expected) + "\n";
		}
	}
	script.guest_free(addr);
	return report;
}

} // namespace bench
