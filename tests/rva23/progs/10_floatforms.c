/* The floating-point code points a compiler does not emit from ordinary
 * C: the eight fused multiply-add forms, the reversed-operand divides and
 * subtracts, the number-aware min/max, the remaining compares, the merge
 * and the slides. See 06_fixedpoint.c for how the comparison works.
 *
 * The multiply-add family is the reason this file exists: its eight code
 * points differ only in which operand is the addend and which sign is
 * flipped, so a transposed pair produces plausible-looking numbers.
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#define N 16
static float  fa[N], fb[N], fc[N];
static double da[N], db[N], dc[N];

/* The hashes fold every NaN to one bit pattern. RISC-V returns a canonical
 * NaN from any operation that produces one, while a host FPU propagates the
 * incoming payload and sign, so hashing NaN bits verbatim would compare the
 * two FPUs' NaN conventions rather than the instruction under test. */
static uint64_t hashf(const float *p)
{
	uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < N; i++) {
		uint32_t b; memcpy(&b, &p[i], 4);
		if ((b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu)) b = 0x7FC00000u;
		h ^= b; h *= 1099511628211ull;
	}
	return h;
}
static uint64_t hashd(const double *p)
{
	uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < N; i++) {
		uint64_t b; memcpy(&b, &p[i], 8);
		if ((b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull
			&& (b & 0xFFFFFFFFFFFFFull))
			b = 0x7FF8000000000000ull;
		h ^= b; h *= 1099511628211ull;
	}
	return h;
}
#define HF(p) ((unsigned long long)hashf(p))
#define HD(p) ((unsigned long long)hashd(p))

#ifdef __riscv_v_intrinsic
#include <riscv_vector.h>
#endif

int main(void)
{
	uint64_t seed = 0x5EED;
	for (int i = 0; i < N; i++) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		/* Values with plenty of low-order bits, so that a doubly-rounded
		 * multiply-add differs from a correctly fused one. */
		fa[i] = (float)((int32_t)(seed >> 33)) * 1.0000001f;
		fb[i] = (float)((int32_t)(seed >> 41)) * 0.9999999f + 1.0f;
		fc[i] = (float)((int32_t)(seed >> 49)) * 1.0000003f + 2.0f;
		da[i] = (double)((int64_t)seed) * 1.0000000000000002;
		db[i] = (double)((int64_t)(seed >> 8)) * 0.9999999999999998 + 1.0;
		dc[i] = (double)((int64_t)(seed >> 16)) + 3.0;
	}
	fb[0] = 0.0f; fb[1] = INFINITY; fb[2] = -INFINITY;
	fa[3] = NAN;

	static float  rf[N];
	static double rd[N];

	/* ---- the eight fused multiply-add forms ----
	 * vd is the accumulator in every one of them; which of the other two
	 * is the addend, and which signs are flipped, is what differs. The
	 * product is not rounded before the addition, so the reference uses
	 * fmaf/fma rather than a bare `a*b+c`. */
#define FMA_CHECK(name, expr, intr)                                        \
	for (int i = 0; i < N; i++) { float a = fa[i], b = fb[i], c = fc[i]; rf[i] = (expr); } \
	IF_VEC(intr)                                                           \
	printf(name " %016llx\n", HF(rf));

#ifdef __riscv_v_intrinsic
#define IF_VEC(intr) { \
		size_t vl = __riscv_vsetvl_e32m8(N); \
		vfloat32m8_t va = __riscv_vle32_v_f32m8(fa, vl); \
		vfloat32m8_t vb = __riscv_vle32_v_f32m8(fb, vl); \
		vfloat32m8_t vc = __riscv_vle32_v_f32m8(fc, vl); \
		(void)va; (void)vb; (void)vc; \
		__riscv_vse32_v_f32m8(rf, intr, vl); \
	}
