#include <poll.h>

// int ppoll(struct pollfd *fds, nfds_t nfds,
//        const struct timespec *timeout_ts, const sigset_t *sigmask);
template <int W>
static void syscall_ppoll(Machine<W>& machine)
{
	const auto g_fds = machine.sysarg(0);
	const auto nfds  = machine.template sysarg<unsigned>(1);
	const auto g_ts = machine.sysarg(2);

	struct timespec ts;
	machine.copy_from_guest(&ts, g_ts, sizeof(ts));
	//printf("Timeout from 0x%lX sec=%ld nsec=%ld\n",
	//	(long)g_ts, ts.tv_sec, ts.tv_nsec);

	std::array<struct pollfd, 128> fds;
	std::array<struct pollfd, 128> linux_fds;
	if (nfds > fds.size()) {
		printf("WARNING: Too many ppoll fds: %u\n", nfds);
		machine.set_result_or_error(-1);
		return;
	}

	if (machine.has_file_descriptors()) {
		machine.copy_from_guest(fds.data(), g_fds, nfds * sizeof(fds[0]));
		// Translate to local FD
		for (unsigned i = 0; i < nfds; i++) {
			linux_fds[i].events = fds[i].events;
			linux_fds[i].fd = machine.fds().translate(fds[i].fd);
		}

		const int res = ppoll(linux_fds.data(), nfds, &ts, NULL);
		// The ppoll system call modifies TS
		//clock_gettime(CLOCK_MONOTONIC, &ts);
		//machine.copy_to_guest(g_ts, &ts, sizeof(ts));

		if (res > 0) {
			// Translate back to virtual FD
			for (unsigned i = 0; i < nfds; i++) {
				fds[i].revents = linux_fds[i].revents;
			}
			machine.copy_to_guest(g_fds, fds.data(), nfds * sizeof(fds[0]));
		}
		machine.set_result_or_error(res);
	} else {
		machine.set_result(-EBADF);
	}
#if defined(SYSCALL_VERBOSE)
	const auto res = (long)machine.return_value();
	const char *info;
	if (res < 0) info = "error";
	else if (res == 0) info = "timeout";
	else info = "good";
	printf("SYSCALL ppoll, nfds: %u = %ld (%s)\n",
		   nfds, res, info);
#endif
}
