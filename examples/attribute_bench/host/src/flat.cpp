#include "flat.hpp"

#include <algorithm>
#include <stdexcept>

namespace bench {

static constexpr int MAX_LEVEL = 8;

void readFlatAttributes(Script& script, Script::gaddr_t nodes, std::size_t count,
	HostAttributes& out, int level)
{
	if (level > MAX_LEVEL)
		throw std::runtime_error("flat attributes: too many nested groups");
	if (count == 0)
		return;

	auto& memory = script.machine().memory;
	const GuestAttribute* array = memory.memarray<const GuestAttribute>(nodes, count);

	for (std::size_t i = 0; i < count; i++) {
		const GuestAttribute& node = array[i];
		// The node carries the key's length, so this is one bounded view and one
		// copy, rather than a scan across (potentially unmapped) guest pages
		std::string key(memory.memview(node.namePtr, node.nameLen));

		switch (node.type) {
		case INT64:  out.set(std::move(key), node.i); break;
		case FLOAT:  out.set(std::move(key), node.f); break;
		case VEC3:   out.set(std::move(key), node.v3); break;
		case VEC4:   out.set(std::move(key), node.v4); break;
		case DVEC3:  out.set(std::move(key), dvec3{node.d3[0], node.d3[1], node.d3[2]}); break;
		case DVEC2:  out.set(std::move(key), dvec2{node.d2[0], node.d2[1]}); break;
		case BOOL:   out.set(std::move(key), node.b); break;
		case STRING:
			out.set(std::move(key),
				std::string(memory.memview(node.valuePtr, node.valueSize)));
			break;
		case GROUP: {
			auto& group = out.addGroup(std::move(key));
			readFlatAttributes(script, node.groupPtr, node.groupSize, group, level + 1);
			break;
		}
		case LIST: {
			std::vector<BaseValue> list;
			list.reserve(node.listSize);
			if (node.listSize != 0) {
				const GuestAttribute* elements =
					memory.memarray<const GuestAttribute>(node.listPtr, node.listSize);
				for (std::size_t j = 0; j < node.listSize; j++) {
					const GuestAttribute& e = elements[j];
					switch (e.type) {
					case INT64:  list.emplace_back(e.i); break;
					case FLOAT:  list.emplace_back(e.f); break;
					case VEC3:   list.emplace_back(e.v3); break;
					case VEC4:   list.emplace_back(e.v4); break;
					case DVEC3:  list.emplace_back(dvec3{e.d3[0], e.d3[1], e.d3[2]}); break;
					case DVEC2:  list.emplace_back(dvec2{e.d2[0], e.d2[1]}); break;
					case BOOL:   list.emplace_back(e.b); break;
					case STRING:
						list.emplace_back(std::string(memory.memview(e.valuePtr, e.valueSize)));
						break;
					default:
						throw std::runtime_error("flat attributes: unknown list element type");
					}
				}
			}
			out.setList(std::move(key), std::move(list));
			break;
		}
		default:
			throw std::runtime_error("flat attributes: unknown value type");
		}
	}
}

/// @brief Copy a run of bytes onto the guest heap. No terminator: every node that
/// points at one of these carries its length. The guest frees it as it consumes
/// the array, so an empty string still gets a block of its own.
static Script::gaddr_t push_string(Script& script, const std::string& str)
{
	const Script::gaddr_t addr = script.guest_alloc(std::max<std::size_t>(str.size(), 1));
	if (addr == 0)
		throw std::bad_alloc();
	script.machine().copy_to_guest(addr, str.data(), str.size());
	return addr;
}

/// @brief Fill one node. Every allocation this entry needs has already been made,
/// so the view of the array is taken once and stays valid.
static void write_node(GuestAttribute& node, const HostAttributes::Attribute& value,
	Script::gaddr_t namePtr, uint32_t nameLen,
	Script::gaddr_t indirect, uint32_t indirectSize)
{
	node.namePtr = namePtr;
	node.nameLen = nameLen;
	node.type = int32_t(value.index());

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
	case GROUP:
	case LIST:
		// All three are (pointer, count) pairs into separately allocated storage
		node.valuePtr = indirect;
		node.valueSize = indirectSize;
		break;
	default:
		throw std::runtime_error("flat attributes: unknown value type");
	}
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
		script.guest_alloc(sizeof(GuestAttribute) * entries.size());
	if (alloc == 0)
		throw std::bad_alloc();

	for (std::size_t i = 0; i < entries.size(); i++) {
		const auto& [key, value] = entries[i];
		// One guest allocation for the key of every single entry
		const Script::gaddr_t namePtr = push_string(script, key);
		Script::gaddr_t indirect = 0;
		uint32_t indirectSize = 0;

		switch (value.index()) {
		case STRING: {
			const std::string& str = std::get<std::string>(value);
			indirect = push_string(script, str);
			indirectSize = uint32_t(str.size());
			break;
		}
		case GROUP: {
			static const HostAttributes empty;
			const auto& group = std::get<HostAttributes::AttributesPtr>(value);
			const auto [addr, count] =
				writeFlatAttributes(script, group ? *group : empty, level + 1);
			indirect = addr;
			indirectSize = uint32_t(count);
			break;
		}
		case LIST: {
			const auto& listPtr = std::get<HostAttributes::AttributeListPtr>(value);
			if (!listPtr || listPtr->empty())
				break;
			const auto& list = *listPtr;
			const Script::gaddr_t listAddr =
				script.guest_alloc(sizeof(GuestAttribute) * list.size());
			if (listAddr == 0)
				throw std::bad_alloc();
			for (std::size_t j = 0; j < list.size(); j++) {
				Script::gaddr_t strAddr = 0;
				uint32_t strSize = 0;
				if (const auto* str = std::get_if<std::string>(&list[j])) {
					strAddr = push_string(script, *str);
					strSize = uint32_t(str->size());
				}
				GuestAttribute& element = script.machine().memory
					.memarray<GuestAttribute>(listAddr, list.size())[j];
				element.namePtr = 0;
				element.nameLen = 0;
				element.type = int32_t(list[j].index());
				std::visit([&] (const auto& v) {
					using T = std::decay_t<decltype(v)>;
					if constexpr (std::is_same_v<T, int64_t>) element.i = v;
					else if constexpr (std::is_same_v<T, double>) element.f = v;
					else if constexpr (std::is_same_v<T, vec3>) element.v3 = v;
					else if constexpr (std::is_same_v<T, vec4>) element.v4 = v;
					else if constexpr (std::is_same_v<T, dvec3>) {
						element.d3[0] = v.x; element.d3[1] = v.y; element.d3[2] = v.z;
					} else if constexpr (std::is_same_v<T, dvec2>) {
						element.d2[0] = v.x; element.d2[1] = v.y;
					} else if constexpr (std::is_same_v<T, bool>) element.b = v;
					else { element.valuePtr = strAddr; element.valueSize = strSize; }
				}, list[j]);
			}
			indirect = listAddr;
			indirectSize = uint32_t(list.size());
			break;
		}
		default:
			break;
		}

		GuestAttribute& node = script.machine().memory
			.memarray<GuestAttribute>(alloc, entries.size())[i];
		write_node(node, value, namePtr, uint32_t(key.size()), indirect, indirectSize);
	}
	return {alloc, entries.size()};
}

} // namespace bench
