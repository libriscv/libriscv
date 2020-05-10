#include "function.hpp"
#include "ringbuffer.hpp"

struct Events {
	using Work = Function<void()>;

	FixedRingBuffer<8, Work> ring;
	bool in_use = false;

	void handle();
	bool delegate(const Work&);
};

inline void Events::handle()
{
	this->in_use = true;
	while (const auto* wrk = ring.read()) {
		(*wrk)();
	}
	this->in_use = false;
}
inline bool Events::delegate(const Work& work) {
	if (in_use == false) {
		return ring.write(work);
	}
	return false;
}

extern void halt();
