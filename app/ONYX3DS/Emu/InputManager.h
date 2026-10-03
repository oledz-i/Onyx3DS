// SPDX-License-Identifier: GPL-3.0-or-later
//
// Xbox controller -> libretro joypad, plus the View-button hotkeys.
//
// Default mapping (positional, like every Nintendo emulator on Xbox):
//   Xbox B / A / Y / X  -> 3DS A / B / X / Y   (same physical spots)
//   LB / RB / LT / RT   -> L / R / ZL / ZR
//   Menu / View         -> Start / Select (View taps; hold View for hotkeys)
//   Left stick          -> Circle Pad,   D-pad -> D-pad
//   Right stick         -> C-Stick / touch-screen pointer, R3 = touch
//   L3                  -> Swap screens / Home (Azahar's libretro binding)
//
// Hotkeys (hold View):
//   + Menu   open the ONYX menu (save/load state, cheats, settings, quit)
//   + RB     fast forward (hold or toggle, per settings)
//   + LB     cycle screen layout
//   + Y      screenshot           + A   show/hide FPS
//   + D-pad Up / Down   save / load state in the current slot
//   + D-pad Left/Right  previous / next state slot
#pragma once

namespace onyx::app {

enum class Hotkey {
    None, OpenMenu, FastForwardDown, FastForwardUp, CycleLayout, Screenshot, ToggleFps,
    SaveState, LoadState, PrevSlot, NextSlot,
};

struct PadState {
    uint16_t buttons = 0;          // RETRO_DEVICE_ID_JOYPAD_* bits
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    bool connected = false;
};

class InputManager {
public:
    InputManager();

    // Called from retro_input_poll on the emulation thread. Returns the
    // hotkeys triggered since the last poll.
    std::vector<Hotkey> Poll();
    const PadState& State() const { return state_; }
    int16_t Query(unsigned device, unsigned index, unsigned id) const;

    void SetSwapFaceButtons(bool swap) { swap_face_ = swap; } // "labels match" layout
    void SetDeadzone(float dz) { deadzone_ = dz; }
    bool AnyController() const { return state_.connected; }
    // Ignore input briefly (after closing the pause menu, so the A press that
    // chose "Resume" does not reach the game).
    void SuppressFor(std::chrono::milliseconds ms);

    // Mouse / touch pointer in normalised [-1, 1] screen space of the frame.
    void SetPointer(float x, float y, bool pressed);

private:
    winrt::Windows::Gaming::Input::Gamepad ActivePad();

    PadState state_{};
    bool swap_face_ = false;
    float deadzone_ = 0.15f;
    // View-button hotkey tracking
    bool view_down_ = false;
    bool view_used_for_hotkey_ = false;
    std::chrono::steady_clock::time_point view_pressed_at_{};
    int select_pulse_frames_ = 0;
    uint32_t prev_raw_ = 0;
    std::chrono::steady_clock::time_point suppress_until_{};
    winrt::Windows::Gaming::Input::Gamepad last_pad_{nullptr};
    // Pointer
    std::atomic<float> ptr_x_{0}, ptr_y_{0};
    std::atomic<bool> ptr_down_{false};
};

} // namespace onyx::app
