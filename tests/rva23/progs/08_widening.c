/* The widening arithmetic, the reciprocal estimates, and the Zvbb bit
 * operations. See 06_fixedpoint.c for how the comparison works.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define N 32
static int32_t   a32[N], b32[N];
static uint32_t  ua32[N], ub32[N];
static int64_t   w64[N];
static uint64_t  uw64[N];
static float     fa[N], fb[N];
static double    dw[N];

static uint64_t hash64(const void *p, int n)
{
	const uint64_t *q = (const uint64_t*)p;
	uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < n; i++) { h ^= q[i]; h *= 1099511628211ull; }
	return h;
}
static uint64_t hash32(const void *p, int n)
{
	const uint32_t *q = (const uint32_t*)p;
	uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < n; i++) { h ^= q[i]; h *= 1099511628211ull; }
	return h;
}
#define H64(p) ((unsigned long long)hash64((p), N))
#define H32(p) ((unsigned long long)hash32((p), N))

#ifdef __riscv_v_intrinsic
#include <riscv_vector.h>
#endif

int main(void)
{
	uint64_t seed = 0x1234567;
	for (int i = 0; i < N; i++) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		a32[i]  = (int32_t)(seed >> 21);
		b32[i]  = (int32_t)(seed >> 3);
		ua32[i] = (uint32_t)(seed >> 30);
		ub32[i] = (uint32_t)(seed >> 9);
		w64[i]  = (int64_t)(seed >> 1);
		uw64[i] = seed;
		fa[i] = (float)((int32_t)(seed >> 40)) * 0.0009765625f;
		fb[i] = (float)((int32_t)(seed >> 44)) * 0.03125f + 1.0f;
	}
	a32[0] = (int32_t)0x80000000; b32[0] = (int32_t)0x80000000;
	ua32[0] = 0xFFFFFFFFu; ub32[0] = 0xFFFFFFFFu;
	fb[0] = 1.0f; fb[1] = 0.5f; fb[2] = 4.0f;

	/* The result buffers are static so that every check hashes exactly
	 * what the step before it wrote, with no stack slots shared between
	 * steps and no uninitialised tail. */
	static int64_t  r64[N]; static uint64_t ru64[N]; static double rd[N];
	static uint32_t ru32[N];

	/* ---- widening integer add/sub, narrow and .w forms ---- */
	for (int i = 0; i < N; i++) r64[i] = (int64_t)a32[i] + (int64_t)b32[i];
	for (int i = 0; i < N; i++) ru64[i] = (uint64_t)ua32[i] + (uint64_t)ub32[i];
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vint32m1_t x = __riscv_vle32_v_i32m1(a32, vl), y = __riscv_vle32_v_i32m1(b32, vl);
		__riscv_vse64_v_i64m2(r64, __riscv_vwadd_vv_i64m2(x, y, vl), vl);
		vuint32m1_t u = __riscv_vle32_v_u32m1(ua32, vl), v = __riscv_vle32_v_u32m1(ub32, vl);
		__riscv_vse64_v_u64m2(ru64, __riscv_vwaddu_vv_u64m2(u, v, vl), vl);
	}
#endif
	printf("wadd %016llx %016llx\n", H64(r64), H64(ru64));

	for (int i = 0; i < N; i++) r64[i] = (int64_t)a32[i] - (int64_t)b32[i];
	for (int i = 0; i < N; i++) ru64[i] = (uint64_t)ua32[i] - (uint64_t)ub32[i];
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vint32m1_t x = __riscv_vle32_v_i32m1(a32, vl), y = __riscv_vle32_v_i32m1(b32, vl);
		__riscv_vse64_v_i64m2(r64, __riscv_vwsub_vv_i64m2(x, y, vl), vl);
		vuint32m1_t u = __riscv_vle32_v_u32m1(ua32, vl), v = __riscv_vle32_v_u32m1(ub32, vl);
		__riscv_vse64_v_u64m2(ru64, __riscv_vwsubu_vv_u64m2(u, v, vl), vl);
	}
