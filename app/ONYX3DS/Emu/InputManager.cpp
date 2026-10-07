// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Emu/InputManager.h"

using namespace winrt::Windows::Gaming::Input;

namespace onyx::app {

namespace {
constexpr uint16_t Bit(unsigned id) {
    return static_cast<uint16_t>(1u << id);
}
bool Has(GamepadButtons all, GamepadButtons b) {
    return (static_cast<uint32_t>(all) & static_cast<uint32_t>(b)) != 0;
}
int16_t Axis(double v, float deadzone) {
    const double mag = std::abs(v);
    if (mag < deadzone) return 0;
    // Rescale so the stick still reaches full deflection past the deadzone.
    const double scaled = (mag - deadzone) / (1.0 - deadzone) * (v < 0 ? -1.0 : 1.0);
    return static_cast<int16_t>(std::clamp(scaled, -1.0, 1.0) * 32767.0);
}
} // namespace

InputManager::InputManager() = default;

void InputManager::SuppressFor(std::chrono::milliseconds ms) {
    suppress_until_ = std::chrono::steady_clock::now() + ms;
}

void InputManager::SetPointer(float x, float y, bool pressed) {
    ptr_x_ = std::clamp(x, -1.0f, 1.0f);
    ptr_y_ = std::clamp(y, -1.0f, 1.0f);
    ptr_down_ = pressed;
}

Gamepad InputManager::ActivePad() {
    // Follow whichever controller was used last, so a second pad can take over.
    auto pads = Gamepad::Gamepads();
    if (pads.Size() == 0) return nullptr;
    for (auto const& pad : pads) {
        const auto r = pad.GetCurrentReading();
        if (r.Buttons != GamepadButtons::None || std::abs(r.LeftThumbstickX) > 0.5 ||
            std::abs(r.LeftThumbstickY) > 0.5) {
            last_pad_ = pad;
            break;
        }
    }
    if (last_pad_) {
        for (auto const& pad : pads)
            if (pad == last_pad_) return pad;
    }
    last_pad_ = pads.GetAt(0);
    return last_pad_;
}

std::vector<Hotkey> InputManager::Poll() {
    std::vector<Hotkey> hotkeys;
    const auto pad = ActivePad();
    state_ = PadState{};
    if (!pad) return hotkeys;
    state_.connected = true;
    const GamepadReading r = pad.GetCurrentReading();
    const auto now = std::chrono::steady_clock::now();
    if (now < suppress_until_) return hotkeys;
    if (blocked_) {
        // Keep the button tracking current so nothing looks freshly pressed when the
        // controller is handed back to the game.
        const uint32_t rb = static_cast<uint32_t>(GamepadButtons::RightShoulder);
        // A fast-forward hold must not stay latched when RB is let go while blocked.
        if ((prev_raw_ & rb) && !(static_cast<uint32_t>(r.Buttons) & rb)) hotkeys.push_back(Hotkey::FastForwardUp);
        prev_raw_ = static_cast<uint32_t>(r.Buttons);
        view_down_ = false;
        view_used_for_hotkey_ = true;
        select_pulse_frames_ = 0;
        return hotkeys;
    }

    const GamepadButtons b = r.Buttons;
    const bool view = Has(b, GamepadButtons::View);
    const uint32_t raw = static_cast<uint32_t>(b);
    const uint32_t pressed = raw & ~prev_raw_;
    const uint32_t released = prev_raw_ & ~raw;
    prev_raw_ = raw;
    auto just = [&](GamepadButtons x) { return (pressed & static_cast<uint32_t>(x)) != 0; };
    auto up = [&](GamepadButtons x) { return (released & static_cast<uint32_t>(x)) != 0; };

    // --- hotkeys --------------------------------------------------------------
    if (view && !view_down_) {
        view_down_ = true;
        view_used_for_hotkey_ = false;
        view_pressed_at_ = now;
    }
    if (view) {
        auto hk = [&](GamepadButtons x, Hotkey h) {
            if (just(x)) {
                hotkeys.push_back(h);
                view_used_for_hotkey_ = true;
            }
        };
        hk(GamepadButtons::Menu, Hotkey::OpenMenu);
        hk(GamepadButtons::RightShoulder, Hotkey::FastForwardDown);
        hk(GamepadButtons::LeftShoulder, Hotkey::CycleLayout);
        hk(GamepadButtons::Y, Hotkey::Screenshot);
        hk(GamepadButtons::A, Hotkey::ToggleFps);
        hk(GamepadButtons::DPadUp, Hotkey::SaveState);
        hk(GamepadButtons::DPadDown, Hotkey::LoadState);
        hk(GamepadButtons::DPadLeft, Hotkey::PrevSlot);
        hk(GamepadButtons::DPadRight, Hotkey::NextSlot);
    }
    if (up(GamepadButtons::RightShoulder)) hotkeys.push_back(Hotkey::FastForwardUp);
    if (!view && view_down_) {
        view_down_ = false;
        // A quick tap of View on its own is Select for the game.
        if (!view_used_for_hotkey_ && now - view_pressed_at_ < std::chrono::milliseconds(400))
            select_pulse_frames_ = 3;
    }

    // --- buttons --------------------------------------------------------------
    uint16_t out = 0;
    if (!view) { // while View is held, buttons belong to hotkeys
        const bool sw = swap_face_;
        if (Has(b, GamepadButtons::B)) out |= Bit(sw ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_A);
        if (Has(b, GamepadButtons::A)) out |= Bit(sw ? RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_B);
        if (Has(b, GamepadButtons::Y)) out |= Bit(sw ? RETRO_DEVICE_ID_JOYPAD_Y : RETRO_DEVICE_ID_JOYPAD_X);
        if (Has(b, GamepadButtons::X)) out |= Bit(sw ? RETRO_DEVICE_ID_JOYPAD_X : RETRO_DEVICE_ID_JOYPAD_Y);
        if (Has(b, GamepadButtons::LeftShoulder)) out |= Bit(RETRO_DEVICE_ID_JOYPAD_L);
        if (Has(b, GamepadButtons::RightShoulder)) out |= Bit(RETRO_DEVICE_ID_JOYPAD_R);
        if (r.LeftTrigger > 0.5) out |= Bit(RETRO_DEVICE_ID_JOYPAD_L2);
        if (r.RightTrigger > 0.5) out |= Bit(RETRO_DEVICE_ID_JOYPAD_R2);
        if (Has(b, GamepadButtons::Menu)) out |= Bit(RETRO_DEVICE_ID_JOYPAD_START);
        if (Has(b, GamepadButtons::DPadUp)) out |= Bit(RETRO_DEVICE_ID_JOYPAD_UP);
        if (Has(b, GamepadButtons::DPadDown)) out |= Bit(RETRO_DEVICE_ID_JOYPAD_DOWN);
        if (Has(b, GamepadButtons::DPadLeft)) out |= Bit(RETRO_DEVICE_ID_JOYPAD_LEFT);
        if (Has(b, GamepadButtons::DPadRight)) out |= Bit(RETRO_DEVICE_ID_JOYPAD_RIGHT);
        if (Has(b, GamepadButtons::LeftThumbstick)) out |= Bit(RETRO_DEVICE_ID_JOYPAD_L3);
        if (Has(b, GamepadButtons::RightThumbstick)) out |= Bit(RETRO_DEVICE_ID_JOYPAD_R3);
    } else if (!view_used_for_hotkey_ && now - view_pressed_at_ >= std::chrono::milliseconds(400)) {
        // View held on its own for a while: the game wants Select held.
        out |= Bit(RETRO_DEVICE_ID_JOYPAD_SELECT);
    }
    if (select_pulse_frames_ > 0) {
        out |= Bit(RETRO_DEVICE_ID_JOYPAD_SELECT);
        --select_pulse_frames_;
    }
    state_.buttons = out;

    // libretro: +Y is down.
    state_.lx = Axis(r.LeftThumbstickX, deadzone_);
    state_.ly = static_cast<int16_t>(-Axis(r.LeftThumbstickY, deadzone_));
    state_.rx = Axis(r.RightThumbstickX, deadzone_);
    state_.ry = static_cast<int16_t>(-Axis(r.RightThumbstickY, deadzone_));
    return hotkeys;
}

int16_t InputManager::Query(unsigned device, unsigned index, unsigned id) const {
    switch (device) {
    case RETRO_DEVICE_JOYPAD:
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return static_cast<int16_t>(state_.buttons);
        return (state_.buttons & Bit(id)) ? 1 : 0;
    case RETRO_DEVICE_ANALOG:
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
            return id == RETRO_DEVICE_ID_ANALOG_X ? state_.lx : state_.ly;
        if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
            return id == RETRO_DEVICE_ID_ANALOG_X ? state_.rx : state_.ry;
        return 0;
    case RETRO_DEVICE_POINTER:
        switch (id) {
        case RETRO_DEVICE_ID_POINTER_X: return static_cast<int16_t>(ptr_x_.load() * 32767.0f);
        case RETRO_DEVICE_ID_POINTER_Y: return static_cast<int16_t>(ptr_y_.load() * 32767.0f);
        case RETRO_DEVICE_ID_POINTER_PRESSED: return ptr_down_ ? 1 : 0;
        case RETRO_DEVICE_ID_POINTER_COUNT: return ptr_down_ ? 1 : 0;
        default: return 0;
        }
    default:
        return 0;
    }
}

} // namespace onyx::app
