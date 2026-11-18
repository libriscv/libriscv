#pragma once
#include <functional>
#include <string>
#include <map>
#include <string_view>
#include "../types.hpp"

#if defined(__APPLE__) || defined(__LINUX__)
#include <errno.h>
#endif

namespace riscv {

struct FileDescriptors
{
#ifdef _WIN32
    typedef uint64_t real_fd_type; // SOCKET is uint64_t
#else
    typedef int real_fd_type;
#endif

	// Insert and manage real FDs, return virtual FD
	int assign_file(real_fd_type fd) { return assign(fd, false); }
	int assign_socket(real_fd_type fd) { return assign(fd, true); }
	int assign(real_fd_type fd, bool socket);
	// Get real FD from virtual FD
    real_fd_type get(int vfd);
    real_fd_type translate(int vfd);
	// Remove virtual FD and return real FD
    real_fd_type erase(int vfd);

	bool is_socket(int) const;
	bool permit_write(int vfd) {
		if (is_socket(vfd)) return true;
		else return proxy_mode;
	}

	static std::string normalize_path(std::string_view path);

	~FileDescriptors();

    std::map<int, real_fd_type> translation;
	std::map<int, std::string> guest_paths;

	// Default working directory (fake root)
	std::string cwd = "/home";

	static constexpr int FILE_D_BASE = 0x1000;
	static constexpr int SOCKET_D_BASE = 0x40001000;
	int file_counter = FILE_D_BASE;
	int socket_counter = SOCKET_D_BASE;

	bool permit_filesystem = false;
	bool permit_sockets = false;
	bool proxy_mode = false;
	bool permit_special_files = false;

	std::function<bool(void*, std::string&)> filter_open = nullptr; /* NOTE: Can modify path */
	std::function<bool(void*, std::string&)> filter_readlink = nullptr; /* NOTE: Can modify path */
	std::function<bool(void*, const std::string&)> filter_stat = nullptr;
	std::function<bool(void*, uint64_t)> filter_ioctl = nullptr;
};

inline int FileDescriptors::assign(FileDescriptors::real_fd_type real_fd, bool socket)
{
	int virtfd;
	if (!socket)
		virtfd = file_counter++;
	else
		virtfd = socket_counter++;

	translation.emplace(virtfd, real_fd);
	return virtfd;
}
inline FileDescriptors::real_fd_type FileDescriptors::get(int virtfd)
{
	auto it = translation.find(virtfd);
	if (it != translation.end()) return it->second;
	return -EBADF;
}
inline FileDescriptors::real_fd_type FileDescriptors::translate(int virtfd)
{
	auto it = translation.find(virtfd);
	if (it != translation.end()) return it->second;
	// Only allow direct access to standard pipes and errors
	return (virtfd >= 0 && virtfd <= 2) ? virtfd : -1;
}
inline FileDescriptors::real_fd_type FileDescriptors::erase(int virtfd)
{
	auto it = translation.find(virtfd);
	if (it != translation.end()) {
        FileDescriptors::real_fd_type real_fd = it->second;
		// Remove the virt FD
		translation.erase(it);
		guest_paths.erase(virtfd);
		return real_fd;
	}
	return -EBADF;
}

inline bool FileDescriptors::is_socket(int virtfd) const
{
	return virtfd >= SOCKET_D_BASE;
}

inline std::string FileDescriptors::normalize_path(std::string_view path)
{
	std::string out;
	out.reserve(path.size() + 1);
	bool trailing = false;
	size_t pos = 0;
	while (pos <= path.size()) {
		size_t next = path.find('/', pos);
		if (next == std::string_view::npos)
			next = path.size();
		const std::string_view part = path.substr(pos, next - pos);
		trailing = part.empty() || part == "." || part == "..";
		if (part == "..") {
			const size_t slash = out.rfind('/');
			out.resize(slash == std::string::npos ? 0 : slash);
		} else if (!trailing) {
			out += '/';
			out += part;
		}
		pos = next + 1;
	}
	if (out.empty() || trailing)
		out += '/';
	return out;
}

} // riscv
