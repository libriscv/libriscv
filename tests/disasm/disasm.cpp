/**
 * Linear disassembler built on libriscv's own decoder and instruction printers.
 *
 * Reads a flat blob of instruction bytes and prints one `addr<TAB>text` line per
 * instruction, where `text` is whatever libriscv's printer for the decoded
 * instruction produced. tests/disasm/verify.py diffs that against the same bytes
 * run through `objdump -d -M no-aliases`, so any disagreement is either a
 * decoding bug or a printer bug -- which is the whole point.
 *
 *   disasm [--rv32] [--base 0x1000] <blob>
 */
#include <libriscv/machine.hpp>
#include <libriscv/rv32i_instr.hpp>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::vector<uint8_t> read_file(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (f == nullptr) {
		fprintf(stderr, "disasm: cannot open %s\n", path);
		exit(1);
	}
	std::vector<uint8_t> data;
	uint8_t chunk[65536];
	size_t n;
	while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
		data.insert(data.end(), chunk, chunk + n);
	fclose(f);
	return data;
}

template <int W>
static void disassemble_blob(const std::vector<uint8_t>& code, uint64_t base)
{
	const std::vector<uint8_t> nothing;
	riscv::Machine<W> machine { nothing };

	size_t offset = 0;
	while (offset + 2 <= code.size())
	{
		riscv::rv32i_instruction instr;
		instr.whole = uint32_t(code[offset]) | (uint32_t(code[offset + 1]) << 8);
		const bool is_long = instr.is_long();
		if (is_long) {
			if (offset + 4 > code.size())
				break;
			instr.whole |= (uint32_t(code[offset + 2]) << 16)
						 | (uint32_t(code[offset + 3]) << 24);
		}

		// Printers resolve PC-relative operands against the current PC, so it
		// has to be the address this instruction actually sits at.
		const uint64_t addr = base + offset;
		machine.cpu.registers().pc = riscv::address_type<W>(addr);

		printf("%" PRIx64 "\t%s\n", addr, machine.cpu.disassemble(instr).c_str());
		offset += is_long ? 4 : 2;
	}
}

int main(int argc, char** argv)
{
	int xlen = 64;
	uint64_t base = 0;
	const char* path = nullptr;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--rv32") == 0)
			xlen = 32;
		else if (strcmp(argv[i], "--rv64") == 0)
			xlen = 64;
		else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc)
			base = strtoull(argv[++i], nullptr, 0);
		else
			path = argv[i];
	}
	if (path == nullptr) {
		fprintf(stderr, "usage: disasm [--rv32|--rv64] [--base ADDR] <blob>\n");
		return 1;
	}

	const auto code = read_file(path);
	if (xlen == 32)
		disassemble_blob<riscv::RISCV32>(code, base);
	else
		disassemble_blob<riscv::RISCV64>(code, base);
	return 0;
}
