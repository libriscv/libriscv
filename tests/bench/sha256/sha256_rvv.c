// Multi-buffer SHA-256 for RVV.
//
// One message per vector lane, so the lane count is the only thing that
// changes between the two modes this benchmark exists to compare:
//
//   single   vl = 1      one message in flight; every vector instruction
//                        retires exactly one element of work
//   wide     vl = VLMAX  one message per lane; the identical instruction
//                        stream, VLMAX times the work
//
// The lane count is a runtime value, so both modes run the same code and
// the same number of instructions -- only the vsetvli at the top differs.
// The ratio between them is therefore the emulator's per-element cost
// measured against its per-instruction dispatch cost, with nothing else
// moving. Run it under the interpreter and under the binary translator to
// see how each backend divides its time between the two.
//
//   sha256_rvv [single|wide|<lanes>] [iterations]
//
// Build with a toolchain whose -march includes v; zvbb is used for the
// rotates when it is available, since that is what a real implementation
// would do.

#include <riscv_vector.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static inline size_t vlenb(void)
{
	size_t v;
	__asm__ volatile("csrr %0, vlenb" : "=r"(v));
	return v;
}

#define MAX_LANES 64          // enough for VLEN=4096 at e32m1
#define BLOCK_WORDS 16

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
static const uint32_t H256[8] = {
	0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
	0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

// ---------------------------------------------------------------- vector

typedef vuint32m1_t V;

#define VADD(a, b)   __riscv_vadd_vv_u32m1((a), (b), vl)
#define VADDK(a, k)  __riscv_vadd_vx_u32m1((a), (k), vl)
#define VXOR(a, b)   __riscv_vxor_vv_u32m1((a), (b), vl)
#define VAND(a, b)   __riscv_vand_vv_u32m1((a), (b), vl)
#define VOR(a, b)    __riscv_vor_vv_u32m1((a), (b), vl)
#define VSLL(a, n)   __riscv_vsll_vx_u32m1((a), (n), vl)
#define VSRL(a, n)   __riscv_vsrl_vx_u32m1((a), (n), vl)
#define VSPLAT(x)    __riscv_vmv_v_x_u32m1((x), vl)

// Zvbb gives the rotate directly; without it a rotate is three instructions,
// which roughly halves the throughput of the whole hash.
#if defined(__riscv_zvbb)
# define VROR(a, n)  __riscv_vror_vx_u32m1((a), (n), vl)
# define ROTATE_COST "zvbb vror"
#else
# define VROR(a, n)  VOR(VSRL((a), (n)), VSLL((a), 32 - (n)))
# define ROTATE_COST "vsll/vsrl/vor"
#endif

#define BSIG0(x)  VXOR(VXOR(VROR((x), 2),  VROR((x), 13)), VROR((x), 22))
#define BSIG1(x)  VXOR(VXOR(VROR((x), 6),  VROR((x), 11)), VROR((x), 25))
#define SSIG0(x)  VXOR(VXOR(VROR((x), 7),  VROR((x), 18)), VSRL((x), 3))
#define SSIG1(x)  VXOR(VXOR(VROR((x), 17), VROR((x), 19)), VSRL((x), 10))

// The cheap forms a real implementation uses: three operations each.
#define CH(e, f, g)   VXOR((g), VAND((e), VXOR((f), (g))))
#define MAJ(a, b, c)  VOR(VAND((a), (b)), VAND((c), VOR((a), (b))))

// One round. d becomes the next e, h becomes the next a, which is what lets
// the caller rotate the names instead of moving any registers.
#define RND(a, b, c, d, e, f, g, h, kt, wt)                            \
	do {                                                               \
		V t1 = VADD(VADD(VADD((h), BSIG1(e)), CH(e, f, g)), (wt));     \
		t1 = VADDK(t1, (kt));                                          \
		V t2 = VADD(BSIG0(a), MAJ(a, b, c));                           \
		(d) = VADD((d), t1);                                           \
		(h) = VADD(t1, t2);                                            \
	} while (0)

// Sixteen rounds. The eight state names rotate by one per round, so after a
// full group they are back where they started and the next group can be
// written the same way. The vector types are sizeless and cannot form an
// array, which is why the schedule is sixteen named variables rather than
// w[16].
#define ROUND16(t)                                            \
	RND(a, b, c, d, e, f, g, h, K256[(t) +  0],  w0);          \
	RND(h, a, b, c, d, e, f, g, K256[(t) +  1],  w1);          \
	RND(g, h, a, b, c, d, e, f, K256[(t) +  2],  w2);          \
	RND(f, g, h, a, b, c, d, e, K256[(t) +  3],  w3);          \
	RND(e, f, g, h, a, b, c, d, K256[(t) +  4],  w4);          \
	RND(d, e, f, g, h, a, b, c, K256[(t) +  5],  w5);          \
	RND(c, d, e, f, g, h, a, b, K256[(t) +  6],  w6);          \
	RND(b, c, d, e, f, g, h, a, K256[(t) +  7],  w7);          \
	RND(a, b, c, d, e, f, g, h, K256[(t) +  8],  w8);          \
	RND(h, a, b, c, d, e, f, g, K256[(t) +  9],  w9);          \
	RND(g, h, a, b, c, d, e, f, K256[(t) + 10], w10);          \
	RND(f, g, h, a, b, c, d, e, K256[(t) + 11], w11);          \
	RND(e, f, g, h, a, b, c, d, K256[(t) + 12], w12);          \
	RND(d, e, f, g, h, a, b, c, K256[(t) + 13], w13);          \
	RND(c, d, e, f, g, h, a, b, K256[(t) + 14], w14);          \
	RND(b, c, d, e, f, g, h, a, K256[(t) + 15], w15)

// The rolling schedule, advanced sixteen words in place:
//   W[t] = W[t-16] + sigma0(W[t-15]) + W[t-7] + sigma1(W[t-2])
// Later entries read the earlier ones this group already rewrote, which is
// exactly what the recurrence asks for.
#define WUP(w, wm15, wm7, wm2)                                          \
	(w) = VADD(VADD(VADD((w), SSIG0(wm15)), (wm7)), SSIG1(wm2))

#define SCHED16()                          \
	WUP( w0,  w1,  w9, w14);               \
	WUP( w1,  w2, w10, w15);               \
	WUP( w2,  w3, w11,  w0);               \
	WUP( w3,  w4, w12,  w1);               \
	WUP( w4,  w5, w13,  w2);               \
	WUP( w5,  w6, w14,  w3);               \
	WUP( w6,  w7, w15,  w4);               \
	WUP( w7,  w8,  w0,  w5);               \
	WUP( w8,  w9,  w1,  w6);               \
	WUP( w9, w10,  w2,  w7);               \
	WUP(w10, w11,  w3,  w8);               \
	WUP(w11, w12,  w4,  w9);               \
	WUP(w12, w13,  w5, w10);               \
	WUP(w13, w14,  w6, w11);               \
	WUP(w14, w15,  w7, w12);               \
	WUP(w15,  w0,  w8, w13)

// Compress one block per lane. The block words are interleaved -- word i of
// lane j lives at blk[i * vl + j] -- so each schedule word is one unit-stride
// load, which is how every multi-buffer implementation lays its input out.
// The eight digest words come back interleaved the same way.
static void sha256_block_mb(const uint32_t *blk, uint32_t *digest, size_t vl)
{
#define LOADW(i) V w##i = __riscv_vle32_v_u32m1(&blk[(size_t)(i) * vl], vl)
	LOADW(0);  LOADW(1);  LOADW(2);  LOADW(3);
	LOADW(4);  LOADW(5);  LOADW(6);  LOADW(7);
	LOADW(8);  LOADW(9);  LOADW(10); LOADW(11);
	LOADW(12); LOADW(13); LOADW(14); LOADW(15);
#undef LOADW

	V a = VSPLAT(H256[0]), b = VSPLAT(H256[1]);
	V c = VSPLAT(H256[2]), d = VSPLAT(H256[3]);
	V e = VSPLAT(H256[4]), f = VSPLAT(H256[5]);
	V g = VSPLAT(H256[6]), h = VSPLAT(H256[7]);

	ROUND16(0);
	SCHED16(); ROUND16(16);
	SCHED16(); ROUND16(32);
	SCHED16(); ROUND16(48);

	__riscv_vse32_v_u32m1(&digest[0 * vl], VADDK(a, H256[0]), vl);
	__riscv_vse32_v_u32m1(&digest[1 * vl], VADDK(b, H256[1]), vl);
	__riscv_vse32_v_u32m1(&digest[2 * vl], VADDK(c, H256[2]), vl);
	__riscv_vse32_v_u32m1(&digest[3 * vl], VADDK(d, H256[3]), vl);
	__riscv_vse32_v_u32m1(&digest[4 * vl], VADDK(e, H256[4]), vl);
	__riscv_vse32_v_u32m1(&digest[5 * vl], VADDK(f, H256[5]), vl);
	__riscv_vse32_v_u32m1(&digest[6 * vl], VADDK(g, H256[6]), vl);
	__riscv_vse32_v_u32m1(&digest[7 * vl], VADDK(h, H256[7]), vl);
}

// ---------------------------------------------------------------- scalar

// A plain reference compression, used to check the vector result. It is the
// same algorithm written the obvious way, so a lane that disagrees with it is
// a vector bug and not a transcription of the same mistake twice.
static void sha256_block_ref(const uint32_t *blk, uint32_t *digest)
{
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
	uint32_t w[64];
	for (int i = 0; i < 16; i++)
		w[i] = blk[i];
	for (int i = 16; i < 64; i++) {
		const uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
		const uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}
	uint32_t s[8];
	for (int i = 0; i < 8; i++)
		s[i] = H256[i];
	for (int i = 0; i < 64; i++) {
		const uint32_t S1 = ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25);
		const uint32_t ch = (s[4] & s[5]) ^ (~s[4] & s[6]);
		const uint32_t t1 = s[7] + S1 + ch + K256[i] + w[i];
		const uint32_t S0 = ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22);
		const uint32_t mj = (s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]);
		const uint32_t t2 = S0 + mj;
		s[7] = s[6]; s[6] = s[5]; s[5] = s[4]; s[4] = s[3] + t1;
		s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = t1 + t2;
	}
	for (int i = 0; i < 8; i++)
		digest[i] = s[i] + H256[i];
