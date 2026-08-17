#pragma once
#include "rv32i_instr.hpp"
#include "rvc.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cinttypes>

/**
 * Formatting helpers shared by every libriscv instruction printer.
 *
 * The printers reproduce `objdump -d -M no-aliases`
 */
namespace riscv
{
	struct RVPRINT
	{
		/// Integer register, ABI name (objdump default for RISC-V)
		static const char* reg(uint32_t r) noexcept
		{
			static const char* const names[32] = {
				"zero", "ra", "sp",  "gp",  "tp", "t0", "t1", "t2",
				"s0",   "s1", "a0",  "a1",  "a2", "a3", "a4", "a5",
				"a6",   "a7", "s2",  "s3",  "s4", "s5", "s6", "s7",
				"s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6",
			};
			return names[r & 31];
		}

		/// Floating-point register, ABI name
		static const char* freg(uint32_t r) noexcept
		{
			static const char* const names[32] = {
				"ft0", "ft1", "ft2",  "ft3",  "ft4", "ft5", "ft6", "ft7",
				"fs0", "fs1", "fa0",  "fa1",  "fa2", "fa3", "fa4", "fa5",
				"fa6", "fa7", "fs2",  "fs3",  "fs4", "fs5", "fs6", "fs7",
				"fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11",
			};
			return names[r & 31];
		}

		/// Vector register, v0 .. v31
		static const char* vreg(uint32_t r) noexcept
		{
			static const char* const names[32] = {
				"v0",  "v1",  "v2",  "v3",  "v4",  "v5",  "v6",  "v7",
				"v8",  "v9",  "v10", "v11", "v12", "v13", "v14", "v15",
				"v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
				"v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
			};
			return names[r & 31];
		}

		/// Compressed-format register field (3 bits, selects x8..x15).
		static const char* creg(uint32_t r) noexcept { return reg(8 + (r & 7)); }
		static const char* cfreg(uint32_t r) noexcept { return freg(8 + (r & 7)); }

		/// Rounding mode. Returns nullptr for `dyn`, which objdump omits.
		static const char* roundmode(uint32_t rm) noexcept
		{
			switch (rm) {
				case 0: return "rne";
				case 1: return "rtz";
				case 2: return "rdn";
				case 3: return "rup";
				case 4: return "rmm";
				case 7: return nullptr; // dyn: operand is not printed at all
				default: return "unknown";
			}
		}

		/// Memory-ordering bits of a FENCE, as objdump spells them.
		/// Only the 15 combinations binutils names are valid; the rest, and the
		/// empty set, come out as "unknown".
		static const char* fence_bits(uint32_t bits) noexcept
		{
			static const char* const names[16] = {
				"unknown", "w",   "r",   "rw",
				"o",       "ow",  "or",  "orw",
				"i",       "iw",  "ir",  "irw",
				"io",      "iow", "ior", "iorw",
			};
			return names[bits & 15];
		}

