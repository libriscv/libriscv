#include "machine.hpp"

template <int W>
inline Machine<W>::Machine(std::vector<uint8_t> binary)
	: cpu(*this), memory(*this, std::move(binary))
{
	cpu.reset();
}

template <int W>
inline bool Machine<W>::stopped() const noexcept {
	return false;
}

template <int W>
inline void Machine<W>::simulate()
{
	cpu.simulate();
}
