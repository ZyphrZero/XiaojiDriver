#pragma once

#include <string>
#include <string_view>

namespace xiaoji {
enum class DeviceState { absent, current, legacy, not_started, unknown };

inline const char* StateName(DeviceState state) {
    switch (state) {
    case DeviceState::absent: return "absent";
    case DeviceState::current: return "current";
    case DeviceState::legacy: return "legacy";
    case DeviceState::not_started: return "not_started";
    default: return "unknown";
    }
}

inline const char* InstallationState(DeviceState keyboard, DeviceState mouse) {
    using State = DeviceState;
    if (keyboard == State::absent && mouse == State::absent) return "not_installed";
    if (keyboard == State::current && mouse == State::current) return "ready";
    if ((keyboard == State::current || keyboard == State::legacy) &&
        (mouse == State::current || mouse == State::legacy)) return "needs_upgrade";
    return "broken";
}

inline std::string JsonString(std::string_view utf8) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result = "\"";
    for (unsigned char byte : utf8) {
        if (byte < 32 || byte == '"' || byte == '\\') {
            result += "\\u00";
            result += hex[byte >> 4]; result += hex[byte & 15];
        } else result += static_cast<char>(byte);
    }
    return result + '"';
}
}
