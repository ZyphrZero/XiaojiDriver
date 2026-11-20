#pragma once
#include <stdint.h>

#ifdef XIAOJI_CLIENT_BUILD
#define XIAOJI_API __declspec(dllexport)
#elif defined(XIAOJI_CLIENT_STATIC)
#define XIAOJI_API
#else
#define XIAOJI_API __declspec(dllimport)
#endif

#ifdef __cplusplus
namespace xiaoji { class InputSession; }
typedef xiaoji::InputSession XiaojiSession;
extern "C" {
#else
typedef struct XiaojiSession XiaojiSession;
#endif

enum { XIAOJI_CLIENT_ABI = 2, XIAOJI_KEYBOARD = 1, XIAOJI_MOUSE = 2 };
enum { XIAOJI_OK = 0, XIAOJI_INVALID_ARGUMENT = 1, XIAOJI_FAILED = 2, XIAOJI_UNCONFIRMED = 3 };

XIAOJI_API uint32_t __cdecl XiaojiGetAbiVersion(void);
XIAOJI_API const char* __cdecl XiaojiLastError(void);
XIAOJI_API int __cdecl XiaojiGetDevices(uint32_t* started_mask);
XIAOJI_API int __cdecl XiaojiCheckConnection(uint32_t device);
XIAOJI_API int __cdecl XiaojiTapKey(uint8_t modifiers, uint8_t usage);
XIAOJI_API int __cdecl XiaojiTypeAscii(const char* text, uint32_t byte_count);
XIAOJI_API int __cdecl XiaojiMoveMouse(int32_t dx, int32_t dy, int32_t wheel);
XIAOJI_API int __cdecl XiaojiSessionOpen(uint32_t devices, uint32_t ready_timeout_ms, uint32_t ack_timeout_ms, XiaojiSession** session);
XIAOJI_API int __cdecl XiaojiSessionKey(XiaojiSession* session, uint8_t usage, int down);
XIAOJI_API int __cdecl XiaojiSessionButton(XiaojiSession* session, uint32_t button, int down);
XIAOJI_API int __cdecl XiaojiSessionWheel(XiaojiSession* session, int32_t detents);
XIAOJI_API int __cdecl XiaojiSessionClose(XiaojiSession* session);

#ifdef __cplusplus
}
#endif
