/* The fixed-point arithmetic, which a compiler never emits from plain C.
 *
 * Each operation is computed twice: once with the vector intrinsic and
 * once with a scalar reference written straight from the specification's
 * pseudocode, and both are printed. The native build has no intrinsics,
 * so it prints the reference twice -- which makes a disagreement between
 * the two show up as a diff against the native run.
 */
#include <stdio.h>
#include <stdint.h>

#define N 64
static int32_t  s32[N], t32[N];
static uint32_t u32[N], v32[N];
static int16_t  s16[N];
static int64_t  s64[N];
static uint64_t u64[N];
static uint8_t  sh[N];

/* roundoff_signed / roundoff_unsigned, verbatim from the spec: shift
 * right by d, plus an increment chosen by the rounding mode. */
static __int128 roundoff(__int128 v, unsigned d, unsigned rm, int is_signed)
{
	if (d == 0) return v;
	__int128 shifted = is_signed ? (v >> d) : (__int128)((__uint128_t)v >> d);
	unsigned lsb = (unsigned)(((__uint128_t)v >> (d - 1)) & 1);
	int rest = d > 1 && (((__uint128_t)v & ((((__uint128_t)1) << (d - 1)) - 1)) != 0);
	switch (rm) {
		case 0: return shifted + lsb;
		case 1: return shifted + ((lsb && (rest || (shifted & 1))) ? 1 : 0);
		case 2: return shifted;
		default: return (lsb || rest) ? (shifted | 1) : shifted;
	}
}

static int64_t sat_s(__int128 v, int bits)
{
	__int128 lo = -(((__int128)1) << (bits - 1));
	__int128 hi = (((__int128)1) << (bits - 1)) - 1;
	if (v < lo) return (int64_t)lo;
	if (v > hi) return (int64_t)hi;
	return (int64_t)v;
}

static uint64_t sat_u(__int128 v, int bits)
{
	__int128 hi = (((__int128)1) << bits) - 1;
	if (v < 0) return 0;
	if (v > hi) return (uint64_t)hi;
	return (uint64_t)v;
}

/* ---- scalar references ---- */

static void ref_vsmul(const int32_t *a, const int32_t *b, int32_t *o, unsigned rm)
{
	for (int i = 0; i < N; i++) {
		__int128 p = (__int128)a[i] * b[i];
		o[i] = (int32_t)sat_s(roundoff(p, 31, rm, 1), 32);
	}
}

static void ref_vaadd(const int32_t *a, const int32_t *b, int32_t *o, unsigned rm, int sub)
{
	for (int i = 0; i < N; i++) {
		__int128 s = sub ? (__int128)a[i] - b[i] : (__int128)a[i] + b[i];
		o[i] = (int32_t)roundoff(s, 1, rm, 1);
	}
}

static void ref_vaaddu(const uint32_t *a, const uint32_t *b, uint32_t *o, unsigned rm, int sub)
{
	for (int i = 0; i < N; i++) {
		/* The difference is exact in SEW+1 bits and then truncated. */
		__uint128_t s = sub ? (__uint128_t)a[i] - b[i] : (__uint128_t)a[i] + b[i];
		o[i] = (uint32_t)roundoff((__int128)s, 1, rm, 0);
	}
}

static void ref_vssrl(const uint32_t *a, const uint8_t *s, uint32_t *o, unsigned rm)
{
	for (int i = 0; i < N; i++)
		o[i] = (uint32_t)roundoff((__int128)(__uint128_t)a[i], s[i] & 31, rm, 0);
}

static void ref_vssra(const int32_t *a, const uint8_t *s, int32_t *o, unsigned rm)
{
	for (int i = 0; i < N; i++)
		o[i] = (int32_t)roundoff((__int128)a[i], s[i] & 31, rm, 1);
}

static void ref_vnclip(const int64_t *a, unsigned s, int32_t *o, unsigned rm)
{
	for (int i = 0; i < N; i++)
		o[i] = (int32_t)sat_s(roundoff((__int128)a[i], s & 63, rm, 1), 32);
}

static void ref_vnclipu(const uint64_t *a, unsigned s, uint32_t *o, unsigned rm)
{
	for (int i = 0; i < N; i++)
		o[i] = (uint32_t)sat_u(roundoff((__int128)(__uint128_t)a[i], s & 63, rm, 0), 32);
}

static void ref_vnsrl(const uint64_t *a, unsigned s, uint32_t *o)
{
	for (int i = 0; i < N; i++) o[i] = (uint32_t)(a[i] >> (s & 63));
}

static void ref_vnsra(const int64_t *a, unsigned s, int32_t *o)
{
	for (int i = 0; i < N; i++) o[i] = (int32_t)(a[i] >> (s & 63));
}

