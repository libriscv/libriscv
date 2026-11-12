#pragma once
#include <sys/types.h>
#include "ws2.hpp"

namespace riscv {

template <int W>
RSP<W>::RSP(riscv::Machine<W>& m, uint16_t port)
	: m_machine{m}
{
    ws2::init();

	this->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    u_long mode = 1;
    ioctlsocket(server_fd, FIONBIO, &mode); // SOCK_NONBLOCK

	int opt = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_BROADCAST, // SO_BROADCAST = SO_REUSEPORT
        (const char*)&opt, sizeof(opt))) {
        closesocket(server_fd);
		throw MachineException(SYSTEM_CALL_FAILED, "Failed to enable REUSEADDR/PORT");
	}
	struct sockaddr_in address;
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);
	if (bind(server_fd, (struct sockaddr*) &address,
			sizeof(address)) < 0) {
        closesocket(server_fd);
		throw MachineException(SYSTEM_CALL_FAILED, "GDB listener failed to bind to port");
	}
	if (listen(server_fd, 2) < 0) {
        closesocket(server_fd);
		throw MachineException(SYSTEM_CALL_FAILED, "GDB listener failed to listen on port");
	}
}
template <int W>
std::unique_ptr<RSPClient<W>> RSP<W>::accept(int timeout_secs)
{
	struct timeval tv {
		.tv_sec = timeout_secs,
		.tv_usec = 0
	};
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(server_fd, &fds);

	const int ret = select(server_fd + 1, &fds, NULL, NULL, &tv);
	if (ret <= 0) {
		return nullptr;
	}

	struct sockaddr_in address;
	int addrlen = sizeof(address);
	int sockfd = ::accept(server_fd, (struct sockaddr*) &address,
			(socklen_t*) &addrlen);
	if (sockfd < 0) {
		return nullptr;
	}
	// Disable Nagle
	int opt = 1;
	if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt))) {
        closesocket(sockfd);
		return nullptr;
	}
	// Set a send timeout; leave reads without a timeout so GDB can remain idle
	DWORD timeout = 60*1000;
	if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout))) {
        closesocket(sockfd);
		return nullptr;
	}
	return std::make_unique<RSPClient<W>>(m_machine, sockfd);
}
template <int W> inline
RSP<W>::~RSP() {
    closesocket(server_fd);
}

template <int W> inline
RSPClient<W>::~RSPClient() {
    if (!is_closed())
        closesocket(this->sockfd);
}

template <int W> inline
void RSPClient<W>::close_now() {
    if (!this->m_closed) {
        this->m_closed = true;
        closesocket(this->sockfd);
    }
}

template <int W>
bool RSPClient<W>::write_raw(const char* data, size_t len)
{
    if (m_closed) return false;
    while (len > 0) {
        const int w = ::send(sockfd, data, (int)len, 0);
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
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sockfd, &fds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int ret = select(0, &fds, NULL, NULL, &tv);
        if (ret == 0)
            return 0;
        if (ret < 0)
            return -1;
    }
    const int r = ::recv(sockfd, buffer, (int)len, 0);
    return (r > 0) ? r : -1;
}

} // riscv
