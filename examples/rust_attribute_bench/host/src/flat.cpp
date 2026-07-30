#include "flat.hpp"

#include <new>
#include <stdexcept>

namespace bench {

static constexpr int MAX_LEVEL = 8;

/// @brief Read one leaf out of a node. Shared by map values and list elements.
static BaseValue readLeaf(Script& script, const GuestFlatAttr& node)
{
	switch (node.tag) {
	case INT64:  return node.i;
	case FLOAT:  return node.f;
	case VEC3:   return node.v3;
	case VEC4:   return node.v4;
	case DVEC3:  return dvec3{node.d3[0], node.d3[1], node.d3[2]};
	case DVEC2:  return dvec2{node.d2[0], node.d2[1]};
	case BOOL:   return node.b;
	case STRING:
		if (node.span.len == 0)
			return std::string();
		return std::string(script.machine().memory.memview(node.span.ptr, node.span.len));
	default:
		throw std::runtime_error("flat attributes: unknown value type");
	}
}

void readFlatAttributes(Script& script, Script::gaddr_t nodes, std::size_t count,
	HostAttributes& out, int level)
{
	if (level > MAX_LEVEL)
		throw std::runtime_error("flat attributes: too many nested groups");
	if (count == 0)
		return;

	auto& memory = script.machine().memory;
	const GuestFlatAttr* array = memory.memarray<const GuestFlatAttr>(nodes, count);

	for (std::size_t i = 0; i < count; i++) {
		const GuestFlatAttr& node = array[i];
		std::string key(memory.memview(node.key, node.key_len));

		switch (node.tag) {
		case GROUP: {
			auto& group = out.addGroup(std::move(key));
			readFlatAttributes(script, node.span.ptr, node.span.len, group, level + 1);
			break;
		}
		case LIST: {
			std::vector<BaseValue> list;
			list.reserve(node.span.len);
			if (node.span.len != 0) {
				const GuestFlatAttr* elements =
					memory.memarray<const GuestFlatAttr>(node.span.ptr, node.span.len);
				for (std::size_t j = 0; j < node.span.len; j++)
					list.emplace_back(readLeaf(script, elements[j]));
			}
			out.setList(std::move(key), std::move(list));
			break;
		}
		default:
			std::visit([&] (auto&& leaf) { out.set(std::move(key), std::move(leaf)); },
				readLeaf(script, node));
			break;
		}
	}
}

/// @brief Copy a run of bytes onto the guest heap, allocating exactly as many
/// bytes as there are. The guest takes the block over with
/// String::from_raw_parts, which is only sound because the arena is its
/// allocator and the capacity it claims is the size that was asked for here.
static Script::gaddr_t push_bytes(Script& script, const std::string& str)
{
	if (str.empty())
		return 0;
	const Script::gaddr_t addr = script.guest_alloc(str.size());
	if (addr == 0)
		throw std::bad_alloc();
	script.machine().copy_to_guest(addr, str.data(), str.size());
	return addr;
}

/// @brief Fill in the leaf part of a node. Any storage it points at has already
/// been allocated, so the view of the array stays valid across this.
///
/// Templated over the variant, because the leaf alternatives of the host tree and
/// of a list element are the same eight types at the same eight indices.
template <typename Variant>
static void write_leaf(GuestFlatAttr& node, const Variant& value,
	Script::gaddr_t indirect, Script::gaddr_t indirect_len)
{
	node.tag = uint32_t(value.index());
	switch (value.index()) {
	case INT64: node.i = std::get<int64_t>(value); break;
	case FLOAT: node.f = std::get<double>(value); break;
	case VEC3:  node.v3 = std::get<vec3>(value); break;
	case VEC4:  node.v4 = std::get<vec4>(value); break;
	case DVEC3: {
		const auto& v = std::get<dvec3>(value);
		node.d3[0] = v.x; node.d3[1] = v.y; node.d3[2] = v.z;
		break;
	}
	case DVEC2: {
		const auto& v = std::get<dvec2>(value);
		node.d2[0] = v.x; node.d2[1] = v.y;
		break;
	}
	case BOOL: node.b = std::get<bool>(value); break;
	case STRING:
		node.span.ptr = indirect;
		node.span.len = indirect_len;
		break;
	default:
		throw std::runtime_error("flat attributes: unknown value type");
	}
}

/// @brief Build the node array of a list.
static std::pair<Script::gaddr_t, std::size_t> writeFlatList(
	Script& script, const std::vector<BaseValue>& list)
{
	if (list.empty())
		return {0, 0};
	const Script::gaddr_t alloc =
		script.guest_alloc(sizeof(GuestFlatAttr) * list.size());
	if (alloc == 0)
		throw std::bad_alloc();

	for (std::size_t i = 0; i < list.size(); i++) {
		Script::gaddr_t bytes = 0;
		Script::gaddr_t length = 0;
		if (const auto* str = std::get_if<std::string>(&list[i])) {
			bytes = push_bytes(script, *str);
			length = str->size();
		}
		// Taken after every allocation this element needed
		GuestFlatAttr& node = script.machine().memory
			.memarray<GuestFlatAttr>(alloc, list.size())[i];
		node.key = 0;
		node.key_len = 0;
		write_leaf(node, list[i], bytes, length);
	}
	return {alloc, list.size()};
}

std::pair<Script::gaddr_t, std::size_t> writeFlatAttributes(
	Script& script, const HostAttributes& attrs, int level)
{
	if (level > MAX_LEVEL)
		throw std::runtime_error("flat attributes: too many nested groups");

	const auto& entries = attrs.getAllAttributes();
	if (entries.empty())
		return {0, 0};

	const Script::gaddr_t alloc =
		script.guest_alloc(sizeof(GuestFlatAttr) * entries.size());
	if (alloc == 0)
		throw std::bad_alloc();

	for (std::size_t i = 0; i < entries.size(); i++) {
		const auto& [key, value] = entries[i];
		// One guest allocation for the key of every single entry
		const Script::gaddr_t key_addr = push_bytes(script, key);
		Script::gaddr_t indirect = 0;
		Script::gaddr_t indirect_len = 0;
		bool nested = false;

		switch (value.index()) {
		case GROUP: {
			static const HostAttributes empty;
			const auto& group = std::get<HostAttributes::AttributesPtr>(value);
			const auto [addr, count] =
				writeFlatAttributes(script, group ? *group : empty, level + 1);
			indirect = addr;
			indirect_len = count;
			nested = true;
			break;
		}
		case LIST: {
			static const std::vector<BaseValue> empty;
			const auto& list = std::get<HostAttributes::AttributeListPtr>(value);
			const auto [addr, count] = writeFlatList(script, list ? *list : empty);
			indirect = addr;
			indirect_len = count;
			nested = true;
			break;
		}
		case STRING: {
			const auto& str = std::get<std::string>(value);
			indirect = push_bytes(script, str);
			indirect_len = str.size();
			break;
		}
		default:
			break;
		}

		// Taken after every allocation this entry needed, so it stays valid
		GuestFlatAttr& node = script.machine().memory
			.memarray<GuestFlatAttr>(alloc, entries.size())[i];
		node.key = key_addr;
		node.key_len = uint32_t(key.size());

		if (nested) {
			node.tag = uint32_t(value.index());
			node.span.ptr = indirect;
			node.span.len = indirect_len;
		} else {
			write_leaf(node, value, indirect, indirect_len);
		}
	}
	return {alloc, entries.size()};
}

} // namespace bench
