/**
 * Zero-copy guest attributes vs. flat HostAttr marshalling.
 *
 * The same attribute tree is sent across the guest/host boundary in both
 * directions, once per strategy, and the harness reports what each costs in wall
 * time, emulated guest instructions and guest heap allocations. Every run also
 * checks the checksum of what arrived, so a faster number is only ever reported
 * for a path that delivered the same tree.
 */
#include "attributes.hpp"
#include "flat.hpp"
#include "script.hpp"
#include "zerocopy.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace bench;

static constexpr int ITERATIONS = 2000;
static constexpr int ROUNDS = 7;

struct Result {
	double ns_per_op = 0;
	double instructions_per_op = 0;
	double allocations_per_op = 0;
	uint64_t checksum = 0;
};

static double median(std::vector<double> values)
{
	std::sort(values.begin(), values.end());
	return values[values.size() / 2];
}

using clock_type = std::chrono::steady_clock;

static double elapsed_ns(clock_type::time_point t0, clock_type::time_point t1)
{
	return double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

/// @brief Guest -> host: the guest loops internally, so no vmcall overhead is
/// folded into the per-operation cost.
static Result measure_out(Script& script, const char* func)
{
	const auto addr = script.address_of(func);
	Result result;
	std::vector<double> times, instructions, allocations;

	for (int r = 0; r < ROUNDS; r++) {
		const unsigned allocs_before = script.allocations();
		const auto t0 = clock_type::now();
		const auto returned = script.call(addr, ITERATIONS);
		const auto t1 = clock_type::now();

		result.checksum = uint64_t(returned);
		times.push_back(elapsed_ns(t0, t1) / ITERATIONS);
		instructions.push_back(double(script.instructions()) / ITERATIONS);
		allocations.push_back(double(script.allocations() - allocs_before) / ITERATIONS);
	}
	result.ns_per_op = median(times);
	result.instructions_per_op = median(instructions);
	result.allocations_per_op = median(allocations);
	return result;
}

/// @brief Host -> guest: the host prepares the argument and makes one vmcall per
/// operation, so the cost includes both. The no-op baseline below is what a bare
/// vmcall costs, for subtracting.
template <typename Op>
static Result measure_in(Script& script, Op&& op)
{
	Result result;
	std::vector<double> times, instructions, allocations;

	for (int r = 0; r < ROUNDS; r++) {
		const unsigned allocs_before = script.allocations();
		uint64_t instr = 0;
		const auto t0 = clock_type::now();
		for (int i = 0; i < ITERATIONS; i++) {
			result.checksum = uint64_t(op());
			instr += script.instructions();
		}
		const auto t1 = clock_type::now();

		times.push_back(elapsed_ns(t0, t1) / ITERATIONS);
		instructions.push_back(double(instr) / ITERATIONS);
		allocations.push_back(double(script.allocations() - allocs_before) / ITERATIONS);
	}
	result.ns_per_op = median(times);
	result.instructions_per_op = median(instructions);
	result.allocations_per_op = median(allocations);
	return result;
}

static void print_row(const char* label, const Result& r)
{
	printf("  %-22s %10.0f %14.0f %13.1f\n",
		label, r.ns_per_op, r.instructions_per_op, r.allocations_per_op);
}

/// @brief Format a ratio, or "none" when the zero-copy path spent nothing at all
/// and the ratio would be a division by zero.
static std::string ratio(double flat_value, double zc_value)
{
	if (zc_value <= 0.0)
		return flat_value > 0.0 ? "none" : "-";
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%.2fx", flat_value / zc_value);
	return buffer;
}

static bool report(const char* direction, const Result& flat, const Result& zc,
	uint64_t expected)
{
	printf("\n %s\n", direction);
	printf("  %-22s %10s %14s %13s\n", "", "ns/op", "guest instr/op", "guest allocs");
	print_row("flat HostAttr", flat);
	print_row("zero-copy", zc);
	printf("  %-22s %10s %14s %13s\n", "zero-copy wins by",
		ratio(flat.ns_per_op, zc.ns_per_op).c_str(),
		ratio(flat.instructions_per_op, zc.instructions_per_op).c_str(),
		ratio(flat.allocations_per_op, zc.allocations_per_op).c_str());

	if (flat.checksum != expected || zc.checksum != expected) {
		printf("  FAIL: checksum mismatch (expected %016lx, flat %016lx, zero-copy %016lx)\n",
			(unsigned long)expected, (unsigned long)flat.checksum, (unsigned long)zc.checksum);
		return false;
	}
	return true;
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <guest.elf>\n", argv[0]);
		return 1;
	}
	Script script("attrbench", argv[1]);
	// Whatever the C++ runtime holds on to after boot (the emergency exception
	// pool, among others) is not ours, and is subtracted from the leak check
	const unsigned resident_after_boot = script.allocations() - script.deallocations();

	// What a vmcall costs before any attributes are involved
	const auto noop_addr = script.address_of("bench_noop");
	double noop_ns = 0;
	{
		std::vector<double> times;
		for (int r = 0; r < ROUNDS; r++) {
			const auto t0 = clock_type::now();
			for (int i = 0; i < ITERATIONS; i++)
				script.call(noop_addr);
			const auto t1 = clock_type::now();
			times.push_back(elapsed_ns(t0, t1) / ITERATIONS);
		}
		noop_ns = median(times);
	}
	printf("Bare vmcall baseline: %.0f ns  (%d iterations x %d rounds, median)\n",
		noop_ns, ITERATIONS, ROUNDS);

	const Workload* workloads[] = { &WORKLOAD_SMALL, &WORKLOAD_STRINGS, &WORKLOAD_NESTED };
	const auto in_flat_addr = script.address_of("bench_in_flat");
	const auto in_obj_addr = script.address_of("bench_in_obj");
	bool ok = true;

	for (int index = 0; index < 3; index++) {
		const Workload& w = *workloads[index];

		// Both sides build the same tree from the same description, and the guest
		// hands back its checksum: if these disagree, nothing below means anything
		const uint64_t guest_sum = uint64_t(script.call("bench_setup", index));
		HostAttributes host_tree;
		build_workload(host_tree, w);
		const uint64_t host_sum = checksum(host_tree);

		printf("\n=== Workload '%s': %d scalars, %d strings of %d bytes, "
			"%d groups of %d, %d lists of %d ===\n",
			w.name, w.scalars, w.strings, w.string_len,
			w.groups, w.group_size, w.lists, w.list_len);
		if (guest_sum != host_sum) {
			printf("  FAIL: the guest and the host built different trees "
				"(%016lx vs %016lx)\n", (unsigned long)guest_sum, (unsigned long)host_sum);
			ok = false;
			continue;
		}

		const Result out_flat = measure_out(script, "bench_out_flat");
		const Result out_zc = measure_out(script, "bench_out_obj");
		ok &= report("guest -> host (the script sends attributes to the engine)",
			out_flat, out_zc, host_sum);

		const Result in_flat = measure_in(script, [&] {
			const auto [addr, count] = writeFlatAttributes(script, host_tree);
			// The guest consumes the array, freeing every allocation in it
			return script.call(in_flat_addr, addr, uint64_t(count));
		});
		const Result in_zc = measure_in(script, [&] {
			const auto addr = createGuestAttributes(script, host_tree);
			const auto returned = script.call(in_obj_addr, addr);
			// The guest moved the tree out, so this frees an empty map
			destroyGuestAttributes(script, addr);
			return returned;
		});
		ok &= report("host -> guest (the engine hands attributes to a script)",
			in_flat, in_zc, host_sum);
	}

	// Every allocation either path made must be back on the guest heap. The
	// guest's own reference tree is dropped first, so anything left is a leak.
	script.call("bench_teardown");
	const unsigned leaked =
		script.allocations() - script.deallocations() - resident_after_boot;
	printf("\nGuest heap: %u allocations, %u frees, %u outstanding\n",
		script.allocations(), script.deallocations(), leaked);
	if (leaked != 0) {
		printf("  FAIL: %u guest allocation(s) leaked\n", leaked);
		ok = false;
	}

	printf("\n%s\n", ok ? "=== All paths delivered identical trees ===" : "=== FAILED ===");
	return ok ? 0 : 1;
}
