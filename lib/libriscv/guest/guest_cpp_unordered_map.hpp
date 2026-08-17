#pragma once
#include "guest_arena_object.hpp"
#include "guest_cpp_hash.hpp"
#include <algorithm>
#include <unordered_map>

namespace riscv {

/// @brief Mirrors std::pair<const K, V>, the value_type of std::unordered_map
template <typename K, typename V>
struct GuestStdPair {
	alignas(guest_alignof_v<K>) K first;
	alignas(guest_alignof_v<V>) V second;
};

/// @brief Mirrors libstdc++'s __detail::_Hash_node, a node in the singly-
/// linked list of every element in an unordered container. The hash code is
/// only stored in the node for slow hash functions, eg. std::hash<std::string>.
template <int W, typename K, typename V, bool CacheHashCode>
struct GuestHashNode;

template <int W, typename K, typename V>
struct GuestHashNode<W, K, V, false> {
	alignas(guest_word_align<W>) riscv::address_type<W> next; // _Hash_node_base::_M_nxt
	GuestStdPair<K, V> value;    // _Hash_node_value_base::_M_storage
};

template <int W, typename K, typename V>
struct GuestHashNode<W, K, V, true> {
	alignas(guest_word_align<W>) riscv::address_type<W> next;
	GuestStdPair<K, V> value;
	alignas(guest_word_align<W>) riscv::address_type<W> hash_code; // _Hash_node_code_cache::_M_hash_code
};

// View into libstdc++'s std::unordered_map<K, V>
//
// The map consists of an array of buckets, and a singly-linked list that
// holds every element in the map. A bucket does not point at its first
// element, but at the element *before* it, which is why the map has a
// before_begin member that acts as the head of the node list. As a result,
// a non-empty map contains a pointer back into itself, and it must be told
// when it is moved to another address (see move()).
//
// NOTE: The map operations that can allocate need to know the address of the
// map itself in guest memory, and it is passed to them as the self argument.
//
// NOTE: libstdc++ decides the node layout from the hash function of the map:
// the hash code of every key is stored in its node unless __is_fast_hash<H>,
// which is true for every hash function except the ones the standard library
// marks as slow (std::hash<std::string> among them). A map that uses a custom
// hash function - even one that just forwards to std::hash - therefore does
// *not* store hash codes, and must be given CacheHashCode = false in order to
// match the guest. Getting it wrong shifts every key and value in the node.
template <int W, typename K, typename V, bool CacheHashCode>
struct alignas(guest_word_align<W>) GuestStdUnorderedMap
{
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	using key_type = K;
	using mapped_type = V;
	using hasher = GuestStdHash<W, K>;
	/// @brief True when the guest stores the hash code of each key in its node
	static constexpr bool cache_hash_code = CacheHashCode;
	using node_type  = GuestHashNode<W, K, V, cache_hash_code>;
	using value_type = GuestStdPair<K, V>;

	// The libstdc++ _Hashtable layout (7 machine words)
	gaddr_t buckets;       // __node_base_ptr* _M_buckets
	gaddr_t bucket_cnt;    // size_type      _M_bucket_count
	gaddr_t before_begin;  // __node_base    _M_before_begin (a lone _M_nxt)
	gaddr_t element_cnt;   // size_type      _M_element_count
	float   max_load;      // _Prime_rehash_policy::_M_max_load_factor
	// The word after the float is padded back to a whole word by the guest,
	// even on a host that would have packed it right behind the float
	alignas(guest_word_align<W>) gaddr_t next_resize; // _Prime_rehash_policy::_M_next_resize
	gaddr_t single_bucket; // __node_base_ptr _M_single_bucket

	// Offsets of the members that the map itself points at
	static constexpr std::size_t BEFORE_BEGIN_OFF  = 2 * sizeof(gaddr_t);
	static constexpr std::size_t SINGLE_BUCKET_OFF = 6 * sizeof(gaddr_t);
	// Offsets of the key and the value inside a node
	static constexpr std::size_t KEY_OFF =
		offsetof(node_type, value) + offsetof(value_type, first);
	static constexpr std::size_t VALUE_OFF =
		offsetof(node_type, value) + offsetof(value_type, second);