		/// Name of a CSR, or nullptr when binutils would print it as hex.
		static const char* csrname(uint32_t csr) noexcept
		{
			struct Entry { uint16_t num; const char* name; };
			static const Entry table[] = {
			{ 0x001, "fflags" },
			{ 0x002, "frm" },
			{ 0x003, "fcsr" },
			{ 0x008, "vstart" },
			{ 0x009, "vxsat" },
			{ 0x00A, "vxrm" },
			{ 0x00F, "vcsr" },
			{ 0x015, "seed" },
			{ 0x100, "sstatus" },
			{ 0x104, "sie" },
			{ 0x105, "stvec" },
			{ 0x106, "scounteren" },
			{ 0x10A, "senvcfg" },
			{ 0x10C, "sstateen0" },
			{ 0x10D, "sstateen1" },
			{ 0x10E, "sstateen2" },
			{ 0x10F, "sstateen3" },
			{ 0x114, "sieh" },
			{ 0x140, "sscratch" },
			{ 0x141, "sepc" },
			{ 0x142, "scause" },
			{ 0x143, "stval" },
			{ 0x144, "sip" },
			{ 0x14D, "stimecmp" },
			{ 0x150, "siselect" },
			{ 0x151, "sireg" },
			{ 0x152, "sireg2" },
			{ 0x153, "sireg3" },
			{ 0x154, "siph" },
			{ 0x155, "sireg4" },
			{ 0x156, "sireg5" },
			{ 0x157, "sireg6" },
			{ 0x15C, "stopei" },
			{ 0x15D, "stimecmph" },
			{ 0x180, "satp" },
			{ 0x200, "vsstatus" },
			{ 0x204, "vsie" },
			{ 0x205, "vstvec" },
			{ 0x214, "vsieh" },
			{ 0x240, "vsscratch" },
			{ 0x241, "vsepc" },
			{ 0x242, "vscause" },
			{ 0x243, "vstval" },
			{ 0x244, "vsip" },
			{ 0x24D, "vstimecmp" },
			{ 0x250, "vsiselect" },
			{ 0x251, "vsireg" },
			{ 0x252, "vsireg2" },
			{ 0x253, "vsireg3" },
			{ 0x254, "vsiph" },
			{ 0x255, "vsireg4" },
			{ 0x256, "vsireg5" },
			{ 0x257, "vsireg6" },
			{ 0x25C, "vstopei" },
			{ 0x25D, "vstimecmph" },
			{ 0x280, "vsatp" },
			{ 0x300, "mstatus" },
			{ 0x301, "misa" },
			{ 0x302, "medeleg" },
			{ 0x303, "mideleg" },
			{ 0x304, "mie" },
			{ 0x305, "mtvec" },
			{ 0x306, "mcounteren" },
			{ 0x308, "mvien" },
			{ 0x309, "mvip" },
			{ 0x30A, "menvcfg" },
			{ 0x30C, "mstateen0" },
			{ 0x30D, "mstateen1" },
			{ 0x30E, "mstateen2" },
			{ 0x30F, "mstateen3" },
			{ 0x310, "mstatush" },
			{ 0x313, "midelegh" },
			{ 0x314, "mieh" },
			{ 0x318, "mvienh" },
			{ 0x319, "mviph" },
			{ 0x31A, "menvcfgh" },
			{ 0x31C, "mstateen0h" },
			{ 0x31D, "mstateen1h" },
			{ 0x31E, "mstateen2h" },
			{ 0x31F, "mstateen3h" },
			{ 0x320, "mcountinhibit" },
			{ 0x321, "mcyclecfg" },
			{ 0x322, "minstretcfg" },
			{ 0x323, "mhpmevent3" },
			{ 0x324, "mhpmevent4" },
			{ 0x325, "mhpmevent5" },
			{ 0x326, "mhpmevent6" },
			{ 0x327, "mhpmevent7" },
			{ 0x328, "mhpmevent8" },
			{ 0x329, "mhpmevent9" },
			{ 0x32A, "mhpmevent10" },
			{ 0x32B, "mhpmevent11" },
			{ 0x32C, "mhpmevent12" },
			{ 0x32D, "mhpmevent13" },
			{ 0x32E, "mhpmevent14" },
			{ 0x32F, "mhpmevent15" },
			{ 0x330, "mhpmevent16" },
			{ 0x331, "mhpmevent17" },
			{ 0x332, "mhpmevent18" },
			{ 0x333, "mhpmevent19" },
			{ 0x334, "mhpmevent20" },
			{ 0x335, "mhpmevent21" },
			{ 0x336, "mhpmevent22" },
			{ 0x337, "mhpmevent23" },
			{ 0x338, "mhpmevent24" },
			{ 0x339, "mhpmevent25" },
			{ 0x33A, "mhpmevent26" },
			{ 0x33B, "mhpmevent27" },
			{ 0x33C, "mhpmevent28" },
			{ 0x33D, "mhpmevent29" },
			{ 0x33E, "mhpmevent30" },
			{ 0x33F, "mhpmevent31" },
			{ 0x340, "mscratch" },
			{ 0x341, "mepc" },
			{ 0x342, "mcause" },
			{ 0x343, "mtval" },
			{ 0x344, "mip" },
			{ 0x34A, "mtinst" },
			{ 0x34B, "mtval2" },
			{ 0x350, "miselect" },
			{ 0x351, "mireg" },
			{ 0x352, "mireg2" },
			{ 0x353, "mireg3" },
			{ 0x354, "miph" },
			{ 0x355, "mireg4" },
			{ 0x356, "mireg5" },
			{ 0x357, "mireg6" },
			{ 0x35C, "mtopei" },
			{ 0x3A0, "pmpcfg0" },
			{ 0x3A1, "pmpcfg1" },
			{ 0x3A2, "pmpcfg2" },
			{ 0x3A3, "pmpcfg3" },
			{ 0x3A4, "pmpcfg4" },
			{ 0x3A5, "pmpcfg5" },
			{ 0x3A6, "pmpcfg6" },
			{ 0x3A7, "pmpcfg7" },
			{ 0x3A8, "pmpcfg8" },
			{ 0x3A9, "pmpcfg9" },
			{ 0x3AA, "pmpcfg10" },
			{ 0x3AB, "pmpcfg11" },
			{ 0x3AC, "pmpcfg12" },
			{ 0x3AD, "pmpcfg13" },
			{ 0x3AE, "pmpcfg14" },
			{ 0x3AF, "pmpcfg15" },
			{ 0x3B0, "pmpaddr0" },
			{ 0x3B1, "pmpaddr1" },
			{ 0x3B2, "pmpaddr2" },
			{ 0x3B3, "pmpaddr3" },
			{ 0x3B4, "pmpaddr4" },
			{ 0x3B5, "pmpaddr5" },
			{ 0x3B6, "pmpaddr6" },
			{ 0x3B7, "pmpaddr7" },
			{ 0x3B8, "pmpaddr8" },
			{ 0x3B9, "pmpaddr9" },
			{ 0x3BA, "pmpaddr10" },
			{ 0x3BB, "pmpaddr11" },
			{ 0x3BC, "pmpaddr12" },
			{ 0x3BD, "pmpaddr13" },
			{ 0x3BE, "pmpaddr14" },
			{ 0x3BF, "pmpaddr15" },
			{ 0x3C0, "pmpaddr16" },
			{ 0x3C1, "pmpaddr17" },
			{ 0x3C2, "pmpaddr18" },
			{ 0x3C3, "pmpaddr19" },
			{ 0x3C4, "pmpaddr20" },
			{ 0x3C5, "pmpaddr21" },
			{ 0x3C6, "pmpaddr22" },
			{ 0x3C7, "pmpaddr23" },
			{ 0x3C8, "pmpaddr24" },
			{ 0x3C9, "pmpaddr25" },
			{ 0x3CA, "pmpaddr26" },
			{ 0x3CB, "pmpaddr27" },
			{ 0x3CC, "pmpaddr28" },
			{ 0x3CD, "pmpaddr29" },
			{ 0x3CE, "pmpaddr30" },
			{ 0x3CF, "pmpaddr31" },
			{ 0x3D0, "pmpaddr32" },
			{ 0x3D1, "pmpaddr33" },
			{ 0x3D2, "pmpaddr34" },
			{ 0x3D3, "pmpaddr35" },
			{ 0x3D4, "pmpaddr36" },
			{ 0x3D5, "pmpaddr37" },
			{ 0x3D6, "pmpaddr38" },
			{ 0x3D7, "pmpaddr39" },
			{ 0x3D8, "pmpaddr40" },
			{ 0x3D9, "pmpaddr41" },
			{ 0x3DA, "pmpaddr42" },
			{ 0x3DB, "pmpaddr43" },
			{ 0x3DC, "pmpaddr44" },
			{ 0x3DD, "pmpaddr45" },
			{ 0x3DE, "pmpaddr46" },
			{ 0x3DF, "pmpaddr47" },
			{ 0x3E0, "pmpaddr48" },
			{ 0x3E1, "pmpaddr49" },
			{ 0x3E2, "pmpaddr50" },
			{ 0x3E3, "pmpaddr51" },
			{ 0x3E4, "pmpaddr52" },
			{ 0x3E5, "pmpaddr53" },
			{ 0x3E6, "pmpaddr54" },
			{ 0x3E7, "pmpaddr55" },
			{ 0x3E8, "pmpaddr56" },
			{ 0x3E9, "pmpaddr57" },
			{ 0x3EA, "pmpaddr58" },
			{ 0x3EB, "pmpaddr59" },
			{ 0x3EC, "pmpaddr60" },
			{ 0x3ED, "pmpaddr61" },
			{ 0x3EE, "pmpaddr62" },
			{ 0x3EF, "pmpaddr63" },
			{ 0x5A8, "scontext" },
			{ 0x600, "hstatus" },
			{ 0x602, "hedeleg" },
			{ 0x603, "hideleg" },
			{ 0x604, "hie" },
			{ 0x605, "htimedelta" },
			{ 0x606, "hcounteren" },
			{ 0x607, "hgeie" },
			{ 0x608, "hvien" },
			{ 0x609, "hvictl" },
			{ 0x60A, "henvcfg" },
			{ 0x60C, "hstateen0" },
			{ 0x60D, "hstateen1" },
			{ 0x60E, "hstateen2" },
			{ 0x60F, "hstateen3" },
			{ 0x613, "hidelegh" },
			{ 0x615, "htimedeltah" },
			{ 0x618, "hvienh" },
			{ 0x61A, "henvcfgh" },
			{ 0x61C, "hstateen0h" },
			{ 0x61D, "hstateen1h" },
			{ 0x61E, "hstateen2h" },
			{ 0x61F, "hstateen3h" },
			{ 0x643, "htval" },
			{ 0x644, "hip" },
			{ 0x645, "hvip" },
			{ 0x646, "hviprio1" },
			{ 0x647, "hviprio2" },
			{ 0x64A, "htinst" },
			{ 0x655, "hviph" },
			{ 0x656, "hviprio1h" },
			{ 0x657, "hviprio2h" },
			{ 0x680, "hgatp" },
			{ 0x6A8, "hcontext" },
			{ 0x721, "mcyclecfgh" },
			{ 0x722, "minstretcfgh" },
			{ 0x723, "mhpmevent3h" },
			{ 0x724, "mhpmevent4h" },
			{ 0x725, "mhpmevent5h" },
			{ 0x726, "mhpmevent6h" },
			{ 0x727, "mhpmevent7h" },
			{ 0x728, "mhpmevent8h" },
			{ 0x729, "mhpmevent9h" },
			{ 0x72A, "mhpmevent10h" },
			{ 0x72B, "mhpmevent11h" },
			{ 0x72C, "mhpmevent12h" },
			{ 0x72D, "mhpmevent13h" },
			{ 0x72E, "mhpmevent14h" },
			{ 0x72F, "mhpmevent15h" },
			{ 0x730, "mhpmevent16h" },
			{ 0x731, "mhpmevent17h" },
			{ 0x732, "mhpmevent18h" },
			{ 0x733, "mhpmevent19h" },
			{ 0x734, "mhpmevent20h" },
			{ 0x735, "mhpmevent21h" },
			{ 0x736, "mhpmevent22h" },
			{ 0x737, "mhpmevent23h" },
			{ 0x738, "mhpmevent24h" },
			{ 0x739, "mhpmevent25h" },
			{ 0x73A, "mhpmevent26h" },
			{ 0x73B, "mhpmevent27h" },
			{ 0x73C, "mhpmevent28h" },
			{ 0x73D, "mhpmevent29h" },
			{ 0x73E, "mhpmevent30h" },
			{ 0x73F, "mhpmevent31h" },
			{ 0x747, "mseccfg" },
			{ 0x757, "mseccfgh" },
			{ 0x7A0, "tselect" },
			{ 0x7A1, "tdata1" },
			{ 0x7A2, "tdata2" },
			{ 0x7A3, "tdata3" },
			{ 0x7A4, "tinfo" },
			{ 0x7A5, "tcontrol" },
			{ 0x7A8, "mcontext" },
			{ 0x7AA, "mscontext" },
			{ 0x7B0, "dcsr" },
			{ 0x7B1, "dpc" },
			{ 0x7B2, "dscratch0" },
			{ 0x7B3, "dscratch1" },
			{ 0xB00, "mcycle" },
			{ 0xB02, "minstret" },
			{ 0xB03, "mhpmcounter3" },
			{ 0xB04, "mhpmcounter4" },
			{ 0xB05, "mhpmcounter5" },
			{ 0xB06, "mhpmcounter6" },
			{ 0xB07, "mhpmcounter7" },
			{ 0xB08, "mhpmcounter8" },
			{ 0xB09, "mhpmcounter9" },
			{ 0xB0A, "mhpmcounter10" },
			{ 0xB0B, "mhpmcounter11" },
			{ 0xB0C, "mhpmcounter12" },
			{ 0xB0D, "mhpmcounter13" },
			{ 0xB0E, "mhpmcounter14" },
			{ 0xB0F, "mhpmcounter15" },
			{ 0xB10, "mhpmcounter16" },
			{ 0xB11, "mhpmcounter17" },
			{ 0xB12, "mhpmcounter18" },
			{ 0xB13, "mhpmcounter19" },
			{ 0xB14, "mhpmcounter20" },
			{ 0xB15, "mhpmcounter21" },
			{ 0xB16, "mhpmcounter22" },
			{ 0xB17, "mhpmcounter23" },
			{ 0xB18, "mhpmcounter24" },
			{ 0xB19, "mhpmcounter25" },
			{ 0xB1A, "mhpmcounter26" },
			{ 0xB1B, "mhpmcounter27" },
			{ 0xB1C, "mhpmcounter28" },
			{ 0xB1D, "mhpmcounter29" },
			{ 0xB1E, "mhpmcounter30" },
			{ 0xB1F, "mhpmcounter31" },
			{ 0xB80, "mcycleh" },
			{ 0xB82, "minstreth" },
			{ 0xB83, "mhpmcounter3h" },
			{ 0xB84, "mhpmcounter4h" },
			{ 0xB85, "mhpmcounter5h" },
			{ 0xB86, "mhpmcounter6h" },
			{ 0xB87, "mhpmcounter7h" },
			{ 0xB88, "mhpmcounter8h" },
			{ 0xB89, "mhpmcounter9h" },
			{ 0xB8A, "mhpmcounter10h" },
			{ 0xB8B, "mhpmcounter11h" },
			{ 0xB8C, "mhpmcounter12h" },
			{ 0xB8D, "mhpmcounter13h" },
			{ 0xB8E, "mhpmcounter14h" },
			{ 0xB8F, "mhpmcounter15h" },
			{ 0xB90, "mhpmcounter16h" },
			{ 0xB91, "mhpmcounter17h" },
			{ 0xB92, "mhpmcounter18h" },
			{ 0xB93, "mhpmcounter19h" },
			{ 0xB94, "mhpmcounter20h" },
			{ 0xB95, "mhpmcounter21h" },
			{ 0xB96, "mhpmcounter22h" },
			{ 0xB97, "mhpmcounter23h" },
			{ 0xB98, "mhpmcounter24h" },
			{ 0xB99, "mhpmcounter25h" },
			{ 0xB9A, "mhpmcounter26h" },
			{ 0xB9B, "mhpmcounter27h" },
			{ 0xB9C, "mhpmcounter28h" },
			{ 0xB9D, "mhpmcounter29h" },
			{ 0xB9E, "mhpmcounter30h" },
			{ 0xB9F, "mhpmcounter31h" },
			{ 0xC00, "cycle" },
			{ 0xC01, "time" },
			{ 0xC02, "instret" },
			{ 0xC03, "hpmcounter3" },
			{ 0xC04, "hpmcounter4" },
			{ 0xC05, "hpmcounter5" },
			{ 0xC06, "hpmcounter6" },
			{ 0xC07, "hpmcounter7" },
			{ 0xC08, "hpmcounter8" },
			{ 0xC09, "hpmcounter9" },
			{ 0xC0A, "hpmcounter10" },
			{ 0xC0B, "hpmcounter11" },
			{ 0xC0C, "hpmcounter12" },
			{ 0xC0D, "hpmcounter13" },
			{ 0xC0E, "hpmcounter14" },
			{ 0xC0F, "hpmcounter15" },
			{ 0xC10, "hpmcounter16" },
			{ 0xC11, "hpmcounter17" },
			{ 0xC12, "hpmcounter18" },
			{ 0xC13, "hpmcounter19" },
			{ 0xC14, "hpmcounter20" },
			{ 0xC15, "hpmcounter21" },
			{ 0xC16, "hpmcounter22" },
			{ 0xC17, "hpmcounter23" },
			{ 0xC18, "hpmcounter24" },
			{ 0xC19, "hpmcounter25" },
			{ 0xC1A, "hpmcounter26" },
			{ 0xC1B, "hpmcounter27" },
			{ 0xC1C, "hpmcounter28" },
			{ 0xC1D, "hpmcounter29" },
			{ 0xC1E, "hpmcounter30" },
			{ 0xC1F, "hpmcounter31" },
			{ 0xC20, "vl" },
			{ 0xC21, "vtype" },
			{ 0xC22, "vlenb" },
			{ 0xC80, "cycleh" },
			{ 0xC81, "timeh" },
			{ 0xC82, "instreth" },
			{ 0xC83, "hpmcounter3h" },
			{ 0xC84, "hpmcounter4h" },
			{ 0xC85, "hpmcounter5h" },
			{ 0xC86, "hpmcounter6h" },
			{ 0xC87, "hpmcounter7h" },
			{ 0xC88, "hpmcounter8h" },
			{ 0xC89, "hpmcounter9h" },
			{ 0xC8A, "hpmcounter10h" },
			{ 0xC8B, "hpmcounter11h" },
			{ 0xC8C, "hpmcounter12h" },
			{ 0xC8D, "hpmcounter13h" },
			{ 0xC8E, "hpmcounter14h" },
			{ 0xC8F, "hpmcounter15h" },
			{ 0xC90, "hpmcounter16h" },
			{ 0xC91, "hpmcounter17h" },
			{ 0xC92, "hpmcounter18h" },
			{ 0xC93, "hpmcounter19h" },
			{ 0xC94, "hpmcounter20h" },
			{ 0xC95, "hpmcounter21h" },
			{ 0xC96, "hpmcounter22h" },
			{ 0xC97, "hpmcounter23h" },
			{ 0xC98, "hpmcounter24h" },
			{ 0xC99, "hpmcounter25h" },
			{ 0xC9A, "hpmcounter26h" },
			{ 0xC9B, "hpmcounter27h" },
			{ 0xC9C, "hpmcounter28h" },
			{ 0xC9D, "hpmcounter29h" },
			{ 0xC9E, "hpmcounter30h" },
			{ 0xC9F, "hpmcounter31h" },
			{ 0xDA0, "scountovf" },
			{ 0xDB0, "stopi" },
			{ 0xE12, "hgeip" },
			{ 0xEB0, "vstopi" },
			{ 0xF11, "mvendorid" },
			{ 0xF12, "marchid" },
			{ 0xF13, "mimpid" },
			{ 0xF14, "mhartid" },
			{ 0xF15, "mconfigptr" },
			{ 0xFB0, "mtopi" },
			};
			// The table is sorted by number, so binary search it.
			size_t lo = 0, hi = sizeof(table) / sizeof(table[0]);
			while (lo < hi) {
				const size_t mid = lo + (hi - lo) / 2;
				if (table[mid].num < csr) lo = mid + 1;
				else if (table[mid].num > csr) hi = mid;
				else return table[mid].name;
			}
			return nullptr;
		}

