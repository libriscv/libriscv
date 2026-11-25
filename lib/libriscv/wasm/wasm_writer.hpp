#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace riscv::wasm
{
	enum ValType : uint8_t {
		I32 = 0x7F, I64 = 0x7E, F32 = 0x7D, F64 = 0x7C,
		FUNCREF = 0x70,
	};
	static constexpr uint8_t BLOCK_VOID = 0x40;

	struct Buffer
	{
		std::vector<uint8_t> data;

		size_t size() const noexcept { return data.size(); }
		void u8(uint8_t v) { data.push_back(v); }
		void bytes(const void* p, size_t n) {
			auto* b = (const uint8_t*)p;
			data.insert(data.end(), b, b + n);
		}
		void append(const Buffer& other) { data.insert(data.end(), other.data.begin(), other.data.end()); }
		void u32(uint32_t v) {
			do {
				uint8_t b = v & 0x7F;
				v >>= 7;
				if (v != 0) b |= 0x80;
				u8(b);
			} while (v != 0);
		}
		void s64(int64_t v) {
			while (true) {
				const uint8_t b = v & 0x7F;
				v >>= 7; // Arithmetic shift
				if ((v == 0 && (b & 0x40) == 0) || (v == -1 && (b & 0x40) != 0)) {
					u8(b);
					return;
				}
				u8(b | 0x80);
			}
		}
		void s32(int32_t v) { s64(v); }
		void name(std::string_view s) { u32(s.size()); bytes(s.data(), s.size()); }
		void section(uint8_t id, const Buffer& contents) {
			u8(id);
			u32(contents.size());
			append(contents);
		}
	};

	// The emitter supplies branch depths.
	struct Code : Buffer
	{
		void unreachable() { u8(0x00); }
		void nop()         { u8(0x01); }
		void block(uint8_t bt = BLOCK_VOID) { u8(0x02); u8(bt); }
		void loop(uint8_t bt = BLOCK_VOID)  { u8(0x03); u8(bt); }
		void if_(uint8_t bt = BLOCK_VOID)   { u8(0x04); u8(bt); }
		void else_() { u8(0x05); }
		// Legacy WebAssembly exception opcodes
		void try_(uint8_t bt = BLOCK_VOID) { u8(0x06); u8(bt); }
		void catch_all() { u8(0x19); }
		void rethrow(uint32_t depth) { u8(0x09); u32(depth); }
		void end()   { u8(0x0B); }
		void br(uint32_t depth)    { u8(0x0C); u32(depth); }
		void br_if(uint32_t depth) { u8(0x0D); u32(depth); }
		void br_table(const std::vector<uint32_t>& depths, uint32_t def) {
			u8(0x0E); u32(depths.size());
			for (auto d : depths) u32(d);
			u32(def);
		}
		void ret()  { u8(0x0F); }
		void call(uint32_t func) { u8(0x10); u32(func); }
		void drop() { u8(0x1A); }
		void select() { u8(0x1B); }

		void local_get(uint32_t i) { u8(0x20); u32(i); }
		void local_set(uint32_t i) { u8(0x21); u32(i); }
		void local_tee(uint32_t i) { u8(0x22); u32(i); }
		void global_get(uint32_t i) { u8(0x23); u32(i); }

		void mem(uint8_t op, uint32_t align, uint32_t offset) { u8(op); u32(align); u32(offset); }
		void i32_load(uint32_t off)     { mem(0x28, 2, off); }
		void i64_load(uint32_t off)     { mem(0x29, 3, off); }
		void f32_load(uint32_t off)     { mem(0x2A, 2, off); }
		void f64_load(uint32_t off)     { mem(0x2B, 3, off); }
		void i64_load8_s(uint32_t off)  { mem(0x30, 0, off); }
		void i64_load8_u(uint32_t off)  { mem(0x31, 0, off); }
		void i64_load16_s(uint32_t off) { mem(0x32, 1, off); }
		void i64_load16_u(uint32_t off) { mem(0x33, 1, off); }
		void i64_load32_s(uint32_t off) { mem(0x34, 2, off); }
		void i64_load32_u(uint32_t off) { mem(0x35, 2, off); }
		void i32_store(uint32_t off)    { mem(0x36, 2, off); }
		void i64_store(uint32_t off)    { mem(0x37, 3, off); }
		void f32_store(uint32_t off)    { mem(0x38, 2, off); }
		void f64_store(uint32_t off)    { mem(0x39, 3, off); }
		void i64_store8(uint32_t off)   { mem(0x3C, 0, off); }
		void i64_store16(uint32_t off)  { mem(0x3D, 1, off); }
		void i64_store32(uint32_t off)  { mem(0x3E, 2, off); }

		void i32_const(int32_t v) { u8(0x41); s32(v); }
		void i64_const(int64_t v) { u8(0x42); s64(v); }
		void f32_const(float v)  { u8(0x43); bytes(&v, 4); }
		void f64_const(double v) { u8(0x44); bytes(&v, 8); }

		void op(uint8_t opcode) { u8(opcode); }
		void op_fc(uint32_t sub) { u8(0xFC); u32(sub); }
	};

	// Opcodes without immediates (Code::op)
	enum Op : uint8_t {
		I32_EQZ = 0x45, I32_EQ = 0x46, I32_NE = 0x47, I32_LT_S = 0x48, I32_LT_U = 0x49,
		I32_GT_S = 0x4A, I32_GT_U = 0x4B, I32_LE_S = 0x4C, I32_LE_U = 0x4D, I32_GE_S = 0x4E, I32_GE_U = 0x4F,
		I64_EQZ = 0x50, I64_EQ = 0x51, I64_NE = 0x52, I64_LT_S = 0x53, I64_LT_U = 0x54,
		I64_GT_S = 0x55, I64_GT_U = 0x56, I64_LE_S = 0x57, I64_LE_U = 0x58, I64_GE_S = 0x59, I64_GE_U = 0x5A,
		F32_EQ = 0x5B, F32_NE = 0x5C, F32_LT = 0x5D, F32_GT = 0x5E, F32_LE = 0x5F, F32_GE = 0x60,
		F64_EQ = 0x61, F64_NE = 0x62, F64_LT = 0x63, F64_GT = 0x64, F64_LE = 0x65, F64_GE = 0x66,
		I32_CLZ = 0x67, I32_CTZ = 0x68, I32_POPCNT = 0x69, I32_ADD = 0x6A, I32_SUB = 0x6B, I32_MUL = 0x6C,
		I32_DIV_S = 0x6D, I32_DIV_U = 0x6E, I32_REM_S = 0x6F, I32_REM_U = 0x70, I32_AND = 0x71, I32_OR = 0x72,
		I32_XOR = 0x73, I32_SHL = 0x74, I32_SHR_S = 0x75, I32_SHR_U = 0x76, I32_ROTL = 0x77, I32_ROTR = 0x78,
		I64_CLZ = 0x79, I64_CTZ = 0x7A, I64_POPCNT = 0x7B, I64_ADD = 0x7C, I64_SUB = 0x7D, I64_MUL = 0x7E,
		I64_DIV_S = 0x7F, I64_DIV_U = 0x80, I64_REM_S = 0x81, I64_REM_U = 0x82, I64_AND = 0x83, I64_OR = 0x84,
		I64_XOR = 0x85, I64_SHL = 0x86, I64_SHR_S = 0x87, I64_SHR_U = 0x88, I64_ROTL = 0x89, I64_ROTR = 0x8A,
		F32_ABS = 0x8B, F32_NEG = 0x8C, F32_SQRT = 0x91, F32_ADD = 0x92, F32_SUB = 0x93, F32_MUL = 0x94,
		F32_DIV = 0x95, F32_MIN = 0x96, F32_MAX = 0x97, F32_COPYSIGN = 0x98,
		F64_ABS = 0x99, F64_NEG = 0x9A, F64_SQRT = 0x9F, F64_ADD = 0xA0, F64_SUB = 0xA1, F64_MUL = 0xA2,
		F64_DIV = 0xA3, F64_MIN = 0xA4, F64_MAX = 0xA5, F64_COPYSIGN = 0xA6,
		I32_WRAP_I64 = 0xA7, I32_TRUNC_F32_S = 0xA8, I32_TRUNC_F64_S = 0xAA,
		I64_EXTEND_I32_S = 0xAC, I64_EXTEND_I32_U = 0xAD,
		F32_CONVERT_I32_S = 0xB2, F32_CONVERT_I32_U = 0xB3, F32_CONVERT_I64_S = 0xB4, F32_CONVERT_I64_U = 0xB5,
		F32_DEMOTE_F64 = 0xB6,
		F64_CONVERT_I32_S = 0xB7, F64_CONVERT_I32_U = 0xB8, F64_CONVERT_I64_S = 0xB9, F64_CONVERT_I64_U = 0xBA,
		F64_PROMOTE_F32 = 0xBB,
		I32_REINTERPRET_F32 = 0xBC, I64_REINTERPRET_F64 = 0xBD, F32_REINTERPRET_I32 = 0xBE, F64_REINTERPRET_I64 = 0xBF,
		I32_EXTEND8_S = 0xC0, I32_EXTEND16_S = 0xC1,
		I64_EXTEND8_S = 0xC2, I64_EXTEND16_S = 0xC3, I64_EXTEND32_S = 0xC4,
	};
	// 0xFC-prefixed: saturating float-to-int conversions
	enum OpFC : uint32_t {
		I32_TRUNC_SAT_F32_S = 0, I32_TRUNC_SAT_F32_U = 1, I32_TRUNC_SAT_F64_S = 2, I32_TRUNC_SAT_F64_U = 3,
		I64_TRUNC_SAT_F32_S = 4, I64_TRUNC_SAT_F32_U = 5, I64_TRUNC_SAT_F64_S = 6, I64_TRUNC_SAT_F64_U = 7,
	};

	enum Section : uint8_t {
		SEC_CUSTOM = 0, SEC_TYPE = 1, SEC_IMPORT = 2, SEC_FUNCTION = 3, SEC_TABLE = 4,
		SEC_MEMORY = 5, SEC_GLOBAL = 6, SEC_EXPORT = 7, SEC_START = 8, SEC_ELEMENT = 9,
		SEC_CODE = 10, SEC_DATA = 11,
	};
} // riscv::wasm
