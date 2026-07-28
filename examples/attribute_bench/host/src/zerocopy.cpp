#include "zerocopy.hpp"

#include <cstdio>
#include <new>
#include <stdexcept>

namespace bench {

namespace {
	using Limits = GuestAttrLimits;

	/// @brief Nodes visited so far, across the whole tree
	struct NodeBudget { std::size_t nodes = 0; };

	[[noreturn]] void reject(const char* what)
	{
		throw std::runtime_error(std::string("Guest attributes: ") + what);
	}

	void readMap(Script& script, const GAttrMap& map, HostAttributes& out,
		std::size_t level, NodeBudget& budget);

	/// @brief Read one scalar alternative. Shared by map values and list
	/// elements, which is why it returns the base variant.
	template <typename Variant>
	BaseValue readBaseValue(Script& script, const Variant& value)
	{
		switch (value.index()) {
		case INT64:  return *value.template get_if<int64_t>();
		case FLOAT:  return *value.template get_if<double>();
		case VEC3:   return *value.template get_if<vec3>();
		case VEC4:   return *value.template get_if<vec4>();
		case DVEC3:  return *value.template get_if<dvec3>();
		case DVEC2:  return *value.template get_if<dvec2>();
		case STRING: return std::string(value.template get_if<GuestString>()
			->to_view(script.machine(), Limits::MAX_STRING));
		case BOOL:   return *value.template get_if<bool>();
		default:
			reject("unknown value type");
		}
	}

	/// @brief Read a guest std::vector<AttributeBaseValue> behind a unique_ptr.
	HostAttributes::AttributeListPtr readList(Script& script, Script::gaddr_t addr,
		NodeBudget& budget)
	{
		auto list = std::make_unique<std::vector<BaseValue>>();
		if (addr == 0) // A moved-from unique_ptr: an empty list, not an error
			return list;

		const GAttrList& glist = *script.machine().memory.memarray<GAttrList>(addr, 1);
		if (glist.ptr_end < glist.ptr_begin || glist.ptr_end > glist.ptr_capacity)
			reject("list has inconsistent begin/end/capacity");
		const std::size_t bytes = glist.ptr_end - glist.ptr_begin;
		if (bytes % sizeof(GAttrBase) != 0)
			reject("list size is not a multiple of the element size");

		const std::size_t count = bytes / sizeof(GAttrBase);
		if (count > Limits::MAX_LIST)
			reject("list is too long");
		if (count == 0)
			return list;
		budget.nodes += count;
		if (budget.nodes > Limits::MAX_NODES)
			reject("too many values in the attribute tree");

		const GAttrBase* elements =
			script.machine().memory.memarray<GAttrBase>(glist.ptr_begin, count);
		list->reserve(count);
		for (std::size_t i = 0; i < count; i++)
			list->emplace_back(readBaseValue(script, elements[i]));
		return list;
	}

	void readValue(Script& script, const GAttrValue& value, std::string&& key,
		HostAttributes& out, std::size_t level, NodeBudget& budget)
	{
		switch (value.index()) {
		case GROUP: {
			auto& group = out.addGroup(std::move(key));
			// A moved-from unique_ptr is a null group, which is simply empty
			if (const auto addr = value.get_if<GuestGroupPtr>()->addr; addr != 0)
				readMap(script, guestAttributes(script, addr), group, level + 1, budget);
			break;
		}
		case LIST:
			out.getAllAttributes().emplace_back(std::move(key),
				readList(script, value.get_if<GuestListPtr>()->addr, budget));
			break;
		default:
			std::visit([&] (auto&& v) { out.set(std::move(key), std::move(v)); },
				readBaseValue(script, value));
			break;
		}
	}

