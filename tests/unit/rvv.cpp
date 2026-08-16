#include <catch2/catch_test_macros.hpp>
#include <libriscv/machine.hpp>
#include <cstdlib>
#include <unistd.h>

extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);

static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 16'000'000ul;
/* Result slots in the guest's out[] array. */
static const unsigned RESULTS = 96;

static std::vector<uint8_t> build_rvv(const std::string& code)
{
	// The guest is pure inline assembly, so any RISC-V toolchain whose
	// assembler knows the ratified vector extension will do. RCC is
	// overridden rather than defaulted: other workflows point it at
	// toolchains that target musl or reject these flags, and this test
	// binary builds nothing but the RVV guest. RVVCC overrides it again
	// for anyone using a bare-metal toolchain instead.
	// -mno-relax: no crt0 sets up gp in a freestanding guest, so
	// GP-relative addressing must not be used.
	const char* rvvcc = getenv("RVVCC");
	setenv("RCC", rvvcc ? rvvcc : "riscv64-linux-gnu-gcc-14", 1);
	return build_and_load(code,
		"-std=gnu11 -O1 -static -march=rv64gcv -mabi=lp64d -nostdlib -ffreestanding -mno-relax -Wl,-z,separate-code");
}

static uint64_t fbits(float value)
{
	union { float f; uint32_t u; } cvt;
	cvt.u = 0;
	cvt.f = value;
	return cvt.u;
}

static const char *GUEST = R"M(
typedef unsigned long ulong;

/* Result words, checked by the host. */
volatile ulong out[96];
static ulong idx = 0;
static void put(ulong v) { out[idx++] = v; }

static const unsigned int isrc1[8] = {1,2,3,4,5,6,7,8};
static const unsigned int isrc2[8] = {10,200,30,4000,50,600000,70,4294967295u};
static unsigned int idst[16];

static ulong fbits(float f) { union { float f; ulong u; } c; c.u = 0; c.f = f; return c.u; }

/* 1. vsetvli clamping with VLEN=256 */
static void t_vsetvli(void)
{
	ulong vl, avl, vt;
	avl = 100; asm volatile("vsetvli %0, %1, e32, m1" : "=r"(vl) : "r"(avl)); put(vl);
	avl = 3;   asm volatile("vsetvli %0, %1, e32, m1" : "=r"(vl) : "r"(avl)); put(vl);
	avl = 100; asm volatile("vsetvli %0, %1, e8, m1"  : "=r"(vl) : "r"(avl)); put(vl);
	avl = 100; asm volatile("vsetvli %0, %1, e64, m2" : "=r"(vl) : "r"(avl)); put(vl);
	avl = 100; asm volatile("vsetvli %0, %1, e32, mf2": "=r"(vl) : "r"(avl)); put(vl);
	avl = 100; asm volatile("vsetvli %0, %1, e16, m4" : "=r"(vl) : "r"(avl)); put(vl);
	avl = 100; asm volatile("vsetvli %0, %1, e64, mf8": "=r"(vl) : "r"(avl)); put(vl);
	/* rs1=x0, rd!=x0: VLMAX */
	asm volatile("vsetvli %0, x0, e16, m1" : "=r"(vl)); put(vl);
	/* vsetivli */
	asm volatile("vsetivli %0, 5, e32, m1" : "=r"(vl)); put(vl);
	asm volatile("vsetivli %0, 31, e32, m1" : "=r"(vl)); put(vl);
	/* vsetvl */
	avl = 100; vt = 0; /* e8, m1 */
	asm volatile("vsetvl %0, %1, %2" : "=r"(vl) : "r"(avl), "r"(vt)); put(vl);
	/* vsetvl with reserved vlmul=100: vill, rd = 0 */
	vt = 4;
	asm volatile("vsetvl %0, %1, %2" : "=r"(vl) : "r"(avl), "r"(vt)); put(vl);
}

