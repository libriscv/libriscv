#pragma once
/**
 * The host-side attribute tree -- the engine's own representation, which both
 * marshalling paths convert to and from.
 *
 * Entries are kept in a vector rather than a hash map on purpose: the host
 * container is identical for both paths, so keeping it cheap and deterministic
 * leaves the measured difference where it belongs, at the guest boundary.
 */
#include "../../common/attributes_common.hpp"

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bench {

struct HostAttributes {
	using AttributesPtr = std::unique_ptr<HostAttributes>;
	using AttributeListPtr = std::unique_ptr<std::vector<BaseValue>>;
	using Attribute = std::variant<int64_t, double, vec3, vec4, dvec3, dvec2,
		std::string, bool, AttributesPtr, AttributeListPtr>;
	using Entry = std::pair<std::string, Attribute>;

	template <typename T>
	void set(std::string name, T value) {
		if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
			m_entries.emplace_back(std::move(name), int64_t(value));
		else if constexpr (std::is_floating_point_v<T>)
			m_entries.emplace_back(std::move(name), double(value));
		else
			m_entries.emplace_back(std::move(name), std::move(value));
	}

	HostAttributes& addGroup(std::string name) {
		m_entries.emplace_back(std::move(name), std::make_unique<HostAttributes>());
		return *std::get<AttributesPtr>(m_entries.back().second);
	}

	void setList(std::string name, std::vector<BaseValue> list) {
		m_entries.emplace_back(std::move(name),
			std::make_unique<std::vector<BaseValue>>(std::move(list)));
	}

	std::size_t size() const noexcept { return m_entries.size(); }
	void clear() noexcept { m_entries.clear(); }

	const std::vector<Entry>& getAllAttributes() const noexcept { return m_entries; }
	std::vector<Entry>& getAllAttributes() noexcept { return m_entries; }

private:
	std::vector<Entry> m_entries;
};

} // namespace bench
