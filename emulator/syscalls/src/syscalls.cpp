#include <include/syscall_helpers.hpp>
#include <stdexcept>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
using namespace riscv;
template <int W>
static const address_type<W> sbrk_start = 0x40000000;
template <int W>
static const address_type<W> sbrk_max   = sbrk_start<W> + 0x1000000;
template <int W>
static const address_type<W> heap_start = sbrk_max<W>;

template <int W> struct guest_iovec;

template <> struct guest_iovec<4> {
    uint32_t iov_base;
    int32_t  iov_len;
};
template <> struct guest_iovec<8> {
    uint64_t iov_base;
    int64_t  iov_len;
};

template <int W>
void syscall_exit(Machine<W>& machine)
{
	auto* state = machine.template get_userdata<State<W>> ();
	state->exit_code = machine.template sysarg<int> (0);
	machine.stop();
	machine.set_result(state->exit_code);
}

template <int W>
void syscall_write(Machine<W>& machine)
{
	const int  fd      = machine.template sysarg<int>(0);
	const auto address = machine.template sysarg<address_type<W>>(1);
	const size_t len   = machine.template sysarg<address_type<W>>(2);
	SYSPRINT("SYSCALL write: addr = 0x%X, len = %zu\n", address, len);
	auto* state = machine.template get_userdata<State<W>> ();
	// we only accept standard pipes, for now :)
	if (fd >= 0 && fd < 3) {
		char buffer[1024];
		const size_t len_g = std::min(sizeof(buffer), len);
		machine.memory.memcpy_out(buffer, address, len_g);
		state->output += std::string(buffer, len_g);
#ifdef RISCV_DEBUG
		machine.set_result(write(fd, buffer, len));
#else
		machine.set_result(len_g);
#endif
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
void syscall_writev(Machine<W>& machine)
{
	const int  fd     = machine.template sysarg<int>(0);
	const auto iov_g  = machine.template sysarg<address_type<W>>(1);
	const auto count  = machine.template sysarg<int>(2);
	if constexpr (false) {
		printf("SYSCALL writev called, iov = %#X  cnt = %d\n", iov_g, count);
	}
	if (count < 0 || count > 256) {
		machine.set_result(-EINVAL);
		return;
	}
	// we only accept standard pipes, for now :)
	if (fd >= 0 && fd < 3) {
        const size_t size = sizeof(guest_iovec<W>) * count;

		auto* state = machine.template get_userdata<State<W>> ();
        std::vector<guest_iovec<W>> vec(count);
        machine.memory.memcpy_out(vec.data(), iov_g, size);

        int res = 0;
        for (const auto& iov : vec)
        {
			char buffer[1024];
            auto src_g = (address_type<W>) iov.iov_base;
            auto len_g = std::min(sizeof(buffer), (size_t) iov.iov_len);
            machine.memory.memcpy_out(buffer, src_g, len_g);
			state->output += std::string(buffer, len_g);
#ifdef RISCV_DEBUG
			res += write(fd, buffer, len_g);
#else
			res += len_g;
#endif
        }
		machine.set_result(res);
        return;
	}
	machine.set_result(-EBADF);
}

template <int W>
void syscall_stub_zero(Machine<W>& machine) {
	machine.set_result(0);
}

template <int W>
void syscall_close(riscv::Machine<W>& machine)
{
	const int fd = machine.template sysarg<int>(0);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL close called, fd = %d\n", fd);
	}
	if (fd <= 2) {
		machine.set_result(0);
		return;
	}
	printf(">>> Close %d\n", fd);
	machine.set_result(-1);
}

template <int W>
void syscall_ebreak(riscv::Machine<W>& machine)
{
	printf("\n>>> EBREAK at %#lX\n", (long) machine.cpu.pc());
#ifdef RISCV_DEBUG
	machine.print_and_pause();
#else
	throw std::runtime_error("Unhandled EBREAK instruction");
#endif
}

template <int W>
void syscall_gettimeofday(Machine<W>& machine)
{
	const auto buffer = machine.template sysarg<address_type<W>>(0);
	SYSPRINT("SYSCALL gettimeofday called, buffer = 0x%X\n", buffer);
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	if constexpr (W == 4) {
		int32_t timeval32[2] = { (int) tv.tv_sec, (int) tv.tv_usec };
		machine.copy_to_guest(buffer, timeval32, sizeof(timeval32));
	} else {
		machine.copy_to_guest(buffer, &tv, sizeof(tv));
	}
    machine.set_result(0);
}

template <int W>
void syscall_openat(Machine<W>& machine)
{
	const int fd = machine.template sysarg<int>(0);
	SYSPRINT("SYSCALL openat called, fd = %d  \n", fd);
	(void) fd;
    machine.set_result(-EBADF);
}

template <int W>
void syscall_readlinkat(Machine<W>& machine)
{
	const int fd = machine.template sysarg<int>(0);
	SYSPRINT("SYSCALL readlinkat called, fd = %d  \n", fd);
	(void) fd;
    machine.set_result(-EBADF);
}

template <int W>
void syscall_brk(Machine<W>& machine)
{
	static address_type<W> sbrk_end = sbrk_start<W>;
	const auto new_end = machine.template sysarg<address_type<W>>(0);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL brk called, current = 0x%X new = 0x%X\n", sbrk_end, new_end);
	}
    if (new_end == 0) {
		machine.set_result(sbrk_end);
		return;
	}
    sbrk_end = new_end;
    sbrk_end = std::max(sbrk_end, sbrk_start<W>);
    sbrk_end = std::min(sbrk_end, sbrk_max<W>);

	if constexpr (verbose_syscalls) {
		printf("* New sbrk() end: 0x%X\n", sbrk_end);
	}
	machine.set_result(sbrk_end);
}

