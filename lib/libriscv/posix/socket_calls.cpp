#include <libriscv/machine.hpp>

//#define SOCKETCALL_VERBOSE 1
#ifdef SOCKETCALL_VERBOSE
#define SYSPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define SYSPRINT(fmt, ...) /* fmt */
#endif

#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#else
#include "../win32/ws2.hpp"
WSADATA riscv::ws2::global_winsock_data;
bool riscv::ws2::winsock_initialized = false;
using ssize_t = long long int;
#endif

namespace riscv {


template <int W>
static void syscall_socket(Machine<W>& machine)
{
	const auto [domain, type, proto] =
		machine.template sysargs<int, int, int> ();

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {
#ifdef WIN32
        ws2::init();
#endif
		auto real_fd = socket(domain, type, proto);
		if (real_fd > 0) {
			const int vfd = machine.fds().assign_socket(real_fd);
			machine.set_result(vfd);
		} else {
			// Translate errno() into kernel API return value
			machine.set_result(-errno);
		}
	} else {
		machine.set_result(-EBADF);
	}
#ifdef SOCKETCALL_VERBOSE
	const char* domname;
	switch (domain & 0xFF) {
		case AF_UNIX: domname = "Unix"; break;
		case AF_INET: domname = "IPv4"; break;
		case AF_INET6: domname = "IPv6"; break;
		default: domname = "unknown";
	}
	const char* typname;
	switch (type & 0xFF) {
		case SOCK_STREAM: typname = "Stream"; break;
		case SOCK_DGRAM: typname = "Datagram"; break;
		case SOCK_SEQPACKET: typname = "Seq.packet"; break;
		case SOCK_RAW: typname = "Raw"; break;
		default: typname = "unknown";
	}
	SYSPRINT("SYSCALL socket, domain: %x (%s) type: %x (%s) proto: %x = %ld\n",
		domain, domname, type, typname, proto, (long)machine.return_value());
#endif
}

template <int W>
static void syscall_bind(Machine<W>& machine)
{
	const auto [vfd, g_addr, addrlen] =
		machine.template sysargs<int, address_type<W>, address_type<W>> ();

	SYSPRINT("SYSCALL bind, vfd: %d addr: 0x%lX len: 0x%lX\n",
		vfd, (long)g_addr, (long)addrlen);

	if (addrlen > 128) {
		machine.set_result(-ENOMEM);
		return;
	}

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {

		const auto real_fd = machine.fds().translate(vfd);
		alignas(16) char buffer[128];
		machine.copy_from_guest(buffer, g_addr, addrlen);

		int res = bind(real_fd, (struct sockaddr *)buffer, addrlen);
		machine.set_result_or_error(res);
	} else {
		machine.set_result(-EBADF);
	}
}

template <int W>
static void syscall_listen(Machine<W>& machine)
{
	const auto [vfd, backlog] =
		machine.template sysargs<int, int> ();

	SYSPRINT("SYSCALL listen, vfd: %d backlog: %d\n",
		vfd, backlog);

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {

		const auto real_fd = machine.fds().translate(vfd);

		int res = listen(real_fd, backlog);
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_accept(Machine<W>& machine)
{
	const auto [vfd, g_addr, g_addrlen] =
		machine.template sysargs<int, address_type<W>, address_type<W>> ();

	SYSPRINT("SYSCALL accept, vfd: %d addr: 0x%lX\n",
		vfd, (long)g_addr);

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {

		const auto real_fd = machine.fds().translate(vfd);
		alignas(16) char buffer[128];
		socklen_t addrlen = sizeof(buffer);

		int res = accept(real_fd, (struct sockaddr *)buffer, &addrlen);
		if (res >= 0) {
			// Assign and translate the new fd to virtual fd
			res = machine.fds().assign_socket(res);
			machine.copy_to_guest(g_addr, buffer, addrlen);
			machine.copy_to_guest(g_addrlen, &addrlen, sizeof(addrlen));
		}
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_connect(Machine<W>& machine)
{
	const auto [vfd, g_addr, addrlen] =
		machine.template sysargs<int, address_type<W>, address_type<W>> ();

	if (addrlen > 256) {
		machine.set_result(-ENOMEM);
		return;
	}
	int real_fd = -EBADFD;

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {

		real_fd = machine.fds().translate(vfd);
		alignas(16) char buffer[256];
		machine.copy_from_guest(buffer, g_addr, addrlen);

#if 0
		char printbuf[INET6_ADDRSTRLEN];
		auto* sin6 = (struct sockaddr_in6 *)buffer;
		inet_ntop(AF_INET6, &sin6->sin6_addr, printbuf, sizeof(printbuf));
		printf("SYSCALL connect IPv6 address: %s\n", printbuf);

		auto* sin4 = (struct sockaddr_in *)buffer;
		inet_ntop(AF_INET, &sin4->sin_addr, printbuf, sizeof(printbuf));
		printf("SYSCALL connect IPv4 address: %s\n", printbuf);
#endif

		const int res = connect(real_fd, (const struct sockaddr *)buffer, addrlen);
		machine.set_result_or_error(res);
	} else {
		machine.set_result(-EBADF);
	}

	SYSPRINT("SYSCALL connect, vfd: %d (real_fd: %d) addr: 0x%lX len: %zu = %ld\n",
		vfd, real_fd, (long)g_addr, (size_t)addrlen, (long)machine.return_value());
}

template <int W>
static void syscall_getsockname(Machine<W>& machine)
{
	const auto [vfd, g_addr, g_addrlen] =
		machine.template sysargs<int, address_type<W>, address_type<W>> ();

	if (machine.has_file_descriptors() && machine.fds().permit_sockets)
	{
		const auto real_fd = machine.fds().translate(vfd);

		struct sockaddr addr {};
		socklen_t addrlen = 0;
		int res = getsockname(real_fd, &addr, &addrlen);
		if (res == 0) {
			machine.copy_to_guest(g_addr, &addr, addrlen);
			machine.copy_to_guest(g_addrlen, &addrlen, sizeof(addrlen));
		}
		machine.set_result_or_error(res);
	} else {
		machine.set_result(-EBADF);
	}

	SYSPRINT("SYSCALL getsockname, fd: %d addr: 0x%lX len: 0x%lX = %ld\n",
		vfd, (long)g_addr, (long)g_addrlen, (long)machine.return_value());
}

template <int W>
static void syscall_getpeername(Machine<W>& machine)
{
	const auto [vfd, g_addr, g_addrlen] =
		machine.template sysargs<int, address_type<W>, address_type<W>> ();

	if (machine.has_file_descriptors() && machine.fds().permit_sockets)
	{
		const auto real_fd = machine.fds().translate(vfd);

		struct sockaddr addr {};
		socklen_t addrlen = 0;
		int res = getpeername(real_fd, &addr, &addrlen);

		if (res == 0) {
			machine.copy_to_guest(g_addr, &addr, addrlen);
			machine.copy_to_guest(g_addrlen, &addrlen, sizeof(addrlen));
		}
		machine.set_result_or_error(res);
	} else {
		machine.set_result(-EBADF);
	}

	SYSPRINT("SYSCALL getpeername, fd: %d addr: 0x%lX len: 0x%lX = %ld\n",
		vfd, (long)g_addr, (long)g_addrlen, (long)machine.return_value());
}

template <int W>
static void syscall_sendto(Machine<W>& machine)
{
	// ssize_t sendto(int vfd, const void *buf, size_t len, int flags,
	//		   const struct sockaddr *dest_addr, socklen_t addrlen);
	const auto [vfd, g_buf, buflen, flags, g_dest_addr, dest_addrlen] =
		machine.template sysargs<int, address_type<W>, address_type<W>, int, address_type<W>, unsigned>();

	if (dest_addrlen > 128) {
		machine.set_result(-ENOMEM);
		return;
	}
	alignas(16) char dest_addr[128];
	machine.copy_from_guest(dest_addr, g_dest_addr, dest_addrlen);

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {

		const auto real_fd = machine.fds().translate(vfd);

#ifdef __linux__
		// Gather up to 1MB of pages we can read into
		riscv::vBuffer buffers[256];
		size_t cnt =
			machine.memory.gather_buffers_from_range(256, buffers, g_buf, buflen);

		struct iovec iov[256];
		for (size_t i = 0; i < cnt; i++) {
			iov[i].iov_base = buffers[i].ptr;
			iov[i].iov_len  = buffers[i].len;
		}

		const struct msghdr hdr {
			.msg_name = dest_addr,
			.msg_namelen = dest_addrlen,
			.msg_iov = iov,
			.msg_iovlen = cnt,
			.msg_control = nullptr,
			.msg_controllen = 0,
			.msg_flags = 0,
		};

		const ssize_t res = sendmsg(real_fd, &hdr, flags);
#else
		// XXX: Write me
		const ssize_t res = -1;
#endif
		machine.set_result_or_error(res);
	} else {
		machine.set_result(-EBADF);
	}
	SYSPRINT("SYSCALL sendto, fd: %d len: %ld flags: %#x = %ld\n",
			 vfd, (long)buflen, flags, (long)machine.return_value());
}

template <int W>
static void syscall_recvfrom(Machine<W>& machine)
{
	// ssize_t recvfrom(int vfd, void *buf, size_t len, int flags,
	// 					struct sockaddr *src_addr, socklen_t *addrlen);
	const auto [vfd, g_buf, buflen, flags, g_src_addr, g_addrlen] =
		machine.template sysargs<int, address_type<W>, address_type<W>, int, address_type<W>, address_type<W>>();

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {

		const auto real_fd = machine.fds().translate(vfd);

#ifdef __linux__
		// Gather up to 1MB of pages we can read into
		riscv::vBuffer buffers[256];
		size_t cnt =
			machine.memory.gather_buffers_from_range(256, buffers, g_buf, buflen);

		struct iovec iov[256];
		for (size_t i = 0; i < cnt; i++) {
			iov[i].iov_base = buffers[i].ptr;
			iov[i].iov_len  = buffers[i].len;
		}

		alignas(16) char dest_addr[128];
		struct msghdr hdr {
			.msg_name = dest_addr,
			.msg_namelen = sizeof(dest_addr),
			.msg_iov = iov,
			.msg_iovlen = cnt,
			.msg_control = nullptr,
			.msg_controllen = 0,
			.msg_flags = 0,
		};

		const ssize_t res = recvmsg(real_fd, &hdr, flags);
		if (res >= 0) {
			if (g_src_addr != 0x0)
				machine.copy_to_guest(g_src_addr, hdr.msg_name, hdr.msg_namelen);
			if (g_addrlen != 0x0)
				machine.copy_to_guest(g_addrlen, &hdr.msg_namelen, sizeof(hdr.msg_namelen));
		}
#else
		// XXX: Write me
		const ssize_t res = -1;
#endif
		machine.set_result_or_error(res);
	} else {
		machine.set_result(-EBADF);
	}
	SYSPRINT("SYSCALL recvfrom, fd: %d len: %ld flags: %#x = %ld\n",
			 vfd, (long)buflen, flags, (long)machine.return_value());
}

template <int W>
static void syscall_setsockopt(Machine<W>& machine)
{
	const auto [vfd, level, optname, g_opt, optlen] =
		machine.template sysargs<int, int, int, address_type<W>, unsigned> ();

	if (optlen > 128) {
		machine.set_result(-ENOMEM);
		return;
	}

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {

		const auto real_fd = machine.fds().translate(vfd);
		alignas(8) char buffer[128];
		machine.copy_from_guest(buffer, g_opt, optlen);

		int res = setsockopt(real_fd, level, optname, buffer, optlen);
		machine.set_result_or_error(res);
	} else {
		machine.set_result(-EBADF);
	}
	SYSPRINT("SYSCALL setsockopt, fd: %d level: %x optname: %#x len: %u = %ld\n",
		vfd, level, optname, optlen, (long)machine.return_value());
}

template <int W>
static void syscall_getsockopt(Machine<W>& machine)
{
	const auto [vfd, level, optname, g_opt, g_optlen] =
		machine.template sysargs<int, int, int, address_type<W>, address_type<W>> ();
	socklen_t optlen = 0;

	if (machine.has_file_descriptors() && machine.fds().permit_sockets)
	{
		const auto real_fd = machine.fds().translate(vfd);

		alignas(8) char buffer[128];
		optlen = std::min(sizeof(buffer), size_t(g_optlen));
		int res = getsockopt(real_fd, level, optname, buffer, &optlen);
		if (res == 0) {
			machine.copy_to_guest(g_optlen, &optlen, sizeof(optlen));
			machine.copy_to_guest(g_opt, buffer, optlen);
		}
		machine.set_result_or_error(res);
	} else {
		machine.set_result(-EBADF);
	}

	SYSPRINT("SYSCALL getsockopt, fd: %d level: %x optname: %#x len: %ld/%ld = %ld\n",
			 vfd, level, optname, (long)optlen, (long)g_optlen, (long)machine.return_value());
}

template <int W>
void add_socket_syscalls(Machine<W>& machine)
{
	machine.install_syscall_handler(198, syscall_socket<W>);
	machine.install_syscall_handler(200, syscall_bind<W>);
	machine.install_syscall_handler(201, syscall_listen<W>);
	machine.install_syscall_handler(202, syscall_accept<W>);
	machine.install_syscall_handler(203, syscall_connect<W>);
	machine.install_syscall_handler(204, syscall_getsockname<W>);
	machine.install_syscall_handler(205, syscall_getpeername<W>);
	machine.install_syscall_handler(206, syscall_sendto<W>);
	machine.install_syscall_handler(207, syscall_recvfrom<W>);
	machine.install_syscall_handler(208, syscall_setsockopt<W>);
	machine.install_syscall_handler(209, syscall_getsockopt<W>);
}

template void add_socket_syscalls<4>(Machine<4>&);
template void add_socket_syscalls<8>(Machine<8>&);

} // riscv
