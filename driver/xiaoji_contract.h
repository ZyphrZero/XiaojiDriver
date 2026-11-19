/*
 * hm_contract.h: the version contract (the design note's decision (c)) as
 * the HID driver (driver.c) and the XUSB companion (companion.c) both read
 * it: the version section's layout, the identity tag, and the identity
 * entry that names a controller's index before its first start. One copy,
 * so the two drivers cannot read it two ways. Include after <windows.h>
 * and <wdf.h>.
 *
 * Derived from HIDMaestro driver/hm_contract.h (MIT licensed).
 */
#pragma once

/* v2.0.0: SOFTWARE\Xiaoji\Identity\<token>, value ControllerIndex
 * (REG_DWORD): the index the SDK gave the controller whose devnodes carry
 * the token, written before any PnP work (decision (c), "The index before
 * the first start"). */
#define XIAOJI_IDENTITY_ROOT    L"SOFTWARE\\Xiaoji\\Identity\\"
#define XIAOJI_IDENTITY_VALUE   L"ControllerIndex"

#pragma pack(push, 1)
/* Version handshake (v2.0.0): Global\XiaojiVersion<N>, 64 bytes,
 * created by the SDK. The SDK writes its half before the device exists:
 * the first 20 bytes and, from protocol 4, the identity tag. The driver
 * writes its half at add and whenever it opens the input section, the
 * same values each time: DriverVersion first, DriverProtocol last, so a
 * reader that sees a nonzero protocol sees the version that goes with it.
 * It clears nothing in its cleanup. The XUSB companion writes its own
 * field at 40 and nothing of the HID driver's.
 *
 * Protocol 1: the input and output sections, the events and the PID state,
 *             through v1.11.0.
 * Protocol 2: adds InputAck and the owner pipe, v1.12.0.
 * Protocol 3: adds this section, v1.13.0, which never shipped.
 * Protocol 4: the version contract, v2.0.0 (the design note's decision
 *             (c)): the identity tag and the identity entry, under which
 *             a driver reads a controller's v2.0.0 values.
 * Protocol 5: the remove events, v2.0.0: on Global\XiaojiRemoveEvent<N>
 *             the driver fails its device (WdfDeviceSetFailed,
 *             WdfDeviceFailedNoRestart), which removes it and its HID
 *             children as an unplug does, and then signals
 *             Global\XiaojiRemovedEvent<N>.
 * A driver keeps every older layout. The SDK runs on a driver whose range
 * from DriverProtocolMin to DriverProtocol holds its protocol. */
#define XIAOJI_VERSION_LAYOUT       1
#define XIAOJI_DRIVER_PROTOCOL      5
#define XIAOJI_DRIVER_PROTOCOL_MIN  4
/* The protocol whose SDK's values this driver reads. */
#define XIAOJI_CONTRACT_PROTOCOL    4
typedef struct _XIAOJI_VERSION_INFO {
    volatile LONG   LayoutVersion;       /* SDK: XIAOJI_VERSION_LAYOUT */
    volatile LONG   SdkProtocolMin;      /* SDK: the oldest driver protocol it runs on */
    volatile LONG   SdkProtocolMax;      /* SDK: the protocol it speaks */
    USHORT          SdkVersion[4];       /* SDK: major, minor, patch, 0 */
    volatile LONG   DriverProtocol;      /* driver: 0 until it has written its half */
    USHORT          DriverVersion[4];    /* driver: major, minor, patch, 0 */
    volatile LONG   DriverCapabilities;  /* driver: XIAOJI_INPUT_ACK_CAP_* it supports */
    volatile LONG   DriverProtocolMin;   /* driver: the oldest SDK protocol it serves */
    volatile LONG   CompanionProtocol;   /* companion: its protocol */
    volatile LONG   CompanionProtocolMin;/* companion: the oldest SDK protocol it serves */
    ULONGLONG       IdentityTag;         /* SDK: XiaojiIdentityTag of the controller's token */
    volatile LONG   DriverConfigError;   /* driver: XIAOJI_CONFIG_ERROR_* before a failed start */
    volatile LONG   CompanionConfigError;/* companion: the same for its values */
} XIAOJI_VERSION_INFO, *PXIAOJI_VERSION_INFO;
C_ASSERT(sizeof(XIAOJI_VERSION_INFO) == 64);
C_ASSERT(FIELD_OFFSET(XIAOJI_VERSION_INFO, DriverProtocolMin) == 36);
C_ASSERT(FIELD_OFFSET(XIAOJI_VERSION_INFO, IdentityTag) == 48);
C_ASSERT(FIELD_OFFSET(XIAOJI_VERSION_INFO, DriverConfigError) == 56);