static void ref_div(const int32_t *a, const int32_t *b, int32_t *o, int rem)
{
	for (int i = 0; i < N; i++) {
		if (b[i] == 0) { o[i] = rem ? a[i] : -1; continue; }
		if (a[i] == (int32_t)0x80000000 && b[i] == -1) { o[i] = rem ? 0 : a[i]; continue; }
		o[i] = rem ? a[i] % b[i] : a[i] / b[i];
	}
}

static void ref_divu(const uint32_t *a, const uint32_t *b, uint32_t *o, int rem)
{
	for (int i = 0; i < N; i++) {
		if (b[i] == 0) { o[i] = rem ? a[i] : 0xFFFFFFFFu; continue; }
		o[i] = rem ? a[i] % b[i] : a[i] / b[i];
	}
}

static uint64_t hash32(const int32_t *p)
{
	uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < N; i++) { h ^= (uint32_t)p[i]; h *= 1099511628211ull; }
	return h;
}
static uint64_t hashu32(const uint32_t *p) { return hash32((const int32_t*)p); }

#ifdef __riscv_v_intrinsic
#include <riscv_vector.h>
#define VL_I32 __riscv_vsetvl_e32m1(N)
#endif

int main(void)
{
	uint64_t seed = 0xDEADBEEF;
	for (int i = 0; i < N; i++) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		s32[i] = (int32_t)(seed >> 20);
		t32[i] = (int32_t)(seed >> 7);
		u32[i] = (uint32_t)(seed >> 33);
		v32[i] = (uint32_t)(seed >> 11);
		s16[i] = (int16_t)(seed >> 40);
		s64[i] = (int64_t)seed;
		u64[i] = seed;
		sh[i]  = (uint8_t)(seed >> 55);
	}
	/* corner values the random fill will not produce */
	s32[0] = (int32_t)0x80000000; t32[0] = -1;
	s32[1] = (int32_t)0x80000000; t32[1] = (int32_t)0x80000000;
	s32[2] = 0; t32[2] = 0;
	u32[2] = 0; v32[2] = 0;
	s64[0] = (int64_t)0x8000000000000000ll;
	u64[0] = 0xFFFFFFFFFFFFFFFFull;

	int32_t  ra[N], rb[N];
	uint32_t ua[N], ub[N];

	/* vxrm has to be a compile-time constant in the intrinsics, so the
	 * four rounding modes are a macro expanded four times rather than a
	 * loop. */
#ifdef __riscv_v_intrinsic
#define VEC_ROUND(RM) do { \
		size_t vl = VL_I32; \
		vint32m1_t a = __riscv_vle32_v_i32m1(s32, vl); \
		vint32m1_t b = __riscv_vle32_v_i32m1(t32, vl); \
		vuint32m1_t c = __riscv_vle32_v_u32m1(u32, vl); \
		vuint32m1_t d = __riscv_vle32_v_u32m1(v32, vl); \
		vuint32m1_t se = __riscv_vzext_vf4_u32m1(__riscv_vle8_v_u8mf4(sh, vl), vl); \
		__riscv_vse32_v_i32m1(ra, __riscv_vsmul_vv_i32m1(a, b, RM, vl), vl); \
		__riscv_vse32_v_i32m1(rb, __riscv_vaadd_vv_i32m1(a, b, RM, vl), vl); \
		__riscv_vse32_v_u32m1(ua, __riscv_vaaddu_vv_u32m1(c, d, RM, vl), vl); \
		__riscv_vse32_v_u32m1(ub, __riscv_vssrl_vv_u32m1(c, se, RM, vl), vl); \
	} while (0)
#define VEC_ROUND2(RM) do { \
		size_t vl = VL_I32; \
		vint32m1_t a = __riscv_vle32_v_i32m1(s32, vl); \
		vint32m1_t b = __riscv_vle32_v_i32m1(t32, vl); \
		vuint32m1_t c = __riscv_vle32_v_u32m1(u32, vl); \
		vuint32m1_t d = __riscv_vle32_v_u32m1(v32, vl); \
		vuint32m1_t se = __riscv_vzext_vf4_u32m1(__riscv_vle8_v_u8mf4(sh, vl), vl); \
		vint64m2_t w = __riscv_vle64_v_i64m2(s64, vl); \
		__riscv_vse32_v_i32m1(ra, __riscv_vasub_vv_i32m1(a, b, RM, vl), vl); \
		__riscv_vse32_v_u32m1(ua, __riscv_vasubu_vv_u32m1(c, d, RM, vl), vl); \
		__riscv_vse32_v_i32m1(rb, __riscv_vssra_vv_i32m1(a, se, RM, vl), vl); \
		__riscv_vse32_v_i32m1((int32_t*)ub, __riscv_vnclip_wx_i32m1(w, 17, RM, vl), vl); \
	} while (0)