#undef ROR
}

// ---------------------------------------------------------------- driver

static uint32_t blk[BLOCK_WORDS * MAX_LANES];
static uint32_t dig[8 * MAX_LANES];

// The one-block padding of the message "abc", which has a digest everyone
// already knows, so the check does not rest on this file's own reference.
static const uint32_t ABC_BLOCK[16] = {
	0x61626380, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000018,
};
static const uint32_t ABC_DIGEST[8] = {
	0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223,
	0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad,
};

static int verify(size_t vl)
{
	for (size_t i = 0; i < BLOCK_WORDS; i++)
		for (size_t j = 0; j < vl; j++)
			blk[i * vl + j] = ABC_BLOCK[i];
	sha256_block_mb(blk, dig, vl);

	int bad = 0;
	for (size_t j = 0; j < vl; j++)
		for (size_t i = 0; i < 8; i++)
			if (dig[i * vl + j] != ABC_DIGEST[i]) {
				printf("verify: lane %zu word %zu: got %08x want %08x\n",
					j, i, dig[i * vl + j], ABC_DIGEST[i]);
				bad = 1;
			}

	// And the same block through the scalar reference, so a build that gets
	// the well-known answer by accident still has to agree with it.
	uint32_t ref[8];
	sha256_block_ref(ABC_BLOCK, ref);
	for (size_t i = 0; i < 8; i++)
		if (ref[i] != ABC_DIGEST[i]) {
			printf("verify: scalar reference word %zu: got %08x want %08x\n",
				i, ref[i], ABC_DIGEST[i]);
			bad = 1;
		}
	return bad;
}

