#include "xiaoji_client.h"
#include "../host/installed_input.h"

#include <cstring>
#include <stdexcept>

namespace {
thread_local char lastError[1024]{};

template <bool session = false, typename Operation>
int Call(Operation operation) noexcept {
    lastError[0] = '\0';
    try {
        operation();
        return XIAOJI_OK;
    } catch (const xiaoji::Unconfirmed& error) {
        strncpy_s(lastError, error.what(), _TRUNCATE);
        return session ? XIAOJI_UNCONFIRMED : XIAOJI_FAILED;
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
        xiaoji::RequireNoSession();
        if (device != XIAOJI_KEYBOARD && device != XIAOJI_MOUSE) throw std::invalid_argument("Select keyboard or mouse.");
        xiaoji::CheckInstalled(device == XIAOJI_KEYBOARD);
    });
}

int XiaojiTapKey(uint8_t modifiers, uint8_t usage) {
    return Call([&] {
        xiaoji::RequireNoSession();
        if (usage < 0x04 || usage > 0x65) throw std::invalid_argument("Expected HID keyboard usage 0x04..0x65.");
        xiaoji::TapInstalled(modifiers, usage);
    });
}

int XiaojiTypeAscii(const char* text, uint32_t byte_count) {
    return Call([&] {
        xiaoji::RequireNoSession();
        if (!text || byte_count == 0 || byte_count > 256) throw std::invalid_argument("Expected 1..256 ASCII bytes.");
        xiaoji::TypeInstalled(std::string(text, byte_count));
    });
}

int XiaojiMoveMouse(int32_t dx, int32_t dy, int32_t wheel) {
    return Call([&] {
        xiaoji::RequireNoSession();
        if (dx < -127 || dx > 127 || dy < -127 || dy > 127 || wheel < -127 || wheel > 127)
            throw std::invalid_argument("Relative X, Y and wheel must each be in -127..127.");
        xiaoji::MoveInstalled(dx, dy, wheel);
    });
}

int XiaojiSessionOpen(uint32_t devices, uint32_t ready_timeout_ms, uint32_t ack_timeout_ms, XiaojiSession** session) {
    return Call<true>([&] {
        if (!session) throw std::invalid_argument("Missing session output.");
        *session = nullptr;
        if (devices < 1 || devices > 3) throw std::invalid_argument("Expected device mask 1..3.");
        if (!ready_timeout_ms || ready_timeout_ms > 10000) throw std::invalid_argument("Expected ready timeout 1..10000 ms.");
        if (!ack_timeout_ms || ack_timeout_ms > 2000) throw std::invalid_argument("Expected acknowledgement timeout 1..2000 ms.");
        *session = xiaoji::OpenSession(devices, ready_timeout_ms, ack_timeout_ms);
    });
}

int XiaojiSessionKey(XiaojiSession* session, uint8_t usage, int down) {
    return Call<true>([&] { xiaoji::SessionKey(session, usage, down != 0); });
}

int XiaojiSessionButton(XiaojiSession* session, uint32_t button, int down) {
    return Call<true>([&] { xiaoji::SessionButton(session, button, down != 0); });
}

int XiaojiSessionWheel(XiaojiSession* session, int32_t detents) {
    return Call<true>([&] { xiaoji::SessionWheel(session, detents); });
}

int XiaojiSessionClose(XiaojiSession* session) {
    return Call<true>([&] { xiaoji::CloseSession(session); });
}