		/// Width suffix of an atomic instruction: .w, .d or .q.
		/// Returns nullptr for the widths that do not exist.
		static const char* amo_width(uint32_t funct3) noexcept
		{
			switch (funct3) {
				case 0x2: return "w";
				case 0x3: return "d";
				case 0x4: return "q";
				default:  return nullptr;
			}
		}

		/// Ordering suffix of an atomic instruction, from the low two bits of
		/// funct7: "", ".rl", ".aq" or ".aqrl".
		static const char* amo_ordering(uint32_t aqrl) noexcept
		{
			static const char* const names[4] = { "", ".rl", ".aq", ".aqrl" };
			return names[aqrl & 3];
		}

		/// `amoadd.w.aqrl\ta5,a2,(a1)`: the AMO family.
		static int amo(char* buf, size_t len, const char* name, uint32_t funct3,
			uint32_t aqrl, uint32_t rd, uint32_t rs1, uint32_t rs2) noexcept
		{
			return snprintf(buf, len, "%s.%s%s\t%s,%s,(%s)",
				name, amo_width(funct3), amo_ordering(aqrl),
				reg(rd), reg(rs2), reg(rs1));
		}

		/// objdump-isms: 0x0000002f prints as 0x002f, 0x000f002f as 0x000f002f.
		static int illegal(char* buf, size_t len, uint32_t whole, uint32_t length) noexcept
		{
			if (length == 2) {
				// All-zeroes is the canonical compressed trap-always encoding,
				// and binutils gives it a name rather than dumping the bits.
				if (uint16_t(whole) == 0x0000)
					return snprintf(buf, len, "c.unimp");
				return snprintf(buf, len, ".insn\t2, 0x%04x", uint16_t(whole));
			}
			if ((whole >> 16) == 0)
				return snprintf(buf, len, ".insn\t4, 0x%04x", whole);
			return snprintf(buf, len, ".insn\t4, 0x%08x", whole);
		}
	};

	inline int rv_expect_mnemonic(char* b, size_t n, const char* expected, int written) noexcept
	{
		if (written < 0)
			return written;
		if (b[0] == '.' || __builtin_strcmp(b, "unimp") == 0
			|| __builtin_strcmp(b, "c.unimp") == 0)
			return written;
		const size_t elen = __builtin_strlen(expected);
		if (size_t(written) >= elen && __builtin_memcmp(b, expected, elen) == 0
			&& (size_t(written) == elen || b[elen] == '\t' || b[elen] == '.'))
			return written;
		return snprintf(b, n, "%s\t<decoder routed a different instruction here>", expected);
	}

	struct RVDISASM
	{
		using instr_t = rv32i_instruction;

