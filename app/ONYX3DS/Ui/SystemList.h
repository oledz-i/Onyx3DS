// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game systems ONYX can show (3DS always; NES and GBA when their core DLL is in the
// package), their short names, and stepping from one to the next.
#pragma once

#include <vector>

#include "Emu/EmulatorSession.h"
#include "onyx/library.h"

namespace onyx::app {

inline const char* SystemLabel(GameSystem system) {
    switch (system) {
    case GameSystem::Nes: return "NES";
    case GameSystem::Gba: return "GBA";
    default: return "3DS";
    }
}

inline std::vector<GameSystem> AvailableSystems() {
    std::vector<GameSystem> out = {GameSystem::N3DS};
    if (EmulatorSession::NesCoreAvailable()) out.push_back(GameSystem::Nes);
    if (EmulatorSession::GbaCoreAvailable()) out.push_back(GameSystem::Gba);
    return out;
}

// The system `step` places after `current` in the tab order (stops at both ends).
inline GameSystem NeighbourSystem(GameSystem current, int step) {
    const auto list = AvailableSystems();
    int index = 0;
    for (int i = 0; i < static_cast<int>(list.size()); ++i)
        if (list[i] == current) index = i;
    index = std::clamp(index + step, 0, static_cast<int>(list.size()) - 1);
    return list[index];
}

} // namespace onyx::app
