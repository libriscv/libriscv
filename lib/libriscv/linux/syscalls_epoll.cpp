#include <sys/epoll.h>
//#define SYSPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)

template <int W>
static void syscall_epoll_create(Machine<W>& machine)
{
	const auto flags = machine.template sysarg<int>(0);

	if (machine.has_file_descriptors()) {
		const int real_fd = epoll_create1(flags);
		if (real_fd > 0) {
			const int vfd = machine.fds().assign_file(real_fd);
			machine.set_result(vfd);
		} else {
			machine.set_result_or_error(real_fd);
		}
	} else {
		machine.set_result(-EBADF);
	}
	SYSPRINT("SYSCALL epoll_create, flags: %d = %d\n",
		flags, machine.template return_value<int>());
}

template <int W>
static void syscall_epoll_ctl(Machine<W>& machine)
{
	// int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
	const auto vepoll_fd = machine.template sysarg<int>(0);
	const auto op  = machine.template sysarg<int>(1);
	const auto vfd = machine.template sysarg<int>(2);
	const auto g_event = machine.template sysarg(3);
	int fd = -EBADF;

	if (machine.has_file_descriptors()) {
		const int epoll_fd = machine.fds().translate(vepoll_fd);
		fd = machine.fds().translate(vfd);

		struct epoll_event event {};
		machine.copy_from_guest(&event, g_event, sizeof(event));

		const int res = epoll_ctl(epoll_fd, op, fd, &event);
		machine.set_result_or_error(res);
	} else {
		machine.set_result(-EBADF);
	}
	SYSPRINT("SYSCALL epoll_ctl, epoll_fd: %d  op: %d vfd: %d (%d)  event: 0x%lX = %ld\n",
		vepoll_fd, op, vfd, fd, (long)g_event, (long)machine.return_value());
}

template <int W>
static void syscall_epoll_pwait(Machine<W>& machine)
{
	//  int epoll_pwait(int epfd, struct epoll_event *events,
	//  				int maxevents, int timeout,
	//  				const sigset_t *sigmask);
	const auto vepoll_fd = machine.template sysarg<int>(0);
	const auto g_events = machine.template sysarg(1);
	const auto maxevents = machine.template sysarg<int>(2);
	const auto timeout = machine.template sysarg<int>(3);

	std::array<struct epoll_event, 128> events;
	if (maxevents < 0 || maxevents > (int)events.size()) {
		SYSPRINT("WARNING: Too many epoll events for %d\n", vepoll_fd);
		machine.set_result_or_error(-1);
		return;
	}

	if (machine.has_file_descriptors()) {
		const int epoll_fd = machine.fds().translate(vepoll_fd);

		const int res = epoll_wait(epoll_fd, events.data(), maxevents, timeout);
		if (res > 0) {
			machine.copy_to_guest(g_events, events.data(), res * sizeof(events[0]));
		}
		machine.set_result_or_error(res);
	} else {
		machine.set_result(-EBADF);
	}
	SYSPRINT("SYSCALL epoll_pwait, epoll_fd: %d = %ld\n",
		   vepoll_fd, (long)machine.return_value());
}
