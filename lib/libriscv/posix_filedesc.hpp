#pragma once
#include <functional>
#include <map>
#include "types.hpp"

namespace riscv {

struct FileDescriptors
{
	int assign_file(int vfd) { return assign(vfd, false); }
	int assign_socket(int vfd) { return assign(vfd, true); }
	int assign(int vfd, bool socket);
	int get(int);
	int translate(int);
	int close(int);

	bool is_socket(int) const;
	bool permit_write(int vfd) {
		if (is_socket(vfd)) return true;
		else return permit_file_write;
	}

	~FileDescriptors();

	std::map<int, int> translation;
	std::set<int> sockets;
	int counter = 0x1000;

	bool permit_filesystem = false;
	bool permit_file_write = false;
	bool permit_sockets = false;

	std::function<bool(void*, const char*)> filter_open = nullptr;
	std::function<bool(void*, const char*)> filter_stat = nullptr;
	std::function<bool(void*, uint64_t)> filter_ioctl = nullptr;
};

inline FileDescriptors::~FileDescriptors()
{
	// Close all the real FDs
	for (const auto& it : translation) {
		close(it.second);
	}
}

inline int FileDescriptors::assign(int real_fd, bool socket)
{
	const int virtfd = counter++;
	translation[virtfd] = real_fd;
	if (socket) sockets.insert(virtfd);
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
inline int FileDescriptors::close(int virtfd)
{
	auto it = translation.find(virtfd);
	if (it != translation.end()) {
		const int real_fd = it->second;
		// Remove the virt FD
		translation.erase(it);
		sockets.erase(virtfd);
		return real_fd;
	}
	return -EBADF;
}

inline bool FileDescriptors::is_socket(int virtfd) const
{
	return sockets.count(virtfd) > 0;
}

} // riscv
