#pragma once

namespace riscv
{
    // Bytecodes for threaded simulation
    enum
    {
        RV32I_BC_ADDI = 0,
        RV32I_BC_LI,

        RV32I_BC_SLLI,
        RV32I_BC_SLTI,
        RV32I_BC_SLTIU,
        RV32I_BC_XORI,
        RV32I_BC_SRLI,
        RV32I_BC_SRAI,
        RV32I_BC_ORI,
        RV32I_BC_ANDI,

        RV32I_BC_LUI,
        RV32I_BC_AUIPC,

        RV32I_BC_LDW,
        RV32I_BC_LDD,

        RV32I_BC_LDB,
        RV32I_BC_LDH,
        RV32I_BC_LDBU,
        RV32I_BC_LDHU,
        RV32I_BC_LDWU,

        RV32I_BC_SDB,
        RV32I_BC_SDH,
        RV32I_BC_SDW,
        RV32I_BC_SDD,

        RV32I_BC_BEQ,
        RV32I_BC_BNE,
        RV32I_BC_BLT,
        RV32I_BC_BGE,
        RV32I_BC_BLTU,
        RV32I_BC_BGEU,

        RV32I_BC_JAL,
        RV32I_BC_JALR,

        RV32I_BC_OP_ADD,
        RV32I_BC_OP_SUB,
        RV32I_BC_OP_SLL,
        RV32I_BC_OP_SLT,
        RV32I_BC_OP_SLTU,
        RV32I_BC_OP_XOR,
        RV32I_BC_OP_SRL,
        RV32I_BC_OP_OR,
        RV32I_BC_OP_AND,
        RV32I_BC_OP_MUL,
        RV32I_BC_OP_MULH,
        RV32I_BC_OP_MULHSU,
        RV32I_BC_OP_MULHU,
        RV32I_BC_OP_DIV,
        RV32I_BC_OP_DIVU,
        RV32I_BC_OP_REM,
        RV32I_BC_OP_REMU,
        RV32I_BC_OP_SRA,
        RV32I_BC_OP_SH1ADD,
        RV32I_BC_OP_SH2ADD,
        RV32I_BC_OP_SH3ADD,

        RV32I_BC_SYSCALL,
        RV32I_BC_NOP,

        RV32F_BC_FLW,
        RV32F_BC_FLD,
        RV32F_BC_FSW,
        RV32F_BC_FSD,
        RV32F_BC_FPFUNC,
        RV32F_BC_FMADD,
        RV32F_BC_FMSUB,
        RV32F_BC_FNMADD,
        RV32F_BC_FNMSUB,
        RV32V_BC_VLE32,
        RV32V_BC_VSE32,
        RV32V_BC_OP,

        RV32I_BC_SYSTEM,
        RV32A_BC_ATOMIC,
        RV32I_BC_INVALID,
    };

    union FasterItype
    {
        uint32_t whole;

        struct
        {
            uint8_t rs2;
            uint8_t rs1;
            int16_t imm;
        };

        int32_t signed_imm() const noexcept {
            return imm;
        }
    };

    union FasterOpType
    {
        uint32_t whole;

        struct
        {
            uint8_t rd;
            uint8_t rs1;
            uint8_t rs2;
            int8_t imm;
        };
    };

} // riscv
