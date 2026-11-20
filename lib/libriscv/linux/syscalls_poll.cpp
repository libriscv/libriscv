#if defined(__APPLE__)
    #include <time.h>
    struct timespec {
        time_t   tv_sec;  /* seconds */
        long     tv_nsec; /* nanoseconds */
    };
#else
    #include <sys/time.h>
#endif

#include <poll.h>

int poll_with_timeout(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout_ts) {
    #if defined(__APPLE__) || defined(__SWITCH__)
        // On macOS (and Switch), we don't have ppoll, so we need to use poll and then handle the timeout manually.
        int timeout_ms;
        if (timeout_ts != NULL) {
            timeout_ms = timeout_ts->tv_sec * 1000 + timeout_ts->tv_nsec / 1000000;
        } else {
            timeout_ms = -1;
        }
        return poll(fds, nfds, timeout_ms);
    #else
        return ppoll(fds, nfds, timeout_ts, NULL);
    #endif
}

struct guest_poll_timespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};
static constexpr int64_t PPOLL_MAX_WAIT_NS = 1'000'000'000LL; // 1s
static constexpr int64_t PPOLL_SHARED_WAIT_NS = 1'000'000LL;  // 1ms (another thread runnable)

// int ppoll(struct pollfd *fds, nfds_t nfds,
//        const struct timespec *timeout_ts, const sigset_t *sigmask);
template <int W>
static void syscall_ppoll(Machine<W>& machine)
{
	const auto g_fds = machine.sysarg(0);
	const auto nfds  = machine.template sysarg<unsigned>(1);
	const auto g_ts = machine.sysarg(2);

	// A NULL timeout means wait forever
	int64_t requested_ns = -1;
	if (g_ts != 0x0) {
		guest_poll_timespec gts;
		machine.copy_from_guest(&gts, g_ts, sizeof(gts));
		if (gts.tv_sec < 0 || gts.tv_nsec < 0 || gts.tv_nsec >= 1'000'000'000LL) {
			machine.set_result(-EINVAL);
			return;
		}
		// Saturate instead of overflowing on huge timeouts
		requested_ns = (gts.tv_sec >= INT64_MAX / 1'000'000'000LL)
			? INT64_MAX : gts.tv_sec * 1'000'000'000LL + gts.tv_nsec;
	}

	std::array<struct pollfd, 128> fds;
	std::array<struct pollfd, 128> linux_fds;
	if (nfds > fds.size()) {
		machine.set_result(-EINVAL);
		return;
	}

	if (!machine.has_file_descriptors()) {
		// No file descriptors, just return 0
		machine.set_result(0);
		return;
	}

	machine.copy_from_guest(fds.data(), g_fds, nfds * sizeof(fds[0]));
	// Translate to local FD. Negative descriptors are ignored, like on
	// Linux, and descriptors the guest does not have are invalid.
	int invalid = 0;
	for (unsigned i = 0; i < nfds; i++) {
		linux_fds[i].events = fds[i].events;
		linux_fds[i].revents = 0;
		linux_fds[i].fd = (fds[i].fd >= 0) ? machine.fds().translate(fds[i].fd) : -1;
		if (fds[i].fd >= 0 && linux_fds[i].fd < 0)
			invalid++;
	}

	// Bound how long the host thread can be blocked, unless the sandbox is
	// off (proxy mode). A negative wait is an infinite one.
	const bool shared = machine.has_threads()
		&& !machine.threads().suspended_threads().empty();
	const bool sandboxed = !machine.fds().proxy_mode;
	const int64_t max_wait_ns = shared ? PPOLL_SHARED_WAIT_NS
		: (sandboxed ? PPOLL_MAX_WAIT_NS : -1);
	int64_t wait_ns = requested_ns;
	if (max_wait_ns >= 0 && (wait_ns < 0 || wait_ns > max_wait_ns))
		wait_ns = max_wait_ns;
	// An invalid descriptor counts as ready, so don't wait at all
	if (invalid > 0)
		wait_ns = 0;
	const bool cut_short = (wait_ns != requested_ns && invalid == 0);

	struct timespec ts;
	ts.tv_sec  = wait_ns / 1'000'000'000LL;
	ts.tv_nsec = wait_ns % 1'000'000'000LL;

	const auto t0 = std::chrono::steady_clock::now();
	int res = poll_with_timeout(linux_fds.data(), nfds, (wait_ns >= 0) ? &ts : nullptr);
	const int64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now() - t0).count();
	if (res < 0)
		res = -errno;
	if (sandboxed && wait_ns != 0 && elapsed_ns > 0)
		machine.penalize(elapsed_ns / 1000);

	if (res >= 0) {
		res += invalid;
		// A wait we cut short did not time out. It was interrupted.
		if (res == 0 && cut_short)
			res = -EINTR;
		// Linux always writes back revents (also when nothing is ready)
		for (unsigned i = 0; i < nfds; i++) {
			// The host ignored the descriptors it does not have
			const bool is_invalid = fds[i].fd >= 0 && linux_fds[i].fd < 0;
			fds[i].revents = is_invalid ? POLLNVAL : linux_fds[i].revents;
		}
		machine.copy_to_guest(g_fds, fds.data(), nfds * sizeof(fds[0]));
	}

	if (g_ts != 0x0 && requested_ns > 0) {
		const int64_t remaining = std::max<int64_t>(0, requested_ns - elapsed_ns);
		const guest_poll_timespec gts { remaining / 1'000'000'000LL, remaining % 1'000'000'000LL };
		try {
			machine.copy_to_guest(g_ts, &gts, sizeof(gts));
		} catch (const MachineException&) {}
	}

	machine.set_result(res);
#if defined(SYSCALL_VERBOSE)
	const char *info;
	if (res < 0) info = "error";
	else if (res == 0) info = "timeout";
	else info = "good";
	if (riscv::verbose_syscalls_enabled)
		printf("SYSCALL ppoll, nfds: %u = %d (%s)\n",
			   nfds, res, info);
#endif
	if (res == -EINTR && shared)
		machine.threads().suspend_and_yield(res);
}
