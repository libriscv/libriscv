#include <array>
#include <cassert>
#include <cstdio>
#include <cfloat>
#include <cmath>
#include <errno.h>
#include "floating-point.hpp"
static constexpr double PI = M_PI;

inline bool kinda32(float val, float expectation) {
	return val >= expectation-FLT_EPSILON
		&& val < expectation+FLT_EPSILON;
}
inline bool kinda64(float val, double expectation) {
	return val >= expectation-FLT_EPSILON
		&& val < expectation+FLT_EPSILON;
}
#define TEST(a, b) do { \
	if (a != b) { \
		printf("TEST FAILED: %s (%f) != %s (%f) (at %s:%d)\n", #a, (double)a, #b, (double)b, __FILE__, __LINE__); \
		exit(1); \
	} \
} while(0)
#define TEST_GREATER(a, b) do { \
	if (!(a > b)) { \
		printf("TEST FAILED: %s (%f) <= %s (at %s:%d)\n", #a, (double)a, #b, __FILE__, __LINE__); \
		exit(1); \
	} \
} while(0)
#define TEST_LESS(a, b) do { \
	if (!(a < b)) { \
		printf("TEST FAILED: %s (%f) >= %s (at %s:%d)\n", #a, (double)a, #b, __FILE__, __LINE__); \
		exit(1); \
	} \
} while(0)

int main()
{
	fprintf(stderr, "Starting floating-point tests...\n");
	fflush(stderr);

	fprintf(stderr, "Testing basic operations...\n");
	TEST(test_fadd(1.0f, 1.0f), 2.0f);
	TEST(test_fadd(2.0f, 2.0f), 4.0f);

	TEST(test_fsub(2.0f, 1.0f), 1.0f);
	TEST(test_fsub(4.0f, 2.0f), 2.0f);

	TEST(test_fmul(2.0f, 2.0f), 4.0f);
	TEST(test_fmul(4.0f, 4.0f), 16.0f);
	TEST(test_fmul(2.5f, 2.5f), (2.5f * 2.5f));

	TEST(test_fdiv(16.0f, 4.0f), 4.0f);
	TEST(test_fdiv(4.0f, 2.0f), 2.0f);
	TEST(test_fdiv(4.0f, 3.0f), (4.0f / 3.0f));

	TEST(test_fmax(4.0f, 3.0f), 4.0f);
	TEST(test_fmax(0.999f, 0.998f), 0.999f);

	TEST(test_fmin(4.0f, 3.0f), 3.0f);
	TEST(test_fmin(0.999f, 0.998f), 0.998f);

	fprintf(stderr, "Testing float to double conversion...\n");
	TEST(test_ftod(4.0f), 4.0);
	TEST(test_ftod(2.0), 2.0f);
	TEST(test_ftod(1.5f), 1.5);
	TEST(kinda64(test_ftod(0.999f), 0.999), true);

	fprintf(stderr, "Testing double to float conversion...\n");
	TEST(test_dtof(4.0), 4.0f);
	TEST(test_dtof(2.0), 2.0f);
	TEST(test_dtof(1.5), 1.5f);
	TEST(kinda32(test_dtof(0.999), 0.999f), true);

	fprintf(stderr, "Testing negation...\n");
	TEST(test_fneg(1.0f), -1.0f);
	TEST(test_fneg(-1.0f), 1.0f);
	TEST(test_dneg(16.0), -16.0);
	TEST(test_dneg(-16.0), 16.0);

	fprintf(stderr, "Testing fused multiply-add/subtract...\n");
	TEST(test_fmadd(4.0f, 4.0f, 16.0f), 32.0f);
	TEST(test_fmadd(4.0f, 2.0f, 0.0f), 8.0f);
	TEST(test_fmadd(1.0f, 1.0f, 31.0f), 32.0f);
	TEST(test_fmsub(4.0f, 4.0f, 16.0f), 0.0f);
	TEST(test_fnmadd(4.0f, 4.0f, 0.0f), -16.0f);
	TEST(test_fnmsub(4.0f, 4.0f, -16.0f), 0.0f);

	fprintf(stderr, "Testing dot product...\n");
	std::array<float, 8> a = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
	std::array<float, 8> b = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
	TEST(test_dotp(a.data(), b.data(), a.size()), 8.0f);

	fprintf(stderr, "Testing square root...\n");
	TEST(test_fsqrt(4.0f), 2.0f);
	TEST_GREATER(test_fsqrt(2.0f), 1.41f);
	TEST_LESS(test_fsqrt(2.0f), 1.42f);
	TEST(test_fsqrt(1.0f), 1.0f);
	TEST(test_dsqrt(1.0), 1.0);
	TEST(test_dsqrt(4.0), 2.0);

	fprintf(stderr, "Testing power function...\n");
	TEST(test_fpow(2.0f, 2.0f), 4.0f);
	TEST(test_fpow(3.0f, 3.0f), 27.0f);
	TEST(test_dpow(2.0, 2.0), 4.0);
	TEST(kinda64(test_dpow(3.0, 3.0), 27.0), true);

	fprintf(stderr, "Testing trigonometric functions...\n");
	TEST(test_sinf(0.0f), 0.0f);
	TEST(test_cosf(0.0f), 1.0f);
	TEST(test_tanf(0.0f), 0.0f);

	printf("sin(0.0pi) = %f\n", test_sinf(0.0*PI)); // ~0.0
	printf("sin(0.5pi) = %f\n", test_sinf(0.5*PI)); // 1.0
	printf("sin(1.0pi) = %f\n", test_sinf(1.0*PI)); // ~0.0
	printf("sin(1.5pi) = %f\n", test_sinf(1.5*PI)); // -1.0
	printf("sin(2.0pi) = %f\n", test_sinf(2.0*PI)); // ~0.0
	//TEST(kinda32(test_sinf(0.0), test_sinf(2.0*PI)));
	TEST(kinda32(test_sinf(PI), 0.0f), true);
	TEST(test_cosf(PI), -1.0f);
	TEST_LESS(test_tanf(PI), 0.001f);

	// FMOD
	fprintf(stderr, "Testing float modulus...\n");
	TEST(test_fmod(5.0f, 2.0f), 1.0f);
	TEST(test_fmod(-5.0f, 2.0f), -1.0f);
	TEST(test_fmod(5.0f, -2.0f), 1.0f);
	TEST(test_fmod(-5.0f, -2.0f), -1.0f);

	fprintf(stderr, "Testing double modulus...\n");
	TEST(test_dmod(5.0, 2.0), 1.0);
	TEST(test_dmod(-5.0, 2.0), -1.0);
	TEST(test_dmod(5.0, -2.0), 1.0);
	TEST(test_dmod(-5.0, -2.0), -1.0);

	printf("All tests passed.\n");
	return 0;
}
