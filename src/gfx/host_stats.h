// Host diagnostics for the native renderer's periodic stats (issue #12: frame-time dips seen on one machine and not
// others). Cheap enough to sample every 600 frames.
#pragma once

#include <cstdint>
#include <string>

namespace bots::gfx::host {

// Process CPU time so far, user and kernel (system) mode. On Windows the SDK's write watches fault into the kernel
// (exception dispatch + VirtualProtect), so a high kernel share points there.
struct CpuTimes {
  uint64_t user_us = 0, kernel_us = 0;
};
CpuTimes ProcessCpuTimes();

// Average clock over the logical CPUs and their maximum, MHz, and the OS's current limit (Windows: MhzLimit, below
// the maximum while the CPU is power or thermal limited). 0 where unknown.
struct CpuClock {
  uint32_t current_mhz = 0, max_mhz = 0, limit_mhz = 0;
};
CpuClock CpuClockMhz();

// Microseconds for a fixed integer workload on the calling thread: the same build compares machines, and moments
// on one machine (a throttled CPU takes longer).
double CpuProbeMicros();

// The process's memory in MB as the OS counts it against the app: the physical footprint on Apple platforms (what iOS
// and tvOS terminate an app for), resident memory (VmRSS) on Linux and Android, the working set on Windows. 0 if unknown.
double ProcessMemoryMB();

// "AC", "battery 54%", plus battery saver, the power plan and (Windows) the power mode where the OS reports them.
// Empty if unknown.
std::string PowerState();

// Overlay, capture and injection modules loaded into the process (RivaTuner, OBS, Discord, Steam, ReShade, ...).
std::string OverlayModules();

}  // namespace bots::gfx::host