// Each iteration feeds the previous digest back into the first eight words of
// the block. That is a real dependency between iterations, so nothing here
// can be hoisted out of the loop or folded away, and the final digest is a
// deterministic function of the iteration count.
static void chain(size_t iters, size_t vl)
{
	for (size_t n = 0; n < iters; n++) {
		sha256_block_mb(blk, dig, vl);
		memcpy(blk, dig, 8 * vl * sizeof(uint32_t));
	}
}

static double now_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	const size_t vlmax = __riscv_vsetvlmax_e32m1();
	size_t lanes = vlmax;
	size_t iters = 50000;

	if (argc > 1) {
		if (!strcmp(argv[1], "single"))
			lanes = 1;
		else if (!strcmp(argv[1], "wide"))
			lanes = vlmax;
		else
			lanes = strtoul(argv[1], NULL, 0);
	}
	if (argc > 2)
		iters = strtoul(argv[2], NULL, 0);

	if (lanes < 1 || lanes > MAX_LANES) {
		fprintf(stderr, "lanes must be 1..%d\n", MAX_LANES);
		return 1;
	}
	// vsetvl clamps to VLMAX, and every intrinsic below is handed the value it
	// returned, so the whole run really does use this many lanes.
	const size_t vl = __riscv_vsetvl_e32m1(lanes);

	printf("sha256-rvv: vlenb=%zu VLMAX=%zu lanes=%zu rotate=%s iters=%zu\n",
		vlenb(), vlmax, vl, ROTATE_COST, iters);

	if (verify(vl)) {
		printf("verify: FAILED\n");
		return 1;
	}
	printf("verify: ok\n");

	// Start from a distinct block per lane so the lanes are not all doing the
	// identical computation, which is what a multi-buffer hash is actually for.
	for (size_t i = 0; i < BLOCK_WORDS; i++)
		for (size_t j = 0; j < vl; j++)
			blk[i * vl + j] = ABC_BLOCK[i] ^ (uint32_t)(j * 0x9e3779b9u);

	const double t0 = now_seconds();
	chain(iters, vl);
	const double elapsed = now_seconds() - t0;

	const double hashes = (double)iters * (double)vl;
	printf("digest[0] =");
	for (size_t i = 0; i < 8; i++)
		printf(" %08x", dig[i * vl]);
	printf("\n");
	printf("time %.4f s  hashes %.0f  %.3f MH/s  %.2f MB/s  %.1f blocks/lane-s\n",
		elapsed, hashes,
		hashes / elapsed / 1e6,
		hashes * 64.0 / elapsed / 1e6,
		(double)iters / elapsed);
	return 0;
}
