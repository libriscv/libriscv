#include "rustattrs.hpp"

#include <cstdio>
#include <stdexcept>

namespace bench {

namespace {
	using Limits = GuestAttrLimits;

	/// @brief Values visited so far, across the whole tree
	struct NodeBudget { std::size_t nodes = 0; };

	[[noreturn]] void reject(const char* what)
	{
		throw std::runtime_error(std::string("Guest attributes: ") + what);
	}

	void readMap(Script& script, const GAttrs& attrs, HostAttributes& out,
		std::size_t level, NodeBudget& budget);

	/// @brief Read one scalar variant. Shared by map values and list elements,
	/// which is why it returns the leaf-only variant.
	BaseValue readBaseValue(Script& script, const GValue& value)
	{
		BaseValue result;
		const bool ok = value.visit([&] (const auto& alt) {
			using T = std::decay_t<decltype(alt)>;
			if constexpr (std::is_same_v<T, GString>)
				result = std::string(alt.to_view(script.machine(), Limits::MAX_STRING));
			else if constexpr (std::is_same_v<T, GGroup> || std::is_same_v<T, GList>)
				reject("a group or a list where a scalar was expected");
			else
				result = alt;
		});
		if (!ok)
			reject("out-of-range discriminant");
		return result;
	}

	/// @brief Read a Box<Vec<Value>> as a host list.
	HostAttributes::AttributeListPtr readList(Script& script, const GList& box,
		NodeBudget& budget)
	{
		auto list = std::make_unique<std::vector<BaseValue>>();
		// An empty box is Option<Box<T>>::None, ie. no list rather than an error
		const auto* vec = box.get_if(script.machine());
		if (vec == nullptr)
			return list;

		if (vec->size() > vec->capacity())
			reject("list has len > capacity");
		const std::size_t count = vec->size();
		if (count > Limits::MAX_LIST)
			reject("list is too long");
		if (count == 0)
			return list;
		budget.nodes += count;
		if (budget.nodes > Limits::MAX_NODES)
			reject("too many values in the attribute tree");

		list->reserve(count);
		for (std::size_t i = 0; i < count; i++)
			list->emplace_back(readBaseValue(script, vec->at(script.machine(), i)));
		return list;
	}

	void readValue(Script& script, const GValue& value, std::string&& key,
		HostAttributes& out, std::size_t level, NodeBudget& budget)
	{
		const bool ok = value.visit([&] (const auto& alt) {
			using T = std::decay_t<decltype(alt)>;
			if constexpr (std::is_same_v<T, GGroup>) {
				auto& group = out.addGroup(std::move(key));
				// An empty box is None, which is simply an empty group
				if (const auto* child = alt.get_if(script.machine()))
					readMap(script, *child, group, level + 1, budget);
			} else if constexpr (std::is_same_v<T, GList>) {
				out.getAllAttributes().emplace_back(std::move(key),
					readList(script, alt, budget));
			} else if constexpr (std::is_same_v<T, GString>) {
				out.set(std::move(key),
					std::string(alt.to_view(script.machine(), Limits::MAX_STRING)));
			} else {
				out.set(std::move(key), alt);
			}
		});
		if (!ok)
			reject("out-of-range discriminant");
	}

	void readMap(Script& script, const GAttrs& attrs, HostAttributes& out,
		std::size_t level, NodeBudget& budget)
	{
		if (level > Limits::MAX_DEPTH)
			reject("too many nested groups");
		// A script is adversarial input: the shape is checked before it is walked
		attrs.validate(script.machine(), Limits::MAX_ENTRIES);
		budget.nodes += attrs.size();
		if (budget.nodes > Limits::MAX_NODES)
			reject("too many values in the attribute tree");

		attrs.for_each(script.machine(), [&] (std::string_view key, const GValue& value) {
			if (key.size() > Limits::MAX_KEY)
				reject("key is too long");
			readValue(script, value, std::string(key), out, level, budget);
		});
	}
} // namespace

void readGuestAttributes(Script& script, Script::gaddr_t addr, HostAttributes& out)
{
	if (addr == 0)
		return;
	// memarray faults on an unmapped or misaligned address, so what is left to
	// check is the vector's own state, which readMap() does
	const GAttrs& attrs = *script.machine().memory.memarray<const GAttrs>(addr, 1);
	NodeBudget budget;
	readMap(script, attrs, out, 0, budget);
}

// --- host -> guest ----------------------------------------------------------

namespace {
	void writeMap(Script& script, GAttrs& attrs, const HostAttributes& tree,
		std::size_t level)
	{
		if (level > Limits::MAX_DEPTH)
			reject("too many nested groups");
		auto& machine = script.machine();
		const auto& entries = tree.getAllAttributes();
		if (entries.empty())
			return;

		// One allocation for all the entries, instead of a grow every few keys
		attrs.reserve(machine, entries.size());

		for (const auto& [key, value] : entries) {
			switch (value.index()) {
			case GROUP: {
				static const HostAttributes empty;
				const auto& group = std::get<HostAttributes::AttributesPtr>(value);
				// Box::new(Attributes::new()), built where it will stay
				auto& child = attrs.emplace<GGroup>(machine, key).get(machine);
				writeMap(script, child, group ? *group : empty, level + 1);
				break;
			}
			case LIST: {
				const auto& list = std::get<HostAttributes::AttributeListPtr>(value);
				auto& vec = attrs.emplace<GList>(machine, key).get(machine);
				if (list && !list->empty()) {
					if (list->size() > Limits::MAX_LIST)
						reject("list is too long");
					vec.resize(machine, list->size());
					for (std::size_t i = 0; i < list->size(); i++)
						vec.at(machine, i).set(machine, (*list)[i]);
				}
				break;
			}
			default:
				// Every remaining alternative is one the enum can select on its
				// own, and it sits at the same index on both sides
				std::visit([&] (const auto& scalar) {
					using T = std::decay_t<decltype(scalar)>;
					if constexpr (!std::is_same_v<T, HostAttributes::AttributesPtr>
						&& !std::is_same_v<T, HostAttributes::AttributeListPtr>)
					{
						attrs.insert_or_assign(machine, key, scalar);
					}
				}, value);
				break;
			}
		}
	}
} // namespace

Script::gaddr_t createGuestAttributes(Script& script, const HostAttributes& attrs)
{
	GGroup root;
	GAttrs& map = root.emplace(script.machine());
	try {
		writeMap(script, map, attrs, 0);
	} catch (...) {
		root.free(script.machine());
		throw;
	}
	// Box::into_raw(): the guest owns the tree from here on
	return root.release();
}

void destroyGuestAttributes(Script& script, Script::gaddr_t addr) noexcept
{
	if (addr == 0)
		return;
	try {
		// Box::from_raw() on the host side: dropping it walks the whole tree
		GGroup box(addr);
		box.free(script.machine());
	} catch (const std::exception& e) {
		fprintf(stderr, "Guest attributes: failed to free host-owned tree: %s\n", e.what());
	}
}

} // namespace bench
