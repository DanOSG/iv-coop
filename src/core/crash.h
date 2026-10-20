#pragma once

namespace ivc::crash {

// Installs a vectored exception handler that writes fatal-looking exceptions (access
// violation, illegal instruction, stack overflow, ...) to our log: code, address, faulting
// module + offset (static VA for GTAIV.exe), registers and a heuristic stack scan. It
// never handles the exception, so behaviour is unchanged; it only makes crashes that
// leave no Windows report diagnosable.
void install();

} // namespace ivc::crash
