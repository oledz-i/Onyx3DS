// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/core_options.h"

#include <nlohmann/json.hpp>

namespace onyx {

using nlohmann::json;

std::string CoreOptionCatalog::ToJson() const {
    json cats = json::array();
    for (const auto& c : categories) cats.push_back({{"key", c.key}, {"label", c.label}, {"info", c.info}});
    json opts = json::array();
    for (const auto& o : options) {
        json values = json::array();
        for (const auto& v : o.values) values.push_back({{"value", v.value}, {"label", v.label}});
        opts.push_back({{"key", o.key}, {"label", o.label}, {"info", o.info}, {"category", o.category},
                        {"default", o.default_value}, {"values", values}});
    }
    return json{{"version", 1}, {"categories", cats}, {"options", opts}}.dump();
}

CoreOptionCatalog CoreOptionCatalog::FromJson(std::string_view text) {
    CoreOptionCatalog c;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return c;
    for (const auto& x : j.value("categories", json::array()))
        c.categories.push_back({x.value("key", ""), x.value("label", ""), x.value("info", "")});
    for (const auto& x : j.value("options", json::array())) {
        CoreOptionDef d;
        d.key = x.value("key", "");
        d.label = x.value("label", "");
        d.info = x.value("info", "");
        d.category = x.value("category", "");
        d.default_value = x.value("default", "");
        for (const auto& v : x.value("values", json::array()))
            d.values.push_back({v.value("value", ""), v.value("label", "")});
        if (!d.key.empty()) c.options.push_back(std::move(d));
    }
    return c;
}

} // namespace onyx
