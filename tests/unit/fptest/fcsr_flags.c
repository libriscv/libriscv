/**
 * Regression tests for FCSR-emulation defects found while merging the
 * floating-point accuracy patches. Each case here failed on at least one
 * execution lane (precise, bytecode, binary-translated) before the fix.
 *
 * Only meaningful when the library is built with RISCV_FCSR=ON; the unit
 * test that loads this file skips it otherwise.
 */
#include <stdio.h>
#include <stdint.h>

static int failures = 0;

static void chk(const char* name, uint64_t got, uint64_t want)
{
	if (got != want) {
		printf("FAIL %-34s got 0x%llx want 0x%llx\n", name,
			(unsigned long long)got, (unsigned long long)want);
		failures++;
	}
}

#define SETFRM(rm) __asm__ volatile("fsrm %0" :: "r"((unsigned)(rm)))
#define CLRFLAGS() __asm__ volatile("fsflags zero")

int main(void)
{
	/* FCVT float->int must not write its source f-register when the
	   operand is not NaN-boxed. */
	{
		uint64_t raw = 0x00000000BF800000ull; /* -1.0f, upper half not all ones */
		uint64_t back;
		long out;
		__asm__ volatile(
			"fmv.d.x  fa0, %2\n"
			"fcvt.w.s %0, fa0\n"
			"fmv.x.d  %1, fa0\n"
			: "=r"(out), "=r"(back) : "r"(raw) : "fa0");
		chk("fcvt.w.s preserves rs1", back, raw);
		/* ...and reads the operand as the canonical qNaN, which saturates. */
		chk("fcvt.w.s non-boxed -> INT32_MAX",
			(uint64_t)(int64_t)out, (uint64_t)(int64_t)2147483647);
	}

	/* The inexact flag must come from the operands, not from the result:
	   rd is allowed to alias rs1. */
	{
		unsigned fl;
		uint32_t a = 0x3F800000u, b = 0x33800000u; /* 1.0f + 2^-24f */
		CLRFLAGS();
		__asm__ volatile(
			"fmv.w.x fa0, %1\n fmv.w.x fa1, %2\n"
			"fadd.s  fa0, fa0, fa1\n"
			"frflags %0\n"
			: "=r"(fl) : "r"(a), "r"(b) : "fa0", "fa1");
		chk("fadd.s NX with rd==rs1", fl & 1u, 1u);

		a = 0x3F800000u; b = 0x40400000u; /* 1.0f / 3.0f */
		CLRFLAGS();
		__asm__ volatile(
			"fmv.w.x fa0, %1\n fmv.w.x fa1, %2\n"
			"fdiv.s  fa0, fa0, fa1\n"
			"frflags %0\n"
			: "=r"(fl) : "r"(a), "r"(b) : "fa0", "fa1");
		chk("fdiv.s NX with rd==rs1", fl & 1u, 1u);
	}

	/* FMUL overflow delivers a sign-dependent value in the directed
	   rounding modes (IEEE 754 section 7.4). */
	{
		static const struct { unsigned rm; uint32_t a, b, want; const char* n; } t[] = {
			{1, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu, "fmul.s RTZ +ovf -> +max"},
			{1, 0xFF7FFFFFu, 0x7F7FFFFFu, 0xFF7FFFFFu, "fmul.s RTZ -ovf -> -max"},
			{2, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu, "fmul.s RDN +ovf -> +max"},
			{2, 0xFF7FFFFFu, 0x7F7FFFFFu, 0xFF800000u, "fmul.s RDN -ovf -> -inf"},
			{3, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F800000u, "fmul.s RUP +ovf -> +inf"},
			{3, 0xFF7FFFFFu, 0x7F7FFFFFu, 0xFF7FFFFFu, "fmul.s RUP -ovf -> -max"},
			{0, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F800000u, "fmul.s RNE +ovf -> +inf"},
		};
		uint32_t r;
		for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
			SETFRM(t[i].rm);
			__asm__ volatile(
				"fmv.w.x fa0, %1\n fmv.w.x fa1, %2\n"
				"fmul.s  fa0, fa0, fa1\n fmv.x.w %0, fa0\n"
				: "=r"(r) : "r"(t[i].a), "r"(t[i].b) : "fa0", "fa1");
			chk(t[i].n, r, t[i].want);
		}
		SETFRM(0);
	}

	/* Integer->float honours the rounding mode. RMM differs from RNE only
	   at an exact halfway point, and a negative tie rounds away from zero. */
	{
		static const struct { unsigned rm; int64_t v; uint32_t want; const char* n; } t[] = {
			{4,  33554433, 0x4C000000u, "fcvt.s.l RMM 2^25+1 (non-tie)"},
			{4,  33554434, 0x4C000001u, "fcvt.s.l RMM 2^25+2 (tie)"},
			{4, -33554434, 0xCC000001u, "fcvt.s.l RMM -(2^25+2) tie"},
			{3,  16777217, 0x4B800001u, "fcvt.s.l RUP 2^24+1"},
			{1,  16777217, 0x4B800000u, "fcvt.s.l RTZ 2^24+1"},
			{2, -16777217, 0xCB800001u, "fcvt.s.l RDN -(2^24+1)"},
			{3, -16777217, 0xCB800000u, "fcvt.s.l RUP -(2^24+1)"},
		};
		uint32_t r;
		for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
			SETFRM(t[i].rm);
			__asm__ volatile("fcvt.s.l fa0, %1\n fmv.x.w %0, fa0\n"
				: "=r"(r) : "r"(t[i].v) : "fa0");
			chk(t[i].n, r, t[i].want);
		}
		SETFRM(0);
	}

	/* Float->int reports inexact. */
	{
		uint32_t v = 0x41780000u; /* 15.5f */
		unsigned fl;
		long out;
		CLRFLAGS();
		__asm__ volatile("fmv.w.x fa0, %2\n fcvt.w.s %0, fa0, rtz\n frflags %1\n"
			: "=r"(out), "=r"(fl) : "r"(v) : "fa0");
		chk("fcvt.w.s 15.5 rtz NX", fl & 1u, 1u);
		chk("fcvt.w.s 15.5 rtz value", (uint64_t)out, 15u);
	}

	/* FSQRT reports inexact, but not for an exact root. */
	{
		uint32_t v = 0x40000000u; /* 2.0f */
		unsigned fl;
		CLRFLAGS();
		__asm__ volatile("fmv.w.x fa0, %1\n fsqrt.s fa0, fa0\n frflags %0\n"
			: "=r"(fl) : "r"(v) : "fa0");
		chk("fsqrt.s 2.0 NX", fl & 1u, 1u);

		v = 0x3F800000u; /* 1.0f */
		CLRFLAGS();
		__asm__ volatile("fmv.w.x fa0, %1\n fsqrt.s fa0, fa0\n frflags %0\n"
			: "=r"(fl) : "r"(v) : "fa0");
		chk("fsqrt.s 1.0 exact (no NX)", fl & 1u, 0u);
	}

	/* A single signaling-NaN operand yields the other operand, which the
	   host std::fmin/fmax do not guarantee. */
	{
		uint32_t snan = 0x7F800001u, one = 0x3F800000u, r;
		__asm__ volatile(
			"fmv.w.x fa0, %1\n fmv.w.x fa1, %2\n"
			"fmin.s  fa0, fa0, fa1\n fmv.x.w %0, fa0\n"
			: "=r"(r) : "r"(snan), "r"(one) : "fa0", "fa1");
		chk("fmin.s(sNaN, 1.0) -> 1.0", r, one);
	}

	if (failures)
		printf("%d FCSR regression failure(s)\n", failures);
	return failures != 0;
}