#endif
	printf("wsub %016llx %016llx\n", H64(r64), H64(ru64));

	/* the .w forms: vs2 is already wide */
	for (int i = 0; i < N; i++) r64[i] = w64[i] + (int64_t)b32[i];
	for (int i = 0; i < N; i++) ru64[i] = uw64[i] - (uint64_t)ub32[i];
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vint64m2_t w = __riscv_vle64_v_i64m2(w64, vl);
		vint32m1_t y = __riscv_vle32_v_i32m1(b32, vl);
		__riscv_vse64_v_i64m2(r64, __riscv_vwadd_wv_i64m2(w, y, vl), vl);
		vuint64m2_t uw = __riscv_vle64_v_u64m2(uw64, vl);
		vuint32m1_t v = __riscv_vle32_v_u32m1(ub32, vl);
		__riscv_vse64_v_u64m2(ru64, __riscv_vwsubu_wv_u64m2(uw, v, vl), vl);
	}
#endif
	printf("wadd_w %016llx wsub_w %016llx\n", H64(r64), H64(ru64));

	/* ---- widening multiplies ---- */
	for (int i = 0; i < N; i++) r64[i] = (int64_t)a32[i] * (int64_t)b32[i];
	for (int i = 0; i < N; i++) ru64[i] = (uint64_t)ua32[i] * (uint64_t)ub32[i];
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vint32m1_t x = __riscv_vle32_v_i32m1(a32, vl), y = __riscv_vle32_v_i32m1(b32, vl);
		__riscv_vse64_v_i64m2(r64, __riscv_vwmul_vv_i64m2(x, y, vl), vl);
		vuint32m1_t u = __riscv_vle32_v_u32m1(ua32, vl), v = __riscv_vle32_v_u32m1(ub32, vl);
		__riscv_vse64_v_u64m2(ru64, __riscv_vwmulu_vv_u64m2(u, v, vl), vl);
	}
#endif
	printf("wmul %016llx %016llx\n", H64(r64), H64(ru64));

	/* vwmulsu: vs2 signed, vs1 unsigned */
	for (int i = 0; i < N; i++) r64[i] = (int64_t)a32[i] * (int64_t)(uint64_t)ub32[i];
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vint32m1_t x = __riscv_vle32_v_i32m1(a32, vl);
		vuint32m1_t v = __riscv_vle32_v_u32m1(ub32, vl);
		__riscv_vse64_v_i64m2(r64, __riscv_vwmulsu_vv_i64m2(x, v, vl), vl);
	}
#endif
	printf("wmulsu %016llx\n", H64(r64));

	/* ---- widening multiply-accumulate, all four signedness mixes ---- */
	for (int i = 0; i < N; i++) r64[i] = w64[i] + (int64_t)a32[i] * (int64_t)b32[i];
	for (int i = 0; i < N; i++) ru64[i] = uw64[i] + (uint64_t)ua32[i] * (uint64_t)ub32[i];
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vint64m2_t w = __riscv_vle64_v_i64m2(w64, vl);
		vint32m1_t x = __riscv_vle32_v_i32m1(a32, vl), y = __riscv_vle32_v_i32m1(b32, vl);
		__riscv_vse64_v_i64m2(r64, __riscv_vwmacc_vv_i64m2(w, x, y, vl), vl);
		vuint64m2_t uw = __riscv_vle64_v_u64m2(uw64, vl);
		vuint32m1_t u = __riscv_vle32_v_u32m1(ua32, vl), v = __riscv_vle32_v_u32m1(ub32, vl);
		__riscv_vse64_v_u64m2(ru64, __riscv_vwmaccu_vv_u64m2(uw, u, v, vl), vl);
	}
#endif
	printf("wmacc %016llx %016llx\n", H64(r64), H64(ru64));

	/* vwmaccsu takes a signed vs1 and an unsigned vs2; vwmaccus is the
	 * mirror image and exists only in the .vx form. */
	for (int i = 0; i < N; i++)
		r64[i] = w64[i] + (int64_t)b32[i] * (int64_t)(uint64_t)ua32[i];
	for (int i = 0; i < N; i++)
		ru64[i] = (uint64_t)(w64[i] + (int64_t)(uint64_t)(uint32_t)7u * (int64_t)a32[i]);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vint64m2_t w = __riscv_vle64_v_i64m2(w64, vl);
		vint32m1_t y = __riscv_vle32_v_i32m1(b32, vl);
		vuint32m1_t u = __riscv_vle32_v_u32m1(ua32, vl);
		__riscv_vse64_v_i64m2(r64, __riscv_vwmaccsu_vv_i64m2(w, y, u, vl), vl);
		vint32m1_t x = __riscv_vle32_v_i32m1(a32, vl);
		__riscv_vse64_v_i64m2((int64_t*)ru64, __riscv_vwmaccus_vx_i64m2(w, 7u, x, vl), vl);
	}