#define VEC_ROUND3(RM) do { \
		size_t vl = VL_I32; \
		vuint64m2_t w = __riscv_vle64_v_u64m2(u64, vl); \
		__riscv_vse32_v_u32m1(ua, __riscv_vnclipu_wx_u32m1(w, 9, RM, vl), vl); \
	} while (0)
#else
#define VEC_ROUND(RM)  do { } while (0)
#define VEC_ROUND2(RM) do { } while (0)
#define VEC_ROUND3(RM) do { } while (0)
#endif

#define ROUND_PASS(RM) do { \
		ref_vsmul(s32, t32, ra, RM); \
		ref_vaadd(s32, t32, rb, RM, 0); \
		ref_vaaddu(u32, v32, ua, RM, 0); \
		ref_vssrl(u32, sh, ub, RM); \
		VEC_ROUND(RM); \
		printf("rm%u smul %016llx aadd %016llx aaddu %016llx ssrl %016llx\n", RM, \
			(unsigned long long)hash32(ra), (unsigned long long)hash32(rb), \
			(unsigned long long)hashu32(ua), (unsigned long long)hashu32(ub)); \
		ref_vaadd(s32, t32, ra, RM, 1); \
		ref_vaaddu(u32, v32, ua, RM, 1); \
		ref_vssra(s32, sh, rb, RM); \
		ref_vnclip(s64, 17, (int32_t*)ub, RM); \
		VEC_ROUND2(RM); \
		printf("rm%u asub %016llx asubu %016llx ssra %016llx nclip %016llx\n", RM, \
			(unsigned long long)hash32(ra), (unsigned long long)hashu32(ua), \
			(unsigned long long)hash32(rb), (unsigned long long)hash32((int32_t*)ub)); \
		ref_vnclipu(u64, 9, ua, RM); \
		VEC_ROUND3(RM); \
		printf("rm%u nclipu %016llx\n", RM, (unsigned long long)hashu32(ua)); \
	} while (0)

	ROUND_PASS(0);
	ROUND_PASS(1);
	ROUND_PASS(2);
	ROUND_PASS(3);

	/* The plain narrowing shifts, which must not round at all. */
	ref_vnsrl(u64, 13, ua);
	ref_vnsra(s64, 13, ra);
	ref_div(s32, t32, rb, 0);
	ref_divu(u32, v32, ub, 0);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = VL_I32;
		vuint64m2_t wu = __riscv_vle64_v_u64m2(u64, vl);
		vint64m2_t  ws = __riscv_vle64_v_i64m2(s64, vl);
		__riscv_vse32_v_u32m1(ua, __riscv_vnsrl_wx_u32m1(wu, 13, vl), vl);
		__riscv_vse32_v_i32m1(ra, __riscv_vnsra_wx_i32m1(ws, 13, vl), vl);
		vint32m1_t a = __riscv_vle32_v_i32m1(s32, vl);
		vint32m1_t b = __riscv_vle32_v_i32m1(t32, vl);
		__riscv_vse32_v_i32m1(rb, __riscv_vdiv_vv_i32m1(a, b, vl), vl);
		vuint32m1_t c = __riscv_vle32_v_u32m1(u32, vl);
		vuint32m1_t d = __riscv_vle32_v_u32m1(v32, vl);
		__riscv_vse32_v_u32m1(ub, __riscv_vdivu_vv_u32m1(c, d, vl), vl);
	}
#endif
	printf("nsrl %016llx nsra %016llx div %016llx divu %016llx\n",
		(unsigned long long)hashu32(ua), (unsigned long long)hash32(ra),
		(unsigned long long)hash32(rb), (unsigned long long)hashu32(ub));

	ref_div(s32, t32, ra, 1);
	ref_divu(u32, v32, ua, 1);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = VL_I32;
		vint32m1_t a = __riscv_vle32_v_i32m1(s32, vl);
		vint32m1_t b = __riscv_vle32_v_i32m1(t32, vl);
		__riscv_vse32_v_i32m1(ra, __riscv_vrem_vv_i32m1(a, b, vl), vl);
		vuint32m1_t c = __riscv_vle32_v_u32m1(u32, vl);
		vuint32m1_t d = __riscv_vle32_v_u32m1(v32, vl);
		__riscv_vse32_v_u32m1(ua, __riscv_vremu_vv_u32m1(c, d, vl), vl);
	}
#endif
	printf("rem %016llx remu %016llx\n",
		(unsigned long long)hash32(ra), (unsigned long long)hashu32(ua));
	(void)s16;
	return 0;
}
