#pragma once
#include "guest_rust_box.hpp"
#include "guest_rust_enum.hpp"
#include <map>

// A guest-side bag of named, dynamically typed attributes, zero-copy in both
// directions:
//
//   #[repr(C)] pub struct Attr { pub key: String, pub value: Value }
//   #[repr(C)] pub struct Attributes { entries: Vec<Attr> }   // sorted by key
//
// A sorted vector rather than a HashMap, on purpose. std::HashMap is hashbrown,
// whose table layout is #[repr(Rust)] and whose default hasher is seeded per
// process, so mirroring it would mean betting on both. A sorted Vec is ordinary
// Rust, and also *faster* here: one allocation for all the entries instead of a
// node per key, and no hasher the two sides have to agree on. A guest that wants
// a real HashMap is one .collect() away. Keys compare as bytes, which is what
// Rust's Ord for str does, so host order and guest order agree.
//
// The tree frees itself: free() drops the vector, which drops each Attr, which
// drops the key and the value, and a value owning a nested Box<Attributes> drops
// that too - all through the free_guest_object dispatch in guest_common.hpp, so
// the recursion is nowhere written out. It is the host-side counterpart of
// Rust's drop glue, and why there is no hand-written destroy function like the
// one the C++ attribute tree needs.

namespace riscv {

/// @brief One entry of a guest attribute map: #[repr(C)] struct Attr.
///
/// A tuple would have been the obvious choice, but its layout is unspecified and
/// the compiler may reorder the fields. A #[repr(C)] struct costs nothing.
template <int W, typename Value>
struct GuestRustAttr {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	using value_type = Value;

	GuestRustString<W> key;
	alignas(guest_alignof_v<Value>) Value value;

	constexpr GuestRustAttr() noexcept : key(), value() {}

	GuestRustAttr(machine_t& machine, std::string_view name)
		: key(machine, name), value() {}

	/// @brief Create an entry whose value is built from a host value
	template <typename Arg>
	GuestRustAttr(machine_t& machine, std::string_view name, const Arg& val)
		: key(machine, name), value()
	{
		this->value.set(machine, val);
	}

	GuestRustAttr(const GuestRustAttr& other) = default;
	GuestRustAttr& operator=(const GuestRustAttr& other) = default;

	/// @brief Drop the key and the value, recursively
	void free(machine_t& machine) {
		free_guest_object<W>(machine, this->key);
		free_guest_object<W>(machine, this->value);
	}
};

/// @brief A guest attribute map: #[repr(C)] struct Attributes, a Vec of
/// entries kept sorted by key.
template <int W, typename Value>
struct GuestRustAttributes {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	using entry_type = GuestRustAttr<W, Value>;
	using vector_type = GuestRustVec<W, entry_type>;
	using key_type = std::string;
	using mapped_type = Value;

	vector_type entries;

	constexpr GuestRustAttributes() noexcept : entries() {}

	explicit GuestRustAttributes(machine_t&) noexcept : entries() {}

	/// @brief Create a map filled with the entries of a host container of
	/// key/value pairs, eg. a std::map or a std::unordered_map.
	template <typename Container>
	GuestRustAttributes(machine_t& machine, const Container& container)
		: entries()
	{
		this->assign(machine, container);
	}
	/// @brief Overload ignoring the map's own guest address, which it does not need
	template <typename Arg>
	GuestRustAttributes(machine_t& machine, gaddr_t /*self*/, const Arg& arg)
		: GuestRustAttributes(machine, arg) {}

	// Copying is shallow, like the other guest containers
	GuestRustAttributes(const GuestRustAttributes& other) = default;
	GuestRustAttributes& operator=(const GuestRustAttributes& other) = default;

	std::size_t size() const noexcept { return this->entries.size(); }
	bool empty() const noexcept { return this->entries.empty(); }
	std::size_t capacity() const noexcept { return this->entries.capacity(); }

	/// @brief Make room for count entries in one allocation
	void reserve(machine_t& machine, std::size_t count) {
		this->entries.reserve(machine, count);
	}

