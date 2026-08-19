/* Every vector addressing mode: unit-stride, strided, indexed in both
 * orderings, segmented at each field count, fault-only-first, the mask
 * transfers, and the whole-register load and store -- across LMUL and
 * every element width. See 06_fixedpoint.c for how the comparison works.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define N 64
static uint8_t  m8[N * 8];
static uint16_t m16[N * 8];
static uint32_t m32[N * 8];
static uint64_t m64[N * 8];
static uint32_t out32[N * 8];
static uint64_t out64[N * 8];
static uint8_t  out8[N * 8];
static uint16_t out16[N * 8];
static uint16_t sidx16[N];
static uint32_t sidx32[N];

static uint64_t hashb(const void *p, size_t bytes)
{
	const uint8_t *q = (const uint8_t*)p;
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < bytes; i++) { h ^= q[i]; h *= 1099511628211ull; }
	return h;
}
#define HB(p, n) ((unsigned long long)hashb((p), (n)))

#ifdef __riscv_v_intrinsic
#include <riscv_vector.h>
#endif

int main(void)
{
	uint64_t seed = 0xABCDEF;
	for (size_t i = 0; i < N * 8; i++) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		m8[i]  = (uint8_t)seed;
		m16[i] = (uint16_t)(seed >> 8);
		m32[i] = (uint32_t)(seed >> 16);
		m64[i] = seed;
	}
	for (int i = 0; i < N; i++) {
		sidx32[i] = (uint32_t)(((i * 37) % N) * 4);   /* byte offsets */
		sidx16[i] = (uint16_t)(((i * 23) % N) * 4);
	}

	/* ---- unit-stride at every width, and at several LMULs ---- */
	memset(out8, 0, sizeof out8);
	memset(out16, 0, sizeof out16);
	memset(out32, 0, sizeof out32);
	memset(out64, 0, sizeof out64);
	for (int i = 0; i < N; i++) { out8[i] = m8[i]; out16[i] = m16[i]; }
	for (int i = 0; i < N; i++) { out32[i] = m32[i]; out64[i] = m64[i]; }
#ifdef __riscv_v_intrinsic
	{
		size_t i = 0;
		while (i < N) { size_t vl = __riscv_vsetvl_e8m2(N - i);
			__riscv_vse8_v_u8m2(out8 + i, __riscv_vle8_v_u8m2(m8 + i, vl), vl); i += vl; }
		i = 0;
		while (i < N) { size_t vl = __riscv_vsetvl_e16m4(N - i);
			__riscv_vse16_v_u16m4(out16 + i, __riscv_vle16_v_u16m4(m16 + i, vl), vl); i += vl; }
		i = 0;
		while (i < N) { size_t vl = __riscv_vsetvl_e32m8(N - i);
			__riscv_vse32_v_u32m8(out32 + i, __riscv_vle32_v_u32m8(m32 + i, vl), vl); i += vl; }
		i = 0;
		while (i < N) { size_t vl = __riscv_vsetvl_e64m1(N - i);
			__riscv_vse64_v_u64m1(out64 + i, __riscv_vle64_v_u64m1(m64 + i, vl), vl); i += vl; }
	}
#endif
	printf("unit %016llx %016llx %016llx %016llx\n",
		HB(out8, N), HB(out16, N * 2), HB(out32, N * 4), HB(out64, N * 8));

	/* ---- strided, forwards and backwards ---- */
	memset(out32, 0, sizeof out32);
	for (int i = 0; i < N; i++) out32[i] = m32[i * 3];
#ifdef __riscv_v_intrinsic
	{
		size_t i = 0;
		while (i < N) {
			size_t vl = __riscv_vsetvl_e32m2(N - i);
			__riscv_vse32_v_u32m2(out32 + i,
				__riscv_vlse32_v_u32m2(m32 + i * 3, 12, vl), vl);
			i += vl;
		}
	}
#endif
	printf("stride %016llx\n", HB(out32, N * 4));

	memset(out32, 0, sizeof out32);
	for (int i = 0; i < N; i++) out32[i] = m32[N - 1 - i];
#ifdef __riscv_v_intrinsic
	{
		size_t i = 0;
		while (i < N) {
			size_t vl = __riscv_vsetvl_e32m2(N - i);
			__riscv_vse32_v_u32m2(out32 + i,
				__riscv_vlse32_v_u32m2(m32 + (N - 1 - i), -4, vl), vl);
			i += vl;
		}
	}
#endif
	printf("negstride %016llx\n", HB(out32, N * 4));

	/* a strided store, which scatters a contiguous vector */
	memset(out32, 0, sizeof out32);
	for (int i = 0; i < N; i++) out32[i * 2] = m32[i];
#ifdef __riscv_v_intrinsic
	{
		size_t i = 0;
		while (i < N) {
			size_t vl = __riscv_vsetvl_e32m2(N - i);
			__riscv_vsse32_v_u32m2(out32 + i * 2, 8,
				__riscv_vle32_v_u32m2(m32 + i, vl), vl);
			i += vl;
		}
	}