template <int W>
void syscall_stat(Machine<W>& machine)
{
	const auto  fd      = machine.template sysarg<int>(0);
	const auto  buffer  = machine.template sysarg<address_type<W>>(1);
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
	}
	machine.set_result(-EBADF);
}

template <int W>
void syscall_uname(Machine<W>& machine)
{
	const auto buffer = machine.template sysarg<address_type<W>>(0);
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
    strcpy(uts.machine, "rv32imafdc");
    strcpy(uts.domain,  "(none)");

    machine.copy_to_guest(buffer, &uts, sizeof(uts32));
	machine.set_result(0);
}

template <int W>
inline void add_mman_syscalls(Machine<W>& machine)
{
	// munmap
	machine.install_syscall_handler(215,
	[] (Machine<W>& machine) {
		const auto addr = machine.template sysarg<address_type<W>> (0);
		const auto len  = machine.template sysarg<address_type<W>> (1);
		SYSPRINT(">>> munmap(0x%X, len=%u)\n", addr, len);
		machine.memory.free_pages(addr, len);
		machine.set_result(0);
	});
	// mmap
	machine.install_syscall_handler(222,
	[] (Machine<W>& machine) {
		const auto addr_g = machine.template sysarg<address_type<W>>(0);
		const auto length = machine.template sysarg<address_type<W>>(1);
		const auto prot   = machine.template sysarg<int>(2);
	    const auto flags  = machine.template sysarg<int>(3);
		SYSPRINT("SYSCALL mmap called, addr %#X  len %u prot %#x flags %#X\n",
	            addr_g, length, prot, flags);
        if (length % Page::size() != 0) {
            machine.set_result(-1); // = MAP_FAILED;
            return;
        }
        static address_type<W> nextfree = heap_start<W>;
	    if (addr_g == 0 || addr_g == nextfree)
	    {
	        const address_type<W> addr = nextfree;
			// anon pages need to be zeroed
			if (flags & MAP_ANONYMOUS) {
				// ... but they are already CoW
				//machine.memory.memset(addr, 0, length);
			}
	        nextfree += length;
			machine.set_result(addr);
	        return;
	    }
		machine.set_result(-1); // = MAP_FAILED;
	});
	// mremap
	machine.install_syscall_handler(163,
	[] (Machine<W>& machine) {
		const auto old_addr = machine.template sysarg<address_type<W>>(0);
		const auto old_size = machine.template sysarg<address_type<W>>(1);
		const auto new_size = machine.template sysarg<address_type<W>>(2);
	    const auto flags    = machine.template sysarg<int>(3);
		SYSPRINT("SYSCALL mremap called, addr %#X  len %u newsize %u flags %#X\n",
	            old_addr, old_size, new_size, flags);
		if (flags & MREMAP_FIXED) {
			// ...
		}
		machine.set_result(-1);
	});
	// mprotect
	machine.install_syscall_handler(226,
	[] (Machine<W>& machine) {
		const auto addr = machine.template sysarg<address_type<W>> (0);
		const auto len  = machine.template sysarg<address_type<W>> (1);
		const int      prot = machine.template sysarg<int> (2);
		SYSPRINT(">>> mprotect(0x%X, len=%u, prot=%x)\n", addr, len, prot);
		machine.memory.set_page_attr(addr, len, {
			.read  = bool(prot & 1),
			.write = bool(prot & 2),
			.exec  = bool(prot & 4)
		});
		machine.set_result(0);
	});
	// madvise
	machine.install_syscall_handler(233,
	[] (Machine<W>& machine) {
		const auto addr  = machine.template sysarg<address_type<W>> (0);
		const auto len   = machine.template sysarg<address_type<W>> (1);
		const int advice = machine.template sysarg<int> (2);
		SYSPRINT(">>> madvise(0x%X, len=%u, prot=%x)\n", addr, len, advice);
		switch (advice) {
			case MADV_NORMAL:
			case MADV_RANDOM:
			case MADV_SEQUENTIAL:
			case MADV_WILLNEED:
				machine.set_result(0);
				return;
			case MADV_DONTNEED:
				machine.memory.free_pages(addr, len);
				machine.set_result(0);
				return;
			case MADV_REMOVE:
			//case MADV_FREE:
				machine.memory.free_pages(addr, len);
				machine.set_result(0);
				return;
			default:
				machine.set_result(-EINVAL);
				return;
		}
	});
}

