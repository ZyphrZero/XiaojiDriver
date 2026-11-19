#include "xiaoji_client.h"
#include "../host/installed_input.h"

#include <cstring>
#include <stdexcept>

namespace {
thread_local char lastError[1024]{};

template <typename Operation>
int Call(Operation operation) noexcept {
    lastError[0] = '\0';
    try {
        operation();
        return XIAOJI_OK;
    } catch (const std::invalid_argument& error) {
        strncpy_s(lastError, error.what(), _TRUNCATE);
        return XIAOJI_INVALID_ARGUMENT;
    } catch (const std::exception& error) {
        strncpy_s(lastError, error.what(), _TRUNCATE);
    } catch (...) {
        strcpy_s(lastError, "Unknown client error.");
    }
    return XIAOJI_FAILED;
}
}

uint32_t XiaojiGetAbiVersion() { return XIAOJI_CLIENT_ABI; }
const char* XiaojiLastError() { return lastError; }

int XiaojiGetDevices(uint32_t* started_mask) {
    return Call([&] {
        if (!started_mask) throw std::invalid_argument("Missing device mask output.");
        *started_mask = 0;
        *started_mask = xiaoji::StartedDevices();
    });
}

int XiaojiCheckConnection(uint32_t device) {
    return Call([&] {
        if (device != XIAOJI_KEYBOARD && device != XIAOJI_MOUSE) throw std::invalid_argument("Select keyboard or mouse.");
        xiaoji::CheckInstalled(device == XIAOJI_KEYBOARD);
    });
}

int XiaojiTapKey(uint8_t modifiers, uint8_t usage) {
    return Call([&] {
        if (usage < 0x04 || usage > 0x65) throw std::invalid_argument("Expected HID keyboard usage 0x04..0x65.");
        xiaoji::TapInstalled(modifiers, usage);
    });
}

int XiaojiTypeAscii(const char* text, uint32_t byte_count) {
    return Call([&] {
        if (!text || byte_count == 0 || byte_count > 256) throw std::invalid_argument("Expected 1..256 ASCII bytes.");
        xiaoji::TypeInstalled(std::string(text, byte_count));
    });
}

int XiaojiMoveMouse(int32_t dx, int32_t dy, int32_t wheel) {
    return Call([&] {
        if (dx < -127 || dx > 127 || dy < -127 || dy > 127 || wheel < -127 || wheel > 127)
            throw std::invalid_argument("Relative X, Y and wheel must each be in -127..127.");
        xiaoji::MoveInstalled(dx, dy, wheel);
    });
}