#endif
	printf("sstore %016llx\n", HB(out32, N * 8));

	/* ---- indexed, both index widths and both orderings ---- */
	memset(out32, 0, sizeof out32);
	for (int i = 0; i < N; i++) out32[i] = m32[sidx32[i] / 4];
#ifdef __riscv_v_intrinsic
	{
		size_t i = 0;
		while (i < N) {
			size_t vl = __riscv_vsetvl_e32m2(N - i);
			__riscv_vse32_v_u32m2(out32 + i,
				__riscv_vluxei32_v_u32m2(m32, __riscv_vle32_v_u32m2(sidx32 + i, vl), vl), vl);
			i += vl;
		}
	}
#endif
	printf("luxei32 %016llx\n", HB(out32, N * 4));

	memset(out32, 0, sizeof out32);
	for (int i = 0; i < N; i++) out32[i] = m32[sidx16[i] / 4];
#ifdef __riscv_v_intrinsic
	{
		size_t i = 0;
		while (i < N) {
			size_t vl = __riscv_vsetvl_e32m2(N - i);
			__riscv_vse32_v_u32m2(out32 + i,
				__riscv_vloxei16_v_u32m2(m32, __riscv_vle16_v_u16m1(sidx16 + i, vl), vl), vl);
			i += vl;
		}
	}
#endif
	printf("loxei16 %016llx\n", HB(out32, N * 4));

	/* an indexed store. The index permutation is one-to-one, so the
	 * ordered and unordered forms must agree. */
	memset(out32, 0, sizeof out32);
	for (int i = 0; i < N; i++) out32[sidx32[i] / 4] = m32[i];
#ifdef __riscv_v_intrinsic
	{
		size_t i = 0;
		while (i < N) {
			size_t vl = __riscv_vsetvl_e32m2(N - i);
			__riscv_vsoxei32_v_u32m2(out32, __riscv_vle32_v_u32m2(sidx32 + i, vl),
				__riscv_vle32_v_u32m2(m32 + i, vl), vl);
			i += vl;
		}
	}
#endif
	printf("soxei32 %016llx\n", HB(out32, N * 4));

	/* ---- segments, at each field count ---- */
	memset(out32, 0, sizeof out32);
	for (int i = 0; i < N; i++) {           /* 2-field: swap the pair */
		out32[i * 2]     = m32[i * 2 + 1];
		out32[i * 2 + 1] = m32[i * 2];
	}
#ifdef __riscv_v_intrinsic
	{
		size_t i = 0;
		while (i < N) {
			size_t vl = __riscv_vsetvl_e32m2(N - i);
			vuint32m2x2_t s = __riscv_vlseg2e32_v_u32m2x2(m32 + i * 2, vl);
			vuint32m2x2_t t = __riscv_vcreate_v_u32m2x2(
				__riscv_vget_v_u32m2x2_u32m2(s, 1), __riscv_vget_v_u32m2x2_u32m2(s, 0));
			__riscv_vsseg2e32_v_u32m2x2(out32 + i * 2, t, vl);
			i += vl;
		}
	}
#endif
	printf("seg2 %016llx\n", HB(out32, N * 8));

	memset(out8, 0, sizeof out8);
	for (int i = 0; i < N; i++) {           /* 3-field: rotate the triple */
		out8[i * 3]     = m8[i * 3 + 2];
		out8[i * 3 + 1] = m8[i * 3];
		out8[i * 3 + 2] = m8[i * 3 + 1];
	}
#ifdef __riscv_v_intrinsic
	{
		size_t i = 0;
		while (i < N) {
			size_t vl = __riscv_vsetvl_e8m1(N - i);
			vuint8m1x3_t s = __riscv_vlseg3e8_v_u8m1x3(m8 + i * 3, vl);
			vuint8m1x3_t t = __riscv_vcreate_v_u8m1x3(
				__riscv_vget_v_u8m1x3_u8m1(s, 2), __riscv_vget_v_u8m1x3_u8m1(s, 0),
				__riscv_vget_v_u8m1x3_u8m1(s, 1));
			__riscv_vsseg3e8_v_u8m1x3(out8 + i * 3, t, vl);
			i += vl;
		}
	}
#endif
	printf("seg3 %016llx\n", HB(out8, N * 3));

	memset(out16, 0, sizeof out16);
	for (int i = 0; i < N; i++) {           /* 4-field: reverse the quad */
		for (int f = 0; f < 4; f++) out16[i * 4 + f] = m16[i * 4 + (3 - f)];
	}
#ifdef __riscv_v_intrinsic
	{
		size_t i = 0;
		while (i < N) {
			size_t vl = __riscv_vsetvl_e16m1(N - i);
			vuint16m1x4_t s = __riscv_vlseg4e16_v_u16m1x4(m16 + i * 4, vl);
			vuint16m1x4_t t = __riscv_vcreate_v_u16m1x4(
				__riscv_vget_v_u16m1x4_u16m1(s, 3), __riscv_vget_v_u16m1x4_u16m1(s, 2),
				__riscv_vget_v_u16m1x4_u16m1(s, 1), __riscv_vget_v_u16m1x4_u16m1(s, 0));
			__riscv_vsseg4e16_v_u16m1x4(out16 + i * 4, t, vl);
			i += vl;
		}
	}