	/// @brief Create an empty map. NOTE: It is not usable by the guest until
	/// it has been given its own guest address using move().
	constexpr GuestStdUnorderedMap() noexcept
		: buckets(0), bucket_cnt(1), before_begin(0), element_cnt(0),
		  max_load(1.0f), next_resize(0), single_bucket(0) {}

	/// @brief Create an empty map that lives at the guest address self.
	GuestStdUnorderedMap(machine_t& machine, gaddr_t self)
		: GuestStdUnorderedMap()
	{
		this->move(machine, self);
	}

	/// @brief Create a map at the guest address self, filled with the
	/// key/value pairs of a host container, eg. a std::unordered_map.
	template <typename Container>
	GuestStdUnorderedMap(machine_t& machine, gaddr_t self, const Container& container)
		: GuestStdUnorderedMap()
	{
		this->move(machine, self);
		this->assign(machine, self, container);
	}

	// Copying is intentionally shallow/fast, like GuestStdVector. A shallow
	// copy shares the nodes with the original, and it must be given its own
	// address with move() before the guest can use it.
	GuestStdUnorderedMap(const GuestStdUnorderedMap& other) = default;
	GuestStdUnorderedMap& operator=(const GuestStdUnorderedMap& other) = default;

	std::size_t size() const noexcept { return this->element_cnt; }
	bool empty() const noexcept { return this->element_cnt == 0; }
	std::size_t bucket_count() const noexcept { return this->bucket_cnt; }
	float max_load_factor() const noexcept { return this->max_load; }
	float load_factor() const noexcept {
		return float(this->element_cnt) / float(this->bucket_cnt);
	}

	void set_max_load_factor(float mlf) {
		this->max_load = mlf;
		if (this->bucket_cnt > 1)
			this->next_resize = floor_size(this->bucket_cnt * double(mlf));
	}

	/// @brief Find the value of the given key.
	/// @return A pointer into guest memory, or null when not found.
	template <typename KeyArg>
	V* find(const machine_t& machine, const KeyArg& key) const {
		const gaddr_t addr = this->find_node(machine, key);
		if (addr == 0)
			return nullptr;
		return &this->node_at(machine, addr).value.second;
	}

	/// @brief Find the value of the given key, or throw std::out_of_range.
	template <typename KeyArg>
	V& at(const machine_t& machine, const KeyArg& key) const {
		if (V* value = this->find(machine, key))
			return *value;
		throw std::out_of_range("Guest std::unordered_map: Key not found");
	}

	template <typename KeyArg>
	bool contains(const machine_t& machine, const KeyArg& key) const {
		return this->find_node(machine, key) != 0;
	}
	template <typename KeyArg>
	std::size_t count(const machine_t& machine, const KeyArg& key) const {
		return this->contains(machine, key) ? 1u : 0u;
	}

	/// @brief Insert a key/value pair, overwriting the value of an existing key.
	/// @param self The address of this map in guest memory.
	/// @return The guest address of the node that holds the pair.
	template <typename KeyArg, typename ValueArg>
	gaddr_t insert_or_assign(machine_t& machine, gaddr_t self,
		const KeyArg& key, const ValueArg& value)
	{
		const gaddr_t code = hasher::hash(machine, key);
		if (this->element_cnt != 0) {
			const gaddr_t prev =
				this->find_before_node(machine, this->bucket_index(code), key, code);
			if (prev != 0) {
				const gaddr_t addr = this->next_ref(machine, prev);
				free_member(machine, this->node_at(machine, addr).value.second);
				store_member<V>(machine, addr + VALUE_OFF, value);
				return addr;
			}
		}
		// Grow the bucket array first, exactly like libstdc++ does
		this->maybe_rehash(machine, self, 1);

		const gaddr_t addr = machine.arena().malloc(sizeof(node_type));
		if (addr == 0)
			throw std::bad_alloc();
		node_type& node = this->node_at(machine, addr);
		node.next = 0;
		if constexpr (cache_hash_code)
			node.hash_code = code;
		store_member<K>(machine, addr + KEY_OFF, key);
		store_member<V>(machine, addr + VALUE_OFF, value);

		this->insert_bucket_begin(machine, self, this->bucket_index(code), addr);
		this->element_cnt += 1;
		return addr;
	}

