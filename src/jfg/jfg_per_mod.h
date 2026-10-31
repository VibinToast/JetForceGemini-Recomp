// Jet Force Gemini port: a setting several mods may ask for at once. Each mod's request is kept by its mod index (from
// recomp::overlays::register_ext_base_export); asking for the setting's neutral value takes the mod's request back.
// The setting is then combined from all requests (any of them, the largest, ...), so one mod turning its request off
// leaves the others' in place.
#pragma once

#include <algorithm>
#include <cstddef>
#include <map>

namespace jfg {

template <class T>
class PerMod {
public:
    void set(size_t mod, T value, T neutral) {
        if (value == neutral) requests.erase(mod);
        else requests[mod] = value;
    }
    bool any() const { return !requests.empty(); }
    T max(T neutral) const {
        T r = neutral;
        for (const auto& [mod, value] : requests) r = std::max(r, value);
        return r;
    }
    T min(T neutral) const {
        T r = neutral;
        for (const auto& [mod, value] : requests) r = std::min(r, value);
        return r;
    }

private:
    std::map<size_t, T> requests;
};

} // namespace jfg
