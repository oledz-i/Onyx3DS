// SPDX-License-Identifier: GPL-3.0-or-later
//
// A tiny sampling profiler for the emulation thread. Every ~2 ms it notes where
// the thread is executing and, every few seconds, writes the hottest spots to
// onyx.log as "ONYX3DS.exe+0x...." offsets (decode them with the ONYX3DS.map
// published next to each release) plus how much time went to JIT-generated
// guest code. That turns "it's slow" into "this function is the cost".
#pragma once

namespace onyx::app {

// Starts sampling the calling thread. Safe to call once per emulation session.
void ProfilerStart();
// Stops the sampler and logs what is left.
void ProfilerStop();

} // namespace onyx::app
