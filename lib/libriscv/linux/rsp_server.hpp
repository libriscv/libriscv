#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

namespace riscv {

template <int W>
RSP<W>::RSP(riscv::Machine<W>& m, uint16_t port)
	: m_machine{m}
{
	this->server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

	int opt = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
		&opt, sizeof(opt))) {
		close(server_fd);
		throw std::runtime_error("Failed to enable REUSEADDR/PORT");
	}
	struct sockaddr_in address;
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);
	if (bind(server_fd, (struct sockaddr*) &address,
			sizeof(address)) < 0) {
		close(server_fd);
		throw std::runtime_error("GDB listener failed to bind to port");
	}
	if (listen(server_fd, 2) < 0) {
		close(server_fd);
		throw std::runtime_error("GDB listener failed to listen on port");
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
	if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt))) {
		close(sockfd);
		return nullptr;
	}
	// Enable receive and send timeouts
	tv = {
		.tv_sec = 60,
		.tv_usec = 0
	};
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO | SO_SNDTIMEO, &tv, sizeof(tv))) {
		close(sockfd);
		return nullptr;
	}
	return std::make_unique<RSPClient<W>>(m_machine, sockfd);
}
template <int W> inline
RSP<W>::~RSP() {
	close(server_fd);
}

} // riscv
