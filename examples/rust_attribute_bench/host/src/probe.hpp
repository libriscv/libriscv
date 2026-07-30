#pragma once
/**
 * The ABI check that runs before anything is measured.
 *
 * A #[repr(C, uN)] enum is a layout the guest *requests* rather than one the host
 * discovers, but that is a promise about a compiler, not a law. The guest reports
 * its own view of the layout -- sizes, discriminants, payload offsets, field order
 * -- and the host compares it against the mirror it is about to read through, so a
 * rustc release that lays it out differently fails here instead of silently
 * reading garbage everywhere else.
 */
#include "script.hpp"
#include <string>

namespace bench {

/// @brief Ask the guest for its layout and compare it against the host mirrors.
/// @return An empty string when they agree, otherwise what disagreed.
std::string probe_guest_layout(Script& script);

} // namespace bench