		/// Resolve a PC-relative target, wrapping at the register width.
		static uint64_t pcrel(uint64_t pc, int64_t offset, bool is64) noexcept
		{
			const uint64_t target = pc + uint64_t(offset);
			return is64 ? target : uint32_t(target);
		}

		static int bad(char* b, size_t n, instr_t i) noexcept
		{
			return RVPRINT::illegal(b, n, i.whole, i.length());
		}

		// ---- operand shapes ------------------------------------------------

		/// `add\ta5,a1,a2`
		static int rrr(char* b, size_t n, const char* m, instr_t i) noexcept
		{
			return snprintf(b, n, "%s\t%s,%s,%s", m, RVPRINT::reg(i.Rtype.rd),
				RVPRINT::reg(i.Rtype.rs1), RVPRINT::reg(i.Rtype.rs2));
		}
		/// `zext.h\ta5,a1`: an R-type whose rs2 is part of the opcode.
		static int rr(char* b, size_t n, const char* m, instr_t i) noexcept
		{
			return snprintf(b, n, "%s\t%s,%s", m, RVPRINT::reg(i.Rtype.rd),
				RVPRINT::reg(i.Rtype.rs1));
		}
		/// `addi\ta5,a1,-12`: signed decimal immediate.
		static int rri(char* b, size_t n, const char* m, instr_t i) noexcept
		{
			return snprintf(b, n, "%s\t%s,%s,%d", m, RVPRINT::reg(i.Itype.rd),
				RVPRINT::reg(i.Itype.rs1), i.Itype.signed_imm());
		}
		/// `slli\ta5,a1,0xc`: shift amounts print as hex.
		static int rrs(char* b, size_t n, const char* m, instr_t i, uint32_t shamt) noexcept
		{
			return snprintf(b, n, "%s\t%s,%s,0x%x", m, RVPRINT::reg(i.Itype.rd),
				RVPRINT::reg(i.Itype.rs1), shamt);
		}
		/// `clz\ta5,a1`: an I-type whose immediate is part of the opcode.
		static int ri(char* b, size_t n, const char* m, instr_t i) noexcept
		{
			return snprintf(b, n, "%s\t%s,%s", m, RVPRINT::reg(i.Itype.rd),
				RVPRINT::reg(i.Itype.rs1));
		}
		/// `lw\ta5,-12(a1)`
		static int load(char* b, size_t n, const char* m, instr_t i) noexcept
		{
			return snprintf(b, n, "%s\t%s,%d(%s)", m, RVPRINT::reg(i.Itype.rd),
				i.Itype.signed_imm(), RVPRINT::reg(i.Itype.rs1));
		}
		/// `sw\ta2,-12(a1)`
		static int store(char* b, size_t n, const char* m, instr_t i) noexcept
		{
			return snprintf(b, n, "%s\t%s,%d(%s)", m, RVPRINT::reg(i.Stype.rs2),
				i.Stype.signed_imm(), RVPRINT::reg(i.Stype.rs1));
		}

		// ---- families ------------------------------------------------------

		/// LOAD (0x03)
		static int op_load(char* b, size_t n, instr_t i, bool is128) noexcept
		{
			static const char* const names[8] =
				{ "lb", "lh", "lw", "ld", "lbu", "lhu", "lwu", "lq" };
			if (i.Itype.funct3 == 0x7 && !is128)
				return bad(b, n, i);
			return load(b, n, names[i.Itype.funct3], i);
		}

		/// STORE (0x23)
		static int op_store(char* b, size_t n, instr_t i, bool is128) noexcept
		{
			static const char* const names[8] =
				{ "sb", "sh", "sw", "sd", "sq", nullptr, nullptr, nullptr };
			const char* m = names[i.Stype.funct3];
			if (m == nullptr || (i.Stype.funct3 == 0x4 && !is128))
				return bad(b, n, i);
			return store(b, n, m, i);
		}

		/// BRANCH (0x63)
		static int op_branch(char* b, size_t n, instr_t i, uint64_t pc, bool is64) noexcept
		{
			static const char* const names[8] =
				{ "beq", "bne", nullptr, nullptr, "blt", "bge", "bltu", "bgeu" };
			const char* m = names[i.Btype.funct3];
			if (m == nullptr)
				return bad(b, n, i);
			return snprintf(b, n, "%s\t%s,%s,0x%" PRIx64, m,
				RVPRINT::reg(i.Btype.rs1), RVPRINT::reg(i.Btype.rs2),
				pcrel(pc, i.Btype.signed_imm(), is64));
		}

		/// JAL (0x6f).
		static int op_jal(char* b, size_t n, instr_t i, uint64_t pc, bool is64) noexcept
		{
			return snprintf(b, n, "jal\t%s,0x%" PRIx64, RVPRINT::reg(i.Jtype.rd),
				pcrel(pc, i.Jtype.jump_offset(), is64));
		}

		/// JALR (0x67): printed in the load's offset(base) shape
		static int op_jalr(char* b, size_t n, instr_t i) noexcept
		{
			if (i.Itype.funct3 != 0)
				return bad(b, n, i);
			return load(b, n, "jalr", i);
		}

		/// LUI (0x37) and AUIPC (0x17): the raw 20-bit field, in hex
		static int op_lui(char* b, size_t n, instr_t i) noexcept
		{
			return snprintf(b, n, "lui\t%s,0x%x", RVPRINT::reg(i.Utype.rd), i.Utype.imm);
		}
		static int op_auipc(char* b, size_t n, instr_t i) noexcept
		{
			return snprintf(b, n, "auipc\t%s,0x%x", RVPRINT::reg(i.Utype.rd), i.Utype.imm);
		}

		/// OP-IMM (0x13)
		static int op_imm(char* b, size_t n, instr_t i, bool is64) noexcept
		{
			const uint32_t imm   = i.Itype.imm;
			const uint32_t shamt = is64 ? (imm & 0x3F) : (imm & 0x1F);
			const uint32_t sel   = is64 ? (imm >> 6)   : (imm >> 5);
			switch (i.Itype.funct3) {
			case 0x0: return rri(b, n, "addi", i);
			case 0x2: return rri(b, n, "slti", i);
			case 0x3: return rri(b, n, "sltiu", i);
			case 0x4: return rri(b, n, "xori", i);
			case 0x6: return rri(b, n, "ori", i);
			case 0x7: return rri(b, n, "andi", i);
			case 0x1:
				if (sel == 0x00)                  return rrs(b, n, "slli", i, shamt);
				if (sel == (is64 ? 0x0Au : 0x14u)) return rrs(b, n, "bseti", i, shamt);
				if (sel == (is64 ? 0x12u : 0x24u)) return rrs(b, n, "bclri", i, shamt);
				if (sel == (is64 ? 0x1Au : 0x34u)) return rrs(b, n, "binvi", i, shamt);
				if (!is64 && sel == 0x04 && shamt == 0x0F)
					return ri(b, n, "zip", i);
				if (sel == (is64 ? 0x18u : 0x30u)) {
					switch (shamt) {
					case 0x0: return ri(b, n, "clz", i);
					case 0x1: return ri(b, n, "ctz", i);
					case 0x2: return ri(b, n, "cpop", i);
					case 0x4: return ri(b, n, "sext.b", i);
					case 0x5: return ri(b, n, "sext.h", i);
					}
				}
				break;
			case 0x5:
				if (sel == 0x00)                  return rrs(b, n, "srli", i, shamt);
				if (sel == (is64 ? 0x10u : 0x20u)) return rrs(b, n, "srai", i, shamt);
				if (sel == (is64 ? 0x12u : 0x24u)) return rrs(b, n, "bexti", i, shamt);
				if (sel == (is64 ? 0x18u : 0x30u)) return rrs(b, n, "rori", i, shamt);
				if (sel == (is64 ? 0x0Au : 0x14u) && shamt == 0x07)
					return ri(b, n, "orc.b", i);
				if (sel == (is64 ? 0x1Au : 0x34u) && shamt == 0x07)
					return ri(b, n, "brev8", i);
				if (sel == (is64 ? 0x1Au : 0x34u) && shamt == (is64 ? 0x38u : 0x18u))
					return ri(b, n, "rev8", i);
				// UNZIP gathers the even/odd bit halves; RV32 only, since the
				// RV64 encoding space is taken by the wider shift amount.
				if (!is64 && sel == 0x04 && shamt == 0x0F)
					return ri(b, n, "unzip", i);
				break;
			}
			return bad(b, n, i);
		}

