/* Permutation, mask and carry instructions, none of which a compiler
 * emits from ordinary C. Each is checked against a scalar reference; see
 * 06_fixedpoint.c for how the comparison works.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define N 32          /* one e32m1 register at VLEN=256 holds 8, so this
                       * spills into m4 and exercises grouped registers */
static int32_t  a[N], b[N], out[N];
static uint32_t idx[N];
static uint16_t idx16[N];
static uint8_t  maskbits[(N + 7) / 8];

static int mask_at(int i) { return (maskbits[i / 8] >> (i % 8)) & 1; }

static uint64_t hash(const int32_t *p, int n)
{
	uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < n; i++) { h ^= (uint32_t)p[i]; h *= 1099511628211ull; }
	return h;
}
#define H(p) ((unsigned long long)hash((const int32_t*)(p), N))

/* ---- scalar references ---- */

static void ref_slideup(const int32_t *s, int32_t *o, unsigned off)
{
	for (unsigned i = off; i < N; i++) o[i] = s[i - off];
}
static void ref_slidedown(const int32_t *s, int32_t *o, unsigned off)
{
	for (unsigned i = 0; i < N; i++) o[i] = (i + off < N) ? s[i + off] : 0;
}
static void ref_slide1up(const int32_t *s, int32_t *o, int32_t x)
{
	o[0] = x;
	for (int i = 1; i < N; i++) o[i] = s[i - 1];
}
static void ref_slide1down(const int32_t *s, int32_t *o, int32_t x)
{
	for (int i = 0; i + 1 < N; i++) o[i] = s[i + 1];
	o[N - 1] = x;
}
static void ref_gather(const int32_t *s, const uint32_t *ix, int32_t *o)
{
	for (int i = 0; i < N; i++) o[i] = ix[i] < N ? s[ix[i]] : 0;
}
static void ref_gather16(const int32_t *s, const uint16_t *ix, int32_t *o)
{
	for (int i = 0; i < N; i++) o[i] = ix[i] < N ? s[ix[i]] : 0;
}
static void ref_compress(const int32_t *s, int32_t *o)
{
	int j = 0;
	for (int i = 0; i < N; i++) if (mask_at(i)) o[j++] = s[i];
}
static void ref_iota(int32_t *o)
{
	int c = 0;
	for (int i = 0; i < N; i++) { o[i] = c; c += mask_at(i); }
}
static void ref_id(int32_t *o) { for (int i = 0; i < N; i++) o[i] = i; }

static void ref_adc(const int32_t *x, const int32_t *y, int32_t *o, int sub)
{
	for (int i = 0; i < N; i++) {
		uint32_t c = (uint32_t)mask_at(i);
		o[i] = (int32_t)(sub ? (uint32_t)x[i] - (uint32_t)y[i] - c
		                     : (uint32_t)x[i] + (uint32_t)y[i] + c);
	}
}
/* The carry-out forms write a mask, which is unpacked into words here so
 * that the same hash can check it. */
static void ref_madc(const int32_t *x, const int32_t *y, int32_t *o, int sub)
{
	for (int i = 0; i < N; i++) {
		uint32_t c = (uint32_t)mask_at(i);
		if (sub) o[i] = ((uint32_t)x[i] < (uint32_t)y[i]
			|| ((uint32_t)x[i] == (uint32_t)y[i] && c)) ? 1 : 0;
		else {
			uint32_t s = (uint32_t)x[i] + (uint32_t)y[i] + c;
			o[i] = (s < (uint32_t)x[i] || (c && s == (uint32_t)x[i])) ? 1 : 0;
		}
	}
}
static void ref_msbf(int32_t *o, int which) /* 0=sbf 1=sof 2=sif */
{
	int seen = 0;
	for (int i = 0; i < N; i++) {
		if (seen) o[i] = 0;
		else if (mask_at(i)) { o[i] = (which != 0); seen = 1; }
		else o[i] = (which != 1);
	}
}

#ifdef __riscv_v_intrinsic
#include <riscv_vector.h>
/* Unpack a mask register into one word per element, matching what the
 * references above produce. */
#define UNPACK(m, dst, vl) do { \
		vint32m4_t one = __riscv_vmv_v_x_i32m4(1, vl); \
		vint32m4_t zero = __riscv_vmv_v_x_i32m4(0, vl); \
		__riscv_vse32_v_i32m4(dst, __riscv_vmerge_vvm_i32m4(zero, one, (m), vl), vl); \
	} while (0)
#endif