	/// @brief Insert a key/value pair, unless the key already exists.
	/// @return The guest address of the node that holds the pair.
	template <typename KeyArg, typename ValueArg>
	gaddr_t try_emplace(machine_t& machine, gaddr_t self,
		const KeyArg& key, const ValueArg& value)
	{
		if (const gaddr_t addr = this->find_node(machine, key); addr != 0)
			return addr;
		return this->insert_or_assign(machine, self, key, value);
	}

	/// @brief Remove the given key from the map.
	/// @return True when an element was removed.
	template <typename KeyArg>
	bool erase(machine_t& machine, gaddr_t self, const KeyArg& key)
	{
		if (this->element_cnt == 0)
			return false;
		const gaddr_t code = hasher::hash(machine, key);
		const std::size_t bkt = this->bucket_index(code);
		const gaddr_t prev = this->find_before_node(machine, bkt, key, code);
		if (prev == 0)
			return false;

		const gaddr_t addr = this->next_ref(machine, prev);
		node_type& node = this->node_at(machine, addr);
		const gaddr_t next = node.next;
		// The bucket of the next node decides whether this bucket becomes empty
		const std::size_t next_bkt = next != 0
			? this->bucket_index(this->node_hash(machine, this->node_at(machine, next)))
			: bkt;

		free_member(machine, node.value.first);
		free_member(machine, node.value.second);
		machine.arena().free(addr);
		this->element_cnt -= 1;

		gaddr_t* bkts = this->bucket_array(machine);
		if (prev == bkts[bkt])
			this->remove_bucket_begin(machine, self, bkt, next, next_bkt);
		else if (next_bkt != bkt)
			bkts[next_bkt] = prev;
		this->next_ref(machine, prev) = next;
		return true;
	}

	/// @brief Remove every element, keeping the bucket array.
	void clear(machine_t& machine)
	{
		gaddr_t addr = this->before_begin;
		while (addr != 0) {
			node_type& node = this->node_at(machine, addr);
			const gaddr_t next = node.next;
			free_member(machine, node.value.first);
			free_member(machine, node.value.second);
			machine.arena().free(addr);
			addr = next;
		}
		this->before_begin = 0;
		this->element_cnt = 0;
		if (this->buckets != 0)
			std::memset(this->bucket_array(machine), 0, this->bucket_cnt * sizeof(gaddr_t));
	}

	/// @brief Free every element and the bucket array. The map is detached
	/// from guest memory afterwards: use move() to make it usable again.
	void free(machine_t& machine)
	{
		this->clear(machine);
		if (this->bucket_cnt != 1 && this->buckets != 0)
			machine.arena().free(this->buckets);
		this->buckets = 0;
		this->bucket_cnt = 1;
		this->single_bucket = 0;
		this->next_resize = 0;
	}

	/// @brief Tell the map that it now lives at the guest address self,
	/// updating the pointers that point back into the map itself.
	void move(machine_t& machine, gaddr_t self)
	{
		if (this->bucket_cnt == 1) {
			// libstdc++ uses the single_bucket member as a 1-element bucket array
			this->buckets = self + SINGLE_BUCKET_OFF;
		}
		if (this->before_begin != 0) {
			// Exactly one bucket points at the before_begin member: the bucket
			// that holds the first node of the global list of nodes.
			gaddr_t* bkts = this->bucket_array(machine);
			const std::size_t bkt = this->bucket_index(
				this->node_hash(machine, this->node_at(machine, this->before_begin)));
			bkts[bkt] = self + BEFORE_BEGIN_OFF;
		}
	}

	/// @brief Replace the contents with the key/value pairs of a host
	/// container, eg. a std::unordered_map or a std::vector of pairs.
	template <typename Container>
	void assign(machine_t& machine, gaddr_t self, const Container& container)
	{
		this->clear(machine);
		this->reserve(machine, self, container.size());
		for (const auto& entry : container)
			this->insert_or_assign(machine, self, entry.first, entry.second);
	}

