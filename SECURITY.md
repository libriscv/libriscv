# Security Guarantees

libriscv provides a safe sandbox that guests can not escape from, short of vulnerabilities in custom system calls installed by the host. This includes the virtual machine and the native helper libraries. Do not use binary translation in production at this time. Do not use linux filesystem or socket system calls in production at this time.

libriscv provides termination guarantees and default resource limits - code should not be able to exhaust CPU or RAM resources on the system during initialization or execution. If blocking calls are used during system calls, use socket timeouts or timers + signals to cancel.

libriscv can be loaded with any program from any untrusted source. Any program or combination of instructions that is able to violate the sandbox integrity, including high resource usage, is considered a vulnerabilty.

# Reporting a Vulnerability

You can report security bugs to fwsGonzo directly at fwsgonzo at hotmail dot com.