#endif
	printf("wmaccsu %016llx wmaccus %016llx\n", H64(r64), H64(ru64));

	/* ---- widening reductions ---- */
	{
		int64_t acc = 5;
		for (int i = 0; i < N; i++) acc += (int64_t)a32[i];
		uint64_t uacc = 7;
		for (int i = 0; i < N; i++) uacc += (uint64_t)ua32[i];
#ifdef __riscv_v_intrinsic
		{
			/* vl need not reach N, so the reduction walks the array in
			 * vl-sized chunks and carries the accumulator across them. */
			vint64m1_t s = __riscv_vmv_s_x_i64m1(5, 1);
			vuint64m1_t us = __riscv_vmv_s_x_u64m1(7, 1);
			for (size_t i = 0; i < N; ) {
				size_t vl = __riscv_vsetvl_e32m1(N - i);
				s = __riscv_vwredsum_vs_i32m1_i64m1(
					__riscv_vle32_v_i32m1(a32 + i, vl), s, vl);
				us = __riscv_vwredsumu_vs_u32m1_u64m1(
					__riscv_vle32_v_u32m1(ua32 + i, vl), us, vl);
				i += vl;
			}
			acc = __riscv_vmv_x_s_i64m1_i64(s);
			uacc = __riscv_vmv_x_s_u64m1_u64(us);
		}
#endif
		printf("wredsum %lld %llu\n", (long long)acc, (unsigned long long)uacc);
	}

	/* ---- widening floating point ---- */
	for (int i = 0; i < N; i++) rd[i] = (double)fa[i] + (double)fb[i];
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vfloat32m1_t x = __riscv_vle32_v_f32m1(fa, vl), y = __riscv_vle32_v_f32m1(fb, vl);
		__riscv_vse64_v_f64m2(rd, __riscv_vfwadd_vv_f64m2(x, y, vl), vl);
	}
#endif
	printf("vfwadd %016llx\n", H64(rd));

	for (int i = 0; i < N; i++) rd[i] = (double)fa[i] * (double)fb[i];
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vfloat32m1_t x = __riscv_vle32_v_f32m1(fa, vl), y = __riscv_vle32_v_f32m1(fb, vl);
		__riscv_vse64_v_f64m2(rd, __riscv_vfwmul_vv_f64m2(x, y, vl), vl);
	}
#endif
	printf("vfwmul %016llx\n", H64(rd));

	for (int i = 0; i < N; i++) dw[i] = (double)i * 0.5 - 4.0;
	for (int i = 0; i < N; i++) rd[i] = dw[i] - (double)fb[i];
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vfloat64m2_t w = __riscv_vle64_v_f64m2(dw, vl);
		vfloat32m1_t y = __riscv_vle32_v_f32m1(fb, vl);
		__riscv_vse64_v_f64m2(rd, __riscv_vfwsub_wv_f64m2(w, y, vl), vl);
	}
#endif
	printf("vfwsub_w %016llx\n", H64(rd));

	for (int i = 0; i < N; i++) rd[i] = dw[i] + (double)fa[i] * (double)fb[i];
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vfloat64m2_t w = __riscv_vle64_v_f64m2(dw, vl);
		vfloat32m1_t x = __riscv_vle32_v_f32m1(fa, vl), y = __riscv_vle32_v_f32m1(fb, vl);
		__riscv_vse64_v_f64m2(rd, __riscv_vfwmacc_vv_f64m2(w, x, y, vl), vl);
	}
