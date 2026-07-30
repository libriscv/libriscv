#pragma once
/**
 * Host-side views of the containers that live in guest memory.
 *
 * Each container has its own header under libriscv/guest/, and this one
 * pulls in all of them. Include only what you need when compile time
 * matters, eg. <libriscv/guest/guest_rust_string.hpp>.
 *
 *   guest_common.hpp          The traits and dispatch every container shares
 *   guest_arena_object.hpp    ScopedArenaObject: an owned object in the arena
 *
 *   guest_cpp_common.hpp        Forward declarations of the C++ containers
 *   guest_cpp_string.hpp        GuestStdString
 *   guest_cpp_vector.hpp        GuestStdVector
 *   guest_cpp_hash.hpp          GuestStdHash, the replica of the guests std::hash
 *   guest_cpp_unordered_map.hpp GuestStdUnorderedMap
 *   guest_cpp_variant.hpp       GuestStdVariant
 *
 *   guest_rust_common.hpp     Forward declarations of the Rust containers
 *   guest_rust_string.hpp     GuestRustString, GuestRustStr
 *   guest_rust_vec.hpp        GuestRustVec, GuestRustSlice
 *   guest_rust_box.hpp        GuestRustBox, GuestRustBoxedSlice, GuestRustBoxedStr
 *   guest_rust_enum.hpp       GuestRustEnum, the #[repr(C, uN)] enum
 *   guest_rust_attributes.hpp GuestRustAttr, GuestRustAttributes
 *
 * A container owns real guest memory, and it is not released by a destructor:
 * call free(machine) when you are done with it, or hold it in a
 * ScopedArenaObject, which does that for you.
**/
#include "guest/guest_common.hpp"
#include "guest/guest_arena_object.hpp"

#include "guest/guest_cpp_string.hpp"
#include "guest/guest_cpp_vector.hpp"
#include "guest/guest_cpp_hash.hpp"
#include "guest/guest_cpp_unordered_map.hpp"
#include "guest/guest_cpp_variant.hpp"

#include "guest/guest_rust_string.hpp"
#include "guest/guest_rust_vec.hpp"
#include "guest/guest_rust_box.hpp"
#include "guest/guest_rust_enum.hpp"
#include "guest/guest_rust_attributes.hpp"
