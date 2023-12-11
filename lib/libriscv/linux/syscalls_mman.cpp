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
		if (addr + len < addr)
			throw MachineException(SYSTEM_CALL_FAILED, "munmap() arguments overflow");
		machine.memory.free_pages(addr, len);
		if (addr >= machine.memory.mmap_start() && addr + len <= machine.memory.mmap_address()) {
			machine.memory.mmap_unmap(addr, len);
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

		auto& nextfree = machine.memory.mmap_address();
		length = (length + PageMask) & ~address_type<W>(PageMask);
		address_type<W> result = address_type<W>(-1);

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
		else if (addr_g == 0)
		{
			auto range = machine.memory.mmap_cache().find(length);
			// Not found in cache, increment MM base address
			if (range.empty()) {
				result = nextfree;
				nextfree += length;
			}
			else
			{
				result = range.addr;
			}
		} else if (addr_g == nextfree) {
			// Fixed mapping at current end of mmap arena
			result = addr_g;
			nextfree += length;
		} else if (addr_g >= machine.memory.mmap_start() && addr_g + length <= nextfree) {
			// Fixed mapping inside mmap arena
			result = addr_g;
		} else if (addr_g > nextfree) {
			// Fixed mapping after current end of mmap arena
			// TODO: Evaluate if relaxation is counter-productive with the new cache
			result = addr_g;
		} else {
			machine.set_result(address_type<W>(-1)); // = MAP_FAILED;
			SYSPRINT("<<< mmap(addr 0x%lX, len %zu, ...) = 0x%lX (MAP_FAILED)\n",
					(long)addr_g, (size_t)length, -1L);
			return;
		}

		// anon pages need to be zeroed
		if (flags & MAP_ANONYMOUS) {
			machine.memory.memdiscard(result, length, true);
		}

		machine.memory.set_page_attr(result, length, attr);
		machine.set_result(result);
		SYSPRINT("<<< mmap(addr 0x%lX, len %zu, ...) = 0x%lX\n",
				(long)addr_g, (size_t)length, (long)result);
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
		machine.set_result(address_type<W>(-1));
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
		SYSPRINT(">>> madvise(0x%lX, len=%zu, advice=%x)\n",
			(uint64_t)addr, (size_t)len, advice);
		switch (advice) {
			case 0: // MADV_NORMAL
			case 1: // MADV_RANDOM
			case 2: // MADV_SEQUENTIAL
			case 3: // MADV_WILLNEED:
			case 15: // MADV_NOHUGEPAGE
			case 18: // MADV_WIPEONFORK
				machine.set_result(0);
				return;
			case 4: // MADV_DONTNEED
				machine.memory.memdiscard(addr, len, true);
				machine.set_result(0);
				return;
			case 8: // MADV_FREE
			case 9: // MADV_REMOVE
				machine.memory.free_pages(addr, len);
				machine.set_result(0);
				return;
			case -1: // Work-around for Zig behavior
				machine.set_result(-EINVAL);
				return;
			default:
				throw MachineException(SYSTEM_CALL_FAILED,
					"Unimplemented madvise() advice", advice);
		}
	});
}