	void readMap(Script& script, const GAttrMap& map, HostAttributes& out,
		std::size_t level, NodeBudget& budget)
	{
		if (level > Limits::MAX_DEPTH)
			reject("too many nested groups");

		const std::size_t count = map.size();
		if (count > Limits::MAX_ENTRIES)
			reject("too many entries");
		budget.nodes += count;
		if (budget.nodes > Limits::MAX_NODES)
			reject("too many values in the attribute tree");

		// Walk the node list ourselves with an explicit step budget, rather than
		// trusting element_cnt: a cycle would otherwise spin here forever.
		std::size_t steps = 0;
		Script::gaddr_t addr = map.before_begin;
		while (addr != 0) {
			if (++steps > count)
				reject("more nodes than the element count (cyclic node list?)");
			const auto& node = map.node_at(script.machine(), addr);
			std::string key(node.value.first.to_view(script.machine(), Limits::MAX_KEY));
			readValue(script, node.value.second, std::move(key), out, level, budget);
			addr = node.next;
		}
		if (steps != count)
			reject("fewer nodes than the element count");
	}
} // namespace

GAttrMap& guestAttributes(Script& script, Script::gaddr_t addr)
{
	// memarray faults on an unmapped or misaligned address, so what is left to
	// check is the internal state that the walk and the insert trust
	GAttrMap& map = *script.machine().memory.memarray<GAttrMap>(addr, 1);
	if (map.bucket_cnt < 1 || map.bucket_cnt > Limits::MAX_BUCKETS)
		reject("bogus bucket count");
	if (map.element_cnt > Limits::MAX_ENTRIES)
		reject("too many entries");
	if (map.element_cnt != 0 && map.before_begin == 0)
		reject("non-empty map without a node list");
	// The bucket arithmetic divides by the load factor, so a zero or non-finite
	// one would turn a rehash into an unbounded allocation (or worse)
	if (!(map.max_load > 0.0f) || map.max_load > 64.0f)
		reject("bogus max load factor");
	return map;
}

void readGuestAttributes(Script& script, Script::gaddr_t addr, HostAttributes& out)
{
	if (addr == 0)
		return;
	NodeBudget budget;
	readMap(script, guestAttributes(script, addr), out, 0, budget);
}

// --- host -> guest ----------------------------------------------------------

namespace {
	/// @brief View a guest object as a writable T. Every allocating map operation
	/// needs the object's own guest address, so the two always travel together.
	template <typename T>
	T& objectAt(Script& script, Script::gaddr_t addr)
	{
		return *script.machine().memory.memarray<T>(addr, 1);
	}

	/// @brief Allocate an empty guest map on the guest heap, which is the arena
	/// the guest's own malloc uses -- so the guest may take ownership.
	Script::gaddr_t createGuestMap(Script& script)
	{
		const Script::gaddr_t addr = script.guest_alloc(sizeof(GAttrMap));
		if (addr == 0)
			throw std::bad_alloc();
		// A non-empty map points back into itself, so it is built knowing where it lives
		new (&objectAt<GAttrMap>(script, addr)) GAttrMap(script.machine(), addr);
		return addr;
	}

	Script::gaddr_t createGuestList(Script& script)
	{
		const Script::gaddr_t addr = script.guest_alloc(sizeof(GAttrList));
		if (addr == 0)
			throw std::bad_alloc();
		new (&objectAt<GAttrList>(script, addr)) GAttrList();
		return addr;
	}

	void writeMap(Script& script, Script::gaddr_t self, const HostAttributes& attrs,
		std::size_t level);

	/// @brief Build a guest std::vector<AttributeBaseValue> for a list attribute.
	/// @return The guest address of the vector, for the unique_ptr alternative.
	Script::gaddr_t writeList(Script& script, const std::vector<BaseValue>& list)
	{
		auto& machine = script.machine();
		if (list.size() > Limits::MAX_LIST)
			reject("list is too long");
		const Script::gaddr_t addr = createGuestList(script);
		if (list.empty())
			return addr;   // an empty vector is three null pointers

		GAttrList& glist = objectAt<GAttrList>(script, addr);
		glist.resize(machine, list.size());
		for (std::size_t i = 0; i < list.size(); i++) {
			// Each element is told its own address, so a short string can point
			// at its in-place SSO buffer
			const Script::gaddr_t element = glist.address_at(i);
			std::visit([&] (const auto& value) {
				glist.at(machine, i).set(machine, element, value);
			}, list[i]);
		}
		return addr;
	}