/* 2.1.0: the signal gate (docs/design/signal-gate.md). Global\XiaojiSignal<N>,
 * 256 bytes, created by an SDK of 2.1.0 or later as it maps the index,
 * after the owner pipe and after it zeroes the input section. An SDK that
 * finds the section already there leaves the counts and InputHandoff as
 * they are. A driver host of a device on the index may be counting in
 * them, and a final SeqNo the program before it left in InputHandoff is a
 * read the worker still owes, which this program's first frame replaces.
 * Outside its frames, the capability words are the one thing an SDK
 * writes in a section it found: its sweep of a removed device on the index
 * clears them (SharedMemoryIO.ForgetDriverLocked).
 * Each driver host maps it once and keeps it until its device goes, and
 * the XUSB companion holds it from its add to its release.
 *
 * The HID driver's worker counts itself in InputWaiters across every wait
 * on the input event, with an interlocked increment and then an
 * interlocked exchange that takes InputHandoff, and an interlocked
 * decrement when the wait returns. InputHandoff holds a frame's even SeqNo
 * once final, the SeqNo with its low bit set while provisional, or 0. A
 * take that finds a final SeqNo reads the section without waiting and
 * sends whatever new frame it holds: the frame taken, or a newer one its
 * program stored since. When the poll after the take
 * returns the stop event, at a foreign stop or at teardown, the worker
 * signals the input event for that frame instead. At teardown with any
 * other result of the poll it leaves with the frame unread and signals
 * nothing. A take that finds a provisional SeqNo drops it. The SDK stores the frame's even SeqNo and, with
 * XIAOJI_SIGNAL_CAP_INPUT_WAITERS set, reads InputHandoff. A final
 * SeqNo still there is a read the worker owes, which finds this frame too:
 * one compare-exchange puts this frame's final SeqNo in its place, and no
 * SetEvent runs. Otherwise it reads InputWaiters. With a worker counted it
 * calls SetEvent and leaves InputHandoff alone. That branch has no
 * interlocked operation, so for every frame with a companion event a full
 * fence comes before the companion's count is read: a volatile read may
 * pass an earlier volatile write, on x64 and on ARM64 with LRCPC. With none it
 * exchanges the provisional SeqNo in, then loads the capability again and
 * InputWaiters. With the capability still set and the count zero, a
 * compare-exchange makes the frame final for the worker's next take.
 * With any other count, or with the capability gone, a compare-exchange
 * takes it back. It calls SetEvent unless it left the frame final: a
 * compare-exchange that fails found the provisional SeqNo
 * taken and dropped. So a frame gets a wakeup that reads the section after
 * its store, the take or the signal. A frame left provisional by a program
 * that died there gets none, as v2.0.3 never read a frame whose program
 * died before its SetEvent. Without the capability it signals every frame.
 *
 * The XUSB companion counts the WAIT_FOR_INPUT requests in its manual queue
 * in CompanionParked: up once the request is in the queue, then a look at
 * the input section's SeqNo that kicks the pump itself when the section
 * holds a frame the last completion did not carry, and down when the pump
 * or the timer takes the request or the framework cancels it. The pump can
 * take a request before its increment, so the count can be -1 for a moment.
 * The SDK rings the companion doorbell when CompanionParked is not zero or
 * CompanionCaps lacks XIAOJI_SIGNAL_CAP_COMPANION_PARKED.
 *
 * Each counted word sits in a cache line of its own, since two processes
 * write them. InputWaiters below zero is never right: a worker that finds
 * it so clears its capability bit, and the SDK signals every frame again. */
#define XIAOJI_SIGNAL_LAYOUT                1
#define XIAOJI_SIGNAL_CAP_INPUT_WAITERS     0x00000001u   /* DriverCaps */
#define XIAOJI_SIGNAL_CAP_COMPANION_PARKED  0x00000001u   /* CompanionCaps */
typedef struct _XIAOJI_SIGNAL {
    volatile LONG   LayoutVersion;       /* SDK: XIAOJI_SIGNAL_LAYOUT, written when it creates the section */
    volatile LONG   DriverCaps;          /* HID driver: XIAOJI_SIGNAL_CAP_* it keeps */
    volatile LONG   CompanionCaps;       /* companion: XIAOJI_SIGNAL_CAP_* it keeps */
    LONG            Reserved0[13];
    volatile LONG   InputWaiters;        /* HID driver workers inside a counted wait */
    volatile LONG   InputHandoff;        /* SDK: a frame's SeqNo handed to a worker, or 0. HID driver: takes it */
    LONG            Reserved1[14];
    volatile LONG   CompanionParked;     /* WAIT_FOR_INPUT requests the companion holds */
    LONG            Reserved2[31];
} XIAOJI_SIGNAL, *PXIAOJI_SIGNAL;
C_ASSERT(sizeof(XIAOJI_SIGNAL) == 256);
C_ASSERT(FIELD_OFFSET(XIAOJI_SIGNAL, InputWaiters) == 64);
C_ASSERT(FIELD_OFFSET(XIAOJI_SIGNAL, InputHandoff) == 68);
C_ASSERT(FIELD_OFFSET(XIAOJI_SIGNAL, CompanionParked) == 128);
#pragma pack(pop)

