#include "syscalls.hpp"
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
using namespace riscv;
static constexpr uint32_t G_SHMEM_BASE = 0x70000000;
static const uint32_t sbrk_start = 0x40000000;
static const uint32_t sbrk_max   = sbrk_start + 0x1000000;
static const uint32_t heap_start = sbrk_max;
static const uint32_t heap_max   = 0xF0000000;

struct iovec32 {
    uint32_t iov_base;
    int32_t  iov_len;
};

template <int W>
long State<W>::syscall_exit(Machine<W>& machine)
{
	this->exit_code = machine.template sysarg<int> (0);
	machine.stop();
	return this->exit_code;
}

template <int W>
long State<W>::syscall_write(Machine<W>& machine)
{
	const int  fd      = machine.template sysarg<int>(0);
	const auto address = machine.template sysarg<address_type<W>>(1);
	const size_t len   = machine.template sysarg<address_type<W>>(2);
	SYSPRINT("SYSCALL write: addr = 0x%X, len = %zu\n", address, len);
	// we only accept standard pipes, for now :)
	if (fd >= 0 && fd < 3) {
		char buffer[1024];
		const size_t len_g = std::min(sizeof(buffer), len);
		machine.memory.memcpy_out(buffer, address, len_g);
		output += std::string(buffer, len_g);
#ifdef RISCV_DEBUG
		write(fd, buffer, len_g);
#endif
		return len_g;
	}
	return -EBADF;
}

template <int W>
long State<W>::syscall_writev(Machine<W>& machine)
{
	const int  fd     = machine.template sysarg<int>(0);
	const auto iov_g  = machine.template sysarg<uint32_t>(1);
	const auto count  = machine.template sysarg<int>(2);
	if constexpr (false) {
		printf("SYSCALL writev called, iov = %#X  cnt = %d\n", iov_g, count);
	}
	if (count < 0 || count > 256) return -EINVAL;
	// we only accept standard pipes, for now :)
	if (fd >= 0 && fd < 3) {
        const size_t size = sizeof(iovec32) * count;

        std::vector<iovec32> vec(count);
        machine.memory.memcpy_out(vec.data(), iov_g, size);

        int res = 0;
        for (const auto& iov : vec)
        {
			char buffer[1024];
            auto src_g = (uint32_t) iov.iov_base;
            auto len_g = std::min(sizeof(buffer), (size_t) iov.iov_len);
            machine.memory.memcpy_out(buffer, src_g, len_g);
			output += std::string(buffer, len_g);
#ifdef RISCV_DEBUG
			write(fd, buffer, len_g);
#endif
			res += len_g;
        }
        return res;
	}
	return -EBADF;
}

template <int W>
long syscall_stub_zero(Machine<W>&) {
	return 0;
}

template <int W>
long syscall_close(riscv::Machine<W>& machine)
{
	const int fd = machine.template sysarg<int>(0);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL close called, fd = %d\n", fd);
	}
	if (fd <= 2) {
		return 0;
	}
	printf(">>> Close %d\n", fd);
	return -1;
}

template <int W>
long syscall_ebreak(riscv::Machine<W>& machine)
{
	printf("\n>>> EBREAK at %#X\n", machine.cpu.pc());
#ifdef RISCV_DEBUG
	machine.print_and_pause();
#else
	throw std::runtime_error("Unhandled EBREAK instruction");
#endif
	return 0;
}

template <int W>
long syscall_openat(Machine<W>& machine)
{
	const int fd = machine.template sysarg<int>(0);
	SYSPRINT("SYSCALL openat called, fd = %d  \n", fd);
    return -EBADF;
}

template <int W>
long syscall_readlinkat(Machine<W>& machine)
{
	const int fd = machine.template sysarg<int>(0);
	SYSPRINT("SYSCALL readlinkat called, fd = %d  \n", fd);
    return -EBADF;
}