		/// OP-IMM-32 (0x1b). SLLI.UW is the odd one out: it takes a full 6-bit
		/// RV64 shift amount even though every other *W shift takes five.
		static int op_imm32(char* b, size_t n, instr_t i) noexcept
		{
			const uint32_t imm = i.Itype.imm;
			switch (i.Itype.funct3) {
			case 0x0: return rri(b, n, "addiw", i);
			case 0x1:
				if ((imm >> 5) == 0x00) return rrs(b, n, "slliw", i, imm & 0x1F);
				if ((imm >> 6) == 0x02) return rrs(b, n, "slli.uw", i, imm & 0x3F);
				if ((imm >> 5) == 0x30) {
					switch (imm & 0x1F) {
					case 0x0: return ri(b, n, "clzw", i);
					case 0x1: return ri(b, n, "ctzw", i);
					case 0x2: return ri(b, n, "cpopw", i);
					}
				}
				break;
			case 0x5:
				if ((imm >> 5) == 0x00) return rrs(b, n, "srliw", i, imm & 0x1F);
				if ((imm >> 5) == 0x20) return rrs(b, n, "sraiw", i, imm & 0x1F);
				if ((imm >> 5) == 0x30) return rrs(b, n, "roriw", i, imm & 0x1F);
				break;
			}
			return bad(b, n, i);
		}

		/// OP (0x33): RV32I/M plus Zba, Zbb, Zbc, Zbs and Zicond.
		static int op_reg(char* b, size_t n, instr_t i, bool is64) noexcept
		{
			const uint32_t f3 = i.Rtype.funct3;
			switch (i.Rtype.funct7) {
			case 0x00: {
				static const char* const m[8] =
					{ "add", "sll", "slt", "sltu", "xor", "srl", "or", "and" };
				return rrr(b, n, m[f3], i);
			}
			case 0x01: {
				static const char* const m[8] =
					{ "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu" };
				return rrr(b, n, m[f3], i);
			}
			case 0x05: {
				static const char* const m[8] =
					{ nullptr, "clmul", "clmulr", "clmulh", "min", "minu", "max", "maxu" };
				if (m[f3]) return rrr(b, n, m[f3], i);
				break;
			}
			case 0x04:
				if (f3 == 4) {
					if (!is64 && i.Rtype.rs2 == 0) return rr(b, n, "zext.h", i);
					return rrr(b, n, "pack", i);
				}
				if (f3 == 7) return rrr(b, n, "packh", i);
				break;
			case 0x07:
				if (f3 == 5) return rrr(b, n, "czero.eqz", i);
				if (f3 == 7) return rrr(b, n, "czero.nez", i);
				break;
			case 0x10:
				if (f3 == 2) return rrr(b, n, "sh1add", i);
				if (f3 == 4) return rrr(b, n, "sh2add", i);
				if (f3 == 6) return rrr(b, n, "sh3add", i);
				break;
			case 0x14:
				if (f3 == 1) return rrr(b, n, "bset", i);
				break;
			case 0x20: {
				static const char* const m[8] =
					{ "sub", nullptr, nullptr, nullptr, "xnor", "sra", "orn", "andn" };
				if (m[f3]) return rrr(b, n, m[f3], i);
				break;
			}
			case 0x24:
				if (f3 == 1) return rrr(b, n, "bclr", i);
				if (f3 == 5) return rrr(b, n, "bext", i);
				break;
			case 0x30:
				if (f3 == 1) return rrr(b, n, "rol", i);
				if (f3 == 5) return rrr(b, n, "ror", i);
				break;
			case 0x34:
				if (f3 == 1) return rrr(b, n, "binv", i);
				break;
			}
			return bad(b, n, i);
		}

		/// OP-32 (0x3b): the word-width forms, plus Zba's add.uw family.
		static int op_reg32(char* b, size_t n, instr_t i) noexcept
		{
			const uint32_t f3 = i.Rtype.funct3;
			switch (i.Rtype.funct7) {
			case 0x00:
				if (f3 == 0) return rrr(b, n, "addw", i);
				if (f3 == 1) return rrr(b, n, "sllw", i);
				if (f3 == 5) return rrr(b, n, "srlw", i);
				break;
			case 0x01: {
				static const char* const m[8] = { "mulw", nullptr, nullptr, nullptr,
					"divw", "divuw", "remw", "remuw" };
				if (m[f3]) return rrr(b, n, m[f3], i);
				break;
			}
			case 0x04:
				if (f3 == 0) return rrr(b, n, "add.uw", i);
				if (f3 == 4) {
					// ZEXT.H is PACKW with rs2 hardwired to x0.
					if (i.Rtype.rs2 == 0) return rr(b, n, "zext.h", i);
					return rrr(b, n, "packw", i);
				}
				break;
			case 0x10:
				if (f3 == 2) return rrr(b, n, "sh1add.uw", i);
				if (f3 == 4) return rrr(b, n, "sh2add.uw", i);
				if (f3 == 6) return rrr(b, n, "sh3add.uw", i);
				break;
			case 0x20:
				if (f3 == 0) return rrr(b, n, "subw", i);
				if (f3 == 5) return rrr(b, n, "sraw", i);
				break;
			case 0x30:
				if (f3 == 1) return rrr(b, n, "rolw", i);
				if (f3 == 5) return rrr(b, n, "rorw", i);
				break;
			}
			return bad(b, n, i);
		}

		/// SYSTEM (0x73): the environment instructions and the Zicsr family.
		static int op_system(char* b, size_t n, instr_t i) noexcept
		{
			if (i.whole == 0xC0001073)
				return snprintf(b, n, "unimp");
			if (i.Itype.funct3 == 0) {
				if (i.Itype.rd != 0 || i.Itype.rs1 != 0)
					return bad(b, n, i);
				switch (i.Itype.imm) {
				case 0x000: return snprintf(b, n, "ecall");
				case 0x001: return snprintf(b, n, "ebreak");
				case 0x102: return snprintf(b, n, "sret");
				case 0x105: return snprintf(b, n, "wfi");
				case 0x302: return snprintf(b, n, "mret");
				// Zawrs: wait on a reservation set, until either a store
				// clears it (nto) or an implementation-defined timeout does.
				case 0x00D: return snprintf(b, n, "wrs.nto");
				case 0x01D: return snprintf(b, n, "wrs.sto");
				}
				return bad(b, n, i);
			}
			//   MOP.R.n   1 n4 00 n3 n2 0111 n1 n0 | rs1 | 100 | rd
			//   MOP.RR.n  1 n2 00 n1 n0 1 | rs2 | rs1 | 100 | rd
			if (i.Itype.funct3 == 4) {
				const uint32_t hi = i.whole >> 25;
				if ((hi & 0b1011000) != 0b1000000)
					return bad(b, n, i);
				if (hi & 1) { // MOP.RR.n: n2 at bit 30, n1 at 27, n0 at 26
					const uint32_t nr = (((i.whole >> 30) & 1) << 2)
						| (((i.whole >> 27) & 1) << 1)
						| ((i.whole >> 26) & 1);
					return snprintf(b, n, "mop.rr.%u	%s,%s,%s", nr,
						RVPRINT::reg(i.Rtype.rd), RVPRINT::reg(i.Rtype.rs1),
						RVPRINT::reg(i.Rtype.rs2));
				}
				if ((i.whole & (0b111u << 22)) != (0b111u << 22))
					return bad(b, n, i);
				// n4 at bit 30, n3 at 27, n2 at 26, n1:n0 at 21:20
				const uint32_t nr = (((i.whole >> 30) & 1) << 4)
					| (((i.whole >> 27) & 1) << 3)
					| (((i.whole >> 26) & 1) << 2)
					| ((i.whole >> 20) & 0x3);
				return snprintf(b, n, "mop.r.%u	%s,%s", nr,
					RVPRINT::reg(i.Itype.rd), RVPRINT::reg(i.Itype.rs1));
			}
			const char* m;
			bool immediate_form;
			switch (i.Itype.funct3) {
			case 1: m = "csrrw";  immediate_form = false; break;
			case 2: m = "csrrs";  immediate_form = false; break;
			case 3: m = "csrrc";  immediate_form = false; break;
			case 5: m = "csrrwi"; immediate_form = true;  break;
			case 6: m = "csrrsi"; immediate_form = true;  break;
			case 7: m = "csrrci"; immediate_form = true;  break;
			default: return bad(b, n, i);
			}
			// Named CSRs print by name, everything else in hex.
			char hex[16];
			const char* csr = RVPRINT::csrname(i.Itype.imm);
			if (csr == nullptr) {
				snprintf(hex, sizeof(hex), "0x%x", i.Itype.imm);
				csr = hex;
			}
			if (immediate_form)
				return snprintf(b, n, "%s\t%s,%s,%u", m,
					RVPRINT::reg(i.Itype.rd), csr, i.Itype.rs1);
			return snprintf(b, n, "%s\t%s,%s,%s", m,
				RVPRINT::reg(i.Itype.rd), csr, RVPRINT::reg(i.Itype.rs1));
		}

