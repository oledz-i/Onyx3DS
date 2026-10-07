// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Emu/KeyboardBridge.h"

#include "Emu/AzaharBridge.h"
#include "Platform/Log.h"

namespace onyx::app {

namespace {
void KeyboardHook(const Frontend::KeyboardRequest* request) {
    KeyboardBridge::Get().OnRequest(request);
}
} // namespace

KeyboardBridge& KeyboardBridge::Get() {
    static KeyboardBridge instance;
    return instance;
}

void KeyboardBridge::SetHandlers(KeyboardHandlers handlers) {
    const bool active = static_cast<bool>(handlers.open);
    {
        std::lock_guard lock(mutex_);
        handlers_ = std::move(handlers);
        mail_ = Mail{};
        has_mail_ = false;
    }
    // Without a page to show it, the core keeps its own behaviour (answer with the user name).
    Frontend::SetKeyboardRequestHook(active ? &KeyboardHook : nullptr);
}

void KeyboardBridge::Submit(std::string utf8_text, int button) {
    std::lock_guard lock(mutex_);
    if (mail_.kind == Mail::Kind::Abort) return; // a pending abort wins
    mail_.kind = Mail::Kind::Submit;
    mail_.text = std::move(utf8_text);
    mail_.button = button;
    has_mail_.store(true, std::memory_order_release);
}

void KeyboardBridge::Abort(std::string fallback_text) {
    std::lock_guard lock(mutex_);
    mail_.kind = Mail::Kind::Abort;
    mail_.text = std::move(fallback_text);
    mail_.button = 0;
    has_mail_.store(true, std::memory_order_release);
}

void KeyboardBridge::Pump() {
    if (!has_mail_.load(std::memory_order_acquire)) return;
    Mail mail;
    KeyboardHandlers handlers;
    {
        std::lock_guard lock(mutex_);
        mail = std::move(mail_);
        mail_ = Mail{};
        has_mail_.store(false, std::memory_order_release);
        handlers = handlers_;
    }
    if (mail.kind == Mail::Kind::Submit) {
        const int rc = Frontend::SubmitKeyboardText(mail.text.c_str(), mail.button);
        if (rc > 0) {
            const char* why = Frontend::KeyboardErrorText(rc);
            ONYX_INFO("Keyboard text refused (%d): %s", rc, why);
            if (handlers.rejected) handlers.rejected(why);
        } else {
            // Accepted (0), or nothing was waiting any more (-1): either way the UI is done.
            ONYX_INFO("Keyboard answered (button %d, %zu bytes)", mail.button, mail.text.size());
            if (handlers.close) handlers.close();
        }
    } else if (mail.kind == Mail::Kind::Abort) {
        const bool was_pending = Frontend::AbortKeyboardRequest(mail.text.c_str());
        ONYX_INFO("Keyboard closed by the player%s", was_pending ? "" : " (nothing was waiting)");
        if (handlers.close) handlers.close();
    }
}

void KeyboardBridge::OnRequest(const void* raw) {
    KeyboardHandlers handlers;
    {
        std::lock_guard lock(mutex_);
        handlers = handlers_;
        mail_ = Mail{}; // an answer meant for an earlier request must not reach this one
        has_mail_.store(false, std::memory_order_release);
    }
    const auto* r = static_cast<const Frontend::KeyboardRequest*>(raw);
    if (!r) { // the game went away with the keyboard open
        if (handlers.close) handlers.close();
        return;
    }
    if (!handlers.open) {
        // No page to show it: never leave the game waiting.
        Frontend::AbortKeyboardRequest("");
        return;
    }
    KeyboardRequestInfo info;
    info.button_config = std::clamp(r->button_config, 0, 3);
    for (int i = 0; i < 3; ++i) info.button_text[i] = r->button_text[i] ? r->button_text[i] : "";
    info.accept_mode = r->accept_mode;
    info.hint = r->hint_text ? r->hint_text : "";
    info.error = r->error_text ? r->error_text : "";
    info.password = r->password;
    info.callback = r->callback;
    info.numpad = r->type == 2 || (r->max_text_length == 0 && r->max_digits > 0);
    TextRules& rules = info.rules;
    rules.max_units = std::max(0, r->max_text_length);
    if (rules.max_units == 0 && info.numpad) rules.max_units = r->max_digits;
    rules.max_digits = std::max(0, r->max_digits);
    rules.prevent_digit = r->prevent_digit;
    rules.prevent_at = r->prevent_at;
    rules.prevent_percent = r->prevent_percent;
    rules.prevent_backslash = r->prevent_backslash;
    rules.digits_only = info.numpad;
    rules.multiline = r->multiline && !info.numpad;
    ONYX_INFO("Keyboard requested: buttons=%d max=%d digits=%d numpad=%d multiline=%d hint='%s'",
              info.button_config, rules.max_units, rules.max_digits, info.numpad ? 1 : 0,
              rules.multiline ? 1 : 0, info.hint.c_str());
    handlers.open(info);
}

} // namespace onyx::app