#endif
	printf("seg4 %016llx\n", HB(out16, N * 8));

	memset(out8, 0, sizeof out8);
	for (int i = 0; i < N; i++) {           /* 8-field, the widest form */
		for (int f = 0; f < 8; f++) out8[i * 8 + f] = m8[i * 8 + ((f + 3) & 7)];
	}
#ifdef __riscv_v_intrinsic
	{
		size_t i = 0;
		while (i < N) {
			size_t vl = __riscv_vsetvl_e8m1(N - i);
			vuint8m1x8_t s = __riscv_vlseg8e8_v_u8m1x8(m8 + i * 8, vl);
			vuint8m1x8_t t = __riscv_vcreate_v_u8m1x8(
				__riscv_vget_v_u8m1x8_u8m1(s, 3), __riscv_vget_v_u8m1x8_u8m1(s, 4),
				__riscv_vget_v_u8m1x8_u8m1(s, 5), __riscv_vget_v_u8m1x8_u8m1(s, 6),
				__riscv_vget_v_u8m1x8_u8m1(s, 7), __riscv_vget_v_u8m1x8_u8m1(s, 0),
				__riscv_vget_v_u8m1x8_u8m1(s, 1), __riscv_vget_v_u8m1x8_u8m1(s, 2));
			__riscv_vsseg8e8_v_u8m1x8(out8 + i * 8, t, vl);
			i += vl;
		}
	}
#endif
	printf("seg8 %016llx\n", HB(out8, N * 8));

	/* a strided segment load, which combines the two addressing modes */
	memset(out32, 0, sizeof out32);
	for (int i = 0; i < N; i++) {
		out32[i * 2]     = m32[i * 6];
		out32[i * 2 + 1] = m32[i * 6 + 1];
	}
#ifdef __riscv_v_intrinsic
	{
		size_t i = 0;
		while (i < N) {
			size_t vl = __riscv_vsetvl_e32m1(N - i);
			vuint32m1x2_t s = __riscv_vlsseg2e32_v_u32m1x2(m32 + i * 6, 24, vl);
			__riscv_vsseg2e32_v_u32m1x2(out32 + i * 2, s, vl);
			i += vl;
		}
	}
#endif
	printf("sseg2 %016llx\n", HB(out32, N * 8));

	/* ---- fault-only-first, which is how a strlen-shaped loop reads up
	 * to a page boundary without knowing where the data ends ---- */
	{
		size_t len = 0;
		static uint8_t str[4096];
		memset(str, 'x', sizeof str);
		str[3000] = 0;
		while (str[len]) len++;
#ifdef __riscv_v_intrinsic
		{
			size_t i = 0, found = 0;
			while (!found) {
				size_t vl = __riscv_vsetvl_e8m2(sizeof str - i);
				size_t got = vl;
				vuint8m2_t v = __riscv_vle8ff_v_u8m2(str + i, &got, vl);
				vbool4_t z = __riscv_vmseq_vx_u8m2_b4(v, 0, got);
				long first = __riscv_vfirst_m_b4(z, got);
				if (first >= 0) { i += (size_t)first; found = 1; }
				else i += got;
			}
			len = i;
		}
#endif
		printf("faultfirst %zu\n", len);
	}

	/* ---- the mask transfers ---- */
	{
		static uint8_t mbits[64], mout[64];
		for (int i = 0; i < 64; i++) mbits[i] = (uint8_t)(i * 37 + 1);
		memset(mout, 0, sizeof mout);
		memcpy(mout, mbits, 8);
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e8m1(64);
			__riscv_vsm_v_b8(mout, __riscv_vlm_v_b8(mbits, vl), vl);
		}
#endif
		printf("maskld %016llx\n", HB(mout, 8));
	}

	/* ---- whole-register load and store, which ignore vl and vtype ---- */
	{
		static uint8_t src[256], dst[256];
		for (int i = 0; i < 256; i++) src[i] = (uint8_t)(i * 5 + 3);
		memset(dst, 0, sizeof dst);
		/* The reference copies two registers' worth, whatever that is;
		 * the vector pair then has to reproduce it exactly. */
		size_t vlenb = 32;
#ifdef __riscv_v_intrinsic
		vlenb = (size_t)__riscv_vlenb();
#endif
		memcpy(dst, src, vlenb * 2);
#ifdef __riscv_v_intrinsic
		{
			/* A deliberately tiny vl must not shorten the transfer, so the
			 * copy is preceded by one and the result still has to match. */
			memset(dst, 0, sizeof dst);
			__riscv_vsetvl_e8m1(1);
			asm volatile("vl2r.v v8, (%0)\n\tvs2r.v v8, (%1)"
				:: "r"(src), "r"(dst) : "memory", "v8", "v9");
		}
#endif
		printf("wholereg %zu %016llx\n", vlenb, HB(dst, 64));
	}
	return 0;
}
