#include "function.hpp"
#include "ringbuffer.hpp"

template <size_t Capacity = 16>
struct Events {
	using Work = Function<void()>;

	FixedRingBuffer<8, Work> ring;
	bool in_use = false;

	void consume_work();
	bool add(const Work&);
};

template <size_t Capacity>
inline void Events<Capacity>::consume_work()
{
	this->in_use = true;
	while (const auto* wrk = ring.read()) {
		(*wrk)();
	}
	this->in_use = false;
}

template <size_t Capacity>
inline bool Events<Capacity>::add(const Work& work) {
	if (in_use == false) {
		return ring.write(work);
	}
	return false;
}