	/// @brief Visit every element in the map, in an unspecified order. The
	/// callback is invoked as callback(const K& key, V& value).
	template <typename F>
	void for_each(const machine_t& machine, F&& callback) const
	{
		gaddr_t addr = this->before_begin;
		while (addr != 0) {
			node_type& node = this->node_at(machine, addr);
			addr = node.next;
			callback(const_cast<const K&>(node.value.first), node.value.second);
		}
	}

	using host_key_type    = guest_host_type_t<W, K>;
	using host_mapped_type = guest_host_type_t<W, V>;

	/// @brief Copy the whole map to the host. Nested guest containers are not
	/// supported here, use for_each() for those.
	std::unordered_map<host_key_type, host_mapped_type> to_map(const machine_t& machine) const
	{
		static_assert(is_host_convertible_guest_object<W, K>::value
			&& is_host_convertible_guest_object<W, V>::value,
			"Guest std::unordered_map: Nested guest containers must be read using for_each()");
		std::unordered_map<host_key_type, host_mapped_type> result;
		result.reserve(this->size());
		this->for_each(machine, [&] (const K& key, const V& value) {
			result.emplace(guest_object_to_host<W>(machine, key),
				guest_object_to_host<W>(machine, value));
		});
		return result;
	}

	/// @brief Set the bucket count to at least count, rehashing the elements.
	/// Mirrors std::unordered_map::rehash().
	void rehash(machine_t& machine, gaddr_t self, std::size_t count)
	{
		const gaddr_t saved_resize = this->next_resize;
		std::size_t buckets =
			std::max(this->bkt_for_elements(std::size_t(this->element_cnt) + 1), count);
		buckets = this->next_bucket_count(buckets);
		if (buckets != this->bucket_cnt)
			this->rehash_buckets(machine, self, buckets);
		else // No rehash: keep the resize threshold consistent with the state
			this->next_resize = saved_resize;
	}

	/// @brief Make room for count elements without rehashing later.
	/// Mirrors std::unordered_map::reserve().
	void reserve(machine_t& machine, gaddr_t self, std::size_t count)
	{
		this->rehash(machine, self, this->bkt_for_elements(count));
	}

	/// @brief The guest address of the node of the given key, or zero.
	template <typename KeyArg>
	gaddr_t find_node(const machine_t& machine, const KeyArg& key) const
	{
		if (this->element_cnt == 0)
			return 0;
		const gaddr_t code = hasher::hash(machine, key);
		const gaddr_t prev =
			this->find_before_node(machine, this->bucket_index(code), key, code);
		return prev != 0 ? this->next_ref(machine, prev) : 0;
	}

	/// @brief View a node in guest memory, given its address.
	node_type& node_at(const machine_t& machine, gaddr_t addr) const {
		return *machine.memory.template memarray<node_type>(addr, 1);
	}

	/// @brief The bucket array in guest memory. When there is only a single
	/// bucket it is the single_bucket member of the map itself.
	gaddr_t *bucket_array(const machine_t& machine) const {
		if (this->buckets == 0)
			throw std::runtime_error("Guest std::unordered_map has no address in guest memory (see move())");
		return machine.memory.template memarray<gaddr_t>(this->buckets, this->bucket_cnt);
	}

	/// @brief The bucket that a hash code belongs to (__detail::_Mod_range_hashing)
	std::size_t bucket_index(gaddr_t code) const {
		// A guest-provided map always has at least one bucket
		if (this->bucket_cnt == 0)
			throw std::runtime_error("Guest std::unordered_map has zero buckets");
		return code % this->bucket_cnt;
	}

	/// @brief The hash code of the key of a node, cached or recomputed.
	gaddr_t node_hash(const machine_t& machine, const node_type& node) const {
		if constexpr (cache_hash_code)
			return node.hash_code;
		else
			return hasher::hash(machine, node.value.first);
	}

	/// @brief The _M_nxt member of a node (or of before_begin), which is
	/// always the first word of a node base.
	gaddr_t& next_ref(const machine_t& machine, gaddr_t node_base) const {
		return *machine.memory.template memarray<gaddr_t>(node_base, 1);
	}