		/// MISC-MEM (0x0f): FENCE and its named variants, plus the Zicbo hints.
		static int op_misc_mem(char* b, size_t n, instr_t i) noexcept
		{
			switch (i.Itype.funct3) {
			case 0x0:
				if (i.Itype.rd != 0 || i.Itype.rs1 != 0)
					return bad(b, n, i);
				if (i.Itype.imm == 0x833)
					return snprintf(b, n, "fence.tso");
				if ((i.Itype.imm >> 8) != 0)
					return bad(b, n, i);
				return snprintf(b, n, "fence\t%s,%s",
					RVPRINT::fence_bits((i.Itype.imm >> 4) & 0xF),
					RVPRINT::fence_bits(i.Itype.imm & 0xF));
			case 0x1:
				if (i.Itype.rd != 0 || i.Itype.rs1 != 0 || i.Itype.imm != 0)
					return bad(b, n, i);
				return snprintf(b, n, "fence.i");
			case 0x2: {
				if (i.Itype.rd != 0)
					return bad(b, n, i);
				const char* m;
				switch (i.Itype.imm) {
				case 0x0: m = "cbo.inval"; break;
				case 0x1: m = "cbo.clean"; break;
				case 0x2: m = "cbo.flush"; break;
				case 0x4: m = "cbo.zero";  break;
				default: return bad(b, n, i);
				}
				return snprintf(b, n, "%s\t(%s)", m, RVPRINT::reg(i.Itype.rs1));
			}
			}
			return bad(b, n, i);
		}
	};

	/**
	 * Disassembler for the compressed (16-bit) encodings.
	 */
	struct RVCDISASM
	{
		using instr_t = rv32c_instruction;

		static int bad(char* b, size_t n, instr_t c) noexcept
		{
			return RVPRINT::illegal(b, n, c.whole, 2);
		}

		/// See rv_expect_mnemonic: guards against the decoder mis-routing.
		static int as(char* b, size_t n, const char* expected, int written) noexcept
		{
			return rv_expect_mnemonic(b, n, expected, written);
		}

		/// `c.lw\ts0,8(s0)`
		static int mem(char* b, size_t n, const char* m, const char* r,
			uint32_t off, const char* base) noexcept
		{
			return snprintf(b, n, "%s\t%s,%u(%s)", m, r, off, base);
		}

		/// Disassemble any 16-bit encoding. `pc` resolves the branch targets.
		static int any(char* b, size_t n, instr_t c, bool is64, uint64_t pc) noexcept
		{
			if (c.whole == 0x0000)
				return snprintf(b, n, "c.unimp");

			const uint32_t quadrant = c.CI.opcode;
			const uint32_t funct3   = c.CI.funct3;

			switch (quadrant) {
			// ---- quadrant 0 ------------------------------------------------
			case 0b00:
				switch (funct3) {
				case 0:
					// nzuimm == 0 is the reserved encoding.
					if (c.CIW.offset() == 0)
						return bad(b, n, c);
					return snprintf(b, n, "c.addi4spn\t%s,sp,%u",
						RVPRINT::creg(c.CIW.srd), c.CIW.offset());
				case 1:
					return mem(b, n, "c.fld", RVPRINT::cfreg(c.CL.srd),
						c.CSD.offset8(), RVPRINT::creg(c.CL.srs1));
				case 2:
					return mem(b, n, "c.lw", RVPRINT::creg(c.CL.srd),
						c.CL.offset(), RVPRINT::creg(c.CL.srs1));
				case 3:
					if (is64)
						return mem(b, n, "c.ld", RVPRINT::creg(c.CL.srd),
							c.CSD.offset8(), RVPRINT::creg(c.CL.srs1));
					return mem(b, n, "c.flw", RVPRINT::cfreg(c.CL.srd),
						c.CL.offset(), RVPRINT::creg(c.CL.srs1));
				case 4:
					// Zcb: five instructions in what the base compressed
					// extension leaves reserved.
					switch (c.CZB.subf3) {
					case 0b000:
						return mem(b, n, "c.lbu", RVPRINT::creg(c.CZB.srd),
							c.CZB.byte_offset(), RVPRINT::creg(c.CZB.srs1));
					case 0b001:
						return mem(b, n, c.CZB.half_signed() ? "c.lh" : "c.lhu",
							RVPRINT::creg(c.CZB.srd), c.CZB.half_offset(),
							RVPRINT::creg(c.CZB.srs1));
					case 0b010:
						return mem(b, n, "c.sb", RVPRINT::creg(c.CZB.srd),
							c.CZB.byte_offset(), RVPRINT::creg(c.CZB.srs1));
					case 0b011:
						// c.sh has no signed counterpart, so the bit that
						// would select one is reserved.
						if (c.CZB.half_signed())
							return bad(b, n, c);
						return mem(b, n, "c.sh", RVPRINT::creg(c.CZB.srd),
							c.CZB.half_offset(), RVPRINT::creg(c.CZB.srs1));
					}
					return bad(b, n, c);
				case 5:
					return mem(b, n, "c.fsd", RVPRINT::cfreg(c.CSD.srs2),
						c.CSD.offset8(), RVPRINT::creg(c.CSD.srs1));
				case 6:
					return mem(b, n, "c.sw", RVPRINT::creg(c.CS.srs2),
						c.CS.offset4(), RVPRINT::creg(c.CS.srs1));
				case 7:
					if (is64)
						return mem(b, n, "c.sd", RVPRINT::creg(c.CSD.srs2),
							c.CSD.offset8(), RVPRINT::creg(c.CSD.srs1));
					return mem(b, n, "c.fsw", RVPRINT::cfreg(c.CS.srs2),
						c.CS.offset4(), RVPRINT::creg(c.CS.srs1));
				}
				return bad(b, n, c);

			// ---- quadrant 1 ------------------------------------------------
			case 0b01:
				switch (funct3) {
				case 0:
					return snprintf(b, n, "c.addi\t%s,%d",
						RVPRINT::reg(c.CI.rd), c.CI.signed_imm());
				case 1:
					if (!is64)
						return snprintf(b, n, "c.jal\t0x%" PRIx64,
							RVDISASM::pcrel(pc, c.CJ.signed_imm(), is64));
					// C.ADDIW is reserved when rd is x0.
					if (c.CI.rd == 0)
						return bad(b, n, c);
					return snprintf(b, n, "c.addiw\t%s,%d",
						RVPRINT::reg(c.CI.rd), c.CI.signed_imm());
				case 2:
					return snprintf(b, n, "c.li\t%s,%d",
						RVPRINT::reg(c.CI.rd), c.CI.signed_imm());
				case 3:
					if (c.CI.rd == 2) {
						if (c.CI16.signed_imm() == 0)
							return bad(b, n, c);
						return snprintf(b, n, "c.addi16sp\tsp,%d", c.CI16.signed_imm());
					}
					if (c.CI.upper_imm() == 0) {
						// Zcmop takes the reserved imm = 0 points that have
						// an odd rd below 16; the field spells n as 0nnn1.
						if ((c.CI.rd & 1) != 0 && c.CI.rd < 16)
							return snprintf(b, n, "c.mop.%u", c.CI.rd);
						return bad(b, n, c);
					}
					// objdump prints the LUI-style 20-bit field, in hex.
					return snprintf(b, n, "c.lui\t%s,0x%x", RVPRINT::reg(c.CI.rd),
						(uint32_t(c.CI.upper_imm()) >> 12) & 0xFFFFF);
				case 4: {
					const uint32_t shamt = is64 ? c.CAB.shift64_imm() : c.CAB.shift_imm();
					switch (c.CAB.funct2) {
					case 0:
						if (shamt == 0)
							return snprintf(b, n, "c.srli64\t%s", RVPRINT::creg(c.CAB.srd));
						return snprintf(b, n, "c.srli\t%s,0x%x",
							RVPRINT::creg(c.CAB.srd), shamt);
					case 1:
						if (shamt == 0)
							return snprintf(b, n, "c.srai64\t%s", RVPRINT::creg(c.CAB.srd));
						return snprintf(b, n, "c.srai\t%s,0x%x",
							RVPRINT::creg(c.CAB.srd), shamt);
					case 2:
						return snprintf(b, n, "c.andi\t%s,%d",
							RVPRINT::creg(c.CAB.srd), c.CAB.signed_imm());
					default: {
						// Bit 12 selects the word-width forms. Zcb adds c.mul
						// alongside them, plus a row of unary operations whose
						// source field is an opcode extension, not a register.
						static const char* const word[4] =
							{ "c.subw", "c.addw", "c.mul", nullptr };
						static const char* const full[4] =
							{ "c.sub", "c.xor", "c.or", "c.and" };
						const bool is_word = (c.whole & (1 << 12)) != 0;
						if (is_word && c.CA.funct2 == 3) {
							static const char* const unary[8] = {
								"c.zext.b", "c.sext.b", "c.zext.h", "c.sext.h",
								"c.zext.w", "c.not",    nullptr,    nullptr,
							};
							const char* m = unary[c.CA.srs2];
							// c.zext.w is the only one needing a wide register.
							if (m == nullptr || (c.CA.srs2 == 4 && !is64))
								return bad(b, n, c);
							return snprintf(b, n, "%s\t%s", m, RVPRINT::creg(c.CA.srd));
						}
						const char* m = is_word ? word[c.CA.funct2] : full[c.CA.funct2];
						// Only the word-width arithmetic is RV64-only; c.mul
						// exists on RV32 as well.
						if (m == nullptr || (is_word && !is64 && c.CA.funct2 < 2))
							return bad(b, n, c);
						return snprintf(b, n, "%s\t%s,%s", m,
							RVPRINT::creg(c.CA.srd), RVPRINT::creg(c.CA.srs2));
					}
					}
				}
				case 5:
					return snprintf(b, n, "c.j\t0x%" PRIx64,
						RVDISASM::pcrel(pc, c.CJ.signed_imm(), is64));
				case 6:
					return snprintf(b, n, "c.beqz\t%s,0x%" PRIx64,
						RVPRINT::creg(c.CB.srs1),
						RVDISASM::pcrel(pc, c.CB.signed_imm(), is64));
				case 7:
					return snprintf(b, n, "c.bnez\t%s,0x%" PRIx64,
						RVPRINT::creg(c.CB.srs1),
						RVDISASM::pcrel(pc, c.CB.signed_imm(), is64));
				}
				return bad(b, n, c);

			// ---- quadrant 2 ------------------------------------------------
			case 0b10:
				switch (funct3) {
				case 0: {
					const uint32_t shamt = is64 ? c.CI.shift64_imm() : c.CI.shift_imm();
					if (shamt == 0)
						return snprintf(b, n, "c.slli64\t%s", RVPRINT::reg(c.CI.rd));
					return snprintf(b, n, "c.slli\t%s,0x%x", RVPRINT::reg(c.CI.rd), shamt);
				}
				case 1:
					return mem(b, n, "c.fldsp", RVPRINT::freg(c.CIFLD.rd),
						c.CIFLD.offset(), "sp");
				case 2:
					if (c.CI2.rd == 0)
						return bad(b, n, c);
					return mem(b, n, "c.lwsp", RVPRINT::reg(c.CI2.rd),
						c.CI2.offset(), "sp");
				case 3:
					if (is64) {
						if (c.CIFLD.rd == 0)
							return bad(b, n, c);
						return mem(b, n, "c.ldsp", RVPRINT::reg(c.CIFLD.rd),
							c.CIFLD.offset(), "sp");
					}
					return mem(b, n, "c.flwsp", RVPRINT::freg(c.CI2.rd),
						c.CI2.offset(), "sp");
				case 4: {
					const bool link = (c.whole & (1 << 12)) != 0;
					if (c.CR.rs2 == 0) {
						if (c.CR.rd == 0)
							return link ? snprintf(b, n, "c.ebreak") : bad(b, n, c);
						return snprintf(b, n, "%s\t%s", link ? "c.jalr" : "c.jr",
							RVPRINT::reg(c.CR.rd));
					}
					return snprintf(b, n, "%s\t%s,%s", link ? "c.add" : "c.mv",
						RVPRINT::reg(c.CR.rd), RVPRINT::reg(c.CR.rs2));
				}
				case 5:
					return mem(b, n, "c.fsdsp", RVPRINT::freg(c.CSFSD.rs2),
						c.CSFSD.offset(), "sp");
				case 6:
					return mem(b, n, "c.swsp", RVPRINT::reg(c.CSS.rs2),
						c.CSS.offset(4), "sp");
				case 7:
					if (is64)
						return mem(b, n, "c.sdsp", RVPRINT::reg(c.CSFSD.rs2),
							c.CSFSD.offset(), "sp");
					return mem(b, n, "c.fswsp", RVPRINT::freg(c.CSS.rs2),
						c.CSS.offset(4), "sp");
				}
				return bad(b, n, c);
			}
			return bad(b, n, c);
		}
	};