#endif
	printf("vfwmacc %016llx\n", H64(rd));

	{   /* the widening float reduction, which is ordered here */
		double acc = 0.25;
		for (int i = 0; i < N; i++) acc += (double)fa[i];
#ifdef __riscv_v_intrinsic
		{
			/* Chunked like the integer reductions above. vfwredosum is
			 * ordered, so chunking gives exactly the sequential sum. */
			vfloat64m1_t s = __riscv_vfmv_s_f_f64m1(0.25, 1);
			for (size_t i = 0; i < N; ) {
				size_t vl = __riscv_vsetvl_e32m1(N - i);
				s = __riscv_vfwredosum_vs_f32m1_f64m1(
					__riscv_vle32_v_f32m1(fa + i, vl), s, vl);
				i += vl;
			}
			acc = __riscv_vfmv_f_s_f64m1_f64(s);
		}
#endif
		printf("vfwredosum %.12f\n", acc);
	}

	/* ---- vfclass, which writes a ten-bit value per element ---- */
	{
		static const float specials[8] = { 0 };
		float sv[N];
		for (int i = 0; i < N; i++) sv[i] = fa[i];
		sv[0] = 0.0f; sv[1] = -0.0f; sv[2] = INFINITY; sv[3] = -INFINITY;
		sv[4] = NAN;  sv[5] = 1.0f;  sv[6] = -1.0f;
		memcpy(&sv[7], &(uint32_t){1}, 4);           /* smallest subnormal */
		for (int i = 0; i < N; i++) {
			float v = sv[i];
			uint32_t bits; memcpy(&bits, &v, 4);
			uint32_t cls;
			if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x7FFFFFu))
				cls = (bits & 0x400000u) ? (1u << 9) : (1u << 8);
			else if ((bits & 0x7FFFFFFFu) == 0x7F800000u)
				cls = (bits >> 31) ? 1u : (1u << 7);
			else if ((bits & 0x7FFFFFFFu) == 0)
				cls = (bits >> 31) ? (1u << 3) : (1u << 4);
			else if ((bits & 0x7F800000u) == 0)
				cls = (bits >> 31) ? (1u << 2) : (1u << 5);
			else
				cls = (bits >> 31) ? (1u << 1) : (1u << 6);
			ru32[i] = cls;
		}
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e32m1(N);
			vfloat32m1_t x = __riscv_vle32_v_f32m1(sv, vl);
			__riscv_vse32_v_u32m1(ru32, __riscv_vfclass_v_u32m1(x, vl), vl);
		}
#endif
		printf("vfclass %016llx\n", H32(ru32));
		(void)specials;
	}

	/* ---- the reciprocal estimates ----
	 * These are defined by table lookup, so rather than restate the
	 * tables the check is the property they exist for: a result within
	 * one part in 2^7 of the true value, and exact special cases. */
	{
		float est[N], src[N];
		for (int i = 0; i < N; i++) src[i] = fb[i] + (float)i * 0.125f + 0.5f;
		src[0] = 1.0f; src[1] = 4.0f; src[2] = 0.25f;
		int bad_rec = 0, bad_rsqrt = 0;
		for (int i = 0; i < N; i++) est[i] = 1.0f / src[i];
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e32m1(N);
			vfloat32m1_t x = __riscv_vle32_v_f32m1(src, vl);
			__riscv_vse32_v_f32m1(est, __riscv_vfrec7_v_f32m1(x, vl), vl);
		}
#endif
		for (int i = 0; i < N; i++) {
			double want = 1.0 / (double)src[i];
			if (fabs((double)est[i] - want) > want / 128.0) bad_rec++;
		}
		for (int i = 0; i < N; i++) est[i] = 1.0f / sqrtf(src[i]);
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e32m1(N);
			vfloat32m1_t x = __riscv_vle32_v_f32m1(src, vl);
			__riscv_vse32_v_f32m1(est, __riscv_vfrsqrt7_v_f32m1(x, vl), vl);
		}
#endif
		for (int i = 0; i < N; i++) {
			double want = 1.0 / sqrt((double)src[i]);
			if (fabs((double)est[i] - want) > want / 128.0) bad_rsqrt++;
		}
		printf("estimates %d %d\n", bad_rec, bad_rsqrt);
	}

	/* ---- Zvbb ---- */
	for (int i = 0; i < N; i++) ru32[i] = ua32[i] & ~ub32[i];
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vuint32m1_t u = __riscv_vle32_v_u32m1(ua32, vl), v = __riscv_vle32_v_u32m1(ub32, vl);
		__riscv_vse32_v_u32m1(ru32, __riscv_vandn_vv_u32m1(u, v, vl), vl);
	}
#endif
	printf("vandn %016llx\n", H32(ru32));

	for (int i = 0; i < N; i++) {
		uint32_t v = ua32[i], r = 0;
		for (int k = 0; k < 32; k++) { r = (r << 1) | (v & 1); v >>= 1; }
		ru32[i] = r;
	}
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vuint32m1_t u = __riscv_vle32_v_u32m1(ua32, vl);
		__riscv_vse32_v_u32m1(ru32, __riscv_vbrev_v_u32m1(u, vl), vl);
	}
