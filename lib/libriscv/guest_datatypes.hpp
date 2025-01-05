#pragma once
#include "native_heap.hpp" // arena()

namespace riscv {

// View into libstdc++'s std::string
template <int W>
struct GuestStdString {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	static constexpr std::size_t SSO = 15;

	gaddr_t ptr;
	gaddr_t size;
	union {
		char data[SSO + 1];
		gaddr_t capacity;
	};

	constexpr GuestStdString() : ptr(0), size(0), capacity(0) {}
	GuestStdString(machine_t& machine, std::string_view str)
		: ptr(0), size(0), capacity(0)
	{
		this->set_string(machine, 0, str);
	}

	std::string to_string(machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		if (this->size <= SSO)
			return std::string(data, size);
		else if (this->size > max_len)
			throw std::runtime_error("Guest std::string too large (size > 16MB)");
		// Copy the string from guest memory
		const auto view = machine.memory.memview(ptr, size);
		return std::string(view.data(), view.size());
	}

	std::string_view to_view(machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		if (this->size <= SSO)
			return std::string_view(data, size);
		else if (this->size > max_len)
			throw std::runtime_error("Guest std::string too large (size > 16MB)");
		// View the string from guest memory
		return machine.memory.memview(ptr, size);
	}

	void set_string(machine_t& machine, gaddr_t self, const void* str, std::size_t len)
	{
		this->free(machine);

		if (len <= SSO)
		{
			this->ptr = self + offsetof(GuestStdString, data);
			this->size = len;
			std::memcpy(this->data, str, len);
			this->data[len] = '\0';
		}
		else
		{
			this->ptr = machine.arena().malloc(len);
			this->size = len;
			this->capacity = len;
			machine.copy_to_guest(this->ptr, str, len);
		}
	}
	void set_string(machine_t& machine, gaddr_t self, std::string_view str)
	{
		this->set_string(machine, self, str.data(), str.size());
	}

	gaddr_t move(gaddr_t self)
	{
		if (size <= SSO) {
			this->ptr = self + offsetof(GuestStdString, data);
		}
		return self;
	}

	void free(machine_t& machine)
	{
		if (size > SSO) {
			machine.arena().free(ptr);
		}
		this->ptr = 0;
		this->size = 0;
	}
};

// View into libstdc++ and LLVM libc++ std::vector (same layout)
template <int W>
struct GuestStdVector {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;

	gaddr_t ptr_begin;
	gaddr_t ptr_end;
	gaddr_t ptr_capacity;

	constexpr GuestStdVector() : ptr_begin(0), ptr_end(0), ptr_capacity(0) {}

	template <typename T>
	GuestStdVector(machine_t& machine, const std::vector<T>& vec)
		: ptr_begin(0), ptr_end(0), ptr_capacity(0)
	{
		if constexpr (std::is_same_v<T, std::string>) {
			// Specialization for std::vector<std::string>
			this->alloc<GuestStdString<W>>(machine, vec.size());
			for (std::size_t i = 0; i < vec.size(); i++)
				this->set_string(machine, i, vec[i]);
		} else {
			this->set(machine, vec);
		}
	}

	gaddr_t data() const noexcept { return ptr_begin; }
	std::size_t size_bytes() const noexcept { return ptr_end - ptr_begin; }
	std::size_t capacity() const noexcept { return ptr_capacity - ptr_begin; }

	template <typename T>
	std::size_t size() const {
		return size_bytes() / sizeof(T);
	}

	template <typename T>
	T& at(machine_t& machine, std::size_t index, std::size_t max_bytes = 16UL << 20) {
		if (index >= size<T>())
			throw std::out_of_range("Guest std::vector index out of range");
		return view_as<T>(machine, max_bytes)[index];
	}
	template <typename T>
	const T& at(machine_t& machine, std::size_t index, std::size_t max_bytes = 16UL << 20) const {
		if (index >= size<T>())
			throw std::out_of_range("Guest std::vector index out of range");
		return view_as<T>(machine, max_bytes)[index];
	}

	// Helper for setting a std::string at a given index in a std::vector<std::string>
	void set_string(machine_t& machine, std::size_t index, const std::string& str) {
		this->at<GuestStdString<W>>(machine, index).set_string(machine, address_at<GuestStdString<W>>(index), str);
	}

	template <typename T>
	gaddr_t address_at(std::size_t index) const {
		if (index >= size<T>())
			throw std::out_of_range("Guest std::vector index out of range");
		return ptr_begin + index * sizeof(T);
	}

	template <typename T>
	T *view_as(const machine_t& machine, std::size_t max_bytes = 16UL << 20) {
		return machine.memory.template memarray<T>(data(), size<T>(max_bytes));
	}
	template <typename T>
	const T *view_as(const machine_t& machine, std::size_t max_bytes = 16UL << 20) const {
		return machine.memory.template memarray<T>(data(), size<T>(max_bytes));
	}

	template <typename T>
	std::vector<T> to_vector(const machine_t& machine) const {
		if (size_bytes() > capacity())
			throw std::runtime_error("Guest std::vector has size > capacity");
		// Copy the vector from guest memory
		const size_t elements = size_bytes() / sizeof(T);
		const T *array = machine.memory.template memarray<T>(data(), elements);
		return std::vector<T>(&array[0], &array[elements]);
	}

	template <typename T>
	inline std::tuple<T *, gaddr_t> alloc(machine_t& machine, std::size_t elements) {
		this->free(machine);

		this->ptr_begin = machine.arena().malloc(elements * sizeof(T));
		this->ptr_end = this->ptr_begin + elements * sizeof(T);
		this->ptr_capacity = this->ptr_end;
		return { machine.memory.template memarray<T>(this->data(), elements), this->data() };
	}

	template <typename T>
	void set(machine_t& machine, const std::vector<T>& vec)
	{
		auto [array, self] = alloc<T>(machine, vec.size());
		(void)self;
		std::copy(vec.begin(), vec.end(), array);
	}

	template <typename T>
	inline void assign_shared(gaddr_t shared_addr, std::size_t elements) {
		this->ptr_begin = shared_addr;
		this->ptr_end = shared_addr + elements * sizeof(T);
		this->ptr_capacity = this->ptr_end;
	}

	void free(machine_t& machine) {
		if (capacity() > 0)
			machine.arena().free(this->data());
	}
};

} // namespace riscv