#else
#define IF_VEC(intr)
#endif

	/* vd = +(vs1 * vd) + vs2 */
	FMA_CHECK("vfmadd",  fmaf(a, c, b), __riscv_vfmadd_vv_f32m8(vc, va, vb, vl))
	/* vd = -(vs1 * vd) - vs2 */
	FMA_CHECK("vfnmadd", -fmaf(a, c, b), __riscv_vfnmadd_vv_f32m8(vc, va, vb, vl))
	/* vd = +(vs1 * vd) - vs2 */
	FMA_CHECK("vfmsub",  fmaf(a, c, -b), __riscv_vfmsub_vv_f32m8(vc, va, vb, vl))
	/* vd = -(vs1 * vd) + vs2 */
	FMA_CHECK("vfnmsub", fmaf(-a, c, b), __riscv_vfnmsub_vv_f32m8(vc, va, vb, vl))
	/* vd = +(vs1 * vs2) + vd */
	FMA_CHECK("vfmacc",  fmaf(a, b, c), __riscv_vfmacc_vv_f32m8(vc, va, vb, vl))
	/* vd = -(vs1 * vs2) - vd */
	FMA_CHECK("vfnmacc", -fmaf(a, b, c), __riscv_vfnmacc_vv_f32m8(vc, va, vb, vl))
	/* vd = +(vs1 * vs2) - vd */
	FMA_CHECK("vfmsac",  fmaf(a, b, -c), __riscv_vfmsac_vv_f32m8(vc, va, vb, vl))
	/* vd = -(vs1 * vs2) + vd */
	FMA_CHECK("vfnmsac", fmaf(-a, b, c), __riscv_vfnmsac_vv_f32m8(vc, va, vb, vl))

	/* the same eight at double precision, in their scalar forms */
	{
		const double s = 1.7320508075688772;
		for (int i = 0; i < N; i++) rd[i] = fma(s, dc[i], db[i]);
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e64m8(N);
			vfloat64m8_t vb = __riscv_vle64_v_f64m8(db, vl);
			vfloat64m8_t vc = __riscv_vle64_v_f64m8(dc, vl);
			__riscv_vse64_v_f64m8(rd,
				__riscv_vfmadd_vf_f64m8(vc, s, vb, vl), vl);
		}
#endif
		printf("vfmadd.vf %016llx\n", HD(rd));
		for (int i = 0; i < N; i++) rd[i] = fma(-s, db[i], dc[i]);
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e64m8(N);
			vfloat64m8_t vb = __riscv_vle64_v_f64m8(db, vl);
			vfloat64m8_t vc = __riscv_vle64_v_f64m8(dc, vl);
			__riscv_vse64_v_f64m8(rd,
				__riscv_vfnmsac_vf_f64m8(vc, s, vb, vl), vl);
		}
#endif
		printf("vfnmsac.vf %016llx\n", HD(rd));
	}

	/* ---- the reversed-operand scalar forms ---- */
	{
		const float s = 3.5f;
		for (int i = 0; i < N; i++) rf[i] = s - fa[i];
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e32m8(N);
			__riscv_vse32_v_f32m8(rf,
				__riscv_vfrsub_vf_f32m8(__riscv_vle32_v_f32m8(fa, vl), s, vl), vl);
		}
#endif
		printf("vfrsub %016llx\n", HF(rf));
		for (int i = 0; i < N; i++) rf[i] = s / fb[i];
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e32m8(N);
			__riscv_vse32_v_f32m8(rf,
				__riscv_vfrdiv_vf_f32m8(__riscv_vle32_v_f32m8(fb, vl), s, vl), vl);
		}
#endif
		printf("vfrdiv %016llx\n", HF(rf));
	}

	/* ---- min and max, which return the other operand for a NaN ---- */
	for (int i = 0; i < N; i++) rf[i] = fminf(fa[i], fb[i]);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m8(N);
		__riscv_vse32_v_f32m8(rf, __riscv_vfmin_vv_f32m8(
			__riscv_vle32_v_f32m8(fa, vl), __riscv_vle32_v_f32m8(fb, vl), vl), vl);
	}
#endif
	printf("vfmin %016llx\n", HF(rf));
	for (int i = 0; i < N; i++) rf[i] = fmaxf(fa[i], fb[i]);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m8(N);
		__riscv_vse32_v_f32m8(rf, __riscv_vfmax_vv_f32m8(
			__riscv_vle32_v_f32m8(fa, vl), __riscv_vle32_v_f32m8(fb, vl), vl), vl);
	}
#endif
	printf("vfmax %016llx\n", HF(rf));

	/* ---- the compares that only exist against a scalar ---- */
	{
		const float s = 1000.0f;
		unsigned gt = 0, ge = 0, ne = 0;
		for (int i = 0; i < N; i++) {
			gt |= (fa[i] > s) << i;
			ge |= (fa[i] >= s) << i;
			ne |= (fa[i] != s) << i;
		}
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e32m8(N);
			vfloat32m8_t va = __riscv_vle32_v_f32m8(fa, vl);
			uint8_t bits[8];
			memset(bits, 0, sizeof bits);
			__riscv_vsm_v_b4(bits, __riscv_vmfgt_vf_f32m8_b4(va, s, vl), vl);
			gt = bits[0] | ((unsigned)bits[1] << 8);
			memset(bits, 0, sizeof bits);
			__riscv_vsm_v_b4(bits, __riscv_vmfge_vf_f32m8_b4(va, s, vl), vl);
			ge = bits[0] | ((unsigned)bits[1] << 8);
			memset(bits, 0, sizeof bits);
			__riscv_vsm_v_b4(bits, __riscv_vmfne_vf_f32m8_b4(va, s, vl), vl);
			ne = bits[0] | ((unsigned)bits[1] << 8);
		}