#endif
	printf("vbrev %016llx\n", H32(ru32));

	for (int i = 0; i < N; i++) {
		uint32_t v = ua32[i], r = 0;
		for (int byte = 0; byte < 4; byte++) {
			uint8_t x = (uint8_t)(v >> (8 * byte)), y = 0;
			for (int k = 0; k < 8; k++) { y = (uint8_t)((y << 1) | (x & 1)); x >>= 1; }
			r |= (uint32_t)y << (8 * byte);
		}
		ru32[i] = r;
	}
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vuint32m1_t u = __riscv_vle32_v_u32m1(ua32, vl);
		__riscv_vse32_v_u32m1(ru32, __riscv_vbrev8_v_u32m1(u, vl), vl);
	}
#endif
	printf("vbrev8 %016llx\n", H32(ru32));

	for (int i = 0; i < N; i++) ru32[i] = __builtin_bswap32(ua32[i]);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vuint32m1_t u = __riscv_vle32_v_u32m1(ua32, vl);
		__riscv_vse32_v_u32m1(ru32, __riscv_vrev8_v_u32m1(u, vl), vl);
	}
#endif
	printf("vrev8 %016llx\n", H32(ru32));

	for (int i = 0; i < N; i++) {
		uint32_t v = ua32[i];
		ru32[i] = (v == 0 ? 32u : (uint32_t)__builtin_clz(v))
			| ((v == 0 ? 32u : (uint32_t)__builtin_ctz(v)) << 8)
			| ((uint32_t)__builtin_popcount(v) << 16);
	}
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vuint32m1_t u = __riscv_vle32_v_u32m1(ua32, vl);
		vuint32m1_t z = __riscv_vclz_v_u32m1(u, vl);
		vuint32m1_t t = __riscv_vctz_v_u32m1(u, vl);
		/* The per-element vcpop.v has no intrinsic under this compiler --
		 * the name is taken by the mask population count -- so it is
		 * written out directly. */
		uint32_t pop[N];
		__riscv_vse32_v_u32m1(pop, u, vl);
		asm volatile("vle32.v v24, (%0)\n\tvcpop.v v24, v24\n\tvse32.v v24, (%0)"
			:: "r"(pop) : "memory");
		z = __riscv_vor_vv_u32m1(z, __riscv_vsll_vx_u32m1(t, 8, vl), vl);
		z = __riscv_vor_vv_u32m1(z,
			__riscv_vsll_vx_u32m1(__riscv_vle32_v_u32m1(pop, vl), 16, vl), vl);
		__riscv_vse32_v_u32m1(ru32, z, vl);
	}
#endif
	printf("vclz/ctz/cpop %016llx\n", H32(ru32));

	for (int i = 0; i < N; i++) {
		unsigned s = (unsigned)(ub32[i] & 31);
		uint32_t v = ua32[i];
		ru32[i] = s ? ((v << s) | (v >> (32 - s))) : v;
	}
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vuint32m1_t u = __riscv_vle32_v_u32m1(ua32, vl), v = __riscv_vle32_v_u32m1(ub32, vl);
		__riscv_vse32_v_u32m1(ru32, __riscv_vrol_vv_u32m1(u, v, vl), vl);
	}
#endif
	printf("vrol %016llx\n", H32(ru32));

	for (int i = 0; i < N; i++) {
		uint32_t v = ua32[i];
		ru32[i] = (v >> 13) | (v << 19);      /* vror.vi by 13 */
	}
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vuint32m1_t u = __riscv_vle32_v_u32m1(ua32, vl);
		__riscv_vse32_v_u32m1(ru32, __riscv_vror_vx_u32m1(u, 13, vl), vl);
	}
#endif
	printf("vror %016llx\n", H32(ru32));

	for (int i = 0; i < N; i++) ru64[i] = (uint64_t)ua32[i] << (ub32[i] & 63);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m1(N);
		vuint32m1_t u = __riscv_vle32_v_u32m1(ua32, vl), v = __riscv_vle32_v_u32m1(ub32, vl);
		__riscv_vse64_v_u64m2(ru64, __riscv_vwsll_vv_u64m2(u, v, vl), vl);
	}
#endif
	printf("vwsll %016llx\n", H64(ru64));

	return 0;
}