	void writeValue(Script& script, GAttrMap& map, Script::gaddr_t self,
		const std::string& key, const HostAttributes::Attribute& value, std::size_t level)
	{
		auto& machine = script.machine();
		switch (value.index()) {
		case GROUP: {
			const auto& group = std::get<HostAttributes::AttributesPtr>(value);
			const Script::gaddr_t addr = createGuestMap(script);
			if (group)
				writeMap(script, addr, *group, level + 1);
			map.insert_or_assign(machine, self, key, GuestGroupPtr{addr});
			break;
		}
		case LIST: {
			static const std::vector<BaseValue> empty;
			const auto& list = std::get<HostAttributes::AttributeListPtr>(value);
			map.insert_or_assign(machine, self, key,
				GuestListPtr{writeList(script, list ? *list : empty)});
			break;
		}
		case STRING:
			map.insert_or_assign(machine, self, key, std::get<std::string>(value));
			break;
		default:
			// Every remaining alternative is a scalar the variant can select on
			// its own, and it lives at the same index on both sides
			std::visit([&] (const auto& scalar) {
				using T = std::decay_t<decltype(scalar)>;
				if constexpr (!std::is_same_v<T, HostAttributes::AttributesPtr>
					&& !std::is_same_v<T, HostAttributes::AttributeListPtr>
					&& !std::is_same_v<T, std::string>)
				{
					map.insert_or_assign(machine, self, key, scalar);
				}
			}, value);
			break;
		}
	}

	void writeMap(Script& script, Script::gaddr_t self, const HostAttributes& attrs,
		std::size_t level)
	{
		if (level > Limits::MAX_DEPTH)
			reject("too many nested groups");

		const auto& entries = attrs.getAllAttributes();
		if (entries.empty())
			return;

		GAttrMap& map = guestAttributes(script, self);
		// One bucket array for the whole insert, instead of a rehash every few keys
		map.reserve(script.machine(), self, map.size() + entries.size());

		for (const auto& [key, value] : entries)
			writeValue(script, map, self, key, value, level);
	}

	void freeMap(Script& script, Script::gaddr_t self, std::size_t level)
	{
		if (level > Limits::MAX_DEPTH)
			reject("too many nested groups");

		auto& machine = script.machine();
		// Validate before walking: the guest is free to mutate the object we gave
		// it, so element_cnt bounds the step budget below only once it is checked
		GAttrMap& map = guestAttributes(script, self);

		// The group and list alternatives are opaque pointers, so the tree below
		// them has to be freed before the map frees the nodes that hold them
		std::size_t steps = 0;
		Script::gaddr_t addr = map.before_begin;
		while (addr != 0) {
			if (++steps > map.size())
				reject("more nodes than the element count");
			const auto& node = map.node_at(machine, addr);
			const GAttrValue& value = node.value.second;
			if (const auto* group = value.get_if<GuestGroupPtr>(); group && group->addr != 0) {
				freeMap(script, group->addr, level + 1);
				script.guest_free(group->addr);
			} else if (const auto* list = value.get_if<GuestListPtr>(); list && list->addr != 0) {
				objectAt<GAttrList>(script, list->addr).free(machine);
				script.guest_free(list->addr);
			}
			addr = node.next;
		}
		// Frees the keys, the string values, every node and the bucket array
		map.free(machine);
	}
} // namespace

void writeGuestAttributes(Script& script, Script::gaddr_t self, const HostAttributes& attrs)
{
	if (self == 0)
		reject("null destination object");
	writeMap(script, self, attrs, 0);   // validates the object it inserts into
}

Script::gaddr_t createGuestAttributes(Script& script, const HostAttributes& attrs)
{
	const Script::gaddr_t addr = createGuestMap(script);
	try {
		writeMap(script, addr, attrs, 0);
	} catch (...) {
		destroyGuestAttributes(script, addr);
		throw;
	}
	return addr;
}

void destroyGuestAttributes(Script& script, Script::gaddr_t self) noexcept
{
	if (self == 0)
		return;
	try {
		freeMap(script, self, 0);
	} catch (const std::exception& e) {
		// A guest that corrupted the tree we handed it loses the memory, but a
		// cleanup path must not throw
		fprintf(stderr, "Guest attributes: failed to free host-owned object: %s\n", e.what());
	}
	script.guest_free(self);
}

} // namespace bench
