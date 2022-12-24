#include <libriscv/machine.hpp>
#include <libriscv/threads.hpp>

//#define SYSCALL_VERBOSE 1
#ifdef SYSCALL_VERBOSE
#define SYSPRINT(fmt, ...) \
	{ char syspbuf[1024]; machine.debug_print(syspbuf, \
		snprintf(syspbuf, sizeof(syspbuf), fmt, ##__VA_ARGS__)); }
static constexpr bool verbose_syscalls = true;
#else
#define SYSPRINT(fmt, ...) /* fmt */
static constexpr bool verbose_syscalls = false;
#endif

#include <fcntl.h>
#include <signal.h>
#undef sa_handler
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#define SA_ONSTACK	0x08000000

namespace riscv {
	template <int W>
	void add_socket_syscalls(Machine<W>&);

template <int W>
struct guest_iovec {
	address_type<W> iov_base;
	address_type<W> iov_len;
};

template <int W>
static void syscall_stub_zero(Machine<W>& machine) {
	SYSPRINT("SYSCALL stubbed (zero): %d\n", (int)machine.cpu.reg(17));
	machine.set_result(0);
}

template <int W>
static void syscall_stub_nosys(Machine<W>& machine) {
	SYSPRINT("SYSCALL stubbed (nosys): %d\n", (int)machine.cpu.reg(17));
	machine.set_result(-ENOSYS);
}

template <int W>
static void syscall_exit(Machine<W>& machine)
{
	// Stop sets the max instruction counter to zero, allowing most
	// instruction loops to end. It is, however, not the only way
	// to exit a program. Tighter integrations with the library should
	// provide their own methods.
	machine.stop();
}

template <int W>
static void syscall_ebreak(riscv::Machine<W>& machine)
{
	printf("\n>>> EBREAK at %#lX\n", (long) machine.cpu.pc());
	throw MachineException(UNHANDLED_SYSCALL, "EBREAK instruction");
}

template <int W>
static void syscall_sigaltstack(Machine<W>& machine)
{
	const auto ss = machine.sysarg(0);
	const auto old_ss = machine.sysarg(1);
	SYSPRINT("SYSCALL sigaltstack, tid=%d ss: 0x%lX old_ss: 0x%lX\n",
		machine.gettid(), (long)ss, (long)old_ss);

	auto& stack = machine.signals().per_thread(machine.gettid()).stack;

	if (old_ss != 0x0) {
		machine.copy_to_guest(old_ss, &stack, sizeof(stack));
	}
	if (ss != 0x0) {
		machine.copy_from_guest(&stack, ss, sizeof(stack));

		SYSPRINT("<<< sigaltstack sp: 0x%lX flags: 0x%X size: 0x%lX\n",
			(long)stack.ss_sp, stack.ss_flags, (long)stack.ss_size);
	}

	machine.set_result(0);
}

template <int W>
static void syscall_sigaction(Machine<W>& machine)
{
	const int sig = machine.sysarg(0);
	const auto action = machine.sysarg(1);
	const auto old_action = machine.sysarg(2);
	SYSPRINT("SYSCALL sigaction, signal: %d, action: 0x%lX old_action: 0x%lX\n",
		sig, (long)action, (long)old_action);
	if (sig == 0) return;

	auto& sigact = machine.sigaction(sig);

	struct riscv_sigaction {
		address_type<W> sa_handler;
		unsigned long sa_flags;
	} sa {};
	if (old_action != 0x0) {
		sa.sa_handler = sigact.handler;
		sa.sa_flags   = (sigact.altstack ? SA_ONSTACK : 0x0);
		machine.copy_to_guest(old_action, &sa, sizeof(sa));
	}
	if (action != 0x0) {
		machine.copy_from_guest(&sa, action, sizeof(sa));
		sigact.handler = sa.sa_handler;
		sigact.altstack = (sa.sa_flags & SA_ONSTACK) != 0;
		SYSPRINT("<<< sigaction %d handler: 0x%lX altstack: %d\n",
			sig, (long)sigact.handler, sigact.altstack);
	}

	machine.set_result(0);
}

template <int W>
void syscall_lseek(Machine<W>& machine)
{
	const int fd      = machine.template sysarg<int>(0);
	const auto offset = machine.template sysarg(1);
	const int whence  = machine.template sysarg<int>(2);
	SYSPRINT("SYSCALL lseek, fd: %d, offset: 0x%lX, whence: %d\n",
		fd, (long)offset, whence);

	if (machine.has_file_descriptors()) {
		const int real_fd = machine.fds().get(fd);
		long res = lseek(real_fd, offset, whence);
		machine.set_result_or_error(res);
	} else {
		machine.set_result(-EBADF);
	}
}
template <int W>
static void syscall_read(Machine<W>& machine)
{
	const int  vfd     = machine.template sysarg<int>(0);
	const auto address = machine.sysarg(1);
	const size_t len   = machine.sysarg(2);
	SYSPRINT("SYSCALL read, vfd: %d addr: 0x%lX, len: %zu\n",
		vfd, (long)address, len);
	// We have special stdin handling
	if (vfd == 0) {
		// Arbitrary maximum read length
		if (len > 1024 * 1024 * 16) {
			machine.set_result(-ENOMEM);
			return;
		}
		// TODO: We can use gather buffers here to avoid the copy
		auto buffer = std::unique_ptr<char[]> (new char[len]);
		long result = machine.stdin_read(buffer.get(), len);
		if (result > 0) {
			machine.copy_to_guest(address, buffer.get(), result);
		}
		machine.set_result_or_error(result);
		return;
	} else if (machine.has_file_descriptors()) {
		const int real_fd = machine.fds().translate(vfd);

		// Gather up to 1MB of pages we can read into
		riscv::vBuffer buffers[256];
		size_t cnt =
			machine.memory.gather_buffers_from_range(256, buffers, address, len);
		const ssize_t res =
			readv(real_fd, (const iovec *)&buffers[0], cnt);
		machine.set_result_or_error(res);
		SYSPRINT("SYSCALL read, fd: %d from vfd: %d = %ld\n",
				 real_fd, vfd, (long)machine.return_value());
	} else {
		machine.set_result(-EBADF);
		SYSPRINT("SYSCALL read, vfd: %d = -EBADF\n", vfd);
	}
}
template <int W>
static void syscall_write(Machine<W>& machine)
{
	const int  vfd     = machine.template sysarg<int>(0);
	const auto address = machine.sysarg(1);
	const size_t len   = machine.sysarg(2);
	SYSPRINT("SYSCALL write, fd: %d addr: 0x%lX, len: %zu\n",
		vfd, (long)address, len);
	if (vfd == 1 || vfd == 2) {
		// Zero-copy retrieval of buffers (64kb)
		riscv::vBuffer buffers[16];
		size_t cnt =
			machine.memory.gather_buffers_from_range(16, buffers, address, len);
		for (size_t i = 0; i < cnt; i++) {
			machine.print(buffers[i].ptr, buffers[i].len);
		}
		machine.set_result(len);
	} else if (machine.has_file_descriptors() && machine.fds().permit_write(vfd)) {
		int real_fd = machine.fds().translate(vfd);
		// Zero-copy retrieval of buffers (256kb)
		riscv::vBuffer buffers[64];
		size_t cnt =
			machine.memory.gather_buffers_from_range(64, buffers, address, len);
		const ssize_t res =
			writev(real_fd, (struct iovec *)&buffers[0], cnt);
		machine.set_result_or_error(res);
	} else {
		machine.set_result(-EBADF);
	}
}

template <int W>
static void syscall_readv(Machine<W>& machine)
{
	const int  vfd    = machine.template sysarg<int>(0);
	const auto iov_g  = machine.sysarg(1);
	const auto count  = machine.template sysarg<int>(2);
	if (count < 1 || count > 128) {
		machine.set_result(-EINVAL);
		return;
	}

	int real_fd = -1;
	if (vfd == 1 || vfd == 2) {
		real_fd = -1;
	} else if (machine.has_file_descriptors()) {
		real_fd = machine.fds().translate(vfd);
	}

	if (real_fd < 0) {
		machine.set_result(-EBADF);
	} else {
		const size_t iov_size = sizeof(guest_iovec<W>) * count;

		// Retrieve the guest IO vec
		std::array<guest_iovec<W>, 128> g_vec;
		machine.copy_from_guest(g_vec.data(), iov_g, iov_size);

		// Convert each iovec buffer to host buffers
		std::array<struct iovec, 256> vec;
		size_t vec_cnt = 0;
		riscv::vBuffer buffer[64];

		for (int i = 0; i < count; i++) {
			// The host buffers come directly from guest memory
			const size_t cnt = machine.memory.gather_buffers_from_range(
				64, buffer, g_vec[i].iov_base, g_vec[i].iov_len);
			for (size_t b = 0; b < cnt; b++) {
				vec.at(vec_cnt++) = {
					.iov_base = buffer[b].ptr,
					.iov_len  = buffer[b].len
				};
			}
		}

		const ssize_t res = readv(real_fd, vec.data(), vec_cnt);
		machine.set_result_or_error(res);
	}
	if constexpr (verbose_syscalls) {
		printf("SYSCALL readv(vfd: %d iov: 0x%lX cnt: %d) = %ld\n",
			vfd, (long)iov_g, count, (long)machine.return_value());
	}
} // readv

template <int W>
static void syscall_writev(Machine<W>& machine)
{
	const int  vfd    = machine.template sysarg<int>(0);
	const auto iov_g  = machine.sysarg(1);
	const auto count  = machine.template sysarg<int>(2);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL writev, iov: 0x%lX  cnt: %d\n", (long)iov_g, count);
	}
	if (count < 0 || count > 256) {
		machine.set_result(-EINVAL);
		return;
	}

	int real_fd = -1;
	if (vfd == 1 || vfd == 2) {
		real_fd = vfd;
	} else if (machine.has_file_descriptors()) {
		real_fd = machine.fds().translate(vfd);
	}

	if (real_fd < 0) {
		machine.set_result(-EBADF);
	} else {
		const size_t size = sizeof(guest_iovec<W>) * count;

		std::array<guest_iovec<W>, 256> vec;
		machine.memory.memcpy_out(vec.data(), iov_g, size);

		ssize_t res = 0;
		for (int i = 0; i < count; i++)
		{
			auto& iov = vec.at(i);
			auto src_g = (address_type<W>) iov.iov_base;
			auto len_g = (size_t) iov.iov_len;
			/* Zero-copy retrieval of buffers */
			riscv::vBuffer buffers[16];
			size_t cnt =
				machine.memory.gather_buffers_from_range(16, buffers, src_g, len_g);

			if (real_fd == 1 || real_fd == 2) {
				// STDOUT, STDERR
				for (size_t i = 0; i < cnt; i++) {
					machine.print(buffers[i].ptr, buffers[i].len);
				}
				res += len_g;
			} else {
				// General file descriptor
				ssize_t written =
					writev(real_fd, (const struct iovec *)buffers, cnt);
				if (written > 0) {
					res += written;
				} else if (written < 0) {
					res = written;
					break;
				} else break; // 0 bytes
			}
		}
		machine.set_result_or_error(res);
	}
} // writev

template <int W>
static void syscall_openat(Machine<W>& machine)
{
	const int dir_fd = machine.template sysarg<int>(0);
	const auto g_path = machine.sysarg(1);
	const int flags  = machine.template sysarg<int>(2);
	// We do it this way to prevent accessing memory out of bounds
	const auto path = machine.memory.memstring(g_path);

	SYSPRINT("SYSCALL openat, dir_fd: %d path: %s flags: %X\n",
		dir_fd, path.c_str(), flags);

	if (machine.has_file_descriptors() && machine.fds().permit_filesystem) {

		if (machine.fds().filter_open != nullptr) {
			if (!machine.fds().filter_open(machine.template get_userdata<void>(), path)) {
				machine.set_result(-EPERM);
				return;
			}
		}
		int real_fd = openat(machine.fds().translate(dir_fd), path.c_str(), flags);
		if (real_fd > 0) {
			const int vfd = machine.fds().assign_file(real_fd);
			machine.set_result(vfd);
		} else {
			// Translate errno() into kernel API return value
			machine.set_result(-errno);
		}
		return;
	}

	machine.set_result(-EBADF);
}

template <int W>
static void syscall_close(riscv::Machine<W>& machine)
{
	const int vfd = machine.template sysarg<int>(0);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL close, fd: %d\n", vfd);
	}

	if (vfd >= 0 && vfd <= 2) {
		// TODO: Do we really want to close them?
		machine.set_result(0);
		return;
	} else if (machine.has_file_descriptors()) {
		const int res = machine.fds().erase(vfd);
		if (res > 0) {
			::close(res);
		}
		machine.set_result(res >= 0 ? 0 : -EBADF);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_dup(Machine<W>& machine)
{
	const int vfd = machine.template sysarg<int>(0);
	SYSPRINT("SYSCALL dup, fd: %d\n", vfd);

	if (machine.has_file_descriptors()) {
		int real_fd = machine.fds().translate(vfd);
		int res = dup(real_fd);
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_pipe2(Machine<W>& machine)
{
	// int pipe2(int pipefd[2], int flags);
	const auto vfd_array = machine.template sysarg(0);
	const auto flags = machine.template sysarg<int>(1);

	if (machine.has_file_descriptors()) {
		int pipes[2];
		int res = pipe2(pipes, flags);
		if (res == 0) {
			int vpipes[2];
			vpipes[0] = machine.fds().assign_file(pipes[0]);
			vpipes[1] = machine.fds().assign_file(pipes[1]);
			machine.copy_to_guest(vfd_array, vpipes, sizeof(vpipes));
			machine.set_result(0);
		} else {
			machine.set_result_or_error(res);
		}
	} else {
		machine.set_result(-EBADF);
	}
	SYSPRINT("SYSCALL pipe2, fd array: 0x%lX flags: %d = %ld\n",
		(long)vfd_array, flags, (long)machine.return_value());
}

template <int W>
static void syscall_fcntl(Machine<W>& machine)
{
	const int vfd = machine.template sysarg<int>(0);
	const auto cmd = machine.template sysarg<int>(1);
	const auto arg1 = machine.sysarg(2);
	const auto arg2 = machine.sysarg(3);
	const auto arg3 = machine.sysarg(4);
	SYSPRINT("SYSCALL fcntl, fd: %d  cmd: 0x%X\n", vfd, cmd);

	if (machine.has_file_descriptors()) {
		int real_fd = machine.fds().translate(vfd);
		int res = fcntl(real_fd, cmd, arg1, arg2, arg3);
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_ioctl(Machine<W>& machine)
{
	const int vfd = machine.template sysarg<int>(0);
	const auto req = machine.template sysarg<uint64_t>(1);
	const auto arg1 = machine.sysarg(2);
	const auto arg2 = machine.sysarg(3);
	const auto arg3 = machine.sysarg(4);
	const auto arg4 = machine.sysarg(5);
	SYSPRINT("SYSCALL ioctl, fd: %d  req: 0x%lX\n", vfd, req);

	if (machine.has_file_descriptors()) {
		if (machine.fds().filter_ioctl != nullptr) {
			if (!machine.fds().filter_ioctl(machine.template get_userdata<void>(), req)) {
				machine.set_result(-EPERM);
				return;
			}
		}

		int real_fd = machine.fds().translate(vfd);
		int res = ioctl(real_fd, req, arg1, arg2, arg3, arg4);
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
void syscall_readlinkat(Machine<W>& machine)
{
	const int vfd = machine.template sysarg<int>(0);
	const auto g_path = machine.sysarg(1);
	const auto g_buf = machine.sysarg(2);
	const auto bufsize = machine.sysarg(3);

	const auto path = machine.memory.memstring(g_path);

	SYSPRINT("SYSCALL readlinkat, fd: %d path: %s buffer: 0x%lX size: %zu\n",
		vfd, path.c_str(), (long)g_buf, (size_t)bufsize);

	char buffer[16384];
	if (bufsize > sizeof(buffer)) {
		machine.set_result(-ENOMEM);
		return;
	}

	if (machine.has_file_descriptors()) {

		if (machine.fds().filter_open != nullptr) {
			if (!machine.fds().filter_open(machine.template get_userdata<void>(), path)) {
				machine.set_result(-EPERM);
				return;
			}
		}
		const int real_fd = machine.fds().translate(vfd);

		const int res = readlinkat(real_fd, path.c_str(), buffer, bufsize);
		if (res > 0) {
			// TODO: Only necessary if g_buf is not sequential.
			machine.copy_to_guest(g_buf, buffer, res);
		}

		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-ENOSYS);
}

// The RISC-V stat structure is different from x86
struct riscv_stat {
	uint64_t st_dev;		/* Device.  */
	uint64_t st_ino;		/* File serial number.  */
	uint32_t st_mode;	/* File mode.  */
	uint32_t st_nlink;	/* Link count.  */
	uint32_t st_uid;		/* User ID of the file's owner.  */
	uint32_t st_gid;		/* Group ID of the file's group. */
	uint64_t st_rdev;	/* Device number, if device.  */
	uint64_t __pad1;
	int64_t  st_size;	/* Size of file, in bytes.  */
	int32_t  st_blksize;	/* Optimal block size for I/O.  */
	int32_t  __pad2;
	int64_t  st_blocks;	/* Number 512-byte blocks allocated. */
	int64_t  rv_atime;	/* Time of last access.  */
	uint64_t rv_atime_nsec;
	int64_t  rv_mtime;	/* Time of last modification.  */
	uint64_t rv_mtime_nsec;
	int64_t  rv_ctime;	/* Time of last status change.  */
	uint64_t rv_ctime_nsec;
	uint32_t __unused4;
	uint32_t __unused5;
};
inline void copy_stat_buffer(struct stat& st, struct riscv_stat& rst)
{
	rst.st_dev = st.st_dev;
	rst.st_ino = st.st_ino;
	rst.st_mode = st.st_mode;
	rst.st_nlink = st.st_nlink;
	rst.st_uid = st.st_uid;
	rst.st_gid = st.st_gid;
	rst.st_rdev = st.st_rdev;
	rst.st_size = st.st_size;
	rst.st_blksize = st.st_blksize;
	rst.st_blocks = st.st_blocks;
	rst.rv_atime = st.st_atime;
	rst.rv_atime_nsec = st.st_atim.tv_nsec;
	rst.rv_mtime = st.st_mtime;
	rst.rv_mtime_nsec = st.st_mtim.tv_nsec;
	rst.rv_ctime = st.st_ctime;
	rst.rv_ctime_nsec = st.st_ctim.tv_nsec;
}

template <int W>
static void syscall_fstatat(Machine<W>& machine)
{
	const auto vfd = machine.template sysarg<int> (0);
	const auto g_path = machine.sysarg(1);
	const auto g_buf = machine.sysarg(2);
	const auto flags = machine.template sysarg<int> (3);

	const auto path = machine.memory.memstring(g_path);

	SYSPRINT("SYSCALL fstatat, fd: %d path: %s buf: 0x%lX flags: %#x)\n",
			vfd, path.c_str(), (long)g_buf, flags);

	if (machine.has_file_descriptors()) {

		int real_fd = machine.fds().translate(vfd);

		struct stat st;
		const int res = ::fstatat(real_fd, path.c_str(), &st, flags);
		if (res == 0) {
			// Convert to RISC-V structure
			struct riscv_stat rst;
			copy_stat_buffer(st, rst);
			machine.copy_to_guest(g_buf, &rst, sizeof(rst));
		}
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-ENOSYS);
}

template <int W>
static void syscall_faccessat(Machine<W>& machine)
{
	const auto fd = AT_FDCWD;
	const auto g_path = machine.sysarg(1);
	const auto mode   = machine.template sysarg<int>(2);
	const auto flags  = machine.template sysarg<int>(3);

	const auto path = machine.memory.memstring(g_path);

	SYSPRINT("SYSCALL faccessat, fd: %d path: %s)\n",
			fd, path.c_str());

	const int res =
		faccessat(fd, path.c_str(), mode, flags);
	machine.set_result_or_error(res);
}

template <int W>
static void syscall_fstat(Machine<W>& machine)
{
	const auto vfd = machine.template sysarg<int> (0);
	const auto g_buf = machine.sysarg(1);

	SYSPRINT("SYSCALL fstat, fd: %d buf: 0x%lX)\n",
			vfd, (long)g_buf);

	if (machine.has_file_descriptors()) {

		int real_fd = machine.fds().translate(vfd);

		struct stat st;
		int res = ::fstat(real_fd, &st);
		if (res == 0) {
			// Convert to RISC-V structure
			struct riscv_stat rst;
			copy_stat_buffer(st, rst);
			machine.copy_to_guest(g_buf, &rst, sizeof(rst));
		}
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-ENOSYS);
}
template <int W>
static void syscall_statx(Machine<W>& machine)
{
	const int   dir_fd = machine.template sysarg<int> (0);
	const auto  g_path = machine.sysarg(1);
	const int    flags = machine.template sysarg<int> (2);
	const auto    mask = machine.template sysarg<uint32_t> (3);
	const auto  buffer = machine.sysarg(4);

	const auto path = machine.memory.memstring(g_path);

	SYSPRINT("SYSCALL statx, fd: %d path: %s flags: %x buf: 0x%lX)\n",
			dir_fd, path.c_str(), flags, (long)buffer);

	if (machine.has_file_descriptors()) {
		if (machine.fds().filter_stat != nullptr) {
			if (!machine.fds().filter_stat(machine.template get_userdata<void>(), path)) {
				machine.set_result(-EPERM);
				return;
			}
		}

		struct statx st;
		int res = ::statx(dir_fd, path.c_str(), flags, mask, &st);
		if (res == 0) {
			machine.copy_to_guest(buffer, &st, sizeof(struct statx));
		}
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-ENOSYS);
}

template <int W>
static void syscall_gettimeofday(Machine<W>& machine)
{
	const auto buffer = machine.sysarg(0);
	SYSPRINT("SYSCALL gettimeofday, buffer: 0x%lX\n", (long)buffer);
	struct timeval tv;
	const int res = gettimeofday(&tv, nullptr);
	if (res >= 0) {
		machine.copy_to_guest(buffer, &tv, sizeof(tv));
	}
	machine.set_result_or_error(res);
}
template <int W>
static void syscall_clock_gettime(Machine<W>& machine)
{
	const auto clkid = machine.template sysarg<int>(0);
	const auto buffer = machine.sysarg(1);
	SYSPRINT("SYSCALL clock_gettime, clkid: %x buffer: 0x%lX\n",
		clkid, (long)buffer);

	struct timespec ts;
	const int res = clock_gettime(clkid, &ts);
	if (res >= 0) {
		machine.copy_to_guest(buffer, &ts, sizeof(ts));
	}
	machine.set_result_or_error(res);
}
template <int W>
static void syscall_nanosleep(Machine<W>& machine)
{
	const auto g_req = machine.sysarg(0);
	const auto g_rem = machine.sysarg(1);
	SYSPRINT("SYSCALL nanosleep, req: 0x%lX rem: 0x%lX\n",
		(long)g_req, (long)g_rem);

	struct timespec ts_req;
	machine.copy_from_guest(&ts_req, g_req, sizeof(ts_req));

	struct timespec ts_rem;
	if (g_rem)
		machine.copy_from_guest(&ts_rem, g_rem, sizeof(ts_rem));

	const int res = nanosleep(&ts_req, g_rem != 0x0 ? &ts_rem : nullptr);
	if (res >= 0) {
		machine.copy_to_guest(g_req, &ts_req, sizeof(ts_req));
		if (g_rem)
			machine.copy_to_guest(g_rem, &ts_rem, sizeof(ts_rem));
	}
	machine.set_result_or_error(res);
}

template <int W>
static void syscall_uname(Machine<W>& machine)
{
	const auto buffer = machine.sysarg(0);
	SYSPRINT("SYSCALL uname, buffer: 0x%lX\n", (long)buffer);
	static constexpr int UTSLEN = 65;
	struct {
		char sysname [UTSLEN];
		char nodename[UTSLEN];
		char release [UTSLEN];
		char version [UTSLEN];
		char machine [UTSLEN];
		char domain  [UTSLEN];
	} uts;
	strcpy(uts.sysname, "RISC-V C++ Emulator");
	strcpy(uts.nodename,"libriscv");
	strcpy(uts.release, "5.6.0");
	strcpy(uts.version, "");
	if constexpr (W == 4)
		strcpy(uts.machine, "rv32imafdc");
	else if constexpr (W == 8)
		strcpy(uts.machine, "rv64imafdc");
	else
		strcpy(uts.machine, "rv128imafdc");
	strcpy(uts.domain,  "(none)");

	machine.copy_to_guest(buffer, &uts, sizeof(uts));
	machine.set_result(0);
}

template <int W>
static void syscall_brk(Machine<W>& machine)
{
	auto new_end = machine.sysarg(0);
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
static void syscall_getrandom(Machine<W>& machine)
{
	const auto g_addr = machine.sysarg(0);
	const auto g_len  = machine.sysarg(1);

	char buffer[256];
	if (g_len > sizeof(buffer)) {
		machine.set_result(-1);
		return;
	}
	const size_t need = std::min((size_t)g_len, sizeof(buffer));
	const ssize_t result = getrandom(buffer, need, 0);
	if (result > 0) {
		machine.copy_to_guest(g_addr, buffer, result);
	}
	machine.set_result(result);

	if constexpr (verbose_syscalls) {
		printf("SYSCALL getrandom(addr=0x%lX, len=%ld) = %ld\n",
			(long)g_addr, (long)g_len, (long)machine.return_value());
	}
}

template <int W>
static void add_mman_syscalls()
{
	// munmap
	Machine<W>::install_syscall_handler(215,
	[] (Machine<W>& machine) {
		const auto addr = machine.sysarg(0);
		const auto len  = machine.sysarg(1);
		SYSPRINT(">>> munmap(0x%lX, len=%zu)\n", (long)addr, (size_t)len);
		machine.memory.free_pages(addr, len);
		auto& nextfree = machine.memory.mmap_address();
		if (addr + len == nextfree) {
			nextfree = addr;
			if (nextfree < machine.memory.mmap_start())
				nextfree = machine.memory.mmap_start();
		}
		machine.set_result(0);
	});
	// mmap
	Machine<W>::install_syscall_handler(222,
	[] (Machine<W>& machine) {
		const auto addr_g = machine.sysarg(0);
		auto length = machine.sysarg(1);
		const auto prot   = machine.template sysarg<int>(2);
		const auto flags  = machine.template sysarg<int>(3);
		SYSPRINT(">>> mmap(addr 0x%lX, len %zu, prot %#x, flags %#X)\n",
				(long)addr_g, (size_t)length, prot, flags);
		if (addr_g % Page::size() != 0) {
			machine.set_result(-1); // = MAP_FAILED;
			SYSPRINT("<<< mmap(addr 0x%lX, len %zu, ...) = MAP_FAILED\n",
					(long)addr_g, (size_t)length);
			return;
		}
		length = (length + PageMask) & ~address_type<W>(PageMask);
		auto& nextfree = machine.memory.mmap_address();
		if (addr_g == 0 || addr_g == nextfree)
		{
			// anon pages need to be zeroed
			if (flags & MAP_ANONYMOUS) {
				// ... but they are already CoW
				//machine.memory.memset(nextfree, 0, length);
			}
			machine.set_result(nextfree);
			SYSPRINT("<<< mmap(addr 0x%lX, len %zu, ...) = 0x%lX\n",
					(long)addr_g, (size_t)length, (long)nextfree);
			nextfree += length;
			return;
		} else if (addr_g < nextfree) {
			//printf("Invalid mapping attempted\n");
			//machine.set_result(-1); // = MAP_FAILED;
			machine.set_result(addr_g);
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
	Machine<W>::install_syscall_handler(163,
	[] (Machine<W>& machine) {
		const auto old_addr = machine.sysarg(0);
		const auto old_size = machine.sysarg(1);
		const auto new_size = machine.sysarg(2);
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
	Machine<W>::install_syscall_handler(226,
	[] (Machine<W>& machine) {
		const auto addr = machine.sysarg(0);
		const auto len  = machine.sysarg(1);
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
	Machine<W>::install_syscall_handler(233,
	[] (Machine<W>& machine) {
		const auto addr  = machine.sysarg(0);
		const auto len   = machine.sysarg(1);
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

#include "syscalls_select.cpp"
#include "syscalls_poll.cpp"
#include "syscalls_epoll.cpp"

template <int W>
void Machine<W>::setup_minimal_syscalls()
{
	install_syscall_handler(SYSCALL_EBREAK, syscall_ebreak<W>);
	install_syscall_handler(57, syscall_stub_zero<W>); // close
	install_syscall_handler(62, syscall_lseek<W>);
	install_syscall_handler(63, syscall_read<W>);
	install_syscall_handler(64, syscall_write<W>);
	install_syscall_handler(80, syscall_stub_nosys<W>); // fstat
	install_syscall_handler(93, syscall_exit<W>);
}

template <int W>
void Machine<W>::setup_newlib_syscalls()
{
	setup_minimal_syscalls();
	install_syscall_handler(214, syscall_brk<W>);
	add_mman_syscalls<W>();
}

template <int W>
void Machine<W>::setup_linux_syscalls(bool filesystem, bool sockets)
{
	this->setup_minimal_syscalls();

	// epoll_create
	this->install_syscall_handler(20, syscall_epoll_create<W>);
	// epoll_ctl
	this->install_syscall_handler(21, syscall_epoll_ctl<W>);
	// epoll_pwait
	this->install_syscall_handler(22, syscall_epoll_pwait<W>);
	// dup
	this->install_syscall_handler(23, syscall_dup<W>);
	// fcntl
	this->install_syscall_handler(25, syscall_fcntl<W>);
	// ioctl
	this->install_syscall_handler(29, syscall_ioctl<W>);
	// faccessat
	this->install_syscall_handler(48, syscall_faccessat<W>);

	this->install_syscall_handler(56, syscall_openat<W>);
	this->install_syscall_handler(57, syscall_close<W>);
	this->install_syscall_handler(59, syscall_pipe2<W>);
	this->install_syscall_handler(65, syscall_readv<W>);
	this->install_syscall_handler(66, syscall_writev<W>);
	this->install_syscall_handler(72, syscall_pselect<W>);
	this->install_syscall_handler(73, syscall_ppoll<W>);
	this->install_syscall_handler(78, syscall_readlinkat<W>);
	// 79: fstatat
	this->install_syscall_handler(79, syscall_fstatat<W>);
	// 80: fstat
	this->install_syscall_handler(80, syscall_fstat<W>);

	// 94: exit_group (single-threaded)
	this->install_syscall_handler(94, syscall_exit<W>);

	// nanosleep
	this->install_syscall_handler(101, syscall_nanosleep<W>);
	// clock_gettime
	this->install_syscall_handler(113, syscall_clock_gettime<W>);
	// sched_getaffinity
	this->install_syscall_handler(123, syscall_stub_nosys<W>);
	// kill
	this->install_syscall_handler(130,
	[] (Machine<W>& machine) {
		const int pid = machine.template sysarg<int> (1);
		const int sig = machine.template sysarg<int> (2);
		SYSPRINT(">>> kill on pid=%d signal=%d\n", pid, sig);
		(void) pid;
		// If the signal zero or unset, ignore it
		if (sig == 0 || machine.sigaction(sig).is_unset()) {
			return;
		} else {
			// Jump to signal handler and change to altstack, if set
			machine.signals().enter(machine, sig);
			SYSPRINT("<<< tgkill signal=%d jumping to 0x%lX (sp=0x%lX)\n",
				sig, (long)machine.cpu.pc(), (long)machine.cpu.reg(REG_SP));
			return;
		}
		machine.stop();
	});
	// sigaltstack
	this->install_syscall_handler(132, syscall_sigaltstack<W>);
	// rt_sigaction
	this->install_syscall_handler(134, syscall_sigaction<W>);
	// rt_sigprocmask
	this->install_syscall_handler(135, syscall_stub_zero<W>);
	// uname
	this->install_syscall_handler(160, syscall_uname<W>);
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
	// getegid
	this->install_syscall_handler(177, syscall_stub_zero<W>);

	this->install_syscall_handler(214, syscall_brk<W>);

	this->install_syscall_handler(278, syscall_getrandom<W>);

	add_mman_syscalls<W>();

	if (filesystem || sockets) {
		// Workaround for a broken "feature"
		// Closing sockets that are already closed cause SIGPIPE signal
		signal(SIGPIPE, SIG_IGN);

		m_fds.reset(new FileDescriptors);
		if (sockets)
			add_socket_syscalls(*this);
	}

	// statx
	this->install_syscall_handler(291, syscall_statx<W>);
}

template void Machine<4>::setup_minimal_syscalls();
template void Machine<4>::setup_newlib_syscalls();
template void Machine<4>::setup_linux_syscalls(bool, bool);

template void Machine<8>::setup_minimal_syscalls();
template void Machine<8>::setup_newlib_syscalls();
template void Machine<8>::setup_linux_syscalls(bool, bool);

FileDescriptors::~FileDescriptors() {
	// Close all the real FDs
	for (const auto& it : translation) {
		::close(it.second);
	}
}

} // riscv
