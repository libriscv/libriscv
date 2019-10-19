#include "cpu.hpp"

template<int W>
inline void CPU<W>::reset()
{
	m_data = {};
}

template<int W>
inline void CPU<W>::simulate()
{
	this->execute();
}