/* 2. Integer vector arithmetic */
static void t_int_ops(void)
{
	ulong v;
	asm volatile("vsetivli zero, 8, e32, m1, ta, ma");
	asm volatile("vle32.v v1, (%0)" :: "r"(isrc1));
	asm volatile("vle32.v v2, (%0)" :: "r"(isrc2));
	asm volatile("vadd.vv v3, v1, v2");
	asm volatile("vsub.vv v4, v2, v1");
	asm volatile("vand.vv v5, v1, v2");
	asm volatile("vor.vv  v6, v1, v2");
	asm volatile("vxor.vv v7, v1, v2");
	asm volatile("vminu.vv v8, v1, v2");
	asm volatile("vsll.vi v9, v1, 4");
	asm volatile("vsrl.vi v10, v2, 3");
	asm volatile("vrsub.vi v11, v1, 15");
	asm volatile("vadd.vi  v12, v1, -7");
	asm volatile("vmv.x.s %0, v3" : "=r"(v)); put(v);        /* 1+10 */
	asm volatile("vse32.v v4, (%0)" :: "r"(&idst[0]) : "memory");
	put(idst[0]); put(idst[1]); put(idst[7]);                 /* vsub */
	asm volatile("vmv.x.s %0, v5" : "=r"(v)); put(v);         /* vand */
	asm volatile("vmv.x.s %0, v6" : "=r"(v)); put(v);         /* vor */
	asm volatile("vrgather.vi v13, v7, 1");
	asm volatile("vmv.x.s %0, v13" : "=r"(v)); put(v);        /* vxor */
	asm volatile("vmv.x.s %0, v8" : "=r"(v)); put(v);         /* vminu */
	asm volatile("vmv.x.s %0, v9" : "=r"(v)); put(v);         /* vsll */
	asm volatile("vmv.x.s %0, v10" : "=r"(v)); put(v);        /* vsrl */
	asm volatile("vmv.x.s %0, v11" : "=r"(v)); put(v);        /* vrsub */
	asm volatile("vmv.x.s %0, v12" : "=r"(v)); put(v);        /* vadd.vi */
	asm volatile("vrgather.vi v13, v3, 6");
	asm volatile("vmv.x.s %0, v13" : "=r"(v)); put(v);        /* vadd[6] */
}

/* 3. Masked operations, tail-undisturbed */
static void t_masked(void)
{
	ulong v;
	ulong beef = 0xBEEF, pat = 0x1234, five = 5;
	asm volatile("vsetivli zero, 8, e32, m1, ta, ma");
	asm volatile("vle32.v v1, (%0)" :: "r"(isrc1));
	asm volatile("vle32.v v2, (%0)" :: "r"(isrc2));
	/* v0 = (isrc1 < 5): bits 0-3 set */
	asm volatile("vmsltu.vx v0, v1, %0" :: "r"(five));
	/* masked vadd into pre-filled v20 */
	asm volatile("vmv.v.x v20, %0" :: "r"(beef));
	asm volatile("vadd.vv v20, v1, v2, v0.t");
	asm volatile("vrgather.vi v21, v20, 0");
	asm volatile("vmv.x.s %0, v21" : "=r"(v)); put(v); /* 1+10 */
	asm volatile("vrgather.vi v21, v20, 4");
	asm volatile("vmv.x.s %0, v21" : "=r"(v)); put(v); /* undisturbed 0xBEEF */
	/* vmerge.vvm: vd = mask ? vs1 : vs2 */
	asm volatile("vmerge.vvm v22, v2, v1, v0");
	asm volatile("vrgather.vi v21, v22, 0");
	asm volatile("vmv.x.s %0, v21" : "=r"(v)); put(v); /* mask=1 -> vs1 */
	asm volatile("vrgather.vi v21, v22, 4");
	asm volatile("vmv.x.s %0, v21" : "=r"(v)); put(v); /* mask=0 -> vs2 */
	/* vl=4, tail-undisturbed: elements 4-7 keep 0x1234 */
	asm volatile("vmv.v.x v21, %0" :: "r"(pat));
	asm volatile("vsetivli zero, 4, e32, m1, tu, mu");
	asm volatile("vadd.vv v21, v1, v2");
	asm volatile("vrgather.vi v23, v21, 3");
	asm volatile("vmv.x.s %0, v23" : "=r"(v)); put(v); /* 4+4000 */
	asm volatile("vrgather.vi v23, v21, 4");
	asm volatile("vmv.x.s %0, v23" : "=r"(v)); put(v); /* tail 0x1234 */
	asm volatile("vsetivli zero, 8, e32, m1, ta, ma");
}

