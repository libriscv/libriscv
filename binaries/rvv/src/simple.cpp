#include <array>
#include <cstdio>

union alignas(32) v256 {
	float  f[8];
	double d[4];

	__attribute__((naked))
	inline void add_f32(v256& b)
	{
		asm("vle32.v v2, %1"
			:
			: "r"(this->f), "m"(this->f[0]));
		asm("vle32.v v3, %1"
			:
			: "r"(b.f), "m"(b.f[0]));

		asm("vfmul.vv v2, v2, v3");
		asm("vfadd.vv v1, v1, v2");

		asm("ret");
	}
	__attribute__((naked))
	inline float sum_v1() {
		asm("vfredusum.vs v1, v0, v1");

		asm("vse32.v v1, %1"
			: "=m"(this->f[0])
			: "m"(this->f[0]));

		asm("flw fa0, %0" : : "m"(f[0]) : "fa0");
		asm("ret");
	}
};

int main()
{
	alignas(32) std::array<float, 4096> floats_a;
	alignas(32) std::array<float, 4096> floats_b;
	// Setup arrays
	for (auto& f : floats_a) f = 2.0f;
	for (auto& f : floats_b) f = 2.0f;

	// Perform RVV dot-product
	for (size_t i = 0; i < floats_a.size(); i += 8) {
		v256* a = (v256 *)&floats_a[i];
		v256* b = (v256 *)&floats_b[i];
		a->add_f32(*b);
	}
	// Sum elements
	v256 vsum;
	const float sum = vsum.sum_v1();

	printf("Sum = %.2f\n", sum);
}