template <int W>
long syscall_brk(Machine<W>& machine)
{
	static uint32_t sbrk_end = sbrk_start;
	const uint32_t new_end = machine.template sysarg<uint32_t>(0);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL brk called, current = 0x%X new = 0x%X\n", sbrk_end, new_end);
	}
    if (new_end == 0) return sbrk_end;
    sbrk_end = new_end;
    sbrk_end = std::max(sbrk_end, sbrk_start);
    sbrk_end = std::min(sbrk_end, sbrk_max);

	if constexpr (verbose_syscalls) {
		printf("* New sbrk() end: 0x%X\n", sbrk_end);
	}
	return sbrk_end;
}

template <int W>
long syscall_stat(Machine<W>& machine)
{
	const auto  fd      = machine.template sysarg<int>(0);
	const auto  buffer  = machine.template sysarg<uint32_t>(1);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL stat called, fd = %d  buffer = 0x%X\n",
				fd, buffer);
	}
	if (false) {
		struct stat result;
		std::memset(&result, 0, sizeof(result));
		result.st_dev     = 6;
		result.st_ino     = fd;
		result.st_mode    = 0x21b6;
		result.st_nlink   = 1;
		result.st_rdev    = 265;
		result.st_blksize = 512;
		result.st_blocks  = 0;
		machine.copy_to_guest(buffer, &result, sizeof(result));
		return 0;
	}
	return -EBADF;
}

template <int W>
long syscall_uname(Machine<W>& machine)
{
	const auto buffer = machine.template sysarg<uint32_t>(0);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL uname called, buffer = 0x%X\n", buffer);
	}
    static constexpr int UTSLEN = 65;
    struct uts32 {
        char sysname [UTSLEN];
        char nodename[UTSLEN];
        char release [UTSLEN];
        char version [UTSLEN];
        char machine [UTSLEN];
        char domain  [UTSLEN];
    } uts;
    strcpy(uts.sysname, "RISC-V C++ Emulator");
    strcpy(uts.nodename,"libriscv");
    strcpy(uts.release, "5.0.0");
    strcpy(uts.version, "");
    strcpy(uts.machine, "rv32imac");
    strcpy(uts.domain,  "(none)");

    machine.copy_to_guest(buffer, &uts, sizeof(uts32));
	return 0;
}

template <int W>
inline void add_mman_syscalls(Machine<W>& machine)
{
	// munmap
	machine.install_syscall_handler(215,
	[] (Machine<W>& machine) {
		const uint32_t addr = machine.template sysarg<uint32_t> (0);
		const uint32_t len  = machine.template sysarg<uint32_t> (1);
		SYSPRINT(">>> munmap(0x%X, len=%u)\n", addr, len);
		// TODO: deallocate pages completely
		machine.memory.set_page_attr(addr, len, {
			.read  = false,
			.write = false,
			.exec  = false
		});
		return 0;
	});
	// mmap
	machine.install_syscall_handler(222,
	[] (Machine<W>& machine) {
		const int  addr_g = machine.template sysarg<uint32_t>(0);
		const auto length = machine.template sysarg<uint32_t>(1);
		const auto prot   = machine.template sysarg<int>(2);
	    const auto flags  = machine.template sysarg<int>(3);
		SYSPRINT("SYSCALL mmap called, addr %#X  len %u prot %#x flags %#X\n",
	            addr_g, length, prot, flags);
	    if (addr_g == 0 && (length % Page::size()) == 0)
	    {
	        static uint32_t nextfree = heap_start;
	        const uint32_t addr = nextfree;
	        //auto& page = machine.memory.create_page(addr);
	        //page.attr.read = prot & PROT_READ;
	        nextfree += length;
	        return addr;
	    }
		return UINT32_MAX; // = MAP_FAILED;
	});
	// mprotect
	machine.install_syscall_handler(226,
	[] (Machine<W>& machine) {
		const uint32_t addr = machine.template sysarg<uint32_t> (0);
		const uint32_t len  = machine.template sysarg<uint32_t> (1);
		const int      prot = machine.template sysarg<int> (2);
		SYSPRINT(">>> mprotect(0x%X, len=%u, prot=%x)\n", addr, len, prot);
		machine.memory.set_page_attr(addr, len, {
			.read  = bool(prot & 1),
			.write = bool(prot & 2),
			.exec  = bool(prot & 4)
		});
		return 0;
	});
	// madvise
	machine.install_syscall_handler(233,
	[] (Machine<W>& machine) {
		const uint32_t addr = machine.template sysarg<uint32_t> (0);
		const uint32_t len  = machine.template sysarg<uint32_t> (1);
		const int      advice = machine.template sysarg<int> (2);
		SYSPRINT(">>> madvise(0x%X, len=%u, prot=%x)\n", addr, len, advice);
		switch (advice) {
			case MADV_NORMAL:
			case MADV_RANDOM:
			case MADV_SEQUENTIAL:
			case MADV_WILLNEED:
				return 0;
			case MADV_DONTNEED:
				machine.memory.free_pages(addr, len);
				return 0;
			case MADV_REMOVE:
			case MADV_FREE:
				machine.memory.free_pages(addr, len);
				return 0;
			default:
				return -EINVAL;
		}
	});
}

