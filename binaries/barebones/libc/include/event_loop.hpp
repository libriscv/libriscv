#include <array>
#include "include/ringbuffer.hpp"

struct Events {
	struct Work {
		void (*event) (const void*);
		const void* data;
	};
	FixedRingBuffer<8, Work> ring;
	bool in_use = false;

	void handle();
	bool delegate(const Work&);
};

inline void Events::handle()
{
	this->in_use = true;
	while (const auto* wrk = ring.read()) {
		wrk->event(wrk->data);
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
