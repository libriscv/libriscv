#pragma once
/**
 * The guest-side attribute tree, and the flat HostAttr representation it used to
 * be marshalled through.
 *
 * This is a trimmed copy of the container a real embedder ships in its script
 * API: a std::unordered_map with a custom hasher, holding a std::variant of the
 * leaf types plus a nested group and a list. Both benchmark paths start and end
 * with this same object -- what differs is how it crosses the boundary.
 */
#include "../common/attributes_common.hpp"

#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bench {

/// @brief A custom hasher, so libstdc++ treats it as a fast hash and does NOT
/// cache the hash code in the nodes. The host mirror must say the same
/// (GuestStdUnorderedMap<..., false>) or every key and value shifts.
struct string_hash {
	using is_transparent = void;
	[[nodiscard]] std::size_t operator()(std::string_view str) const noexcept {
		return std::hash<std::string_view>{}(str);
	}
};

struct Attributes {
	using AttributesPtr = std::unique_ptr<Attributes>;
	using AttributeBaseValue = BaseValue;
	using AttributeListPtr = std::unique_ptr<std::vector<AttributeBaseValue>>;
	using Attribute = std::variant<int64_t, double, vec3, vec4, dvec3, dvec2,
		std::string, bool, AttributesPtr, AttributeListPtr>;
	using AttributesMap = std::unordered_map<std::string, Attribute, string_hash, std::equal_to<>>;

	template <typename T>
	void set(const std::string& name, T value) {
		if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
			m_attributes.insert_or_assign(name, int64_t(value));
		else if constexpr (std::is_floating_point_v<T>)
			m_attributes.insert_or_assign(name, double(value));
		else
			m_attributes.insert_or_assign(name, std::move(value));
	}

	Attributes& set(const std::string& name, Attributes&& group) {
		auto [it, _] = m_attributes.insert_or_assign(name,
			std::make_unique<Attributes>(std::move(group)));
		return *std::get<AttributesPtr>(it->second);
	}

	Attributes& addGroup(const std::string& name) {
		auto [it, _] = m_attributes.try_emplace(name, std::make_unique<Attributes>());
		return *std::get<AttributesPtr>(it->second);
	}

	void setList(const std::string& name, std::vector<AttributeBaseValue> list) {
		m_attributes.insert_or_assign(name,
			std::make_unique<std::vector<AttributeBaseValue>>(std::move(list)));
	}

	template <typename T>
	const T* get(std::string_view name) const {
		auto it = m_attributes.find(name);
		return it == m_attributes.end() ? nullptr : std::get_if<T>(&it->second);
	}

	std::size_t size() const noexcept { return m_attributes.size(); }
	void clear() noexcept { m_attributes.clear(); }

	const AttributesMap& getAllAttributes() const& noexcept { return m_attributes; }
	AttributesMap& getAllAttributes() & noexcept { return m_attributes; }

	// --- The flat representation ---------------------------------------------
	//
	// One node per key: the key as (pointer, length), a type tag, and a union
	// that either holds the value inline or points at heap-allocated storage. A
	// whole tree becomes an array of these, plus one array per nested group and
	// list.
	//
	// The key carries its length for the same reason the string values do: the
	// map's key is a std::string, so size() is already there for free, and
	// whichever side receives the node gets to skip a strlen -- the host a
	// memstring scanning guest memory for a terminator, the guest the one hidden
	// in set(const char*). The length lives in what would otherwise be padding
	// ahead of the union, so the node is 40 bytes either way.

	struct HostAttr {
		const char* key;
		unsigned    klen;
		AttrType    type;
		union {
			struct { const char* strval; unsigned slen; };
			int64_t ival;
			double  fval;
			vec3    vecval;
			vec4    vec4val;
			double  dvec3val[3];
			double  dvec2val[2];
			bool    bval;
			struct { const HostAttr* group_nodes; unsigned group_count; };
			struct { const HostAttr* list_nodes;  unsigned list_count; };
		};

		HostAttr() : key(nullptr), klen(0), type(INT64), ival(0) {}
		HostAttr(std::string_view k, std::string_view v)
			: key(k.data()), klen(unsigned(k.size())), type(STRING),
			  strval(v.data()), slen(unsigned(v.size())) {}
		HostAttr(std::string_view k, int64_t v)
			: key(k.data()), klen(unsigned(k.size())), type(INT64), ival(v) {}
		HostAttr(std::string_view k, double v)
			: key(k.data()), klen(unsigned(k.size())), type(FLOAT), fval(v) {}
		HostAttr(std::string_view k, const vec3& v)
			: key(k.data()), klen(unsigned(k.size())), type(VEC3), vecval(v) {}
		HostAttr(std::string_view k, const vec4& v)
			: key(k.data()), klen(unsigned(k.size())), type(VEC4), vec4val(v) {}
		HostAttr(std::string_view k, const dvec3& v)
			: key(k.data()), klen(unsigned(k.size())), type(DVEC3) {
			dvec3val[0] = v.x; dvec3val[1] = v.y; dvec3val[2] = v.z;
		}
		HostAttr(std::string_view k, const dvec2& v)
			: key(k.data()), klen(unsigned(k.size())), type(DVEC2) {
			dvec2val[0] = v.x; dvec2val[1] = v.y;
		}
		HostAttr(std::string_view k, bool v)
			: key(k.data()), klen(unsigned(k.size())), type(BOOL), bval(v) {}
		HostAttr(std::string_view k, const HostAttr* nodes, unsigned count)
			: key(k.data()), klen(unsigned(k.size())), type(GROUP),
			  group_nodes(nodes), group_count(count) {}
		HostAttr(std::string_view k, const HostAttr* nodes, unsigned count, bool)
			: key(k.data()), klen(unsigned(k.size())), type(LIST),
			  list_nodes(nodes), list_count(count) {}

		~HostAttr() {
			if (type == GROUP) delete[] group_nodes;
			else if (type == LIST) delete[] list_nodes;
		}
	};

	/// @brief Flatten the tree for the host. Every group and every list costs one
	/// guest heap allocation; keys and string values are passed by pointer into
	/// the map's own storage, so the tree must outlive the array.
	std::span<const HostAttr> createHostAttr() const;
	static void destroyHostAttr(std::span<const HostAttr> nodes) { delete[] nodes.data(); }

	/// @brief Rebuild a tree from a flat array the host wrote into guest memory.
	/// Consumes it: every key string, string value, group array and list array
	/// was malloc'd by the host on this heap and is freed here.
	static Attributes fromGuestAttributes(std::span<const HostAttr> nodes, bool freeNodes = true);

private:
	AttributesMap m_attributes;
};

} // namespace bench