	/// @brief The value of the given key, or null when it is not there.
	Value* find(const machine_t& machine, std::string_view key) const
	{
		const std::size_t count = this->entries.size();
		if (count == 0)
			return nullptr;
		entry_type* array = this->entry_array(machine);
		const std::size_t i = this->lower_bound(machine, array, count, key);
		if (i < count && this->key_of(machine, array[i]) == key)
			return &array[i].value;
		return nullptr;
	}

	/// @brief The value of the given key, or throw std::out_of_range.
	Value& at(const machine_t& machine, std::string_view key) const
	{
		Value* value = this->find(machine, key);
		if (value == nullptr)
			throw std::out_of_range("Guest Rust attribute not found: " + std::string(key));
		return *value;
	}

	bool contains(const machine_t& machine, std::string_view key) const {
		return this->find(machine, key) != nullptr;
	}

	/// @brief Insert an entry, overwriting the value of an existing key.
	/// @return A reference to the value, in guest memory.
	template <typename Arg>
	Value& insert_or_assign(machine_t& machine, std::string_view key, const Arg& value)
	{
		Value& dst = this->entry_for(machine, key);
		dst.set(machine, value);
		return dst;
	}

	/// @brief Insert an entry whose value is an explicitly chosen variant, eg.
	/// emplace<GuestRustBox<W, Attributes>>(machine, "player") for a group.
	/// @return A reference to the new variant, in guest memory.
	template <typename T, typename... Args>
	T& emplace(machine_t& machine, std::string_view key, Args&&... args)
	{
		Value& dst = this->entry_for(machine, key);
		return dst.template emplace<T>(machine, std::forward<Args>(args)...);
	}

	/// @brief Remove the given key.
	/// @return True when it was there.
	bool erase(machine_t& machine, std::string_view key)
	{
		const std::size_t count = this->entries.size();
		if (count == 0)
			return false;
		entry_type* array = this->entry_array(machine);
		const std::size_t i = this->lower_bound(machine, array, count, key);
		if (i >= count || this->key_of(machine, array[i]) != key)
			return false;

		array[i].free(machine);
		if (i + 1 < count)
			move_entries(&array[i], &array[i + 1], count - i - 1);
		this->entries.len -= 1;
		return true;
	}

	/// @brief Drop every entry, keeping the allocation.
	void clear(machine_t& machine) {
		this->entries.clear(machine);
	}

	/// @brief Visit every entry in key order, as (std::string_view, Value&).
	template <typename F>
	void for_each(const machine_t& machine, F&& callback) const
	{
		const std::size_t count = this->entries.size();
		if (count == 0)
			return;
		entry_type* array = this->entry_array(machine);
		for (std::size_t i = 0; i < count; i++)
			callback(this->key_of(machine, array[i]), array[i].value);
	}

	/// @brief Replace the contents with the entries of a host container of
	/// key/value pairs. The entries end up sorted, in one allocation.
	template <typename Container>
	void assign(machine_t& machine, const Container& container)
	{
		this->free(machine);
		this->reserve(machine, container.size());
		for (const auto& [key, value] : container)
			this->insert_or_assign(machine, std::string_view(key), value);
	}
	template <typename Container>
	void assign(machine_t& machine, gaddr_t /*self*/, const Container& container) {
		this->assign(machine, container);
	}

	/// @brief Copy the whole map to the host. Only valid when the value type can
	/// become a host value on its own: read a tree with for_each() instead.
	template <typename V = Value>
	auto to_map(const machine_t& machine) const
		-> std::map<std::string, decltype(std::declval<const V&>().to_variant(machine))>
	{
		std::map<std::string, decltype(std::declval<const V&>().to_variant(machine))> out;
		this->for_each(machine, [&] (std::string_view key, const Value& value) {
			out.emplace(std::string(key), value.to_variant(machine));
		});
		return out;
	}

	/// @brief Drop every entry and release the allocation, recursively, the way
	/// the guest's drop glue would.
	void free(machine_t& machine) {
		this->entries.free(machine);
	}