/* 4. Element widths */
static void t_widths(void)
{
	static const unsigned char bsrc[32] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
		17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
	static const unsigned short hsrc[16] = {100,200,300,400,500,600,700,800,
		900,1000,1100,1200,1300,1400,1500,1600};
	static const unsigned long long dsrc[4] = {0x100000000ull, 2, 0xdeadbeefcafeull, 42};
	ulong v;

	asm volatile("vsetivli zero, 31, e8, m1, ta, ma");
	asm volatile("vle8.v v1, (%0)" :: "r"(bsrc));
	asm volatile("vadd.vi v1, v1, 1");
	asm volatile("vrgather.vi v2, v1, 0"); asm volatile("vmv.x.s %0, v2" : "=r"(v)); put(v);
	asm volatile("vrgather.vi v2, v1, 30"); asm volatile("vmv.x.s %0, v2" : "=r"(v)); put(v);

	asm volatile("vsetivli zero, 16, e16, m1, ta, ma");
	asm volatile("vle16.v v2, (%0)" :: "r"(hsrc));
	asm volatile("vmul.vv v3, v2, v2");
	asm volatile("vrgather.vi v4, v3, 0"); asm volatile("vmv.x.s %0, v4" : "=r"(v)); put(v);
	asm volatile("vrgather.vi v4, v3, 15"); asm volatile("vmv.x.s %0, v4" : "=r"(v)); put(v);

	asm volatile("vsetivli zero, 4, e64, m1, ta, ma");
	asm volatile("vle64.v v4, (%0)" :: "r"(dsrc));
	asm volatile("vadd.vv v5, v4, v4");
	asm volatile("vrgather.vi v6, v5, 0"); asm volatile("vmv.x.s %0, v6" : "=r"(v)); put(v);
	asm volatile("vrgather.vi v6, v5, 2"); asm volatile("vmv.x.s %0, v6" : "=r"(v)); put(v);
}

/* 5. Reductions, slides, gathers */
static void t_reduce_slide(void)
{
	ulong v, one = 1, s77 = 77;
	asm volatile("vsetivli zero, 8, e32, m1, ta, ma");
	asm volatile("vle32.v v1, (%0)" :: "r"(isrc1));
	asm volatile("vle32.v v4, (%0)" :: "r"(isrc2));
	/* vredsum: sum(1, 1..8) = 37 */
	asm volatile("vmv.s.x v2, %0" :: "r"(one));
	asm volatile("vredsum.vs v3, v1, v2");
	asm volatile("vmv.x.s %0, v3" : "=r"(v)); put(v);
	/* vredmaxu: max(0, isrc2) = 4294967295 */
	asm volatile("vmv.s.x v2, zero");
	asm volatile("vredmaxu.vs v5, v4, v2");
	asm volatile("vmv.x.s %0, v5" : "=r"(v)); put(v);
	/* vslidedown.vi 3 */
	asm volatile("vslidedown.vi v6, v1, 3");
	asm volatile("vrgather.vi v7, v6, 0"); asm volatile("vmv.x.s %0, v7" : "=r"(v)); put(v);
	asm volatile("vrgather.vi v7, v6, 4"); asm volatile("vmv.x.s %0, v7" : "=r"(v)); put(v);
	asm volatile("vrgather.vi v7, v6, 5"); asm volatile("vmv.x.s %0, v7" : "=r"(v)); put(v);
	/* vslide1down.vx: scalar 77 lands in element 7 */
	asm volatile("vslide1down.vx v8, v1, %0" :: "r"(s77));
	asm volatile("vrgather.vi v9, v8, 0"); asm volatile("vmv.x.s %0, v9" : "=r"(v)); put(v);
	asm volatile("vrgather.vi v9, v8, 7"); asm volatile("vmv.x.s %0, v9" : "=r"(v)); put(v);
	/* vslideup.vi 2 into v10 pre-filled with 100s: elements 0,1 keep 100 */
	ulong hundo = 100;
	asm volatile("vmv.v.x v10, %0" :: "r"(hundo));
	asm volatile("vslideup.vi v10, v1, 2");
	asm volatile("vrgather.vi v9, v10, 0"); asm volatile("vmv.x.s %0, v9" : "=r"(v)); put(v);
	asm volatile("vrgather.vi v9, v10, 1"); asm volatile("vmv.x.s %0, v9" : "=r"(v)); put(v);
	asm volatile("vrgather.vi v9, v10, 2"); asm volatile("vmv.x.s %0, v9" : "=r"(v)); put(v);
	asm volatile("vrgather.vi v9, v10, 3"); asm volatile("vmv.x.s %0, v9" : "=r"(v)); put(v);
	/* vrgather.vi 5, vrgather.vx 3 */
	asm volatile("vrgather.vi v11, v1, 5");
	asm volatile("vmv.x.s %0, v11" : "=r"(v)); put(v);
	ulong three = 3;
	asm volatile("vrgather.vx v12, v1, %0" :: "r"(three));
	asm volatile("vmv.x.s %0, v12" : "=r"(v)); put(v);
}

/* 6. Compares and mask operations */
static void t_compares(void)
{
	ulong v;
	asm volatile("vsetivli zero, 8, e32, m1, ta, ma");
	asm volatile("vle32.v v1, (%0)" :: "r"(isrc1));
	asm volatile("vle32.v v2, (%0)" :: "r"(isrc2));
	asm volatile("vmseq.vi v3, v1, 5");
	asm volatile("vmv.x.s %0, v3" : "=r"(v)); put(v); /* bit 4 */
	asm volatile("vmsne.vv v4, v1, v2");
	asm volatile("vmand.mm v5, v4, v4");
	asm volatile("vmv.x.s %0, v5" : "=r"(v)); put(v); /* all ones */
	asm volatile("vcpop.m %0, v3" : "=r"(v)); put(v);
	asm volatile("vfirst.m %0, v3" : "=r"(v)); put(v);
}

