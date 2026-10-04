// SPDX-License-Identifier: GPL-3.0-or-later
//
// A tiny sampling profiler for the emulator. Every ~2 ms it notes where the
// emulation thread and the software renderer's worker threads are executing
// and, every 10 s, writes the hottest spots to onyx.log as "ONYX3DS.exe+0x...."
// offsets. Decode them with the map file CI publishes for the build whose
// "Build stamp" appears in the log. That turns "it's slow" into "this function
// is the cost".
#pragma once

namespace onyx::app {

// Starts sampling the calling thread (the emulation thread) and any worker
// threads the core names "SwRenderer workers". One call per emulation session.
void ProfilerStart();
// Stops the sampler and logs what is left.
void ProfilerStop();

} // namespace onyx::app
