/* The library string and memory routines, which is where the strided,
 * fault-only-first and whole-register forms actually show up. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char big[65536], other[65536];

static int cmpi(const void *x, const void *y)
{
	int a = *(const int*)x, b = *(const int*)y;
	return (a > b) - (a < b);
}

int main(void)
{
	for (int i = 0; i < 65536; i++) big[i] = (char)((i * 31 + 7) % 251 + 1);
	memcpy(other, big, sizeof(big));
	printf("memcmp %d\n", memcmp(big, other, sizeof(big)));
	other[40000] = 0;
	printf("memcmp2 %d\n", memcmp(big, other, sizeof(big)) != 0);
	memset(other, 0x5A, 30000);
	unsigned long s = 0;
	for (int i = 0; i < 65536; i++) s += (unsigned char)other[i];
	printf("memset %lu\n", s);

	/* strlen / strcpy / strcmp over many alignments and lengths */
	char buf[4096];
	unsigned long lens = 0, cmps = 0;
	for (int off = 0; off < 64; off++) {
		for (int len = 0; len < 300; len += 7) {
			char *p = buf + off;
			for (int i = 0; i < len; i++) p[i] = (char)('a' + (i % 26));
			p[len] = 0;
			lens += strlen(p);
			char tmp[512];
			strcpy(tmp, p);
			cmps += (strcmp(tmp, p) == 0);
			cmps += (unsigned long)(strchr(p, 'z') != NULL);
			cmps += (unsigned long)(memchr(p, 'q', len) != NULL);
		}
	}
	printf("str %lu %lu\n", lens, cmps);

	/* strided access: every 3rd element of a struct-of-arrays walk */
	static int grid[3 * 5000];
	for (int i = 0; i < 3 * 5000; i++) grid[i] = i * 7 - 3;
	long long a = 0, b = 0, c = 0;
	for (int i = 0; i < 5000; i++) { a += grid[3*i]; b += grid[3*i+1]; c += grid[3*i+2]; }
	printf("stride %lld %lld %lld\n", a, b, c);

	/* gather-shaped indirect access */
	static int idx[5000], src[5000];
	for (int i = 0; i < 5000; i++) { src[i] = i * 13; idx[i] = (i * 2503) % 5000; }
	long long g = 0;
	for (int i = 0; i < 5000; i++) g += src[idx[i]];
	printf("gather %lld\n", g);

	/* qsort/bsearch as a broad correctness workout */
	int *arr = malloc(20000 * sizeof(int));
	for (int i = 0; i < 20000; i++) arr[i] = (i * 7919) % 100003;
	qsort(arr, 20000, sizeof(int), cmpi);
	long long chk = 0;
	for (int i = 0; i < 20000; i++) chk += (long long)arr[i] * (i % 13);
	printf("qsort %d %d %lld\n", arr[0], arr[19999], chk);
	free(arr);
	return 0;
}