	/// @brief The address of the node *before* the node of the given key, as
	/// stored in the buckets. Mirrors _M_find_before_node().
	template <typename KeyArg>
	gaddr_t find_before_node(const machine_t& machine, std::size_t bkt,
		const KeyArg& key, gaddr_t code) const
	{
		gaddr_t* bkts = this->bucket_array(machine);
		gaddr_t prev = bkts[bkt];
		if (prev == 0)
			return 0;
		for (gaddr_t addr = this->next_ref(machine, prev); addr != 0; ) {
			node_type& node = this->node_at(machine, addr);
			if (this->node_equals(machine, node, key, code))
				return prev;
			// Stop at the end of the list, or when leaving this bucket
			if (node.next == 0)
				break;
			if (this->bucket_index(this->node_hash(machine, this->node_at(machine, node.next))) != bkt)
				break;
			prev = addr;
			addr = node.next;
		}
		return 0;
	}

	template <typename KeyArg>
	bool node_equals(const machine_t& machine, const node_type& node,
		const KeyArg& key, gaddr_t code) const
	{
		if constexpr (cache_hash_code) {
			if (node.hash_code != code)
				return false;
		} else {
			(void)code;
		}
		return hasher::equals(machine, node.value.first, key);
	}

	/// @brief Grow the bucket array when inserting n_ins elements would
	/// exceed the load factor. Mirrors _Prime_rehash_policy::_M_need_rehash().
	/// @return True when the map was rehashed.
	bool maybe_rehash(machine_t& machine, gaddr_t self, std::size_t n_ins)
	{
		if (std::size_t(this->element_cnt) + n_ins <= this->next_resize)
			return false;
		// A zero resize threshold means that no buckets have been allocated
		// yet, in which case libstdc++ starts out with at least 11 buckets
		const std::size_t elements = std::max(std::size_t(this->element_cnt) + n_ins,
			this->next_resize == 0 ? std::size_t(11) : std::size_t(0));
		const double min_buckets = elements / double(this->max_load);
		if (min_buckets >= double(this->bucket_cnt)) {
			const std::size_t count = this->next_bucket_count(
				std::max(floor_size(min_buckets) + 1, std::size_t(this->bucket_cnt) * 2));
			this->rehash_buckets(machine, self, count);
			return true;
		}
		this->next_resize = floor_size(this->bucket_cnt * double(this->max_load));
		return false;
	}

	/// @brief The bucket count that libstdc++ would choose for count buckets,
	/// which also updates the resize threshold. Mirrors _M_next_bkt().
	std::size_t next_bucket_count(std::size_t count)
	{
		using policy = GuestStdHashPolicy;
		if (count < sizeof(policy::fast_bkt)) {
			if (count == 0) {
				// Keep the threshold at zero, so that the next insertion allocates
				return 1;
			}
			this->next_resize = floor_size(policy::fast_bkt[count] * double(this->max_load));
			return policy::fast_bkt[count];
		}
		// The last prime is never selected by the search, just like in libstdc++
		const uint32_t* const last = policy::primes + policy::prime_count - 1;
		const uint32_t* const it = std::lower_bound(policy::primes + 6, last, count);
		if (it == last) {
			// Never rehash again, as we have reached the biggest bucket count
			this->next_resize = std::size_t(-1);
			return std::max(count, std::size_t(*last));
		}
		this->next_resize = floor_size(*it * double(this->max_load));
		return *it;
	}

	/// @brief The bucket count needed to hold count elements
	std::size_t bkt_for_elements(std::size_t count) const {
		return ceil_size(count / double(this->max_load));
	}

	std::size_t size_bytes() const noexcept {
		return this->bucket_cnt * sizeof(gaddr_t) + this->element_cnt * sizeof(node_type);
	}

