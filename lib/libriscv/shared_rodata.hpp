#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace riscv
{
	/// @brief A sealed, read-only image of a programs initial read-only data.
	/// @details Machines loaded from the same binary map the same image over the
	/// low part of their arena, so that the text and rodata of the program exists
	/// in host memory exactly once. The image spans guest memory [0, end), where
	/// end is the page-aligned end of the read-only ELF segments; everything
	/// below the programs load address is zeroes.
	struct SharedRodataImage
	{
		SharedRodataImage(int fd, uint64_t end) noexcept
			: m_fd(fd), m_end(end) {}
		~SharedRodataImage();
		SharedRodataImage(const SharedRodataImage&) = delete;
		SharedRodataImage& operator=(const SharedRodataImage&) = delete;

		/// @brief The page-aligned end of the shared region, in guest memory.
		uint64_t end() const noexcept { return m_end; }

		/// @brief Map the image read-only over [arena, arena + end()).
		/// @details The arena keeps its original mapping when this fails.
		bool map_over(void* arena) const noexcept;

		/// @brief Map the whole image privately, hand it to fn, unmap it again.
		/// @details Returns false when the image could not be mapped.
		using Inspector = std::function<bool(const uint8_t* image, size_t len)>;
		bool inspect(const Inspector& fn) const noexcept;

	private:
		// Unused on platforms without sealable anonymous files
		[[maybe_unused]] int m_fd;
		uint64_t m_end;
	};

	/// @brief One read-only region of a program, in guest memory.
	struct RodataSegment
	{
		uint64_t vaddr = 0;
		uint64_t size  = 0;

		bool operator==(const RodataSegment& other) const noexcept {
			return vaddr == other.vaddr && size == other.size;
		}
	};

	/// @brief Identifies the read-only layout of one program in one arena.
	/// @details The layout only selects the candidates, whose contents are then
	/// compared byte for byte. Segments are sorted by address, so the key does
	/// not depend on the order of the program headers.
	struct RodataKey
	{
		uint64_t rodata_end = 0;
		uint64_t arena_size = 0;
		std::vector<RodataSegment> segments;

		bool operator==(const RodataKey& other) const noexcept {
			return rodata_end == other.rodata_end
				&& arena_size == other.arena_size
				&& segments == other.segments;
		}
	};

	/// @brief True when this platform can share read-only memory between machines.
	bool shared_rodata_supported() noexcept;

	/// @brief Find an image with this layout whose contents verify() accepts.
	/// @details verify() is called with a read-only mapping of each candidate,
	/// and must compare every byte: the layout alone is not identity enough.
	std::shared_ptr<SharedRodataImage> find_shared_rodata_image(
		const RodataKey&, const SharedRodataImage::Inspector& verify);

	/// @brief Create an image for this key from len bytes at src, and remember it
	/// for the next machine that asks for the same key. Returns an existing image
	/// when an identical one was created in the meantime, and nullptr when
	/// unsupported or when the image could not be created.
	std::shared_ptr<SharedRodataImage> create_shared_rodata_image(const RodataKey&, const void* src, size_t len);
}