#endif
		printf("vmfgt %04x vmfge %04x vmfne %04x\n", gt, ge, ne);
	}

	/* ---- merge, broadcast and the float slides ---- */
	{
		static const uint8_t sel[8] = { 0xA9, 0x36, 0, 0, 0, 0, 0, 0 };
		const float s = -7.25f;
		for (int i = 0; i < N; i++)
			rf[i] = ((sel[i / 8] >> (i % 8)) & 1) ? s : fa[i];
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e32m8(N);
			__riscv_vse32_v_f32m8(rf, __riscv_vfmerge_vfm_f32m8(
				__riscv_vle32_v_f32m8(fa, vl), s, __riscv_vlm_v_b4(sel, vl), vl), vl);
		}
#endif
		printf("vfmerge %016llx\n", HF(rf));

		rf[0] = s;
		for (int i = 1; i < N; i++) rf[i] = fa[i - 1];
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e32m8(N);
			__riscv_vse32_v_f32m8(rf, __riscv_vfslide1up_vf_f32m8(
				__riscv_vle32_v_f32m8(fa, vl), s, vl), vl);
		}
#endif
		printf("vfslide1up %016llx\n", HF(rf));

		for (int i = 0; i + 1 < N; i++) rf[i] = fa[i + 1];
		rf[N - 1] = s;
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e32m8(N);
			__riscv_vse32_v_f32m8(rf, __riscv_vfslide1down_vf_f32m8(
				__riscv_vle32_v_f32m8(fa, vl), s, vl), vl);
		}
#endif
		printf("vfslide1down %016llx\n", HF(rf));
	}

	/* ---- the unordered and ordered float reductions, and min/max ---- */
	{
		float sum = 0.5f, mn = fa[0], mx = fa[0];
		for (int i = 0; i < N; i++) sum += fa[i];
		for (int i = 0; i < N; i++) { mn = fminf(mn, fa[i]); mx = fmaxf(mx, fa[i]); }
#ifdef __riscv_v_intrinsic
		{
			/* Chunked, since vl need not reach N. vfredosum is ordered, so
			 * chunking reproduces the sequential sum exactly. */
			vfloat32m1_t acc = __riscv_vfmv_s_f_f32m1(0.5f, 1);
			vfloat32m1_t vmn = __riscv_vfmv_s_f_f32m1(fa[0], 1);
			vfloat32m1_t vmx = __riscv_vfmv_s_f_f32m1(fa[0], 1);
			for (size_t i = 0; i < N; ) {
				size_t vl = __riscv_vsetvl_e32m1(N - i);
				vfloat32m8_t v = __riscv_vle32_v_f32m8(fa + i, vl);
				acc = __riscv_vfredosum_vs_f32m8_f32m1(v, acc, vl);
				vmn = __riscv_vfredmin_vs_f32m8_f32m1(v, vmn, vl);
				vmx = __riscv_vfredmax_vs_f32m8_f32m1(v, vmx, vl);
				i += vl;
			}
			sum = __riscv_vfmv_f_s_f32m1_f32(acc);
			mn = __riscv_vfmv_f_s_f32m1_f32(vmn);
			mx = __riscv_vfmv_f_s_f32m1_f32(vmx);
		}
#endif
		printf("vfred %.6f %.6f %.6f\n", (double)sum, (double)mn, (double)mx);
	}

	/* ---- the widening multiply-add forms that negate ---- */
	for (int i = 0; i < N; i++) da[i] = (double)fc[i] * 1000.0;
	for (int i = 0; i < N; i++)
		rd[i] = -((double)fa[i] * (double)fb[i]) - da[i];
#ifdef __riscv_v_intrinsic
	{
		/* The destination is one step further up the LMUL scale than
		 * the sources, so these two run at m4 where the rest runs at m8. */
		size_t vl = __riscv_vsetvl_e32m4(N);
		vfloat64m8_t w = __riscv_vle64_v_f64m8(da, vl);
		vfloat32m4_t x = __riscv_vle32_v_f32m4(fa, vl);
		vfloat32m4_t y = __riscv_vle32_v_f32m4(fb, vl);
		__riscv_vse64_v_f64m8(rd, __riscv_vfwnmacc_vv_f64m8(w, x, y, vl), vl);
	}
#endif
	printf("vfwnmacc %016llx\n", HD(rd));

	for (int i = 0; i < N; i++)
		rd[i] = (double)fa[i] * (double)fb[i] - da[i];
#ifdef __riscv_v_intrinsic
	{
		/* The destination is one step further up the LMUL scale than
		 * the sources, so these two run at m4 where the rest runs at m8. */
		size_t vl = __riscv_vsetvl_e32m4(N);
		vfloat64m8_t w = __riscv_vle64_v_f64m8(da, vl);
		vfloat32m4_t x = __riscv_vle32_v_f32m4(fa, vl);
		vfloat32m4_t y = __riscv_vle32_v_f32m4(fb, vl);
		__riscv_vse64_v_f64m8(rd, __riscv_vfwmsac_vv_f64m8(w, x, y, vl), vl);
	}
#endif
	printf("vfwmsac %016llx\n", HD(rd));
	return 0;
}