	/// @brief Move every node into a newly allocated bucket array.
	/// Mirrors _M_rehash_aux() for containers with unique keys.
	void rehash_buckets(machine_t& machine, gaddr_t self, std::size_t count)
	{
		const gaddr_t bbegin = self + BEFORE_BEGIN_OFF;
		gaddr_t new_buckets;
		if (count == 1) {
			this->single_bucket = 0;
			new_buckets = self + SINGLE_BUCKET_OFF;
		} else {
			new_buckets = machine.arena().malloc(count * sizeof(gaddr_t));
			if (new_buckets == 0)
				throw std::bad_alloc();
			std::memset(machine.memory.template memarray<gaddr_t>(new_buckets, count),
				0, count * sizeof(gaddr_t));
		}
		gaddr_t* bkts = machine.memory.template memarray<gaddr_t>(new_buckets, count);

		// Re-insert every node, which also reverses the order of the list
		gaddr_t addr = this->before_begin;
		this->before_begin = 0;
		std::size_t bbegin_bkt = 0;
		while (addr != 0) {
			node_type& node = this->node_at(machine, addr);
			const gaddr_t next = node.next;
			const std::size_t bkt = this->node_hash(machine, node) % count;
			if (bkts[bkt] == 0) {
				node.next = this->before_begin;
				this->before_begin = addr;
				bkts[bkt] = bbegin;
				if (node.next != 0)
					bkts[bbegin_bkt] = addr;
				bbegin_bkt = bkt;
			} else {
				gaddr_t& prev_next = this->next_ref(machine, bkts[bkt]);
				node.next = prev_next;
				prev_next = addr;
			}
			addr = next;
		}

		if (this->bucket_cnt != 1 && this->buckets != 0)
			machine.arena().free(this->buckets);
		this->bucket_cnt = count;
		this->buckets = new_buckets;
	}

	/// @brief Insert a node at the beginning of a bucket.
	/// Mirrors _M_insert_bucket_begin().
	void insert_bucket_begin(machine_t& machine, gaddr_t self, std::size_t bkt, gaddr_t addr)
	{
		gaddr_t* bkts = this->bucket_array(machine);
		if (bkts[bkt] != 0) {
			// The bucket is not empty: insert after the node before the bucket
			gaddr_t& prev_next = this->next_ref(machine, bkts[bkt]);
			this->node_at(machine, addr).next = prev_next;
			prev_next = addr;
		} else {
			// The bucket is empty: the node becomes the first node of the
			// list, and the bucket points at our before_begin member
			node_type& node = this->node_at(machine, addr);
			node.next = this->before_begin;
			this->before_begin = addr;
			if (node.next != 0) {
				// Update the bucket that pointed at before_begin
				const std::size_t old_bkt = this->bucket_index(
					this->node_hash(machine, this->node_at(machine, node.next)));
				bkts[old_bkt] = addr;
			}
			bkts[bkt] = self + BEFORE_BEGIN_OFF;
		}
	}

	/// @brief Unlink the first node of a bucket. Mirrors _M_remove_bucket_begin().
	void remove_bucket_begin(machine_t& machine, gaddr_t self, std::size_t bkt,
		gaddr_t next, std::size_t next_bkt)
	{
		if (next == 0 || next_bkt != bkt) {
			// The bucket is now empty
			gaddr_t* bkts = this->bucket_array(machine);
			if (next != 0)
				bkts[next_bkt] = bkts[bkt];
			if (bkts[bkt] == self + BEFORE_BEGIN_OFF)
				this->before_begin = next;
			bkts[bkt] = 0;
		}
	}

	/// @brief Construct a key or a value of a node in guest memory
	template <typename T, typename Arg>
	static void store_member(machine_t& machine, gaddr_t addr, const Arg& src)
	{
		T& dst = *machine.memory.template memarray<T>(addr, 1);
		construct_guest_object<W, T>(machine, addr, dst, src);
	}

	/// @brief Free a key or a value of a node
	template <typename T>
	static void free_member(machine_t& machine, T& member) {
		free_guest_object<W>(machine, member);
	}

