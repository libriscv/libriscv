#include <cassert>
#include <cstdio>
#include <cfloat>
#include "floating-point.hpp"

inline bool kinda32(float val, float expectation) {
	return val >= expectation-FLT_EPSILON
		&& val < expectation+FLT_EPSILON;
}
inline bool kinda64(float val, double expectation) {
	return val >= expectation-FLT_EPSILON
		&& val < expectation+FLT_EPSILON;
}

int main()
{
	assert(test_fadd(1.0f, 1.0f) == 2.0f);
	assert(test_fadd(2.0f, 2.0f) == 4.0f);

	assert(test_fsub(2.0f, 1.0f) == 1.0f);
	assert(test_fsub(4.0f, 2.0f) == 2.0f);

	assert(test_fmul(2.0f, 2.0f) == 4.0f);
	assert(test_fmul(4.0f, 4.0f) == 16.0f);
	assert(test_fmul(2.5f, 2.5f) == (2.5f * 2.5f));

	assert(test_fdiv(16.0f, 4.0f) == 4.0f);
	assert(test_fdiv(4.0f, 2.0f) == 2.0f);
	assert(test_fdiv(4.0f, 3.0f) == (4.0f / 3.0f));

	assert(test_fmax(4.0f, 3.0f) == 4.0f);
	assert(test_fmax(0.999f, 0.998f) == 0.999f);

	assert(test_fmin(4.0f, 3.0f) == 3.0f);
	assert(test_fmin(0.999f, 0.998f) == 0.998f);

	assert(test_ftod(4.0f) == 4.0);
	assert(test_ftod(1.5f) == 1.5);
	assert(kinda64(test_ftod(0.999f), 0.999));

	assert(test_dtof(4.0) == 4.0f);
	assert(test_dtof(1.5) == 1.5f);
	assert(kinda32(test_dtof(0.999), 0.999f));

	assert(test_fmadd(4.0f, 4.0f, 16.0f) == 32.0f);
	assert(test_fmsub(4.0f, 4.0f, 16.0f) == 0.0f);
	assert(test_fnmadd(4.0f, 4.0f, 0.0f) == -16.0f);
	assert(test_fnmsub(4.0f, 4.0f, -16.0f) == 0.0f);

	assert(test_fsqrt(4.0f) == 2.0f);

	assert(test_sinf(0.0f) == 0.0f);
	assert(test_cosf(0.0f) == 1.0f);
	assert(test_tanf(0.0f) == 0.0f);
}
