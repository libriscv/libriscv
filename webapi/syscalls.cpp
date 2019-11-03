static const uint32_t sbrk_start = 0x40000000;
static const uint32_t sbrk_max   = sbrk_start + 0x1000000;

template <int W>
struct State
{
	int exit_code = 0;
	uint32_t sbrk_end = sbrk_start;
	std::string output;

	uint32_t syscall_exit(riscv::Machine<W>& machine);
	uint32_t syscall_write(riscv::Machine<W>& machine);
	uint32_t syscall_brk(riscv::Machine<W>& machine);

	uint32_t syscall_dummy(riscv::Machine<W>& machine);
};

template <int W>
uint32_t State<W>::syscall_exit(riscv::Machine<W>& machine)
{
	this->exit_code = machine.template sysarg<int>(0);
	machine.stop();
	return 0;
}

template <int W>
uint32_t State<W>::syscall_write(riscv::Machine<W>& machine)
{
	const int  fd      = machine.template sysarg<int>(0);
	const auto address = machine.template sysarg<uint32_t>(1);
	const auto len     = machine.template sysarg<size_t>(2);
	// we only accept standard pipes, for now :)
	if (fd >= 0 && fd < 3) {
		uint8_t buffer[len];
		machine.memory.memcpy_out(buffer, address, len);
		output.append( (const char*) buffer, len );
	}
	return -1;
}

template <int W>
uint32_t State<W>::syscall_brk(riscv::Machine<W>& machine)
{
	const uint32_t new_end = machine.template sysarg<uint32_t>(0);
    if (new_end == 0) return sbrk_end;
    sbrk_end = new_end;
    sbrk_end = std::max(sbrk_end, sbrk_start);
    sbrk_end = std::min(sbrk_end, sbrk_max);
	return sbrk_end;
}

template <int W>
uint32_t State<W>::syscall_dummy(riscv::Machine<W>& machine)
{
	printf("Unhandled system call: %d\n", machine.template sysarg<int>(7));
	return -1;
}
