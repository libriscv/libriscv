/// Linux memory mapping system call emulation
/// Works on all platforms
#define MAP_ANONYMOUS        0x20

template <int W>
static void add_mman_syscalls()
{
	// munmap
	Machine<W>::install_syscall_handler(215,
	[] (Machine<W>& machine) {
		const auto addr = machine.sysarg(0);
		const auto len  = machine.sysarg(1);
		SYSPRINT(">>> munmap(0x%lX, len=%zu)\n", (long)addr, (size_t)len);
		machine.memory.free_pages(addr, len);
		auto& nextfree = machine.memory.mmap_address();
		if (addr + len == nextfree) {
			nextfree = addr;
			if (nextfree < machine.memory.mmap_start())
				nextfree = machine.memory.mmap_start();
		}
		machine.set_result(0);
	});
	// mmap
	Machine<W>::install_syscall_handler(222,
	[] (Machine<W>& machine) {
		const auto addr_g = machine.sysarg(0);
		auto length = machine.sysarg(1);
		const auto prot   = machine.template sysarg<int>(2);
		const auto flags  = machine.template sysarg<int>(3);
		const auto vfd    = machine.template sysarg<int>(4);
		const auto voff   = machine.sysarg(5);
		PageAttributes attr{
			.read  = (prot & 1) != 0,
			.write = (prot & 2) != 0,
			.exec  = (prot & 4) != 0,
		};
		SYSPRINT(">>> mmap(addr 0x%lX, len %zu, prot %#x, flags %#X, vfd=%d voff=%zu)\n",
				(long)addr_g, (size_t)length, prot, flags, vfd, size_t(voff));

		if (addr_g % Page::size() != 0) {
			machine.set_result(-1); // = MAP_FAILED;
			SYSPRINT("<<< mmap(addr 0x%lX, len %zu, ...) = MAP_FAILED\n",
					(long)addr_g, (size_t)length);
			return;
		}

		length = (length + PageMask) & ~address_type<W>(PageMask);

		auto& nextfree = machine.memory.mmap_address();

		if (vfd != -1)
		{
#ifdef __linux__
			if (machine.has_file_descriptors())
			{
				const int real_fd = machine.fds().translate(vfd);

				address_type<W> dst = 0x0;
				if (addr_g == 0x0) {
					dst = nextfree;
					nextfree += length;
				} else {
					dst = addr_g;
				}
				lseek(real_fd, voff, SEEK_SET);
				// Make the area read-write
				machine.memory.set_page_attr(dst, length, PageAttributes{});
				// Readv into the area
				std::array<riscv::vBuffer, 256> buffers;
				const size_t cnt =
					machine.memory.gather_writable_buffers_from_range(buffers.size(), buffers.data(), dst, length);
				(void)readv(real_fd, (const iovec *)&buffers[0], cnt);
				// Set new page protections on area
				machine.memory.set_page_attr(dst, length, attr);
				machine.set_result(dst);
				return;
			}
			else
			{
				throw MachineException(FEATURE_DISABLED, "mmap() with fd, but file descriptors disabled");
			}
#endif
		}
		else if (addr_g == 0 || addr_g == nextfree)
		{
			// anon pages need to be zeroed
			if (flags & MAP_ANONYMOUS) {
				// ... but they are already CoW
				// XXX: Check if page is dirty
				//machine.memory.memset(nextfree, 0, length);
			}
			machine.memory.set_page_attr(nextfree, length, attr);
			machine.set_result(nextfree);
			SYSPRINT("<<< mmap(addr 0x%lX, len %zu, ...) = 0x%lX\n",
					(long)addr_g, (size_t)length, (long)nextfree);
			nextfree += length;
			return;
		} else if (addr_g < nextfree) {
			//printf("Invalid mapping attempted\n");
			//machine.set_result(-1); // = MAP_FAILED;
			machine.memory.set_page_attr(addr_g, length, attr);
			machine.set_result(addr_g);
			return;
		} else { // addr_g != 0x0
			address_type<W> addr_end = addr_g + length;
			for (address_type<W> addr = addr_g; addr < addr_end; addr += Page::size())
			{
				// do nothing?
			}
			machine.memory.set_page_attr(addr_g, length, attr);
			machine.set_result(addr_g);
			return;
		}
		(void) flags;
		(void) prot;
		machine.set_result(-1); // = MAP_FAILED;
	});
	// mremap
	Machine<W>::install_syscall_handler(163,
	[] (Machine<W>& machine) {
		const auto old_addr = machine.sysarg(0);
		const auto old_size = machine.sysarg(1);
		const auto new_size = machine.sysarg(2);
		const auto flags    = machine.template sysarg<int>(3);
		SYSPRINT(">>> mremap(addr 0x%lX, len %zu, newsize %zu, flags %#X)\n",
				(long)old_addr, (size_t)old_size, (size_t)new_size, flags);
		auto& nextfree = machine.memory.mmap_address();
		// We allow the common case of reallocating the
		// last mapping to a bigger one
		if (old_addr + old_size == nextfree) {
			nextfree = old_addr + new_size;
			machine.set_result(old_addr);
			return;
		}
		(void) flags;
		machine.set_result(-1);
	});
	// mprotect
	Machine<W>::install_syscall_handler(226,
	[] (Machine<W>& machine) {
		const auto addr = machine.sysarg(0);
		const auto len  = machine.sysarg(1);
		const int  prot = machine.template sysarg<int> (2);
		SYSPRINT(">>> mprotect(0x%lX, len=%zu, prot=%x)\n",
			(long)addr, (size_t)len, prot);
		machine.memory.set_page_attr(addr, len, {
			.read  = (prot & 1) != 0,
			.write = (prot & 2) != 0,
			.exec  = (prot & 4) != 0
		});
		machine.set_result(0);
	});
	// madvise
	Machine<W>::install_syscall_handler(233,
	[] (Machine<W>& machine) {
		const auto addr  = machine.sysarg(0);
		const auto len   = machine.sysarg(1);
		const int advice = machine.template sysarg<int> (2);
		SYSPRINT(">>> madvise(0x%lX, len=%zu, prot=%x)\n",
			(uint64_t)addr, (size_t)len, advice);
		switch (advice) {
			case 0: // MADV_NORMAL
			case 1: // MADV_RANDOM
			case 2: // MADV_SEQUENTIAL
			case 3: // MADV_WILLNEED:
				machine.set_result(0);
				return;
			case 4: // MADV_DONTNEED
				machine.memory.free_pages(addr, len);
				machine.set_result(0);
				return;
			case 8: // MADV_FREE
			case 9: // MADV_REMOVE
				machine.memory.free_pages(addr, len);
				machine.set_result(0);
				return;
			default:
				machine.set_result(-EINVAL);
				return;
		}
	});
}
