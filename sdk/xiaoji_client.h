#pragma once
#include <stdint.h>

#ifdef XIAOJI_CLIENT_BUILD
#define XIAOJI_API __declspec(dllexport)
#else
#define XIAOJI_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum { XIAOJI_CLIENT_ABI = 1, XIAOJI_KEYBOARD = 1, XIAOJI_MOUSE = 2 };
enum { XIAOJI_OK = 0, XIAOJI_INVALID_ARGUMENT = 1, XIAOJI_FAILED = 2 };

XIAOJI_API uint32_t __cdecl XiaojiGetAbiVersion(void);
XIAOJI_API const char* __cdecl XiaojiLastError(void);
XIAOJI_API int __cdecl XiaojiGetDevices(uint32_t* started_mask);
XIAOJI_API int __cdecl XiaojiCheckConnection(uint32_t device);
XIAOJI_API int __cdecl XiaojiTapKey(uint8_t modifiers, uint8_t usage);
XIAOJI_API int __cdecl XiaojiTypeAscii(const char* text, uint32_t byte_count);
XIAOJI_API int __cdecl XiaojiMoveMouse(int32_t dx, int32_t dy, int32_t wheel);

#ifdef __cplusplus
}
#endif
