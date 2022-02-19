#include <libriscv/machine.hpp>

//#define SYSCALL_VERBOSE 1
#ifdef SYSCALL_VERBOSE
#define SYSPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
static constexpr bool verbose_syscalls = true;
#else
#define SYSPRINT(fmt, ...) /* fmt */
static constexpr bool verbose_syscalls = false;
#endif

#include <linux/limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>

namespace riscv {

template <int W>
struct guest_iovec {
	address_type<W> iov_base;
	address_type<W> iov_len;
};

template <int W>
void syscall_stub_zero(Machine<W>& machine) {
	machine.set_result(0);
}

template <int W>
void syscall_stub_nosys(Machine<W>& machine) {
	machine.set_result(-ENOSYS);
}

template <int W>
void syscall_exit(Machine<W>& machine)
{
	// Stop sets the max instruction counter to zero, allowing most
	// instruction loops to end. It is, however, not the only way
	// to exit a program. Tighter integrations with the library should
	// provide their own methods.
	machine.stop();
}

template <int W>
void syscall_lseek(Machine<W>& machine)
{
	const int fd      = machine.template sysarg<int>(0);
	const auto offset = machine.template sysarg<int64_t>(1);
	const int whence  = machine.template sysarg<int>(2);
	SYSPRINT("SYSCALL lseek, fd: %d, offset: 0x%lX, whence: %d\n",
		fd, (long)offset, whence);

	const int real_fd = machine.fds().get(fd);
	int64_t res = lseek(real_fd, offset, whence);
	machine.set_result(res);
}
template <int W>
void syscall_read(Machine<W>& machine)
{
	const int  fd      = machine.template sysarg<int>(0);
	const auto address = machine.template sysarg<address_type<W>>(1);
	const size_t len   = machine.template sysarg<address_type<W>>(2);
	SYSPRINT("SYSCALL read, addr: 0x%lX, len: %zu\n", (long)address, len);
	// We have special stdin handling
	if (fd == 0) {
		// Gather up to 64kb in pages we can read into
		riscv::vBuffer buffers[16];
		size_t cnt =
			machine.memory.gather_buffers_from_range(16, buffers, address, len);
		for (size_t i = 0; i < cnt; i++) {
			machine.stdin(buffers[i].ptr, buffers[i].len);
		}
		machine.set_result(len);
		return;
	} else if (machine.has_file_descriptors()) {
		const int real_fd = machine.fds().get(fd);
		// Gather up to 1MB of pages we can read into
		riscv::vBuffer buffers[256];
		size_t cnt =
			machine.memory.gather_buffers_from_range(256, buffers, address, len);
		// Could probably be a writev call, tbh
		for (size_t i = 0; i < cnt; i++) {
			read(real_fd, buffers[i].ptr, buffers[i].len);
		}
		machine.set_result(len);
		return;
	}
	machine.set_result(-EBADF);
}
template <int W>
void syscall_write(Machine<W>& machine)
{
	const int  fd      = machine.template sysarg<int>(0);
	const auto address = machine.template sysarg<address_type<W>>(1);
	const size_t len   = machine.template sysarg<address_type<W>>(2);
	SYSPRINT("SYSCALL write, addr: 0x%lX, len: %zu\n", (long)address, len);
	// We only accept standard output pipes, for now :)
	if (fd == 1 || fd == 2) {
		// Zero-copy retrieval of buffers (64kb)
		riscv::vBuffer buffers[16];
		size_t cnt =
			machine.memory.gather_buffers_from_range(16, buffers, address, len);
		for (size_t i = 0; i < cnt; i++) {
			machine.print(buffers[i].ptr, buffers[i].len);
		}
		machine.set_result(len);
		return;
	} else if (machine.has_file_descriptors()) {
		int real_fd = machine.fds().get(fd);
		// Zero-copy retrieval of buffers (256kb)
		riscv::vBuffer buffers[64];
		size_t cnt =
			machine.memory.gather_buffers_from_range(64, buffers, address, len);
		// Could probably be a writev call, tbh
		for (size_t i = 0; i < cnt; i++) {
			write(real_fd, buffers[i].ptr, buffers[i].len);
		}
		machine.set_result(len);
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
		printf("SYSCALL writev, iov: %#X  cnt: %d\n", iov_g, count);
	}
	if (count < 0 || count > 256) {
		machine.set_result(-EINVAL);
		return;
	}
	// We only accept standard output pipes, for now :)
	if (fd == 1 || fd == 2) {
		const size_t size = sizeof(guest_iovec<W>) * count;

		std::vector<guest_iovec<W>> vec(count);
		machine.memory.memcpy_out(vec.data(), iov_g, size);

		ssize_t res = 0;
		for (const auto& iov : vec)
		{
			auto src_g = (address_type<W>) iov.iov_base;
			auto len_g = (size_t) iov.iov_len;
			/* Zero-copy retrieval of buffers */
			riscv::vBuffer buffers[4];
			size_t cnt =
				machine.memory.gather_buffers_from_range(4, buffers, src_g, len_g);
			for (size_t i = 0; i < cnt; i++) {
				machine.print(buffers[i].ptr, buffers[i].len);
			}
			res += len_g;
		}
		machine.set_result(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
void syscall_close(riscv::Machine<W>& machine)
{
	const int fd = machine.template sysarg<int>(0);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL close, fd: %d\n", fd);
	}
	if (fd <= 2) {
		// TODO: Do we really want to close them?
		machine.set_result(0);
		return;
	} else if (machine.has_file_descriptors()) {
		machine.set_result(
			machine.fds().close(fd) >= 0 ? 0 : -EBADF);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
void syscall_ebreak(riscv::Machine<W>& machine)
{
	printf("\n>>> EBREAK at %#lX\n", (long) machine.cpu.pc());
#ifdef RISCV_DEBUG
	machine.print_and_pause();
#else
	throw MachineException(UNHANDLED_SYSCALL, "EBREAK instruction");
#endif
}

static inline bool is_exception_signal(int sig) {
	// SIGILL, SIGABRT, SIGFPE, SIGSEGV
	return sig == 4 || sig == 6 || sig == 8 || sig == 11;
}

template <int W>
void syscall_sigaction(Machine<W>& machine)
{
	const int signal = machine.template sysarg<address_type<W>>(0);
	const auto buffer = machine.template sysarg<address_type<W>>(1);
	struct sigaction sa;
	machine.copy_from_guest(&sa, buffer, sizeof(sa));

	if (is_exception_signal(signal)) {
		// There is typically only one relevant handler,
		// and languages use it to print backtraces.
		//printf("Signal %d handler: 0x%lX\n", signal, (uintptr_t)sa.sa_handler);
		machine.set_sighandler((address_type<W>)(uintptr_t)sa.sa_handler);
	}
	machine.set_result(0);
}

template <int W>
void syscall_gettimeofday(Machine<W>& machine)
{
	const auto buffer = machine.template sysarg<address_type<W>>(0);
	SYSPRINT("SYSCALL gettimeofday, buffer: 0x%lX\n", (long)buffer);
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
	const int dir_fd = machine.template sysarg<int>(0);
	const auto g_path = machine.template sysarg<address_type<W>>(1);
	const int flags  = machine.template sysarg<int>(2);
	char path[PATH_MAX];
	machine.copy_from_guest(path, g_path, sizeof(path)-1);
	path[sizeof(path)-1] = 0;

	SYSPRINT("SYSCALL openat, dir_fd: %d path: %s flags: %X\n",
		dir_fd, path, flags);

	if (machine.has_file_descriptors()) {
		int real_fd = openat(machine.fds().translate(dir_fd), path, flags);
		const int vfd = machine.fds().assign(real_fd);

		machine.set_result(vfd);
		return;
	}

	machine.set_result(-EBADF);
}

template <int W>
void syscall_readlinkat(Machine<W>& machine)
{
	const int fd = machine.template sysarg<int>(0);
	SYSPRINT("SYSCALL readlinkat, fd: %d\n", fd);
	(void) fd;
	machine.set_result(-ENOSYS);
}

template <int W>
void syscall_brk(Machine<W>& machine)
{
	auto new_end = machine.template sysarg<address_type<W>>(0);
	if (new_end > machine.memory.heap_address() + Memory<W>::BRK_MAX) {
		new_end = machine.memory.heap_address() + Memory<W>::BRK_MAX;
	} else if (new_end < machine.memory.heap_address()) {
		new_end = machine.memory.heap_address();
	}

	if constexpr (verbose_syscalls) {
		printf("SYSCALL brk, new_end: 0x%lX\n", (long)new_end);
	}
	machine.set_result(new_end);
}

template <int W>
void syscall_stat(Machine<W>& machine)
{
	const auto  fd      = machine.template sysarg<int>(0);
	const auto  buffer  = machine.template sysarg<address_type<W>>(1);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL stat, fd: %d  buffer: 0x%lX\n",
				fd, (long)buffer);
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
		printf("SYSCALL uname, buffer: 0x%lX\n", (long)buffer);
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
		SYSPRINT(">>> munmap(0x%lX, len=%zu)\n", (long)addr, (size_t)len);
		machine.memory.free_pages(addr, len);
		auto& nextfree = machine.memory.mmap_address();
		if (addr + len == nextfree) {
			nextfree = addr;
			if (nextfree < machine.memory.heap_address() + Memory<W>::BRK_MAX)
				nextfree = machine.memory.heap_address() + Memory<W>::BRK_MAX;
		}
		machine.set_result(0);
	});
	// mmap
	machine.install_syscall_handler(222,
	[] (Machine<W>& machine) {
		const auto addr_g = machine.template sysarg<address_type<W>>(0);
		const auto length = machine.template sysarg<address_type<W>>(1);
		const auto prot   = machine.template sysarg<int>(2);
		const auto flags  = machine.template sysarg<int>(3);
		SYSPRINT(">>> mmap(addr 0x%lX, len %zu, prot %#x, flags %#X)\n",
				(long)addr_g, (size_t)length, prot, flags);
		if (addr_g % Page::size() != 0 || length % Page::size() != 0) {
			machine.set_result(-1); // = MAP_FAILED;
			return;
		}
		auto& nextfree = machine.memory.mmap_address();
		if (addr_g == 0 || addr_g == nextfree)
		{
			// anon pages need to be zeroed
			if (flags & MAP_ANONYMOUS) {
				// ... but they are already CoW
				//machine.memory.memset(nextfree, 0, length);
			}
			machine.set_result(nextfree);
			nextfree += length;
			return;
		} else if (addr_g < nextfree) {
			printf("Invalid mapping attempted\n");
			machine.set_result(-1); // = MAP_FAILED;
			return;
		} else { // addr_g != 0x0
			address_type<W> addr_end = addr_g + length;
			for (address_type<W> addr = addr_g; addr < addr_end; addr += Page::size())
			{
				// do nothing?
			}
			machine.set_result(addr_g);
			return;
		}
		(void) flags;
		(void) prot;
		machine.set_result(-1); // = MAP_FAILED;
	});
	// mremap
	machine.install_syscall_handler(163,
	[] (Machine<W>& machine) {
		const auto old_addr = machine.template sysarg<address_type<W>>(0);
		const auto old_size = machine.template sysarg<address_type<W>>(1);
		const auto new_size = machine.template sysarg<address_type<W>>(2);
		const auto flags    = machine.template sysarg<int>(3);
		SYSPRINT(">>> mremap(addr 0x%lX, len %zu, newsize %zu, flags %#X)\n",
				(long)old_addr, (size_t)old_size, (size_t)new_size, flags);
		auto& nextfree = machine.memory.mmap_address();
		// We allow the common case of reallocating the
		// last mapping to a bigger one
		if (old_addr + old_size == nextfree) {
			nextfree = old_addr + new_size;
			machine.set_result(old_addr);
			return;
		}
		(void) flags;
		machine.set_result(-1);
	});
	// mprotect
	machine.install_syscall_handler(226,
	[] (Machine<W>& machine) {
		const auto addr = machine.template sysarg<address_type<W>> (0);
		const auto len  = machine.template sysarg<address_type<W>> (1);
		const int  prot = machine.template sysarg<int> (2);
		SYSPRINT(">>> mprotect(0x%lX, len=%zu, prot=%x)\n",
			(long)addr, (size_t)len, prot);
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
		SYSPRINT(">>> madvise(0x%lX, len=%zu, prot=%x)\n",
			(uint64_t)addr, (size_t)len, advice);
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
void Machine<W>::setup_minimal_syscalls()
{
	this->install_syscall_handler(SYSCALL_EBREAK, syscall_ebreak<W>);
	this->install_syscall_handler(62, syscall_lseek<W>);
	this->install_syscall_handler(63, syscall_read<W>);
	this->install_syscall_handler(64, syscall_write<W>);
	this->install_syscall_handler(93, syscall_exit<W>);
}

template <int W>
void Machine<W>::setup_newlib_syscalls()
{
	this->setup_minimal_syscalls();
	this->install_syscall_handler(214, syscall_brk<W>);
	add_mman_syscalls(*this);
}

template <int W>
void Machine<W>::setup_linux_syscalls(bool filesystem, bool sockets)
{
	this->setup_minimal_syscalls();

	// fcntl
	this->install_syscall_handler(25, syscall_stub_zero<W>);
	// ioctl
	this->install_syscall_handler(29, syscall_stub_zero<W>);
	// faccessat
	this->install_syscall_handler(48, syscall_stub_nosys<W>);

	// rt_sigaction
	this->install_syscall_handler(134, syscall_sigaction<W>);
	// rt_sigprocmask
	this->install_syscall_handler(135, syscall_stub_zero<W>);

	// gettimeofday
	this->install_syscall_handler(169, syscall_gettimeofday<W>);
	// getpid
	this->install_syscall_handler(172, syscall_stub_zero<W>);
	// getuid
	this->install_syscall_handler(174, syscall_stub_zero<W>);
	// geteuid
	this->install_syscall_handler(175, syscall_stub_zero<W>);
	// getgid
	this->install_syscall_handler(176, syscall_stub_zero<W>);
	// getegid
	this->install_syscall_handler(177, syscall_stub_zero<W>);

	this->install_syscall_handler(56, syscall_openat<W>);
	this->install_syscall_handler(57, syscall_close<W>);
	this->install_syscall_handler(66, syscall_writev<W>);
	this->install_syscall_handler(78, syscall_readlinkat<W>);
	this->install_syscall_handler(80, syscall_stat<W>);

	this->install_syscall_handler(160, syscall_uname<W>);
	this->install_syscall_handler(214, syscall_brk<W>);

	add_mman_syscalls(*this);

	if (filesystem || sockets)
		m_fds.reset(new FileDescriptors);

	// statx
	this->install_syscall_handler(291,
	[] (Machine<W>& machine) {
		struct statx {
			uint32_t stx_mask;
			uint32_t stx_blksize = 512;
			uint64_t stx_attributes;
			uint32_t stx_nlink = 1;
			uint32_t stx_uid = 0;
			uint32_t stx_gid = 0;
			uint16_t stx_mode = S_IFCHR;
			uint64_t stx_size = 0;
			uint64_t stx_blocks = 0;
		};
		const int      fd   = machine.template sysarg<int> (0);
		const auto     path = machine.template sysarg<address_type<W>> (1);
		const int     flags = machine.template sysarg<int> (2);
		const auto   buffer = machine.template sysarg<address_type<W>> (4);
		SYSPRINT(">>> xstat(fd=%d, path=0x%lX, flags=%x, buf=0x%lX)\n",
				fd, (long)path, flags, (long)buffer);
		(void) fd;
		(void) path;
		statx s;
		s.stx_mask = flags;
		machine.copy_to_guest(buffer, &s, sizeof(statx));
		machine.set_result(0);
	});
}

template void Machine<4>::setup_minimal_syscalls();
template void Machine<4>::setup_newlib_syscalls();
template void Machine<4>::setup_linux_syscalls(bool, bool);

template void Machine<8>::setup_minimal_syscalls();
template void Machine<8>::setup_newlib_syscalls();
template void Machine<8>::setup_linux_syscalls(bool, bool);

} // riscv
