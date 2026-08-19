/* Integer autovectorisation across every element width, including the
 * widening and narrowing conversions between them. */
#include <stdio.h>
#include <stdint.h>

#define N 2003   /* deliberately not a multiple of any VLMAX */
static int8_t   i8[N];
static int16_t  i16[N];
static int32_t  i32[N];
static int64_t  i64[N];
static uint8_t  u8[N];
static uint16_t u16[N];
static uint32_t u32[N];
static uint64_t u64[N];

static uint64_t mix(uint64_t x) { return x * 6364136223846793005ull + 1442695040888963407ull; }

int main(void)
{
	uint64_t seed = 12345;
	for (int i = 0; i < N; i++) {
		seed = mix(seed);
		i8[i]  = (int8_t)seed;   u8[i]  = (uint8_t)(seed >> 8);
		i16[i] = (int16_t)(seed >> 16); u16[i] = (uint16_t)(seed >> 24);
		i32[i] = (int32_t)(seed >> 32); u32[i] = (uint32_t)(seed >> 8);
		i64[i] = (int64_t)seed;  u64[i] = seed;
	}

	/* widening accumulate at each width */
	int64_t a = 0, b = 0, c = 0;
	for (int i = 0; i < N; i++) a += (int16_t)i8[i] * (int16_t)i8[i];
	for (int i = 0; i < N; i++) b += (int32_t)i16[i] * (int32_t)i16[i];
	for (int i = 0; i < N; i++) c += (int64_t)i32[i] * (int64_t)i32[i];
	printf("wacc %lld %lld %lld\n", (long long)a, (long long)b, (long long)c);

	uint64_t ua = 0, ub = 0, uc = 0;
	for (int i = 0; i < N; i++) ua += (uint16_t)u8[i] * (uint16_t)u8[i];
	for (int i = 0; i < N; i++) ub += (uint32_t)u16[i] * (uint32_t)u16[i];
	for (int i = 0; i < N; i++) uc += (uint64_t)u32[i] * (uint64_t)u32[i];
	printf("uwacc %llu %llu %llu\n",
		(unsigned long long)ua, (unsigned long long)ub, (unsigned long long)uc);

	/* narrowing casts */
	for (int i = 0; i < N; i++) i8[i]  = (int8_t)(i32[i] >> 5);
	for (int i = 0; i < N; i++) i16[i] = (int16_t)(i64[i] >> 11);
	for (int i = 0; i < N; i++) u8[i]  = (uint8_t)(u32[i] >> 3);
	for (int i = 0; i < N; i++) u16[i] = (uint16_t)(u64[i] >> 7);
	long long s = 0;
	for (int i = 0; i < N; i++) s += i8[i] + i16[i] + u8[i] + u16[i];
	printf("narrow %lld\n", s);

	/* divide and remainder, both signednesses */
	long long d = 0;
	for (int i = 0; i < N; i++) { int32_t q = i32[i] / 1000 + i32[i] % 7; d += q; }
	for (int i = 0; i < N; i++) { uint32_t q = u32[i] / 1000u + u32[i] % 7u; d += q; }
	printf("div %lld\n", d);

	/* min/max/abs and saturating-shaped code */
	long long m = 0;
	for (int i = 0; i < N; i++) {
		int32_t v = i32[i];
		m += v < 0 ? -(long long)v : v;
		m += v > 1000 ? 1000 : (v < -1000 ? -1000 : v);
	}
	printf("minmax %lld\n", m);

	/* shifts by a vector amount */
	long long sh = 0;
	for (int i = 0; i < N; i++) sh += (int64_t)(i32[i] >> (u8[i] & 31));
	for (int i = 0; i < N; i++) sh += (int64_t)(u32[i] >> (u8[i] & 31));
	for (int i = 0; i < N; i++) sh += (int64_t)(u32[i] << (u8[i] & 31));
	printf("shift %lld\n", sh);

	/* compare-and-select, which becomes masks and vmerge */
	long long sel = 0;
	for (int i = 0; i < N; i++) sel += (i32[i] > 0) ? i16[i] : (int16_t)-i16[i];
	printf("select %lld\n", sel);
	return 0;
}
