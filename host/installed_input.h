#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace xiaoji {
class InputSession;
class Unconfirmed : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
void RequireNoSession();
InputSession* OpenSession(std::uint32_t devices, std::uint32_t readyMs, std::uint32_t ackMs);
void SessionKey(InputSession* session, std::uint8_t usage, bool down);
void SessionButton(InputSession* session, std::uint32_t button, bool down);
void SessionWheel(InputSession* session, std::int32_t detents);
void CloseSession(InputSession* session);
std::uint32_t StartedDevices();
void CheckInstalled(bool keyboard);
void TapInstalled(std::uint8_t modifiers, std::uint8_t usage);
void TypeInstalled(const std::string& text);
void MoveInstalled(int dx, int dy, int wheel);
}
