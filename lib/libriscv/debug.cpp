#include "cpu.hpp"
#include "machine.hpp"
#include <cstdio>
#include <cstdlib>

namespace riscv
{
static inline std::vector<std::string> split(const std::string& txt, char ch)
{
    size_t pos = txt.find(ch);
    size_t initialPos = 0;
    std::vector<std::string> strs;

    while (pos != std::string::npos)
    {
        strs.push_back(txt.substr(initialPos, pos - initialPos));
        initialPos = pos + 1;

        pos = txt.find(ch, initialPos);
    }

    // Add the last one
    strs.push_back(txt.substr(initialPos, std::min(pos, txt.size()) - initialPos + 1));
    return strs;
}

static void print_help()
{
    const char* help_text = R"V0G0N(
  usage: command [options]
    commands:
      ?, help               Show this informational text
      c, continue           Continue execution, disable stepping
      s, step [steps=1]     Run [steps] instructions, then break
      v, verbose            Toggle verbose instruction execution
      b, break [addr]       Breakpoint on executing [addr]
      rb [addr]             Breakpoint on reading from [addr]
      wb [addr]             Breakpoint on writing to [addr]
      clear                 Clear all breakpoints
      reset                 Reset the machine
      read [addr] (len=1)   Read from [addr] (len) bytes and print
      write [addr] [value]  Write [value] to memory location [addr]
      print [addr] [length] Print [addr] as a string of [length] bytes
      debug                 Trigger the debug interrupt handler
      frame                 Show frame number and extra frame info
)V0G0N";
    printf("%s\n", help_text);
}

template <int W>
static bool execute_commands(CPU<W>& cpu)
{
    printf("Enter = cont, help, quit: ");
    std::string text;
    while (true)
    {
        const int c = getchar(); // press any key
        if (c == '\n' || c < 0)
            break;
        else
            text.append(1, (char) c);
    }
    if (text.empty()) return false;
    std::vector<std::string> params = split(text, ' ');
    const auto& cmd = params[0];

    // continue
    if (cmd == "c" || cmd == "continue")
    {
        cpu.break_on_steps(0);
        return false;
    }
    // stepping
    if (cmd == "")
	{
		return false;
	}
    else if (cmd == "s" || cmd == "step")
    {
        cpu.machine().verbose_instructions = true; // ???
        int steps = 1;
        if (params.size() > 1) steps = std::stoi(params[1]);
        printf("Pressing Enter will now execute %d steps\n", steps);
        cpu.break_on_steps(steps);
        return false;
    }
    // breaking
    else if (cmd == "b" || cmd == "break")
    {
        if (params.size() < 2)
        {
            printf(">>> Not enough parameters: break [addr]\n");
            return true;
        }
        unsigned long hex = std::strtoul(params[1].c_str(), 0, 16);
        cpu.breakpoint(hex);
        return true;
    }
    else if (cmd == "clear")
    {
        cpu.breakpoints().clear();
        return true;
    }
    // verbose instructions
    else if (cmd == "v" || cmd == "verbose")
    {
        bool& v = cpu.machine().verbose_instructions;
        v = !v;
        printf("Verbose instructions are now %s\n", v ? "ON" : "OFF");
        return true;
    }
    else if (cmd == "r" || cmd == "run")
    {
        cpu.machine().verbose_instructions = false;
        cpu.break_on_steps(0);
        return false;
    }
    else if (cmd == "q" || cmd == "quit" || cmd == "exit")
    {
        cpu.machine().stop();
        return false;
    }
    else if (cmd == "reset")
    {
        cpu.machine().reset();
        return true;
    }
    // read 0xAddr size
    else if (cmd == "lw" || cmd == "read")
    {
        if (params.size() < 2)
        {
            printf(">>> Not enough parameters: read [addr]\n");
            return true;
        }
        unsigned long addr = std::strtoul(params[1].c_str(), 0, 16);
		auto value = cpu.machine().memory.template read<uint32_t>(addr);
        printf("0x%lX: 0x%X\n", addr, value);
        return true;
    }
    // write 0xAddr value
    else if (cmd == "sw" || cmd == "write")
    {
        if (params.size() < 3)
        {
            printf(">>> Not enough parameters: write [addr] [value]\n");
            return true;
        }
        unsigned long hex = std::strtoul(params[1].c_str(), 0, 16);
        int value = std::stoi(params[2]) & 0xff;
        printf("0x%04lx -> 0x%02x\n", hex, value);
        cpu.machine().memory.template write<uint32_t>(hex, value);
        return true;
    }
    // print 0xAddr size
    else if (cmd == "print")
    {
        if (params.size() < 3)
        {
            printf(">>> Not enough parameters: print addr length\n");
            return true;
        }
        uint32_t src = std::strtoul(params[1].c_str(), 0, 16);
        int bytes = std::stoi(params[2]);
        char* buffer = new char[bytes];
        cpu.machine().memory.memcpy_out(buffer, src, bytes);
        printf("0x%X: %.*s\n", src, bytes, buffer);
        delete[] buffer;
        return true;
    }
    else if (cmd == "debug")
    {
        cpu.machine().system_call(EBREAK_SYSCALL);
        return true;
    }
    else if (cmd == "help" || cmd == "?")
    {
        print_help();
        return true;
    }
    else
    {
        printf(">>> Unknown command: '%s'\n", cmd.c_str());
        print_help();
        return true;
    }
    return false;
}

template<int W>
void Machine<W>::print_and_pause()
{
	try {
		const auto instruction = cpu.read_instruction(cpu.pc());
		const auto& handler = cpu.decode(instruction);
		const auto string = CPU<W>::isa_t::to_string(cpu, instruction, handler);
		printf("\n>>> Breakpoint \t%s\n\n", string.c_str());
	} catch (const std::exception& e) {
		printf("\n>>> Breakpoint \tError reading instruction: %s\n\n", e.what());
	}
    // CPU registers
    printf("%s", cpu.registers().to_string().c_str());
    // Memory subsystem
    printf("[MEM PAGES %8zu / %8zu]\n", memory.pages_active(), memory.pages_total());

    while (execute_commands(cpu))
        ;
} // print_and_pause(...)

template<int W>
bool CPU<W>::break_time() const
{
    if (UNLIKELY(m_break_steps_cnt != 0))
    {
        m_break_steps--;
        if (m_break_steps <= 0)
        {
            m_break_steps = m_break_steps_cnt;
            return true;
        }
    }
    return false;
}

template<int W>
void CPU<W>::break_on_steps(int steps)
{
    assert(steps >= 0);
    this->m_break_steps_cnt = steps;
    this->m_break_steps = steps;
}

template<int W>
void CPU<W>::break_checks()
{
    if (UNLIKELY(this->break_time()))
    {
        // pause for each instruction
        machine().print_and_pause();
    }
    if (UNLIKELY(!m_breakpoints.empty()))
    {
        // look for breakpoints
        auto it = m_breakpoints.find(registers().pc);
        if (it != m_breakpoints.end())
        {
            auto& callback = it->second;
            callback(*this);
        }
    }
}

	template class CPU<4>;
} // namespace riscv
