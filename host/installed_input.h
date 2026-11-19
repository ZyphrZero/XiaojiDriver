#pragma once

#include <cstdint>
#include <string>

namespace xiaoji {
std::uint32_t StartedDevices();
void CheckInstalled(bool keyboard);
void TapInstalled(std::uint8_t modifiers, std::uint8_t usage);
void TypeInstalled(const std::string& text);
void MoveInstalled(int dx, int dy, int wheel);
}
