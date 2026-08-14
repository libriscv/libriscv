#include <sys/select.h>

template <int W>
static void syscall_pselect(Machine<W>& machine)
{
    // Without file descriptors nothing can ever become ready.
    // Match ppoll() and report a timeout instead of throwing.
    if (!machine.has_file_descriptors()) {
        machine.set_result(0);
        return;
    }
    throw MachineException(SYSTEM_CALL_FAILED, "pselect() not implemented");
}