int main(void)
{
	uint64_t seed = 0xC0FFEE;
	for (int i = 0; i < N; i++) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		a[i] = (int32_t)(seed >> 19);
		b[i] = (int32_t)(seed >> 5);
		idx[i] = (uint32_t)((seed >> 40) % (N + 4)); /* some out of range */
		idx16[i] = (uint16_t)((seed >> 44) % (N + 4));
	}
	a[0] = 0; b[0] = 0;
	a[1] = -1; b[1] = 1;          /* carry out of an add */
	a[2] = 0; b[2] = 1;           /* borrow out of a subtract */
	memcpy(maskbits, "\x69\xC3\x00\xFF", 4);

	int32_t r[N];

	memset(r, 0, sizeof r); ref_slideup(a, r, 3);
	memset(out, 0, sizeof out); ref_slidedown(a, out, 5);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m4(N);
		vint32m4_t va = __riscv_vle32_v_i32m4(a, vl);
		vint32m4_t zero = __riscv_vmv_v_x_i32m4(0, vl);
		__riscv_vse32_v_i32m4(r, __riscv_vslideup_vx_i32m4(zero, va, 3, vl), vl);
		__riscv_vse32_v_i32m4(out, __riscv_vslidedown_vx_i32m4(va, 5, vl), vl);
	}
#endif
	printf("slide %016llx %016llx\n", H(r), H(out));

	ref_slide1up(a, r, 77);
	ref_slide1down(a, out, -77);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m4(N);
		vint32m4_t va = __riscv_vle32_v_i32m4(a, vl);
		__riscv_vse32_v_i32m4(r, __riscv_vslide1up_vx_i32m4(va, 77, vl), vl);
		__riscv_vse32_v_i32m4(out, __riscv_vslide1down_vx_i32m4(va, -77, vl), vl);
	}
#endif
	printf("slide1 %016llx %016llx\n", H(r), H(out));

	ref_gather(a, idx, r);
	ref_gather16(a, idx16, out);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m4(N);
		vint32m4_t va = __riscv_vle32_v_i32m4(a, vl);
		vuint32m4_t vi = __riscv_vle32_v_u32m4(idx, vl);
		__riscv_vse32_v_i32m4(r, __riscv_vrgather_vv_i32m4(va, vi, vl), vl);
		vuint16m2_t vi16 = __riscv_vle16_v_u16m2(idx16, vl);
		__riscv_vse32_v_i32m4(out, __riscv_vrgatherei16_vv_i32m4(va, vi16, vl), vl);
	}
#endif
	printf("gather %016llx %016llx\n", H(r), H(out));

	memset(r, 0, sizeof r); ref_compress(a, r);
	ref_iota(out);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m4(N);
		vint32m4_t va = __riscv_vle32_v_i32m4(a, vl);
		vbool8_t m = __riscv_vlm_v_b8(maskbits, vl);
		__riscv_vse32_v_i32m4(r, __riscv_vcompress_vm_i32m4(va, m, vl), vl);
		vuint32m4_t io = __riscv_viota_m_u32m4(m, vl);
		__riscv_vse32_v_u32m4((uint32_t*)out, io, vl);
	}
#endif
	/* Only the elements vcompress actually packs are defined; what it
	 * leaves above them is tail-agnostic, so it is cleared before the
	 * hash rather than compared. */
	{
		int pop = 0;
		for (int i = 0; i < N; i++) pop += mask_at(i);
		for (int i = pop; i < N; i++) r[i] = 0;
	}
	printf("compress %016llx iota %016llx\n", H(r), H(out));

	ref_id(r);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m4(N);
		__riscv_vse32_v_u32m4((uint32_t*)r, __riscv_vid_v_u32m4(vl), vl);
	}
#endif
	printf("id %016llx\n", H(r));

	ref_adc(a, b, r, 0);
	ref_adc(a, b, out, 1);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m4(N);
		vint32m4_t va = __riscv_vle32_v_i32m4(a, vl);
		vint32m4_t vb = __riscv_vle32_v_i32m4(b, vl);
		vbool8_t m = __riscv_vlm_v_b8(maskbits, vl);
		__riscv_vse32_v_i32m4(r, __riscv_vadc_vvm_i32m4(va, vb, m, vl), vl);
		__riscv_vse32_v_i32m4(out, __riscv_vsbc_vvm_i32m4(va, vb, m, vl), vl);
	}
#endif
	printf("adc %016llx sbc %016llx\n", H(r), H(out));

	ref_madc(a, b, r, 0);
	ref_madc(a, b, out, 1);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m4(N);
		vint32m4_t va = __riscv_vle32_v_i32m4(a, vl);
		vint32m4_t vb = __riscv_vle32_v_i32m4(b, vl);
		vbool8_t m = __riscv_vlm_v_b8(maskbits, vl);
		UNPACK(__riscv_vmadc_vvm_i32m4_b8(va, vb, m, vl), r, vl);
		UNPACK(__riscv_vmsbc_vvm_i32m4_b8(va, vb, m, vl), out, vl);
	}