template <int W>
inline void setup_minimal_syscalls(State<W>& state, Machine<W>& machine)
{
	machine.install_syscall_handler(EBREAK_SYSCALL, syscall_ebreak<W>);
	machine.install_syscall_handler(64, {&state, &State<W>::syscall_write});
	machine.install_syscall_handler(93, {&state, &State<W>::syscall_exit});
}

template <int W>
inline void setup_newlib_syscalls(State<W>& state, Machine<W>& machine)
{
	setup_minimal_syscalls<W>(state, machine);
	machine.install_syscall_handler(214, syscall_brk<W>);
	add_mman_syscalls(machine);
}

template <int W>
void setup_linux_syscalls(State<W>& state, Machine<W>& machine)
{
	setup_minimal_syscalls<W>(state, machine);

	// fcntl
	machine.install_syscall_handler(25, syscall_stub_zero<W>);
	// ioctl
	machine.install_syscall_handler(29, syscall_stub_zero<W>);
	// rt_sigprocmask
	machine.install_syscall_handler(135, syscall_stub_zero<W>);
	// getpid
	machine.install_syscall_handler(172, syscall_stub_zero<W>);
	// getuid
	machine.install_syscall_handler(174, syscall_stub_zero<W>);
	// geteuid
	machine.install_syscall_handler(175, syscall_stub_zero<W>);
	// getgid
	machine.install_syscall_handler(176, syscall_stub_zero<W>);
	// getegid
	machine.install_syscall_handler(177, syscall_stub_zero<W>);

	machine.install_syscall_handler(56, syscall_openat<W>);
	machine.install_syscall_handler(57, syscall_close<W>);
	machine.install_syscall_handler(66, {&state, &State<W>::syscall_writev});
	machine.install_syscall_handler(78, syscall_readlinkat<W>);
	machine.install_syscall_handler(80, syscall_stat<W>);

	machine.install_syscall_handler(160, syscall_uname<W>);
	machine.install_syscall_handler(214, syscall_brk<W>);

	add_mman_syscalls(machine);

	// statx
	machine.install_syscall_handler(291,
	[] (Machine<W>& machine) {
		struct statx {
			uint32_t stx_mask;
			uint32_t stx_blksize = 512;
			uint64_t stx_attributes;
			uint32_t stx_nlink = 1;
			uint32_t stx_uid = 0;
			uint32_t stx_gid = 0;
			uint32_t stx_mode = S_IFCHR;
		};
		const int      fd   = machine.template sysarg<int> (0);
		const uint32_t path = machine.template sysarg<uint32_t> (1);
		const int     flags = machine.template sysarg<int> (2);
		const uint32_t buffer = machine.template sysarg<uint32_t> (4);
		SYSPRINT(">>> xstat(fd=%d, path=0x%X, flags=%x, buf=0x%X)\n",
				fd, path, flags, buffer);
		statx s;
		s.stx_mask = flags;
		machine.copy_to_guest(buffer, &s, sizeof(statx));
		return 0;
	});
}

/* le sigh */
template void setup_minimal_syscalls<4>(State<4>&, Machine<4>&);
template void setup_newlib_syscalls<4>(State<4>&, Machine<4>&);
template void setup_linux_syscalls<4>(State<4>&, Machine<4>&);
