// SPDX-License-Identifier: GPL-3.0-or-later
//
// A copy of the option catalogue the Azahar core announces through
// RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2. The Advanced emulation page is built
// from this list, so any option a future Azahar version adds shows up in the
// app without code changes.
#pragma once

#include <string>
#include <vector>

#include "onyx/settings.h"

namespace onyx {

struct CoreOptionValue {
    std::string value;
    std::string label;
};

struct CoreOptionCategory {
    std::string key;
    std::string label;
    std::string info;
};

struct CoreOptionDef {
    std::string key;
    std::string label;
    std::string info;
    std::string category;
    std::string default_value;
    std::vector<CoreOptionValue> values;

    std::string LabelFor(const std::string& value) const {
        for (const auto& v : values)
            if (v.value == value) return v.label.empty() ? v.value : v.label;
        return value;
    }
    // Next/previous value, for left/right on the controller.
    std::string Step(const std::string& current, int delta) const {
        if (values.empty()) return current;
        int idx = 0;
        for (int i = 0; i < static_cast<int>(values.size()); ++i)
            if (values[i].value == current) idx = i;
        const int n = static_cast<int>(values.size());
        return values[((idx + delta) % n + n) % n].value;
    }
};

struct CoreOptionCatalog {
    std::vector<CoreOptionCategory> categories;
    std::vector<CoreOptionDef> options;

    const CoreOptionDef* Find(const std::string& key) const {
        for (const auto& o : options)
            if (o.key == key) return &o;
        return nullptr;
    }
    // The value the core will see: stored value if valid, else the default.
    std::string Value(const CoreOptions& stored, const std::string& key) const {
        const CoreOptionDef* def = Find(key);
        auto it = stored.find(key);
        if (!def) return it == stored.end() ? std::string{} : it->second;
        if (it != stored.end())
            for (const auto& v : def->values)
                if (v.value == it->second) return it->second;
        return def->default_value;
    }

    // Cached to disk after the core announces its options, so the Advanced
    // page works before the first game of a session has started.
    std::string ToJson() const;
    static CoreOptionCatalog FromJson(std::string_view json);
};

} // namespace onyx
