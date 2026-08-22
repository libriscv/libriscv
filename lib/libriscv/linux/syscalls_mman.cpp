/// Linux memory mapping system call emulation
/// Works on all platforms
#define LINUX_MAP_ANONYMOUS        0x20
#define LINUX_MAP_NORESERVE     0x04000
#define LINUX_MAP_FIXED         0x10
#define LINUX_MAP_FIXED_NOREPLACE 0x100000

template <int W>
static void add_mman_syscalls()
{
	// munmap
	Machine<W>::install_syscall_handler(215,
	[] (Machine<W>& machine) {
		const auto addr = machine.sysarg(0);
		const auto len  = machine.sysarg(1);
		if (addr + len < addr)
			throw MachineException(SYSTEM_CALL_FAILED, "munmap() arguments overflow");
		machine.memory.free_pages(addr, len);
		if (addr >= machine.memory.mmap_start() && addr + len <= machine.memory.mmap_address()) {
			machine.memory.mmap_unmap(addr, len);
		}
		machine.set_result(0);
		SYSPRINT(">>> munmap(0x%lX, len=%zu) => %d\n",
			(long)addr, (size_t)len, (int)machine.return_value());
	});
	// mmap
	Machine<W>::install_syscall_handler(222,
	[] (Machine<W>& machine) {
		const auto addr_g = machine.sysarg(0);
		auto length       = machine.sysarg(1);
		const auto prot   = machine.template sysarg<int>(2);
		auto flags        = machine.template sysarg<int>(3);
		const auto vfd    = machine.template sysarg<int>(4);
		const auto voff   = machine.sysarg(5);
		PageAttributes attr{
			.read  = (prot & 1) != 0,
			.write = (prot & 2) != 0,
			.exec  = (prot & 4) != 0,
		};
		SYSPRINT(">>> mmap(addr 0x%lX, len %zu, prot %#x, flags %#X, vfd=%d voff=%zu)\n",
				(long)addr_g, (size_t)length, prot, flags, vfd, size_t(voff));
		#define MMAP_HAS_FAILED() { \
			machine.set_result(address_type<W>(-1)); \
			SYSPRINT("<<< mmap(addr 0x%lX, len %zu, ...) = MAP_FAILED\n", (long)addr_g, (size_t)length); \
			return; \
		}
		#define MMAP_IS_TAKEN() { \
			machine.set_result(-EEXIST); \
			SYSPRINT("<<< mmap(addr 0x%lX, len %zu, ...) = EEXIST\n", (long)addr_g, (size_t)length); \
			return; \
		}

		if (addr_g % Page::size() != 0)
			MMAP_HAS_FAILED();

		auto& nextfree = machine.memory.mmap_address();
		const auto prev_nextfree = nextfree;
		length = (length + PageMask) & ~address_type<W>(PageMask);
		if (length == 0)
			MMAP_HAS_FAILED();
		address_type<W> result = address_type<W>(-1);

		if (vfd != -1)
		{
			if (machine.has_file_descriptors())
			{
				const int real_fd = machine.fds().translate(vfd);

				address_type<W> dst = 0x0;
				if (addr_g == 0x0) {
					if (nextfree + length < nextfree)
						MMAP_HAS_FAILED();
					dst = nextfree;
					nextfree += length;
				} else {
					if (addr_g + length < addr_g)
						MMAP_HAS_FAILED();
					dst = addr_g;
				}
				// Make the area read-write
				machine.memory.set_page_attr(dst, length, PageAttributes{});
				// Readv into the area
				std::array<riscv::vBuffer, 256> buffers;
				const size_t cnt =
					machine.memory.gather_writable_buffers_from_range(buffers.size(), buffers.data(), dst, length);
				// Seek to the given offset in the file and read the contents into guest memory
#ifdef _WIN32
				if (_lseek(real_fd, voff, SEEK_SET) == -1L)
					MMAP_HAS_FAILED();
				for (size_t i = 0; i < cnt; i++) {
					if (_read(real_fd, buffers.at(i).ptr, buffers.at(i).len) != buffers.at(i).len)
						MMAP_HAS_FAILED();
				}
#elif defined(__wasm__)
				if (voff != 0) // lseek: Not supported
					MMAP_HAS_FAILED();
				if (readv(real_fd, (const iovec*)&buffers[0], cnt) < 0)
					MMAP_HAS_FAILED();
#else
				if (lseek(real_fd, voff, SEEK_SET) == (off_t)-1)
					MMAP_HAS_FAILED();
				if (readv(real_fd, (const iovec*)&buffers[0], cnt) < 0)
					MMAP_HAS_FAILED();
#endif
				// Set new page protections on area
				machine.memory.set_page_attr(dst, length, attr);
				machine.set_result(dst);
				return;
			}
		else
		{
			// File descriptors are disabled: report failure like the
			// kernel would, instead of throwing a machine exception
			MMAP_HAS_FAILED();
		}
		}
		else if (addr_g != 0 && (flags & LINUX_MAP_FIXED_NOREPLACE) != 0)
		{
			// MAP_FIXED_NOREPLACE maps at exactly the given address or fails
			// with EEXIST, and must never be relocated: guests use the error
			// to probe for a free region.
			if (addr_g + length < addr_g || addr_g < machine.memory.mmap_start())
				MMAP_IS_TAKEN();
			if constexpr (riscv::encompassing_Nbit_arena > 0) {
				if (addr_g + length > riscv::encompassing_arena_mask)
					MMAP_IS_TAKEN();
			}
			if (!riscv::virtual_paging_enabled
				&& addr_g + length > machine.memory.memory_arena_size())
				MMAP_IS_TAKEN();
			if (addr_g >= nextfree) {
				// Untouched territory: reserve it by moving the arena cursor
				// above it, and donate the skipped-over gap to the free list
				// so that later kernel-chosen mappings can still use it.
				if (addr_g > nextfree)
					machine.memory.mmap_cache().insert(nextfree, addr_g - nextfree);
				nextfree = addr_g + length;
			} else if (!machine.memory.mmap_cache().carve(addr_g, length)) {
				// Below the cursor and not entirely free: occupied
				MMAP_IS_TAKEN();
			}
			result = addr_g;
		}
		else if (addr_g == 0 || (flags & LINUX_MAP_FIXED) == 0)
		{
			// Without MAP_FIXED the address is only a hint that the kernel is
			// free to ignore, so always allocate inside the mmap arena instead.
			// Guest hints regularly point outside of it, eg. V8 hints at
			// randomized addresses spread over the whole 64-bit address space.
			auto range = machine.memory.mmap_cache().find(length);
			// Not found in cache, increment MM base address
			if (range.empty()) {
				if (nextfree + length < nextfree)
					MMAP_HAS_FAILED();
				if (!riscv::virtual_paging_enabled
					&& nextfree + length > machine.memory.memory_arena_size())
					MMAP_HAS_FAILED();
				result = nextfree;
				nextfree += length;
			}
			else
			{
				result = range.addr;
			}
		} else if (!riscv::virtual_paging_enabled
			&& addr_g + length > machine.memory.memory_arena_size()) {
			MMAP_HAS_FAILED();
		} else if (addr_g < machine.memory.mmap_start()) {
			// A fixed range below the mmap arena start, we do nothing except return the address
			result    = addr_g;
		} else if (addr_g + length > addr_g && addr_g + length <= nextfree) {
			// Fixed mapping inside mmap arena: it may land on top of a range
			// that is sitting in the free list, which must not be handed out
			// a second time.
			if (!machine.memory.mmap_cache().carve(addr_g, length))
				machine.memory.mmap_cache().invalidate(addr_g, length);
			result = addr_g;
		} else if (addr_g + length > addr_g) {
			// Fixed mapping ending after the current end of the mmap arena
			// TODO: Evaluate if relaxation is counter-productive with the new cache
			if constexpr (riscv::encompassing_Nbit_arena > 0) {
				// We have to force the address to be within the arena
				if (addr_g + length > riscv::encompassing_arena_mask) {
					if (nextfree + length < nextfree || nextfree + length > riscv::encompassing_arena_mask)
						MMAP_HAS_FAILED();
					result = nextfree;
					nextfree += length;
				} else {
					if (addr_g > nextfree)
						machine.memory.mmap_cache().insert(nextfree, addr_g - nextfree);
					else
						machine.memory.mmap_cache().invalidate(addr_g, length);
					result = addr_g;
					nextfree = std::max(nextfree, address_type<W>(addr_g + length));
				}
			} else {
				if (addr_g > nextfree)
					machine.memory.mmap_cache().insert(nextfree, addr_g - nextfree);
				else
					machine.memory.mmap_cache().invalidate(addr_g, length);
				result = addr_g;
				nextfree = std::max(nextfree, address_type<W>(addr_g + length));
			}
		} else {
			MMAP_HAS_FAILED();
		}

		const bool untouched = (result >= prev_nextfree);
		// anon pages need to be zeroed
		if (flags & LINUX_MAP_ANONYMOUS) {
			machine.memory.memdiscard(result, length, true);
		}
		// avoid potentially creating pages when MAP_NORESERVE is set
		if ((flags & LINUX_MAP_NORESERVE) == 0 && !(prot == 0 && untouched))
		{
			machine.memory.set_page_attr(result, length, attr);
		}
		machine.set_result(result);
		SYSPRINT("<<< mmap(addr 0x%lX, len %zu, ...) = 0x%lX\n",
				(long)addr_g, (size_t)length, (long)result);
	});
	// mremap
	Machine<W>::install_syscall_handler(216,
	[] (Machine<W>& machine) {
		[[maybe_unused]] static constexpr int LINUX_MREMAP_MAYMOVE = 0x0001;
		[[maybe_unused]] static constexpr int LINUX_MREMAP_FIXED   = 0x0002;
		const auto old_addr = machine.sysarg(0);
		const auto old_size = machine.sysarg(1);
		const auto new_size = machine.sysarg(2);
		const auto flags    = machine.template sysarg<int>(3);
		SYSPRINT(">>> mremap(addr 0x%lX, len %zu, newsize %zu, flags %#X)\n",
				(long)old_addr, (size_t)old_size, (size_t)new_size, flags);
		auto& nextfree = machine.memory.mmap_address();
		// We allow the common case of reallocating the
		// last mapping to a bigger one
		if ((flags & LINUX_MREMAP_FIXED) != 0) {
			// Not supported
		}
		else if (old_addr + old_size >= old_addr && old_addr + old_size == nextfree
			&& old_addr + new_size >= old_addr) {
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
		if (addr % Page::size() != 0) {
			machine.set_result(-EINVAL);
			SYSPRINT(">>> mprotect(0x%lX, len=%zu, prot=%x) => %d\n",
				(long)addr, (size_t)len, prot, (int)machine.return_value());
			return;
		}
		if (addr + len < addr) {
			machine.set_result(-ENOMEM);
			SYSPRINT(">>> mprotect(0x%lX, len=%zu, prot=%x) => %d\n",
				(long)addr, (size_t)len, prot, (int)machine.return_value());
			return;
		}
		machine.memory.set_page_attr(addr, len, {
			.read  = (prot & 1) != 0,
			.write = (prot & 2) != 0,
			.exec  = (prot & 4) != 0
		});
		machine.set_result(0);
		SYSPRINT(">>> mprotect(0x%lX, len=%zu, prot=%x) => %d\n",
			(long)addr, (size_t)len, prot, (int)machine.return_value());
	});
	// madvise
	Machine<W>::install_syscall_handler(233,
	[] (Machine<W>& machine) {
		const auto addr  = machine.sysarg(0);
		const auto len   = machine.sysarg(1);
		const int advice = machine.template sysarg<int> (2);
		switch (advice) {
			case 0: // MADV_NORMAL
			case 1: // MADV_RANDOM
			case 2: // MADV_SEQUENTIAL
			case 3: // MADV_WILLNEED:
			case 10: // MADV_DONTFORK
			case 11: // MADV_DOFORK
			case 12: // MADV_MERGEABLE
			case 15: // MADV_NOHUGEPAGE
			case 18: // MADV_WIPEONFORK
				machine.set_result(0);
				break;
			case 4: // MADV_DONTNEED
				machine.memory.memdiscard(addr, len, true);
				machine.set_result(0);
				break;
			case 8: // MADV_FREE
			case 9: // MADV_REMOVE
				machine.memory.free_pages(addr, len);
				machine.set_result(0);
				break;
			case -1: // Work-around for Zig behavior
				machine.set_result(-EINVAL);
				break;
			default:
				throw MachineException(SYSTEM_CALL_FAILED,
					"Unimplemented madvise() advice", advice);
		}
		SYSPRINT(">>> madvise(0x%lX, len=%zu, advice=%x) => %d\n",
			(uint64_t)addr, (size_t)len, advice, (int)machine.return_value());
	});
}
