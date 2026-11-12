#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cerrno>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

namespace riscv {

template <int W>
RSP<W>::RSP(riscv::Machine<W>& m, uint16_t port)
	: m_machine{m}
{
	this->server_fd = socket(AF_INET,
	                         SOCK_STREAM
#ifdef SOCK_NONBLOCK
	                         	| SOCK_NONBLOCK
#endif
	                        ,0);
	if (server_fd < 0)
		throw MachineException(SYSTEM_CALL_FAILED, "GDB listener failed to create socket");
	int opt = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
		close(server_fd);
		throw MachineException(SYSTEM_CALL_FAILED, "Failed to enable SO_REUSEADDR");
	}
	struct sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);
	if (bind(server_fd, (struct sockaddr*) &address,
			sizeof(address)) < 0) {
		close(server_fd);
		throw MachineException(SYSTEM_CALL_FAILED, "GDB listener failed to bind to port");
	}
	if (listen(server_fd, 2) < 0) {
		close(server_fd);
		throw MachineException(SYSTEM_CALL_FAILED, "GDB listener failed to listen on port");
	}
}
template <int W>
std::unique_ptr<RSPClient<W>> RSP<W>::accept(int timeout_secs)
{
	struct pollfd pfd {};
	pfd.fd = server_fd;
	pfd.events = POLLIN;
	int ret;
	do {
		ret = poll(&pfd, 1, (timeout_secs < 0) ? -1 : timeout_secs * 1000);
	} while (ret < 0 && errno == EINTR);
	if (ret <= 0) {
		return nullptr;
	}

	struct sockaddr_in address;
	socklen_t addrlen = sizeof(address);
	int sockfd = ::accept(server_fd, (struct sockaddr*) &address, &addrlen);
	if (sockfd < 0) {
		return nullptr;
	}
	// Ensure the client socket is blocking; some systems inherit O_NONBLOCK
	int flags = fcntl(sockfd, F_GETFL, 0);
	if (flags >= 0 && (flags & O_NONBLOCK))
		fcntl(sockfd, F_SETFL, flags & ~O_NONBLOCK);
	// Disable Nagle
	int opt = 1;
	if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt))) {
		close(sockfd);
		return nullptr;
	}
#ifdef SO_NOSIGPIPE
	setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif
	// Set a send timeout to prevent an unresponsive GDB from blocking the host.
	// Leave reads without a timeout: GDB may remain idle for hours.
	struct timeval tv {};
	tv.tv_sec = 60;
	if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))) {
		close(sockfd);
		return nullptr;
	}
	return std::make_unique<RSPClient<W>>(m_machine, sockfd);
}
template <int W> inline
RSP<W>::~RSP() {
	close(server_fd);
}

template <int W> inline
RSPClient<W>::~RSPClient() {
	if (!is_closed())
		close(this->sockfd);
}

template <int W> inline
void RSPClient<W>::close_now() {
	if (!this->m_closed) {
		this->m_closed = true;
		close(this->sockfd);
	}
}

template <int W>
bool RSPClient<W>::write_raw(const char* data, size_t len)
{
	if (m_closed) return false;
#ifdef MSG_NOSIGNAL
	static constexpr int flags = MSG_NOSIGNAL;
#else
	static constexpr int flags = 0;
#endif
	while (len > 0) {
		const ssize_t w = ::send(sockfd, data, len, flags);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0) {
			this->close_now();
			return false;
		}
		data += w;
		len  -= w;
	}
	return true;
}

template <int W>
int RSPClient<W>::read_raw(char* buffer, size_t len, int timeout_ms)
{
	if (m_closed) return -1;
	if (timeout_ms >= 0) {
		struct pollfd pfd {};
		pfd.fd = sockfd;
		pfd.events = POLLIN;
		const int ret = ::poll(&pfd, 1, timeout_ms);
		if (ret == 0 || (ret < 0 && errno == EINTR))
			return 0;
		if (ret < 0)
			return -1;
	}
	ssize_t r;
	do {
		r = ::read(sockfd, buffer, len);
	} while (r < 0 && errno == EINTR);
	return (r > 0) ? int(r) : -1;
}

} // riscv