	/**
	 * Disassembler for the floating-point opcodes: RV32F/D, plus the Zfa
	 * additions that libriscv implements.
	 *
	 *   - the rounding-mode operand is dropped entirely when it is `dyn`, and
	 *     also whenever the conversion is exact and so cannot round at all
	 *     (fcvt.d.s, fcvt.d.w, fcvt.d.wu). In those cases rm is part of the
	 *     opcode and only the zero encoding is accepted.
	 *   - Zfa's FLI does not print an immediate but the name of the constant it
	 *     loads, in C99 hex-float spelling.
	 */
	struct RVFDISASM
	{
		using instr_t = rv32i_instruction;

		static int bad(char* b, size_t n, instr_t i) noexcept
		{
			return RVPRINT::illegal(b, n, i.whole, 4);
		}

		/// Format field: 0 = single, 1 = double, 2 = half, 3 = quad.
		static const char* fmtname(uint32_t fmt) noexcept
		{
			static const char* const names[4] = { "s", "d", "h", "q" };
			return names[fmt & 3];
		}
		/// Mantissa ordering of the formats, for deciding if a conversion rounds.
		static int fmtrank(uint32_t fmt) noexcept
		{
			static const int ranks[4] = { 1, 2, 0, 3 }; // s, d, h, q
			return ranks[fmt & 3];
		}
		/// libriscv implements single and double arithmetic. Half appears
		/// only in Zfhmin's loads, stores, moves and conversions.
		static bool fmt_supported(uint32_t fmt) noexcept
		{
			return (fmt & 3) <= 1;
		}
		/// Formats a Zfhmin instruction may name, which adds half.
		static bool fmt_or_half(uint32_t fmt) noexcept
		{
			return (fmt & 3) <= 2;
		}
		/// The integer width a conversion names: w, wu, l, lu.
		static const char* intname(uint32_t rs2) noexcept
		{
			static const char* const names[4] = { "w", "wu", "l", "lu" };
			return names[rs2 & 3];
		}

		/// Append the rounding mode, unless it is `dyn` -- objdump omits that.
		static int with_rm(char* b, size_t n, int written, uint32_t rm) noexcept
		{
			const char* mode = RVPRINT::roundmode(rm);
			if (mode == nullptr || written < 0)
				return written;
			return written + snprintf(b + written, n - written, ",%s", mode);
		}

		/// The 32 constants Zfa's FLI can materialise, spelled as objdump does.
		static const char* fli_constant(uint32_t index) noexcept
		{
			static const char* const names[32] = {
				"-0x1p+0", "min",       "0x1p-16",  "0x1p-15",
				"0x1p-8",  "0x1p-7",    "0x1p-4",   "0x1p-3",
				"0x1p-2",  "0x1.4p-2",  "0x1.8p-2", "0x1.cp-2",
				"0x1p-1",  "0x1.4p-1",  "0x1.8p-1", "0x1.cp-1",
				"0x1p+0",  "0x1.4p+0",  "0x1.8p+0", "0x1.cp+0",
				"0x1p+1",  "0x1.4p+1",  "0x1.8p+1", "0x1p+2",
				"0x1p+3",  "0x1p+4",    "0x1p+7",   "0x1p+8",
				"0x1p+15", "0x1p+16",   "inf",      "nan",
			};
			return names[index & 31];
		}

		/// FP LOAD (0x07) and STORE (0x27): funct3 picks the width.
		static int op_load(char* b, size_t n, instr_t i) noexcept
		{
			const char* m = i.Itype.funct3 == 0x1 ? "flh" // Zfhmin
						  : i.Itype.funct3 == 0x2 ? "flw"
						  : i.Itype.funct3 == 0x3 ? "fld" : nullptr;
			if (m == nullptr)
				return bad(b, n, i);
			return snprintf(b, n, "%s\t%s,%d(%s)", m, RVPRINT::freg(i.Itype.rd),
				i.Itype.signed_imm(), RVPRINT::reg(i.Itype.rs1));
		}
		static int op_store(char* b, size_t n, instr_t i) noexcept
		{
			const char* m = i.Stype.funct3 == 0x1 ? "fsh" // Zfhmin
						  : i.Stype.funct3 == 0x2 ? "fsw"
						  : i.Stype.funct3 == 0x3 ? "fsd" : nullptr;
			if (m == nullptr)
				return bad(b, n, i);
			return snprintf(b, n, "%s\t%s,%d(%s)", m, RVPRINT::freg(i.Stype.rs2),
				i.Stype.signed_imm(), RVPRINT::reg(i.Stype.rs1));
		}

