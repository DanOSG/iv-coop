#pragma once

namespace ivc::crash {

// Installs a vectored exception handler that writes fatal-looking exceptions (access
// violation, illegal instruction, stack overflow, ...) to our log: code, address, faulting
// module + offset (static VA for GTAIV.exe), registers and a heuristic stack scan. It
// never handles the exception, so behaviour is unchanged; it only makes crashes that
// leave no Windows report diagnosable.
// Also logs Debug-runtime assertion failures (instead of a hidden message box) and starts
// a hang watchdog that logs the game and render threads' stacks when both stop beating.
void install();
// Heartbeats for the watchdog: once per game tick / once per presented frame.
void beat_tick();
void beat_render();

} // namespace ivc::crash