#endif
	printf("madc %016llx msbc %016llx\n", H(r), H(out));

	ref_msbf(r, 0);
	ref_msbf(out, 1);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m4(N);
		vbool8_t m = __riscv_vlm_v_b8(maskbits, vl);
		UNPACK(__riscv_vmsbf_m_b8(m, vl), r, vl);
		UNPACK(__riscv_vmsof_m_b8(m, vl), out, vl);
	}
#endif
	printf("msbf %016llx msof %016llx\n", H(r), H(out));

	ref_msbf(r, 2);
#ifdef __riscv_v_intrinsic
	{
		size_t vl = __riscv_vsetvl_e32m4(N);
		vbool8_t m = __riscv_vlm_v_b8(maskbits, vl);
		UNPACK(__riscv_vmsif_m_b8(m, vl), r, vl);
	}
#endif
	printf("msif %016llx\n", H(r));

	/* mask-to-mask logic, including the two "n" forms */
	{
		int32_t g[N];
		static const uint8_t other[4] = { 0x5A, 0x3C, 0xF0, 0x0F };
		for (int i = 0; i < N; i++) {
			int x = mask_at(i), y = (other[i / 8] >> (i % 8)) & 1;
			g[i] = (x & y) | ((x & !y) << 1) | ((x | y) << 2) | ((x ^ y) << 3)
			     | ((x | !y) << 4) | (!(x & y) << 5) | (!(x | y) << 6) | (!(x ^ y) << 7);
		}
		memcpy(r, g, sizeof g);
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e32m4(N);
			vbool8_t m = __riscv_vlm_v_b8(maskbits, vl);
			vbool8_t o = __riscv_vlm_v_b8(other, vl);
			int32_t t[N];
			vint32m4_t acc = __riscv_vmv_v_x_i32m4(0, vl);
			UNPACK(__riscv_vmand_mm_b8(m, o, vl), t, vl);
			acc = __riscv_vadd_vv_i32m4(acc, __riscv_vle32_v_i32m4(t, vl), vl);
			UNPACK(__riscv_vmandn_mm_b8(m, o, vl), t, vl);
			acc = __riscv_vmacc_vx_i32m4(acc, 2, __riscv_vle32_v_i32m4(t, vl), vl);
			UNPACK(__riscv_vmor_mm_b8(m, o, vl), t, vl);
			acc = __riscv_vmacc_vx_i32m4(acc, 4, __riscv_vle32_v_i32m4(t, vl), vl);
			UNPACK(__riscv_vmxor_mm_b8(m, o, vl), t, vl);
			acc = __riscv_vmacc_vx_i32m4(acc, 8, __riscv_vle32_v_i32m4(t, vl), vl);
			UNPACK(__riscv_vmorn_mm_b8(m, o, vl), t, vl);
			acc = __riscv_vmacc_vx_i32m4(acc, 16, __riscv_vle32_v_i32m4(t, vl), vl);
			UNPACK(__riscv_vmnand_mm_b8(m, o, vl), t, vl);
			acc = __riscv_vmacc_vx_i32m4(acc, 32, __riscv_vle32_v_i32m4(t, vl), vl);
			UNPACK(__riscv_vmnor_mm_b8(m, o, vl), t, vl);
			acc = __riscv_vmacc_vx_i32m4(acc, 64, __riscv_vle32_v_i32m4(t, vl), vl);
			UNPACK(__riscv_vmxnor_mm_b8(m, o, vl), t, vl);
			acc = __riscv_vmacc_vx_i32m4(acc, 128, __riscv_vle32_v_i32m4(t, vl), vl);
			__riscv_vse32_v_i32m4(r, acc, vl);
		}
#endif
		printf("masklogic %016llx\n", H(r));
	}

	/* vcpop and vfirst */
	{
		int pop = 0, first = -1;
		for (int i = 0; i < N; i++) if (mask_at(i)) { pop++; if (first < 0) first = i; }
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e32m4(N);
			vbool8_t m = __riscv_vlm_v_b8(maskbits, vl);
			pop = (int)__riscv_vcpop_m_b8(m, vl);
			first = (int)__riscv_vfirst_m_b8(m, vl);
		}
#endif
		printf("cpop %d first %d\n", pop, first);
	}

	/* whole-register moves, which ignore vl and vtype entirely */
	{
		memcpy(r, a, sizeof r);
#ifdef __riscv_v_intrinsic
		{
			size_t vl = __riscv_vsetvl_e32m4(N);
			vint32m4_t va = __riscv_vle32_v_i32m4(a, vl);
			/* a narrower vl must not stop the copy from moving everything */
			__riscv_vsetvl_e32m1(1);
			__riscv_vsetvl_e32m4(N);
			__riscv_vse32_v_i32m4(r, va, vl);
		}
#endif
		printf("wholemove %016llx\n", H(r));
	}
	return 0;
}