	/// @brief Tell a key or a value that it has moved to another address
	template <typename T>
	static void relocate_member(machine_t& machine, T& member, gaddr_t addr) {
		relocate_guest_object<W>(machine, member, addr);
	}

private:
	// std::floor and std::ceil for non-negative values
	static std::size_t floor_size(double value) noexcept {
		return value > 0.0 ? std::size_t(value) : 0u;
	}
	static std::size_t ceil_size(double value) noexcept {
		const std::size_t result = floor_size(value);
		return double(result) < value ? result + 1 : result;
	}
};

// Verify the layouts against libstdc++ (verified with GCC 12 and 14)
template <int W>
struct GuestStdUnorderedMapLayout {
	using map_type = GuestStdUnorderedMap<W, int, int>;
	static constexpr std::size_t WORD = sizeof(riscv::address_type<W>);
	static_assert(sizeof(map_type) == 7 * WORD, "The guest std::unordered_map is 7 words");
	static_assert(offsetof(map_type, buckets) == 0 * WORD, "_M_buckets");
	static_assert(offsetof(map_type, bucket_cnt) == 1 * WORD, "_M_bucket_count");
	static_assert(offsetof(map_type, before_begin) == 2 * WORD, "_M_before_begin");
	static_assert(offsetof(map_type, element_cnt) == 3 * WORD, "_M_element_count");
	static_assert(offsetof(map_type, max_load) == 4 * WORD, "_M_max_load_factor");
	static_assert(offsetof(map_type, next_resize) == 5 * WORD, "_M_next_resize");
	static_assert(offsetof(map_type, single_bucket) == 6 * WORD, "_M_single_bucket");
	static_assert(alignof(map_type) == WORD, "Aligned like a guest word");
	// A 64-bit value is 8-aligned on a 32-bit guest too, which is a layout the
	// host does not always share (see guest_alignof in guest_common.hpp)
	using wide_pair = GuestStdPair<int32_t, int64_t>;
	static_assert(sizeof(wide_pair) == 16, "pair<int, int64_t>");
	static_assert(offsetof(wide_pair, second) == 8, "pair<int, int64_t>");
};
template struct GuestStdUnorderedMapLayout<4>;
template struct GuestStdUnorderedMapLayout<8>;

// The node sizes, as measured with RISC-V GCC 12 and 14
static_assert(sizeof(GuestHashNode<4, int, int, false>) == 12, "_Hash_node<pair<int, int>>");
static_assert(sizeof(GuestHashNode<8, int, int, false>) == 16, "_Hash_node<pair<int, int>>");
static_assert(sizeof(GuestHashNode<4, GuestStdString<4>, int, true>) == 36, "_Hash_node<pair<string, int>>");
static_assert(sizeof(GuestHashNode<8, GuestStdString<8>, int, true>) == 56, "_Hash_node<pair<string, int>>");
// A string-keyed map with a custom hash function has no cached hash code
static_assert(sizeof(GuestHashNode<4, GuestStdString<4>, int, false>) == 32, "_Hash_node<pair<string, int>>");
static_assert(sizeof(GuestHashNode<8, GuestStdString<8>, int, false>) == 48, "_Hash_node<pair<string, int>>");
static_assert(sizeof(GuestHashNode<4, GuestStdString<4>, GuestStdString<4>, true>) == 56, "_Hash_node<pair<string, string>>");
static_assert(sizeof(GuestHashNode<8, GuestStdString<8>, GuestStdString<8>, true>) == 80, "_Hash_node<pair<string, string>>");
// A 64-bit value is 8-aligned on a 32-bit guest too, which is a layout the host
// does not always share (see guest_alignof in guest_common.hpp)
static_assert(sizeof(GuestHashNode<4, GuestStdString<4>, double, true>) == 48, "_Hash_node<pair<string, double>>");
static_assert(sizeof(GuestHashNode<8, GuestStdString<8>, double, true>) == 56, "_Hash_node<pair<string, double>>");

/// @brief A guest std::unordered_map that lives in the arena, and which
/// frees itself (and every node) at the end of the scope.
template <int W, typename K, typename V,
	bool CacheHashCode = GuestStdHash<W, K>::cache_hash_code>
using ScopedGuestStdUnorderedMap = ScopedArenaObject<W, GuestStdUnorderedMap<W, K, V, CacheHashCode>>;

template <int W, typename T>
struct is_scoped_guest_stdunordered_map : std::false_type {};

template <int W, typename K, typename V, bool CacheHashCode>
struct is_scoped_guest_stdunordered_map<W, ScopedArenaObject<W, GuestStdUnorderedMap<W, K, V, CacheHashCode>>> : std::true_type {};

} // riscv
