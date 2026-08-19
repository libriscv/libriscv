/* Interleaved (array-of-structs) access, which is what makes the compiler
 * reach for the segment loads and stores, plus explicit strided walks. */
#include <stdio.h>
#include <stdint.h>

#define N 4001
struct rgb { uint8_t r, g, b; };
struct rgba { uint8_t r, g, b, a; };
struct cplx { float re, im; };
struct pair { int32_t x, y; };

static struct rgb  img[N];
static struct rgba img4[N];
static struct cplx cs[N], cd[N];
static struct pair pr[N];
static uint8_t gray[N];

int main(void)
{
	for (int i = 0; i < N; i++) {
		img[i].r = (uint8_t)(i * 7);
		img[i].g = (uint8_t)(i * 13 + 1);
		img[i].b = (uint8_t)(i * 29 + 2);
		img4[i].r = (uint8_t)(i * 3);
		img4[i].g = (uint8_t)(i * 5);
		img4[i].b = (uint8_t)(i * 11);
		img4[i].a = (uint8_t)(i * 17);
		cs[i].re = (float)((i * 37) % 601) * 0.5f - 100.0f;
		cs[i].im = (float)((i * 41) % 397) * 0.25f - 40.0f;
		pr[i].x = i * 3 - 7;
		pr[i].y = (i * 11) % 1009;
	}

	/* 3-field segment: RGB to greyscale */
	for (int i = 0; i < N; i++)
		gray[i] = (uint8_t)((img[i].r * 77 + img[i].g * 151 + img[i].b * 28) >> 8);
	unsigned long gs = 0;
	for (int i = 0; i < N; i++) gs += gray[i];
	printf("gray %lu\n", gs);

	/* 4-field segment: premultiply and write back */
	for (int i = 0; i < N; i++) {
		unsigned a = img4[i].a;
		img4[i].r = (uint8_t)((img4[i].r * a) >> 8);
		img4[i].g = (uint8_t)((img4[i].g * a) >> 8);
		img4[i].b = (uint8_t)((img4[i].b * a) >> 8);
	}
	unsigned long ps = 0;
	for (int i = 0; i < N; i++) ps += img4[i].r + img4[i].g + img4[i].b + img4[i].a;
	printf("premul %lu\n", ps);

	/* 2-field float segment: complex multiply */
	for (int i = 0; i < N; i++) {
		float re = cs[i].re, im = cs[i].im;
		cd[i].re = re * re - im * im;
		cd[i].im = 2.0f * re * im;
	}
	double cr = 0, ci = 0;
	for (int i = 0; i < N; i++) { cr += cd[i].re; ci += cd[i].im; }
	printf("cplx %.4f %.4f\n", cr, ci);

	/* 2-field integer segment, read and written in place */
	for (int i = 0; i < N; i++) {
		int32_t x = pr[i].x, y = pr[i].y;
		pr[i].x = x + y;
		pr[i].y = x - y;
	}
	long long px = 0, py = 0;
	for (int i = 0; i < N; i++) { px += pr[i].x; py += pr[i].y; }
	printf("pair %lld %lld\n", px, py);

	/* deinterleave into separate planes and back */
	static uint8_t pr_[N], pg_[N], pb_[N];
	for (int i = 0; i < N; i++) { pr_[i] = img[i].r; pg_[i] = img[i].g; pb_[i] = img[i].b; }
	for (int i = 0; i < N; i++) { img[i].r = pb_[i]; img[i].g = pr_[i]; img[i].b = pg_[i]; }
	unsigned long ds = 0;
	for (int i = 0; i < N; i++) ds += img[i].r * 3u + img[i].g * 5u + img[i].b * 7u;
	printf("deint %lu\n", ds);
	return 0;
}
