/* Floating-point autovectorisation, including the f32/f64 widening forms
 * and the integer/float conversions in both directions. */
#include <stdio.h>
#include <math.h>
#include <stdint.h>

#define N 1777
static float  f[N], g[N], h[N];
static double d[N], e[N], q[N];
static int32_t  ii[N];
static uint32_t uu[N];

int main(void)
{
	for (int i = 0; i < N; i++) {
		f[i] = (float)((i * 37) % 1009) * 0.125f - 60.0f;
		g[i] = (float)((i * 53) % 397) + 1.5f;
		d[i] = (double)((i * 71) % 2003) * 0.0625 - 60.0;
		e[i] = (double)((i * 29) % 503) + 1.25;
		ii[i] = (i * 8191) % 100003 - 50000;
		uu[i] = (uint32_t)((i * 65521u) % 1000003u);
	}

	double s = 0;
	for (int i = 0; i < N; i++) h[i] = f[i] * g[i] + f[i] / g[i] - g[i];
	for (int i = 0; i < N; i++) s += h[i];
	printf("f32 %.6f\n", s);

	for (int i = 0; i < N; i++) q[i] = d[i] * e[i] + d[i] / e[i] - e[i];
	s = 0; for (int i = 0; i < N; i++) s += q[i];
	printf("f64 %.9f\n", s);

	/* sqrt, fabs, min/max */
	s = 0;
	for (int i = 0; i < N; i++) s += sqrtf(fabsf(f[i])) + (f[i] > g[i] ? f[i] : g[i]);
	printf("sqrtf %.6f\n", s);
	s = 0;
	for (int i = 0; i < N; i++) s += sqrt(fabs(d[i])) + (d[i] < e[i] ? d[i] : e[i]);
	printf("sqrtd %.9f\n", s);

	/* f32 -> f64 widening accumulate, and the reverse narrowing */
	s = 0;
	for (int i = 0; i < N; i++) s += (double)f[i] * (double)g[i];
	printf("widen %.9f\n", s);
	for (int i = 0; i < N; i++) h[i] = (float)(d[i] * e[i]);
	s = 0; for (int i = 0; i < N; i++) s += h[i];
	printf("narrow %.6f\n", s);

	/* int <-> float conversions, both signednesses and both widths */
	for (int i = 0; i < N; i++) f[i] = (float)ii[i];
	for (int i = 0; i < N; i++) g[i] = (float)uu[i];
	for (int i = 0; i < N; i++) d[i] = (double)ii[i];
	for (int i = 0; i < N; i++) e[i] = (double)uu[i];
	s = 0; for (int i = 0; i < N; i++) s += f[i] + g[i] + d[i] + e[i];
	printf("i2f %.9f\n", s);

	long long t = 0;
	for (int i = 0; i < N; i++) t += (int32_t)(f[i] * 0.5f);
	for (int i = 0; i < N; i++) t += (uint32_t)(g[i] * 0.25f);
	for (int i = 0; i < N; i++) t += (int64_t)(d[i] * 0.5);
	for (int i = 0; i < N; i++) t += (int32_t)(e[i] * 0.125);
	printf("f2i %lld\n", t);

	/* comparisons producing masks */
	int cnt = 0;
	for (int i = 0; i < N; i++) if (f[i] > g[i]) cnt++;
	for (int i = 0; i < N; i++) if (d[i] <= e[i]) cnt += 2;
	printf("cmp %d\n", cnt);
	return 0;
}
