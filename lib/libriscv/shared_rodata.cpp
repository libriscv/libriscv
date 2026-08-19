#include "shared_rodata.hpp"

#include <cstring>
#include <mutex>
#include <unordered_map>

// Sharing needs a sealable anonymous file that can be mapped over a sub-range
// of the arena, ie. a sealed memfd, which is Linux-only.
#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
// Bionic declares memfd_create from API level 30 only, and the older libc stubs
// do not export it either, so Android below that cannot seal a memfd. The
// MFD_* and F_SEAL_* macros come from the kernel headers and are present
// regardless, so they cannot be used to detect this on their own.
#if defined(__ANDROID__) && (!defined(__ANDROID_API__) || __ANDROID_API__ < 30)
#define RISCV_NO_MEMFD_CREATE 1
#endif
#if defined(MFD_CLOEXEC) && defined(MFD_ALLOW_SEALING) && defined(F_ADD_SEALS) \
	&& defined(F_SEAL_WRITE) && !defined(RISCV_NO_MEMFD_CREATE)
#define RISCV_HAS_SHARED_RODATA 1
#endif
#endif

namespace std {
	template <>
	struct hash<riscv::RodataKey> {
		size_t operator()(const riscv::RodataKey& key) const {
			size_t h = size_t(key.rodata_end) ^ (size_t(key.arena_size) * 31);
			for (const auto& seg : key.segments) {
				h = h * 1099511628211ull ^ size_t(seg.vaddr);
				h = h * 1099511628211ull ^ size_t(seg.size);
			}
			return h;
		}
	};
}

namespace riscv
{
	// The cache only observes images, so the last machine to let go of one also
	// frees the memfd. Two different programs can share a layout, so one key can
	// hold several images, told apart by comparing their contents.
	static std::mutex rodata_mutex;
	static std::unordered_multimap<RodataKey, std::weak_ptr<SharedRodataImage>> rodata_images;

	bool shared_rodata_supported() noexcept
	{
#ifdef RISCV_HAS_SHARED_RODATA
		return true;
#else
		return false;
#endif
	}

	SharedRodataImage::~SharedRodataImage()
	{
#ifdef RISCV_HAS_SHARED_RODATA
		if (m_fd >= 0)
			close(m_fd);
#endif
	}

	bool SharedRodataImage::map_over([[maybe_unused]] void* arena) const noexcept
	{
#ifdef RISCV_HAS_SHARED_RODATA
		if (m_fd < 0 || m_end == 0)
			return false;
		// MAP_FIXED replaces the anonymous arena pages atomically, splitting the
		// arena mapping in two. On failure the original mapping is left in place.
		void* result = mmap(arena, m_end, PROT_READ,
			MAP_SHARED | MAP_FIXED, m_fd, 0);
		return result == arena;
#else
		return false;
#endif
	}

	bool SharedRodataImage::inspect([[maybe_unused]] const Inspector& fn) const noexcept
	{
#ifdef RISCV_HAS_SHARED_RODATA
		if (m_fd < 0 || m_end == 0)
			return false;
		void* view = mmap(NULL, m_end, PROT_READ, MAP_SHARED, m_fd, 0);
		if (view == MAP_FAILED)
			return false;
		bool result = false;
		try {
			result = fn((const uint8_t *)view, size_t(m_end));
		} catch (...) {
			result = false;
		}
		munmap(view, m_end);
		return result;
#else
		return false;
#endif
	}

#ifdef RISCV_HAS_SHARED_RODATA
	// Try each image with this layout, dropping the dead ones, until one
	// verifies. Caller holds the lock.
	static std::shared_ptr<SharedRodataImage> find_verified(
		const RodataKey& key, const SharedRodataImage::Inspector& verify)
	{
		auto range = rodata_images.equal_range(key);
		for (auto it = range.first; it != range.second; )
		{
			auto image = it->second.lock();
			if (image == nullptr) {
				// The last machine using this image is gone
				it = rodata_images.erase(it);
				continue;
			}
			if (image->inspect(verify))
				return image;
			++it;
		}
		return nullptr;
	}
#endif

	std::shared_ptr<SharedRodataImage> find_shared_rodata_image(
		[[maybe_unused]] const RodataKey& key,
		[[maybe_unused]] const SharedRodataImage::Inspector& verify)
	{
#ifdef RISCV_HAS_SHARED_RODATA
		std::lock_guard<std::mutex> lock(rodata_mutex);
		return find_verified(key, verify);
#else
		return nullptr;
#endif
	}

	std::shared_ptr<SharedRodataImage> create_shared_rodata_image(
		[[maybe_unused]] const RodataKey& key,
		[[maybe_unused]] const void* src, [[maybe_unused]] size_t len)
	{
#ifdef RISCV_HAS_SHARED_RODATA
		if (len == 0)
			return nullptr;

		std::lock_guard<std::mutex> lock(rodata_mutex);

		// Creating an image is rare and the lock is already held, so sweep the
		// entries of programs that were loaded once and never again
		for (auto it = rodata_images.begin(); it != rodata_images.end(); ) {
			if (it->second.expired())
				it = rodata_images.erase(it);
			else
				++it;
		}

		// Another thread may have created the image while we were loading
		auto existing = find_verified(key,
			[src, len] (const uint8_t* image, size_t image_len) {
				return image_len == len && std::memcmp(image, src, len) == 0;
			});
		if (existing != nullptr)
			return existing;

		int fd = memfd_create("libriscv-rodata", MFD_CLOEXEC | MFD_ALLOW_SEALING);
		if (fd < 0)
			return nullptr;

		if (ftruncate(fd, len) != 0) {
			close(fd);
			return nullptr;
		}

		// Fill the image through a temporary writable mapping
		void* fill = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (fill == MAP_FAILED) {
			close(fd);
			return nullptr;
		}
		std::memcpy(fill, src, len);
		munmap(fill, len);

		// Seal permanently. F_SEAL_WRITE requires that no writable mapping
		// exists, which is why the fill mapping is already gone.
		if (fcntl(fd, F_ADD_SEALS,
			F_SEAL_SEAL | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE) != 0)
		{
			close(fd);
			return nullptr;
		}

		auto image = std::make_shared<SharedRodataImage>(fd, len);
		rodata_images.emplace(key, image);
		return image;
#else
		return nullptr;
#endif
	}
}
