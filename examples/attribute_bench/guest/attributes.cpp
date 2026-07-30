#include "attributes.hpp"

namespace bench {

std::span<const Attributes::HostAttr> Attributes::createHostAttr() const
{
	HostAttr* nodes = new HostAttr[m_attributes.size()];
	unsigned i = 0;
	for (const auto& [key, value] : m_attributes) {
		HostAttr& node = nodes[i++];
		switch (value.index()) {
		case INT64:
			new (&node) HostAttr(key, std::get<int64_t>(value));
			break;
		case FLOAT:
			new (&node) HostAttr(key, std::get<double>(value));
			break;
		case VEC3:
			new (&node) HostAttr(key, std::get<vec3>(value));
			break;
		case VEC4:
			new (&node) HostAttr(key, std::get<vec4>(value));
			break;
		case DVEC3:
			new (&node) HostAttr(key, std::get<dvec3>(value));
			break;
		case DVEC2:
			new (&node) HostAttr(key, std::get<dvec2>(value));
			break;
		case STRING:
			new (&node) HostAttr(key, std::string_view(std::get<std::string>(value)));
			break;
		case BOOL:
			new (&node) HostAttr(key, std::get<bool>(value));
			break;
		case GROUP: {
			const auto group = std::get<AttributesPtr>(value)->createHostAttr();
			new (&node) HostAttr(key, group.data(), unsigned(group.size()));
			break;
		}
		case LIST: {
			const auto& listPtr = std::get<AttributeListPtr>(value);
			if (!listPtr || listPtr->empty()) {
				new (&node) HostAttr(key, (const HostAttr*)nullptr, 0u, true);
				break;
			}
			const auto& list = *listPtr;
			HostAttr* elements = new HostAttr[list.size()];
			for (std::size_t j = 0; j < list.size(); j++) {
				std::visit([elements, j] (const auto& v) {
					using T = std::decay_t<decltype(v)>;
					if constexpr (std::is_same_v<T, std::string>)
						new (&elements[j]) HostAttr(std::string_view(), std::string_view(v));
					else
						new (&elements[j]) HostAttr(std::string_view(), v);
				}, list[j]);
			}
			new (&node) HostAttr(key, elements, unsigned(list.size()), true);
			break;
		}
		default:
			throw std::runtime_error("createHostAttr(): Unknown attribute type");
		}
	}
	return std::span<const HostAttr>(nodes, i);
}

Attributes Attributes::fromGuestAttributes(std::span<const HostAttr> nodes, bool freeNodes)
{
	Attributes attrs;
	// Size the map up front: it receives exactly nodes.size() entries, so this
	// avoids the incremental rehashes that one-at-a-time insertion would trigger
	// (every node allocation and rehash here is emulated guest code).
	attrs.getAllAttributes().reserve(nodes.size());

	for (const HostAttr& node : nodes) {
		// The node carries the key's length, so this is one allocation and one
		// copy -- no strlen over a string the sender already knew the size of
		const std::string key(node.key, node.klen);

		switch (node.type) {
		case INT64:
			attrs.set(key, node.ival);
			break;
		case FLOAT:
			attrs.set(key, node.fval);
			break;
		case VEC3:
			attrs.set(key, node.vecval);
			break;
		case VEC4:
			attrs.set(key, node.vec4val);
			break;
		case DVEC3:
			attrs.set(key, dvec3{node.dvec3val[0], node.dvec3val[1], node.dvec3val[2]});
			break;
		case DVEC2:
			attrs.set(key, dvec2{node.dvec2val[0], node.dvec2val[1]});
			break;
		case STRING:
			attrs.set(key, std::string(node.strval, node.slen));
			std::free((void*)node.strval);
			break;
		case BOOL:
			attrs.set(key, node.bval);
			break;
		case GROUP: {
			attrs.set(key,
				fromGuestAttributes({node.group_nodes, node.group_count}, false));
			std::free((void*)node.group_nodes);
			break;
		}
		case LIST: {
			std::vector<AttributeBaseValue> list;
			list.reserve(node.list_count);
			for (const HostAttr& element : std::span(node.list_nodes, node.list_count)) {
				switch (element.type) {
				case INT64: list.emplace_back(element.ival); break;
				case FLOAT: list.emplace_back(element.fval); break;
				case VEC3:  list.emplace_back(element.vecval); break;
				case VEC4:  list.emplace_back(element.vec4val); break;
				case DVEC3: list.emplace_back(dvec3{element.dvec3val[0], element.dvec3val[1], element.dvec3val[2]}); break;
				case DVEC2: list.emplace_back(dvec2{element.dvec2val[0], element.dvec2val[1]}); break;
				case STRING:
					list.emplace_back(std::string(element.strval, element.slen));
					std::free((void*)element.strval);
					break;
				case BOOL:  list.emplace_back(element.bval); break;
				default:
					throw std::runtime_error("fromGuestAttributes(): Unknown list element type");
				}
			}
			if (node.list_nodes != nullptr)
				std::free((void*)node.list_nodes);
			attrs.setList(key, std::move(list));
			break;
		}
		default:
			throw std::runtime_error("fromGuestAttributes(): Unknown attribute type");
		}
		std::free((void*)node.key);
	}
	if (freeNodes)
		std::free((void*)nodes.data());
	return attrs;
}

} // namespace bench