template <int W>
inline void setup_minimal_syscalls(State<W>& state, Machine<W>& machine)
{
	machine.set_userdata(&state);
	machine.install_syscall_handler(SYSCALL_EBREAK, syscall_ebreak<W>);
	machine.install_syscall_handler(64, syscall_write<W>);
	machine.install_syscall_handler(93, syscall_exit<W>);
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
	// rt_sigprocmask
	machine.install_syscall_handler(169, syscall_gettimeofday<W>);
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
	machine.install_syscall_handler(66, syscall_writev<W>);
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
		const auto     path = machine.template sysarg<address_type<W>> (1);
		const int     flags = machine.template sysarg<int> (2);
		const auto   buffer = machine.template sysarg<address_type<W>> (4);
		SYSPRINT(">>> xstat(fd=%d, path=0x%X, flags=%x, buf=0x%X)\n",
				fd, path, flags, buffer);
		(void) fd;
		statx s;
		s.stx_mask = flags;
		machine.copy_to_guest(buffer, &s, sizeof(statx));
		machine.set_result(0);
	});
}

/* le sigh */
template void setup_minimal_syscalls<4>(State<4>&, Machine<4>&);
template void setup_newlib_syscalls<4>(State<4>&, Machine<4>&);
template void setup_linux_syscalls<4>(State<4>&, Machine<4>&);

template void setup_minimal_syscalls<8>(State<8>&, Machine<8>&);
template void setup_newlib_syscalls<8>(State<8>&, Machine<8>&);
template void setup_linux_syscalls<8>(State<8>&, Machine<8>&);
