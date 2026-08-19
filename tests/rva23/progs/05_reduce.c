/* Every reduction shape: sum, and/or/xor, min/max, count, and the
 * ordered/unordered floating-point sums, at each element width. */
#include <stdio.h>
#include <stdint.h>
#include <float.h>

#define N 3331
static int8_t   a8[N];
static int16_t  a16[N];
static int32_t  a32[N];
static int64_t  a64[N];
static uint32_t u32[N];
static float    f[N];
static double   d[N];

int main(void)
{
	uint64_t seed = 99;
	for (int i = 0; i < N; i++) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		a8[i]  = (int8_t)seed;
		a16[i] = (int16_t)(seed >> 13);
		a32[i] = (int32_t)(seed >> 21);
		a64[i] = (int64_t)(seed >> 3);
		u32[i] = (uint32_t)(seed >> 17);
		f[i] = (float)((int32_t)(seed >> 40)) * 0.001f;
		d[i] = (double)((int32_t)(seed >> 36)) * 0.0001;
	}

	long long s8 = 0, s16 = 0, s32 = 0, s64 = 0;
	for (int i = 0; i < N; i++) s8  += a8[i];
	for (int i = 0; i < N; i++) s16 += a16[i];
	for (int i = 0; i < N; i++) s32 += a32[i];
	for (int i = 0; i < N; i++) s64 += a64[i];
	printf("sum %lld %lld %lld %lld\n", s8, s16, s32, s64);

	int32_t and32 = -1, or32 = 0, xor32 = 0;
	for (int i = 0; i < N; i++) and32 &= a32[i];
	for (int i = 0; i < N; i++) or32  |= a32[i];
	for (int i = 0; i < N; i++) xor32 ^= a32[i];
	printf("logic %d %d %d\n", and32, or32, xor32);

	int8_t  mn8 = 127, mx8 = -128;
	int32_t mn32 = 2147483647, mx32 = -2147483648;
	uint32_t mnu = 0xFFFFFFFFu, mxu = 0;
	int64_t mn64 = 0x7FFFFFFFFFFFFFFFll, mx64 = -mn64 - 1;
	for (int i = 0; i < N; i++) { if (a8[i] < mn8) mn8 = a8[i]; if (a8[i] > mx8) mx8 = a8[i]; }
	for (int i = 0; i < N; i++) { if (a32[i] < mn32) mn32 = a32[i]; if (a32[i] > mx32) mx32 = a32[i]; }
	for (int i = 0; i < N; i++) { if (u32[i] < mnu) mnu = u32[i]; if (u32[i] > mxu) mxu = u32[i]; }
	for (int i = 0; i < N; i++) { if (a64[i] < mn64) mn64 = a64[i]; if (a64[i] > mx64) mx64 = a64[i]; }
	printf("minmax %d %d %d %d %u %u %lld %lld\n", mn8, mx8, mn32, mx32,
		mnu, mxu, (long long)mn64, (long long)mx64);

	/* counting, which is a mask population count */
	int c1 = 0, c2 = 0, c3 = 0;
	for (int i = 0; i < N; i++) if (a32[i] < 0) c1++;
	for (int i = 0; i < N; i++) if (u32[i] > 0x80000000u) c2++;
	for (int i = 0; i < N; i++) if (f[i] > 0.0f) c3++;
	printf("count %d %d %d\n", c1, c2, c3);

	/* floating-point sums, and the min/max that must ignore the ordering */
	float  fs = 0.0f, fmn = FLT_MAX, fmx = -FLT_MAX;
	double ds = 0.0, dmn = DBL_MAX, dmx = -DBL_MAX;
	for (int i = 0; i < N; i++) fs += f[i];
	for (int i = 0; i < N; i++) { if (f[i] < fmn) fmn = f[i]; if (f[i] > fmx) fmx = f[i]; }
	for (int i = 0; i < N; i++) ds += d[i];
	for (int i = 0; i < N; i++) { if (d[i] < dmn) dmn = d[i]; if (d[i] > dmx) dmx = d[i]; }
	printf("fp %.4f %.4f %.4f %.8f %.8f %.8f\n", fs, fmn, fmx, ds, dmn, dmx);

	/* dot products, the widening reductions */
	long long dot = 0; double fdot = 0, ddot = 0;
	for (int i = 0; i < N; i++) dot += (int32_t)a16[i] * (int32_t)a16[i];
	for (int i = 0; i < N; i++) fdot += (double)f[i] * (double)f[i];
	for (int i = 0; i < N; i++) ddot += d[i] * d[i];
	printf("dot %lld %.6f %.8f\n", dot, fdot, ddot);

	/* early-exit search: first index matching a predicate */
	int first = -1;
	for (int i = 0; i < N; i++) if (a32[i] == a32[N - 1]) { first = i; break; }
	printf("first %d\n", first);
	return 0;
}