/* 7. LMUL=2 register groups */
static void t_lmul2(void)
{
	static const unsigned int a[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
	static const unsigned int b[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
	ulong v;
	asm volatile("vsetivli zero, 16, e32, m2, ta, ma");
	asm volatile("vle32.v v2, (%0)" :: "r"(a));
	asm volatile("vle32.v v4, (%0)" :: "r"(b));
	asm volatile("vadd.vv v6, v2, v4");
	asm volatile("vrgather.vi v8, v6, 8");
	asm volatile("vmv.x.s %0, v8" : "=r"(v)); put(v); /* 9+8 */
	asm volatile("vrgather.vi v8, v6, 15");
	asm volatile("vmv.x.s %0, v8" : "=r"(v)); put(v); /* 16+15 */
}

/* 8. Floating-point */
static const float fsrc1[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
static const float fsrc2[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
static void t_float(void)
{
	ulong v;
	float res;
	asm volatile("vsetivli zero, 8, e32, m1, ta, ma");
	asm volatile("vle32.v v1, (%0)" :: "r"(fsrc1));
	asm volatile("vle32.v v2, (%0)" :: "r"(fsrc2));
	asm volatile("vfadd.vv v3, v1, v2");
	asm volatile("vfsub.vv v4, v1, v2");
	asm volatile("vfmul.vv v5, v1, v2");
	asm volatile("vrgather.vi v6, v3, 0"); asm volatile("vfmv.f.s %0, v6" : "=f"(res)); put(fbits(res));
	asm volatile("vrgather.vi v6, v4, 0"); asm volatile("vfmv.f.s %0, v6" : "=f"(res)); put(fbits(res));
	asm volatile("vrgather.vi v6, v5, 7"); asm volatile("vfmv.f.s %0, v6" : "=f"(res)); put(fbits(res));
	/* vfredusum with seed 0: 1+..+8 = 36 */
	asm volatile("vmv.s.x v8, zero");
	asm volatile("vfredusum.vs v9, v1, v8");
	asm volatile("vfmv.f.s %0, v9" : "=f"(res)); put((ulong)res);
	/* vfmacc: v10 = v1*v2 + v10(=1.0f) = 1.5 .. 5.0 */
	ulong one_f = fbits(1.0f);
	asm volatile("vmv.v.x v10, %0" :: "r"(one_f));
	asm volatile("vfmacc.vv v10, v1, v2");
	asm volatile("vrgather.vi v6, v10, 0"); asm volatile("vfmv.f.s %0, v6" : "=f"(res)); put(fbits(res));
	asm volatile("vrgather.vi v6, v10, 7"); asm volatile("vfmv.f.s %0, v6" : "=f"(res)); put(fbits(res));
	/* vfadd.vf with 10.0f */
	float ten = 10.0f;
	asm volatile("vfadd.vf v11, v1, %0" :: "f"(ten));
	asm volatile("vrgather.vi v6, v11, 0"); asm volatile("vfmv.f.s %0, v6" : "=f"(res)); put(fbits(res));
	/* vfcvt.rtz.x.f.v: 1.5 -> 1, 8.5 -> 8 */
	asm volatile("vfcvt.rtz.x.f.v v12, v3");
	asm volatile("vrgather.vi v6, v12, 0"); asm volatile("vmv.x.s %0, v6" : "=r"(v)); put(v);
	asm volatile("vrgather.vi v6, v12, 7"); asm volatile("vmv.x.s %0, v6" : "=r"(v)); put(v);
}

/* 9. vmv and saturation */
static void t_misc(void)
{
	ulong v, eight = 8, one = 1, forty2 = 42;
	asm volatile("vsetivli zero, 8, e32, m1, ta, ma");
	asm volatile("vmv.v.x v1, %0" :: "r"(forty2));
	asm volatile("vadd.vx v2, v1, %0" :: "r"(eight));
	asm volatile("vmv.x.s %0, v2" : "=r"(v)); put(v); /* 50 */
	asm volatile("vle32.v v3, (%0)" :: "r"(isrc2));
	asm volatile("vsaddu.vi v4, v3, 1");
	asm volatile("vrgather.vi v5, v4, 7");
	asm volatile("vmv.x.s %0, v5" : "=r"(v)); put(v); /* saturated */
	asm volatile("vslide1up.vx v6, v3, %0" :: "r"(one));
	asm volatile("vrgather.vi v5, v6, 0"); asm volatile("vmv.x.s %0, v5" : "=r"(v)); put(v);
	asm volatile("vrgather.vi v5, v6, 1"); asm volatile("vmv.x.s %0, v5" : "=r"(v)); put(v);
	/* vmv<nr>r.v copies whole registers, ignoring vl and vtype. GCC emits
	   it whenever a vector value outlives its defining instruction. */
	asm volatile("vle32.v v8, (%0)" :: "r"(isrc1));
	asm volatile("vsetivli zero, 1, e32, m1, ta, ma"); /* vl=1 must not matter */
	asm volatile("vmv1r.v v9, v8");
	asm volatile("vsetivli zero, 8, e32, m1, ta, ma");
	asm volatile("vrgather.vi v5, v9, 7"); asm volatile("vmv.x.s %0, v5" : "=r"(v)); put(v);
	/* vmv2r.v moves an aligned pair, here filled one register at a time */
	asm volatile("vle32.v v10, (%0)" :: "r"(isrc1));
	asm volatile("vle32.v v11, (%0)" :: "r"(isrc2));
	asm volatile("vmv2r.v v12, v10");
	asm volatile("vrgather.vi v5, v13, 5"); asm volatile("vmv.x.s %0, v5" : "=r"(v)); put(v);
}

/* 10. Unit-stride vle32/vse32 and element-wise float arithmetic outside the
   whole-lane configuration. The binary translator inlines those two as long as
   vtype says e32/m1 with vl == VLMAX and the instruction is unmasked; every
   case below deliberately breaks one of those conditions, or moves a lane
   through an address with no natural alignment. */
static unsigned char raw[64];
static void t_vlsu_edges(void)
{
	ulong v, pat = 0xABCD, five = 5;
	float res;

	/* Whole lane through an address that is not 4-byte aligned. */
	unsigned char *ua = &raw[1];
	asm volatile("vsetivli zero, 8, e32, m1, ta, ma");
	asm volatile("vle32.v v1, (%0)" :: "r"(isrc1));
	asm volatile("vse32.v v1, (%0)" :: "r"(ua) : "memory");
	asm volatile("vmv.v.x v2, zero");
	asm volatile("vle32.v v2, (%0)" :: "r"(ua));
	asm volatile("vrgather.vi v3, v2, 0"); asm volatile("vmv.x.s %0, v3" : "=r"(v)); put(v); /* 1 */
	asm volatile("vrgather.vi v3, v2, 7"); asm volatile("vmv.x.s %0, v3" : "=r"(v)); put(v); /* 8 */

	/* vl < VLMAX: the load stops after 4 elements, the tail is undisturbed. */
	asm volatile("vmv.v.x v4, %0" :: "r"(pat));
	asm volatile("vsetivli zero, 4, e32, m1, tu, mu");
	asm volatile("vle32.v v4, (%0)" :: "r"(isrc2));
	asm volatile("vsetivli zero, 8, e32, m1, tu, mu");
	asm volatile("vrgather.vi v5, v4, 3"); asm volatile("vmv.x.s %0, v5" : "=r"(v)); put(v); /* 4000 */
	asm volatile("vrgather.vi v5, v4, 4"); asm volatile("vmv.x.s %0, v5" : "=r"(v)); put(v); /* 0xABCD */

	/* vl < VLMAX on a store: memory past the 4th element is left alone. */
	idst[4] = 0x5A5A;
	asm volatile("vsetivli zero, 4, e32, m1, tu, mu");
	asm volatile("vse32.v v1, (%0)" :: "r"(&idst[0]) : "memory");
	asm volatile("vsetivli zero, 8, e32, m1, tu, mu");
	put(idst[3]); put(idst[4]);                                              /* 4, 0x5A5A */

	/* Masked load: inactive elements keep their previous value. */
	asm volatile("vle32.v v6, (%0)" :: "r"(isrc1));
	asm volatile("vmsltu.vx v0, v6, %0" :: "r"(five));  /* bits 0-3 set */
	asm volatile("vmv.v.x v7, %0" :: "r"(pat));
	asm volatile("vle32.v v7, (%0), v0.t" :: "r"(isrc2));
	asm volatile("vrgather.vi v8, v7, 0"); asm volatile("vmv.x.s %0, v8" : "=r"(v)); put(v); /* 10 */
	asm volatile("vrgather.vi v8, v7, 4"); asm volatile("vmv.x.s %0, v8" : "=r"(v)); put(v); /* 0xABCD */

	/* Masked store: only the first 4 words reach memory. */
	idst[8] = 0x77; idst[12] = 0x99;
	asm volatile("vse32.v v6, (%0), v0.t" :: "r"(&idst[8]) : "memory");
	put(idst[8]); put(idst[12]);                                             /* 1, 0x99 */

	/* Float arithmetic with vl < VLMAX, tail undisturbed. */
	ulong nine_f = fbits(9.0f);
	asm volatile("vmv.v.x v9, %0" :: "r"(nine_f));
	asm volatile("vle32.v v10, (%0)" :: "r"(fsrc1));
	asm volatile("vle32.v v11, (%0)" :: "r"(fsrc2));
	asm volatile("vsetivli zero, 4, e32, m1, tu, mu");
	asm volatile("vfadd.vv v9, v10, v11");
	asm volatile("vsetivli zero, 8, e32, m1, tu, mu");
	asm volatile("vrgather.vi v12, v9, 3"); asm volatile("vfmv.f.s %0, v12" : "=f"(res)); put(fbits(res)); /* 4.5 */
	asm volatile("vrgather.vi v12, v9, 4"); asm volatile("vfmv.f.s %0, v12" : "=f"(res)); put(fbits(res)); /* 9.0 */

	/* Masked vfmul.vf: inactive elements keep their old value. */
	float two = 2.0f;
	asm volatile("vmv.v.x v13, %0" :: "r"(nine_f));
	asm volatile("vfmul.vf v13, v10, %0, v0.t" :: "f"(two));
	asm volatile("vrgather.vi v12, v13, 0"); asm volatile("vfmv.f.s %0, v12" : "=f"(res)); put(fbits(res)); /* 2.0 */
	asm volatile("vrgather.vi v12, v13, 4"); asm volatile("vfmv.f.s %0, v12" : "=f"(res)); put(fbits(res)); /* 9.0 */

	/* Same float ops at e64: the fast path is e32-only. */
	static double dsrc[4] = {1.0, 2.0, 3.0, 4.0};
	asm volatile("vsetivli zero, 4, e64, m1, ta, ma");
	asm volatile("vle64.v v14, (%0)" :: "r"(dsrc));
	asm volatile("vfadd.vv v15, v14, v14");
	asm volatile("vfmul.vv v16, v14, v14");
	asm volatile("vse64.v v15, (%0)" :: "r"(dsrc) : "memory");
	put((ulong)dsrc[3]);                                                     /* 8 */
	asm volatile("vse64.v v16, (%0)" :: "r"(dsrc) : "memory");
	put((ulong)dsrc[3]);                                                     /* 16 */
	asm volatile("vsetivli zero, 8, e32, m1, ta, ma");
}

void _start(void)
{
	t_vsetvli();
	t_int_ops();
	t_masked();
	t_widths();
	t_reduce_slide();
	t_compares();
	t_lmul2();
	t_float();
	t_misc();
	t_vlsu_edges();
	asm volatile("li a7, 93; li a0, 0; ecall");
	__builtin_unreachable();
}
)M";

TEST_CASE("RVV 1.0 basics", "[rvv]")
{
	const auto binary = build_rvv(GUEST);
	// The freestanding newlib toolchain links a single RWX segment.
	riscv::Machine<riscv::RISCV64> machine { binary, {
		.memory_max = MAX_MEMORY,
		.allow_write_exec_segment = true,
	} };
	machine.setup_linux_syscalls();
	machine.setup_linux({"rvv"}, {"LC_ALL=C"});
	// The block-based simulator reports the instruction at the end of a
	// block on exceptions; only simulate_precise() pinpoints the actual
	// faulting instruction. Use it while the emulator is unverified.
	// Unlike machine.simulate(), it needs the limit configured up front.
	machine.set_max_instructions(MAX_INSTRUCTIONS);
	try {
		machine.cpu.simulate_precise();
	} catch (const riscv::MachineException& e) {
		const auto pc = e.data() != 0 ? e.data() : machine.cpu.pc();
		const auto instr = machine.memory.read<uint32_t>(pc);
		fprintf(stderr, "Exception at pc=0x%lX: %s (instr 0x%08x)\n",
			(unsigned long)pc, e.what(), instr);
		throw;
	}
	REQUIRE(machine.instruction_limit_reached() == false);

	const auto out = machine.address_of("out");
	REQUIRE(out != 0x0);

	std::vector<uint64_t> r;
	for (unsigned i = 0; i < RESULTS; i++)
		r.push_back(machine.memory.read<uint64_t>(out + 8 * i));

	unsigned n = 0;
	/* t_vsetvli: VLMAX per SEW/LMUL with VLEN=256 */
	REQUIRE(r[n++] == 8);    /* e32 m1 */
	REQUIRE(r[n++] == 3);    /* clamped to avl */
	REQUIRE(r[n++] == 32);   /* e8 m1 */
	REQUIRE(r[n++] == 8);    /* e64 m2 */
	REQUIRE(r[n++] == 4);    /* e32 mf2 */
	REQUIRE(r[n++] == 64);   /* e16 m4 */
	REQUIRE(r[n++] == 1);    /* e64 mf8: max(1, 256/64/8) */
	REQUIRE(r[n++] == 16);   /* e16 m1 VLMAX */
	REQUIRE(r[n++] == 5);    /* vsetivli 5 */
	REQUIRE(r[n++] == 8);    /* vsetivli 31 clamps to 8 */
	REQUIRE(r[n++] == 32);   /* vsetvl, vtype=0 (e8 m1) */
	REQUIRE(r[n++] == 0);    /* vill: vlmul=100 reserved */

	/* t_int_ops */
	REQUIRE(r[n++] == 11);              /* vadd[0] = 1+10 */
	REQUIRE(r[n++] == 9);               /* vsub[0] = 10-1 */
	REQUIRE(r[n++] == 198);             /* vsub[1] = 200-2 */
	REQUIRE(r[n++] == 4294967295ul - 8);/* vsub[7] */
	REQUIRE(r[n++] == 0);               /* vand[0] = 1&10 */
	REQUIRE(r[n++] == 11);              /* vor[0] = 1|10 */
	REQUIRE(r[n++] == (2ul ^ 200ul));   /* vxor[1] */
	REQUIRE(r[n++] == 1);               /* vminu[0] */
	REQUIRE(r[n++] == 16);              /* vsll[0] = 1<<4 */
	REQUIRE(r[n++] == 1);               /* vsrl[0] = 10>>3 */
	REQUIRE(r[n++] == 14);              /* vrsub[0] = 15-1 */
	REQUIRE(r[n++] == 0xFFFFFFFFFFFFFFFAul);/* vadd.vi -7 wraps, vmv.x.s sign-extends */
	REQUIRE(r[n++] == 7 + 70);          /* vadd[6] via vrgather */

	/* t_masked */
	REQUIRE(r[n++] == 11);      /* active element: 1+10 */
	REQUIRE(r[n++] == 0xBEEF);  /* masked-off, undisturbed */
	REQUIRE(r[n++] == 1);       /* vmerge: mask=1 -> vs1 */
	REQUIRE(r[n++] == 50);      /* vmerge: mask=0 -> vs2[4] */
	REQUIRE(r[n++] == 4 + 4000);/* vl=4 body */
	REQUIRE(r[n++] == 0x1234);  /* tail-undisturbed */

	/* t_widths */
	REQUIRE(r[n++] == 2);       /* e8: 1+1 */
	REQUIRE(r[n++] == 32);      /* e8: bsrc[30]+1 */
	REQUIRE(r[n++] == 100u * 100u); /* e16 */
	REQUIRE(r[n++] == 0x1000u);     /* e16: 1600*1600 truncated to 16 bits */
	REQUIRE(r[n++] == 0x200000000ull); /* e64 */
	REQUIRE(r[n++] == 2 * 0xdeadbeefcafeull);

	/* t_reduce_slide */
	REQUIRE(r[n++] == 1 + 1+2+3+4+5+6+7+8); /* vredsum */
	REQUIRE(r[n++] == 0xFFFFFFFFFFFFFFFFul); /* vredmaxu, vmv.x.s sign-extends */
	REQUIRE(r[n++] == 4);   /* slidedown 3: src[3] */
	REQUIRE(r[n++] == 8);   /* dest[4] = src[7] */
	REQUIRE(r[n++] == 0);   /* dest[5]: src[8] beyond VLMAX -> 0 */
	REQUIRE(r[n++] == 2);   /* slide1down: dest[0] = src[1] */
	REQUIRE(r[n++] == 77);  /* scalar in last element */
	REQUIRE(r[n++] == 100); /* slideup: dest[0] undisturbed */
	REQUIRE(r[n++] == 100); /* slideup: dest[1] undisturbed */
	REQUIRE(r[n++] == 1);   /* dest[2] = src[0] */
	REQUIRE(r[n++] == 2);   /* dest[3] = src[1] */
	REQUIRE(r[n++] == 6);   /* vrgather.vi 5 */
	REQUIRE(r[n++] == 4);   /* vrgather.vx 3 */

	/* t_compares */
	REQUIRE(r[n++] == 0x10);   /* vmseq.vi 5 -> bit 4 */
	REQUIRE(r[n++] == 0xFF);   /* vmand of vmsne: 8 mask bits set */
	REQUIRE(r[n++] == 1);      /* vcpop */
	REQUIRE(r[n++] == 4);      /* vfirst */

	/* t_lmul2 */
	REQUIRE(r[n++] == 17);     /* element 8: 9+8 */
	REQUIRE(r[n++] == 31);     /* element 15: 16+15 */

	/* t_float */
	REQUIRE(r[n++] == fbits(1.0f + 0.5f));  /* vfadd */
	REQUIRE(r[n++] == fbits(1.0f - 0.5f));  /* vfsub */
	REQUIRE(r[n++] == fbits(8.0f * 0.5f));  /* vfmul */
	REQUIRE(r[n++] == 36);     /* vfredusum */
	REQUIRE(r[n++] == fbits(1.0f * 0.5f + 1.0f)); /* vfmacc */
	REQUIRE(r[n++] == fbits(8.0f * 0.5f + 1.0f));
	REQUIRE(r[n++] == fbits(1.0f + 10.0f)); /* vfadd.vf */
	REQUIRE(r[n++] == 1);      /* vfcvt.rtz 1.5 */
	REQUIRE(r[n++] == 8);      /* vfcvt.rtz 8.5 */

	/* t_misc */
	REQUIRE(r[n++] == 50);            /* vmv.v.i + vadd.vx */
	REQUIRE(r[n++] == 0xFFFFFFFFFFFFFFFFul); /* vsaddu saturates, vmv.x.s sign-extends */
	REQUIRE(r[n++] == 1);             /* vslide1up scalar */
	REQUIRE(r[n++] == 10);            /* vslide1up shifts up */
	REQUIRE(r[n++] == 8);             /* vmv1r.v copied all 8 elements */
	REQUIRE(r[n++] == 600000);        /* vmv2r.v copied the second register */

	/* t_vlsu_edges */
	REQUIRE(r[n++] == 1);             /* unaligned lane round-trip, element 0 */
	REQUIRE(r[n++] == 8);             /* unaligned lane round-trip, element 7 */
	REQUIRE(r[n++] == 4000);          /* vl=4 load, last active element */
	REQUIRE(r[n++] == 0xABCD);        /* vl=4 load, tail undisturbed */
	REQUIRE(r[n++] == 4);             /* vl=4 store wrote the 4th word */
	REQUIRE(r[n++] == 0x5A5A);        /* vl=4 store left the 5th word alone */
	REQUIRE(r[n++] == 10);            /* masked load, active element */
	REQUIRE(r[n++] == 0xABCD);        /* masked load, inactive element */
	REQUIRE(r[n++] == 1);             /* masked store, active element */
	REQUIRE(r[n++] == 0x99);          /* masked store, inactive element */
	REQUIRE(r[n++] == fbits(4.0f + 0.5f)); /* vfadd at vl=4, last active */
	REQUIRE(r[n++] == fbits(9.0f));        /* vfadd at vl=4, tail undisturbed */
	REQUIRE(r[n++] == fbits(2.0f));        /* masked vfmul.vf, active element */
	REQUIRE(r[n++] == fbits(9.0f));        /* masked vfmul.vf, inactive element */
	REQUIRE(r[n++] == 8);             /* e64 vfadd */
	REQUIRE(r[n++] == 16);            /* e64 vfmul */

	REQUIRE(n <= RESULTS);
}

/* The same guest through the block-based simulator must produce the
 * exact same results: the two paths share handlers, but decoding and
 * exception reporting differ. */
TEST_CASE("RVV 1.0 fast-path parity", "[rvv]")
{
	const auto binary = build_rvv(GUEST);
	riscv::Machine<riscv::RISCV64> machine { binary, {
		.memory_max = MAX_MEMORY,
		.allow_write_exec_segment = true,
	} };
	machine.setup_linux_syscalls();
	machine.setup_linux({"rvv"}, {"LC_ALL=C"});
	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.instruction_limit_reached() == false);

	const auto out = machine.address_of("out");
	REQUIRE(out != 0x0);

	std::vector<uint64_t> fast;
	for (unsigned i = 0; i < RESULTS; i++)
		fast.push_back(machine.memory.read<uint64_t>(out + 8 * i));

	riscv::Machine<riscv::RISCV64> precise { binary, {
		.memory_max = MAX_MEMORY,
		.allow_write_exec_segment = true,
	} };
	precise.setup_linux_syscalls();
	precise.setup_linux({"rvv"}, {"LC_ALL=C"});
	precise.set_max_instructions(MAX_INSTRUCTIONS);
	precise.cpu.simulate_precise();
	REQUIRE(precise.instruction_limit_reached() == false);

	for (unsigned i = 0; i < RESULTS; i++)
		REQUIRE(fast[i] == precise.memory.read<uint64_t>(out + 8 * i));
}
