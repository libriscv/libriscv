#pragma once
#include <functional>
#include <map>
#include "types.hpp"

namespace riscv {

struct FileDescriptors
{
	// Insert and manage real FDs, return virtual FD
	int assign_file(int fd) { return assign(fd, false); }
	int assign_socket(int fd) { return assign(fd, true); }
	int assign(int fd, bool socket);
	// Get real FD from virtual FD
	int get(int vfd);
	int translate(int vfd);
	// Remove virtual FD and return real FD
	int erase(int vfd);

	bool is_socket(int) const;
	bool permit_write(int vfd) {
		if (is_socket(vfd)) return true;
		else return permit_file_write;
	}

	~FileDescriptors();

	std::map<int, int> translation;

	static constexpr int FILE_D_BASE = 0x1000;
	static constexpr int SOCKET_D_BASE = 0x40001000;
	int file_counter = FILE_D_BASE;
	int socket_counter = SOCKET_D_BASE;

	bool permit_filesystem = false;
	bool permit_file_write = false;
	bool permit_sockets = false;

	std::function<bool(void*, const char*)> filter_open = nullptr;
	std::function<bool(void*, const char*)> filter_stat = nullptr;
	std::function<bool(void*, uint64_t)> filter_ioctl = nullptr;
};

inline int FileDescriptors::assign(int real_fd, bool socket)
{
	int virtfd;
	if (!socket)
		virtfd = file_counter++;
	else
		virtfd = socket_counter++;

	translation.emplace(virtfd, real_fd);
	return virtfd;
}
inline int FileDescriptors::get(int virtfd)
{
	auto it = translation.find(virtfd);
	if (it != translation.end()) return it->second;
	return -EBADF;
}
inline int FileDescriptors::translate(int virtfd)
{
	auto it = translation.find(virtfd);
	if (it != translation.end()) return it->second;
	// Only allow direct access to standard pipes and errors
	return (virtfd <= 2) ? virtfd : -1;
}
inline int FileDescriptors::erase(int virtfd)
{
	auto it = translation.find(virtfd);
	if (it != translation.end()) {
		const int real_fd = it->second;
		// Remove the virt FD
		translation.erase(it);
		return real_fd;
	}
	return -EBADF;
}

inline bool FileDescriptors::is_socket(int virtfd) const
{
	return virtfd >= SOCKET_D_BASE;
}

} // riscv