		/// FMADD / FMSUB / FNMSUB / FNMADD (0x43, 0x47, 0x4b, 0x4f).
		static int op_fused(char* b, size_t n, instr_t i) noexcept
		{
			static const char* const names[4] =
				{ "fmadd", "fmsub", "fnmsub", "fnmadd" };
			const uint32_t fmt = (i.whole >> 25) & 3;
			if (!fmt_supported(fmt))
				return bad(b, n, i);
			const int written = snprintf(b, n, "%s.%s\t%s,%s,%s,%s",
				names[(i.Rtype.opcode >> 2) & 3], fmtname(fmt),
				RVPRINT::freg(i.Rtype.rd), RVPRINT::freg(i.Rtype.rs1),
				RVPRINT::freg(i.Rtype.rs2), RVPRINT::freg(i.whole >> 27));
			return with_rm(b, n, written, i.Rtype.funct3);
		}

		/// OP-FP (0x53).
		static int op_fp(char* b, size_t n, instr_t i, bool is64) noexcept
		{
			const uint32_t funct7 = i.Rtype.funct7;
			const uint32_t fmt    = funct7 & 3;
			const uint32_t op     = funct7 >> 2;
			const uint32_t rm     = i.Rtype.funct3;
			const uint32_t rs2    = i.Rtype.rs2;
			const char* const fs  = fmtname(fmt);

			// Half passes the gate here and is turned away inside the
			// groups that have no half form; the three Zfhmin ones let it
			// through.
			if (!fmt_or_half(fmt))
				return bad(b, n, i);

			const char* const frd  = RVPRINT::freg(i.Rtype.rd);
			const char* const frs1 = RVPRINT::freg(i.Rtype.rs1);
			const char* const frs2 = RVPRINT::freg(rs2);
			const char* const xrd  = RVPRINT::reg(i.Rtype.rd);
			const char* const xrs1 = RVPRINT::reg(i.Rtype.rs1);

			// Everything but the conversions and the two register moves is
			// Zfh, not Zfhmin.
			const bool half = (fmt == 2);
			if (half && op != 0x08 && op != 0x1C && op != 0x1E)
				return bad(b, n, i);

			switch (op) {
			case 0x00: case 0x01: case 0x02: case 0x03: {
				static const char* const names[4] = { "fadd", "fsub", "fmul", "fdiv" };
				const int w = snprintf(b, n, "%s.%s\t%s,%s,%s",
					names[op], fs, frd, frs1, frs2);
				return with_rm(b, n, w, rm);
			}
			case 0x04: {
				static const char* const names[8] =
					{ "fsgnj", "fsgnjn", "fsgnjx", nullptr, nullptr, nullptr, nullptr, nullptr };
				if (names[rm] == nullptr)
					return bad(b, n, i);
				return snprintf(b, n, "%s.%s\t%s,%s,%s", names[rm], fs, frd, frs1, frs2);
			}
			case 0x05: {
				// rm 2 and 3 are Zfa's IEEE-754-2019 minimumNumber/maximumNumber.
				static const char* const names[8] =
					{ "fmin", "fmax", "fminm", "fmaxm", nullptr, nullptr, nullptr, nullptr };
				if (names[rm] == nullptr)
					return bad(b, n, i);
				return snprintf(b, n, "%s.%s\t%s,%s,%s", names[rm], fs, frd, frs1, frs2);
			}
			case 0x08: {
				// Zfa's round-to-integer shares the format-conversion opcode.
				if (rs2 == 4 || rs2 == 5) {
					if (half)
						return bad(b, n, i);
					const int w = snprintf(b, n, "%s.%s\t%s,%s",
						rs2 == 4 ? "fround" : "froundnx", fs, frd, frs1);
					return with_rm(b, n, w, rm);
				}
				// Zfhmin converts between half and each of the other two, so
				// half is allowed on exactly one side of the pair.
				if (rs2 > 3 || rs2 == fmt || !fmt_or_half(rs2))
					return bad(b, n, i);
				const int w = snprintf(b, n, "fcvt.%s.%s\t%s,%s",
					fs, fmtname(rs2), frd, frs1);
				// Widening cannot round, so rm is part of the opcode instead.
				if (fmtrank(fmt) > fmtrank(rs2))
					return rm == 0 ? w : bad(b, n, i);
				return with_rm(b, n, w, rm);
			}
			case 0x0B: {
				if (rs2 != 0)
					return bad(b, n, i);
				const int w = snprintf(b, n, "fsqrt.%s\t%s,%s", fs, frd, frs1);
				return with_rm(b, n, w, rm);
			}
			case 0x14: {
				// rm 4 and 5 are Zfa's quiet comparisons.
				static const char* const names[8] =
					{ "fle", "flt", "feq", nullptr, "fleq", "fltq", nullptr, nullptr };
				if (names[rm] == nullptr)
					return bad(b, n, i);
				return snprintf(b, n, "%s.%s\t%s,%s,%s", names[rm], fs, xrd, frs1, frs2);
			}
			case 0x18: {
				// Zfa's fcvtmod.w.d: modular conversion, always truncating.
				if (rs2 == 8) {
					if (fmt != 1 || rm != 1)
						return bad(b, n, i);
					return snprintf(b, n, "fcvtmod.w.d\t%s,%s,rtz", xrd, frs1);
				}
				if (rs2 > 3 || (rs2 >= 2 && !is64))
					return bad(b, n, i);
				const int w = snprintf(b, n, "fcvt.%s.%s\t%s,%s",
					intname(rs2), fs, xrd, frs1);
				return with_rm(b, n, w, rm);
			}
			case 0x1A: {
				if (rs2 > 3 || (rs2 >= 2 && !is64))
					return bad(b, n, i);
				const int w = snprintf(b, n, "fcvt.%s.%s\t%s,%s",
					fs, intname(rs2), frd, xrs1);
				// A double represents every 32-bit integer exactly, so those two
				// conversions cannot round and take no rounding mode.
				if (fmt == 1 && rs2 <= 1)
					return rm == 0 ? w : bad(b, n, i);
				return with_rm(b, n, w, rm);
			}
			case 0x1C: {
				// FMVH.X.D reads the upper half of a double; RV32 only, since on
				// RV64 a single FMV.X.D moves the whole value.
				if (!is64 && fmt == 1 && rs2 == 1 && rm == 0)
					return snprintf(b, n, "fmvh.x.d\t%s,%s", xrd, frs1);
				if (rs2 != 0)
					return bad(b, n, i);
				if (rm == 1) {
					if (half) // FCLASS.H needs Zfh
						return bad(b, n, i);
					return snprintf(b, n, "fclass.%s\t%s,%s", fs, xrd, frs1);
				}
				if (rm != 0)
					return bad(b, n, i);
				// FMV.X.W moves a single, FMV.X.D a double: RV64 only.
				// FMV.X.H is Zfhmin's, and sign-extends its sixteen bits.
				if (fmt == 1 && !is64)
					return bad(b, n, i);
				return snprintf(b, n, "fmv.x.%s\t%s,%s",
					fmt == 0 ? "w" : fmt == 1 ? "d" : "h", xrd, frs1);
			}
			case 0x16: {
				// FMVP.D.X builds a double out of two 32-bit registers: RV32 only.
				if (is64 || fmt != 1 || rm != 0)
					return bad(b, n, i);
				return snprintf(b, n, "fmvp.d.x\t%s,%s,%s", frd, xrs1, RVPRINT::reg(rs2));
			}
			case 0x1E: {
				if (rm != 0)
					return bad(b, n, i);
				if (rs2 == 1) {
					if (half) // FLI.H needs Zfh
						return bad(b, n, i);
					return snprintf(b, n, "fli.%s\t%s,%s", fs, frd,
						fli_constant(i.Rtype.rs1));
				}
				if (rs2 != 0)
					return bad(b, n, i);
				if (fmt == 1 && !is64)
					return bad(b, n, i);
				return snprintf(b, n, "fmv.%s.x\t%s,%s",
					fmt == 0 ? "w" : fmt == 1 ? "d" : "h", frd, xrs1);
			}
			}
			return bad(b, n, i);
		}
	};

	inline int rv_print_hint(char* b, size_t n, rv32i_instruction i,
		bool is64, bool is128, uint64_t pc) noexcept
	{
		if (i.length() == 2)
			return RVCDISASM::any(b, n, rv32c_instruction { i }, is64, pc);

		switch (i.opcode()) {
		case 0b0010011: return RVDISASM::op_imm(b, n, i, is64);
		case 0b0011011: return RVDISASM::op_imm32(b, n, i);
		case 0b0110011: return RVDISASM::op_reg(b, n, i, is64);
		case 0b0111011: return RVDISASM::op_reg32(b, n, i);
		case 0b0110111: return RVDISASM::op_lui(b, n, i);
		case 0b0010111: return RVDISASM::op_auipc(b, n, i);
		case 0b0000011: return RVDISASM::op_load(b, n, i, is128);
		case 0b1110011: return RVDISASM::op_system(b, n, i);
		case 0b1010011: return RVFDISASM::op_fp(b, n, i, is64);
		}
		return RVDISASM::bad(b, n, i);
	}
}
