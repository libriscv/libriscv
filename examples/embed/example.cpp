#include <libriscv/machine.hpp>
using namespace riscv;

int main()
{
    std::vector<uint8_t> binary;

    Machine<RISCV64> machine{binary};
    machine.setup_linux(
        {"example"},
        {"LC_TYPE=C", "LC_ALL=C", "USER=root"});
    machine.setup_linux_syscalls();

    machine.simulate(32'000'000ULL);
}
