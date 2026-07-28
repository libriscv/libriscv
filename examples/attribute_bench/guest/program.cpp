/**
 * The guest half of the benchmark.
 *
 * One reference tree is built per workload and then shipped across the boundary
 * over and over, both ways, once per marshalling strategy. Every entry point
 * returns a checksum so the harness can prove the two strategies deliver
 * identical data.
 */
#include "attributes.hpp"
#include "hostcalls.hpp"

GENERATE_SYSCALL_WRAPPER(sys_attr_out_flat, SYSCALL_OUT_FLAT)
GENERATE_SYSCALL_WRAPPER(sys_attr_out_obj,  SYSCALL_OUT_OBJ)
GENERATE_SYSCALL_WRAPPER(sys_print,         SYSCALL_PRINT)

#define PUBLIC(x) extern "C" __attribute__((used, retain)) x

using namespace bench;

static const Workload* workload_at(int index)
{
	static const Workload* workloads[] = {
		&WORKLOAD_SMALL, &WORKLOAD_STRINGS, &WORKLOAD_NESTED
	};
	if (index < 0 || index >= int(sizeof(workloads) / sizeof(workloads[0])))
		return nullptr;
	return workloads[index];
}

/// @brief The tree the guest sends out. Built once per workload.
static Attributes g_reference;

PUBLIC(long bench_setup(int index))
{
	const Workload* w = workload_at(index);
	if (w == nullptr)
		return -1;
	g_reference.clear();
	build_workload(g_reference, *w);
	return long(checksum(g_reference));
}

// --- guest -> host ----------------------------------------------------------

/// @brief Flatten the tree and hand the host the array, once per iteration.
PUBLIC(long bench_out_flat(int iterations))
{
	long result = 0;
	for (int i = 0; i < iterations; i++) {
		const auto nodes = g_reference.createHostAttr();
		result = sys_attr_out_flat(nodes.data(), nodes.size());
		Attributes::destroyHostAttr(nodes);
	}
	return result;
}

/// @brief Hand the host the address of the tree itself, once per iteration.
PUBLIC(long bench_out_obj(int iterations))
{
	long result = 0;
	for (int i = 0; i < iterations; i++)
		result = sys_attr_out_obj(&g_reference);
	return result;
}

// --- host -> guest ----------------------------------------------------------

/// @brief Rebuild a tree from a flat array the host wrote into guest memory,
/// which also frees every allocation the host made for it.
PUBLIC(long bench_in_flat(const Attributes::HostAttr* nodes, size_t count))
{
	const Attributes attrs = Attributes::fromGuestAttributes({nodes, count});
	return long(checksum(attrs));
}

/// @brief Take ownership of a tree the host built directly on the guest heap.
/// The move is a handful of pointer swaps; the host is left with an empty map.
PUBLIC(long bench_in_obj(Attributes* attrs))
{
	const Attributes owned = std::move(*attrs);
	return long(checksum(owned));
}

/// @brief The vmcall baseline, so the harness can report what a call costs
/// before any attributes are involved.
PUBLIC(long bench_noop())
{
	return 0;
}

/// @brief Release the reference tree, so the harness can account for the guest
/// heap without the live tree counting as outstanding. clear() alone would keep
/// the map's bucket array, so the whole map is swapped out and destroyed.
PUBLIC(void bench_teardown())
{
	Attributes empty;
	empty.getAllAttributes().swap(g_reference.getAllAttributes());
}

extern "C" __attribute__((noreturn)) void fast_exit(int);

int main()
{
	sys_print("Guest: ready, pausing.\n");
	fast_exit(0);
}