	// --- Validation of guest-provided trees ----------------------------------
	//
	// A script is adversarial input: it can hand over an Attributes whose length
	// exceeds its capacity, or whose entries are out of order. Neither can
	// corrupt host memory - the vector bounds every access and a failed binary
	// search only fails to find - but a walk should still be bounded, and a
	// scrambled order noticed rather than silently missed.

	/// @brief Check that the map is usable and no larger than max_entries.
	void validate(const machine_t& machine, std::size_t max_entries = 4096) const
	{
		if (this->entries.size() > this->entries.capacity())
			throw std::runtime_error("Guest Rust Attributes has len > capacity");
		if (this->entries.size() > max_entries)
			throw std::runtime_error("Guest Rust Attributes has more entries than allowed");
		// Faults here if the vector does not point at readable guest memory
		if (!this->entries.empty())
			(void)this->entry_array(machine);
	}

	/// @brief True when the entries are in the order that find() needs, which is
	/// what both the host and the guest-side type maintain.
	bool is_sorted(const machine_t& machine) const
	{
		const std::size_t count = this->entries.size();
		if (count < 2)
			return true;
		entry_type* array = this->entry_array(machine);
		for (std::size_t i = 1; i < count; i++) {
			if (this->key_of(machine, array[i - 1]) >= this->key_of(machine, array[i]))
				return false;
		}
		return true;
	}

private:
	/// @brief Relocate entries inside the guest array.
	///
	/// A Rust move is always a memcpy, and no Rust container points back into
	/// itself, so moving the bytes is the whole operation. The cast says so to
	/// the compiler, which otherwise warns that the host mirror is not trivially
	/// copyable (it has a move constructor, for the host's benefit).
	static void move_entries(entry_type* dst, const entry_type* src, std::size_t count) {
		std::memmove(static_cast<void*>(dst), static_cast<const void*>(src),
			count * sizeof(entry_type));
	}

	entry_type* entry_array(const machine_t& machine) const {
		return const_cast<vector_type&>(this->entries).as_array(machine);
	}

	static std::string_view key_of(const machine_t& machine, const entry_type& entry) {
		return entry.key.to_view(machine);
	}

	/// @brief The index of the first entry whose key is not less than key.
	std::size_t lower_bound(const machine_t& machine, entry_type* array,
		std::size_t count, std::string_view key) const
	{
		std::size_t lo = 0, hi = count;
		while (lo < hi) {
			const std::size_t mid = lo + (hi - lo) / 2;
			if (this->key_of(machine, array[mid]) < key)
				lo = mid + 1;
			else
				hi = mid;
		}
		return lo;
	}

	/// @brief The value slot of the given key, inserting a default-valued
	/// entry in sorted position when it is not there yet.
	Value& entry_for(machine_t& machine, std::string_view key)
	{
		const std::size_t count = this->entries.size();
		std::size_t i = count;
		if (count != 0) {
			entry_type* array = this->entry_array(machine);
			i = this->lower_bound(machine, array, count, key);
			if (i < count && this->key_of(machine, array[i]) == key)
				return array[i].value;
		}

		// Make room, and build the entry, before taking the host pointer to the
		// array: reserve() may move the array, so the pointer has to be the last
		// thing taken before the bytes are touched.
		this->entries.reserve(machine, count + 1);
		entry_type entry(machine, key);

		entry_type* array = machine.memory.template memarray<entry_type>(
			this->entries.ptr, count + 1);
		if (i < count)
			move_entries(&array[i + 1], &array[i], count - i);
		// A shallow copy, which transfers ownership of the key allocation
		new (&array[i]) entry_type(entry);
		this->entries.len += 1;
		return array[i].value;
	}
};

/// @brief A guest attribute map that lives in the arena, and which frees
/// itself and the whole tree below it at the end of the scope.
template <int W, typename Value>
using ScopedGuestRustAttributes = ScopedArenaObject<W, GuestRustAttributes<W, Value>>;

} // riscv