/* The identity tag (decision (c)): the 64-bit FNV-1a hash of the token's
 * UTF-16LE code units, no terminator, as the SDK's DeviceIdentity.TagOf
 * computes it. XIAOJI_0000 gives 0x1FD048916B8D461F and XIAOJI_0123456789ABCDEF
 * 0x3B86F3634086D831. */
static __inline ULONGLONG XiaojiIdentityTag(_In_z_ const WCHAR *token)
{
    ULONGLONG h = 0xCBF29CE484222325ULL;
    for (SIZE_T i = 0; token[i]; i++) {
        h ^= (UCHAR)(token[i] & 0xFF);
        h *= 0x100000001B3ULL;
        h ^= (UCHAR)((token[i] >> 8) & 0xFF);
        h *= 0x100000001B3ULL;
    }
    return h;
}

/* DEVPKEY_Device_InstanceId (devpkey.h), written out so no file needs
 * initguid.h. */
static const DEVPROPKEY g_XiaojiInstanceIdKey =
    { { 0x78c34fc8, 0x104a, 0x4aca, { 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57 } }, 256 };

/* The identity token, the last segment of the devnode's instance ID
 * (ROOT\<enumerator>\<token> or SWD\<enumerator>\<token>, issue #60), read
 * through WdfFdoInitQueryPropertyEx before WdfDeviceCreate, as Learn says
 * a driver calls it. Empty when the query fails. */
static __inline VOID
XiaojiReadInstanceToken(_In_ PWDFDEVICE_INIT DeviceInit, _Out_writes_(cap) WCHAR *token, _In_ SIZE_T cap)
{
    WCHAR id[200];
    ULONG len = 0;
    DEVPROPTYPE type = 0;
    WDF_DEVICE_PROPERTY_DATA data;
    token[0] = 0;
    WDF_DEVICE_PROPERTY_DATA_INIT(&data, &g_XiaojiInstanceIdKey);
    if (!NT_SUCCESS(WdfFdoInitQueryPropertyEx(DeviceInit, &data, sizeof(id), id, &len, &type))
        || type != DEVPROP_TYPE_STRING || len < sizeof(WCHAR))
        return;
    id[ARRAYSIZE(id) - 1] = 0;
    SIZE_T n = 0, last = 0;
    while (id[n]) { if (id[n] == L'\\') last = n + 1; n++; }
    SIZE_T t = 0;
    while (id[last + t] && t + 1 < cap) { token[t] = id[last + t]; t++; }
    token[t] = 0;
}

/* The index the SDK wrote under XIAOJI_IDENTITY_ROOT<token>, value
 * XIAOJI_IDENTITY_VALUE, before any PnP work. */
static __inline BOOLEAN
XiaojiReadIdentityIndex(_In_ const WCHAR *token, _Out_ ULONG *index)
{
    static const WCHAR prefix[] = XIAOJI_IDENTITY_ROOT;
    WCHAR path[96];
    HKEY hKey;
    DWORD val = 0, sz = sizeof(val), type = 0;
    SIZE_T n = 0;
    *index = 0;
    if (!token[0]) return FALSE;
    for (SIZE_T i = 0; prefix[i]; i++) path[n++] = prefix[i];
    for (SIZE_T i = 0; token[i] && n + 1 < ARRAYSIZE(path); i++) path[n++] = token[i];
    path[n] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return FALSE;
    BOOLEAN ok = RegQueryValueExW(hKey, XIAOJI_IDENTITY_VALUE, NULL, &type, (LPBYTE)&val, &sz) == ERROR_SUCCESS
              && type == REG_DWORD && sz == sizeof(val);
    RegCloseKey(hKey);
    if (ok) *index = val;
    return ok;
}

/* 2.1.0: Controller<N>\AckSignal (REG_DWORD), read under the contract. 0
 * when no thread of the controller's SDK waits on the delivery
 * acknowledgement's event, so the driver stores DeliveredSeqNo and skips
 * the SetEvent. Any other value, or none (a program before 2.1.0), and the
 * driver signals every delivered frame. */
#define XIAOJI_ACK_SIGNAL_VALUE  L"AckSignal"

/* The signal section of a controller's index, mapped read and write, or
 * NULL when there is none or its layout is another. *handle is the
 * section's handle, or NULL. */
static __inline PXIAOJI_SIGNAL
XiaojiMapSignal(_In_z_ const WCHAR *name, _Out_ HANDLE *handle)
{
    PXIAOJI_SIGNAL view;
    *handle = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (*handle == NULL) return NULL;
    view = (PXIAOJI_SIGNAL)MapViewOfFile(*handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(XIAOJI_SIGNAL));
    if (view != NULL && view->LayoutVersion == XIAOJI_SIGNAL_LAYOUT) return view;
    if (view != NULL) UnmapViewOfFile(view);
    CloseHandle(*handle);
    *handle = NULL;
    return NULL;
}
