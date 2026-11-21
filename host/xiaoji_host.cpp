#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
extern "C" {
#include <hidsdi.h>
#include <hidusage.h>
}
#include <newdev.h>
#include <shlobj.h>
#include "host_entry.h"
#include "installed_input.h"
#include "pnp_management.h"
#ifdef XIAOJI_SESSION_TEST
#include "local_signing.h"
#define XIAOJI_CLIENT_STATIC
#include "../sdk/xiaoji_client.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "advapi32.lib")

namespace xiaoji {

constexpr GUID kHidClassGuid = {
    0x745a17a0, 0x74d3, 0x11d0, {0xb6, 0xfe, 0x00, 0xa0, 0xc9, 0x0f, 0x57, 0xda}};

constexpr wchar_t kHardwareId[] = L"ROOT\\XIAOJI";
constexpr wchar_t kKeyboardInstance[] = L"ROOT\\XIAOJI\\KBD0001";
constexpr wchar_t kMouseInstance[] = L"ROOT\\XIAOJI\\MOU0001";
constexpr wchar_t kSecuritySddl[] =
    L"O:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GR;;;WD)";

#pragma pack(push, 1)
struct SharedInput {
    volatile ULONG seqNo;
    ULONG dataSize;
    BYTE data[256];
    BYTE gipData[14];
    ULONG extendedReportSize;
    BYTE extendedReportData[80];
};

struct InputAckFrame {
    ULONG size;
    BYTE data[80];
};

struct InputAck {
    volatile LONG deliveredSeqNo;
    volatile LONG capabilities;
    volatile LONG releaseSeq;
    volatile LONG releaseCount;
    InputAckFrame release[8];
};
#pragma pack(pop)

static_assert(sizeof(SharedInput) == 362, "shared input layout must match HIDMaestro/xiaoji driver");
static_assert(offsetof(SharedInput, data) == 8, "shared input data offset");
static_assert(offsetof(SharedInput, extendedReportSize) == 278, "shared input extended size offset");
static_assert(offsetof(SharedInput, extendedReportData) == 282, "shared input extended data offset");
static_assert(sizeof(InputAck) == 688, "input acknowledgement layout");
static_assert(offsetof(InputAck, release) == 16, "input acknowledgement release offset");

constexpr LONG kAckDelivered = 0x00000001;

// ABI 1 uses the first 8 bytes (modifiers, reserved, six usages). The driver
// zero-extends short frames, so the appended bitmap is clear for old writers.
constexpr DWORD kLegacyKeyboardBytes = 8;
constexpr BYTE kBitmapUsages = 160;
constexpr DWORD kKeyboardBytes = kLegacyKeyboardBytes + kBitmapUsages / 8;
constexpr DWORD kMouseBytes = 4;
constexpr BYTE kLegacyMouseButtons = 3, kMouseButtons = 5;

constexpr BYTE kLegacyKeyboardDescriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07,
    0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

constexpr auto kKeyboardDescriptor = [] {
    constexpr BYTE tail[] = {
        0x05, 0x07, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, kBitmapUsages,
        0x19, 0x00, 0x29, kBitmapUsages - 1, 0x81, 0x02, 0xC0,
    };
    constexpr size_t prefix = sizeof(kLegacyKeyboardDescriptor) - 1; // before End Collection
    std::array<BYTE, prefix + sizeof(tail)> descriptor{};
    for (size_t pos = 0; pos < prefix; ++pos) descriptor[pos] = kLegacyKeyboardDescriptor[pos];
    for (size_t pos = 0; pos < sizeof(tail); ++pos) descriptor[prefix + pos] = tail[pos];
    return descriptor;
}();

// Button bits grow into the padding; relative X, Y and wheel stay at bytes 1..3.
template<BYTE Buttons> constexpr BYTE MouseDescriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01,
    0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, Buttons,
    0x15, 0x00, 0x25, 0x01, 0x95, Buttons, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 8 - Buttons, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0xC0, 0xC0,
};

struct ReportLayout {
    const BYTE* descriptor;
    DWORD size;
    DWORD inputBytes;
};
constexpr ReportLayout kCurrentLayouts[] = {
    {kKeyboardDescriptor.data(), static_cast<DWORD>(kKeyboardDescriptor.size()), kKeyboardBytes},
    {MouseDescriptor<kMouseButtons>, sizeof(MouseDescriptor<kMouseButtons>), kMouseBytes},
};
constexpr ReportLayout kLegacyLayouts[] = {
    {kLegacyKeyboardDescriptor, sizeof(kLegacyKeyboardDescriptor), kLegacyKeyboardBytes},
    {MouseDescriptor<kLegacyMouseButtons>, sizeof(MouseDescriptor<kLegacyMouseButtons>), kMouseBytes},
};

bool MatchesLayout(const ReportLayout& stored, const ReportLayout& expected) {
    return stored.inputBytes == expected.inputBytes && stored.size == expected.size &&
        std::memcmp(stored.descriptor, expected.descriptor, expected.size) == 0;
}

LayoutVersion ClassifyConfig(int index, const ReportLayout& stored) {
    if (MatchesLayout(stored, kCurrentLayouts[index])) return LayoutVersion::current;
    if (MatchesLayout(stored, kLegacyLayouts[index])) return LayoutVersion::legacy;
    return LayoutVersion::unknown;
}

bool NeedsLayoutRewrite(LayoutVersion stored) { return stored != LayoutVersion::current; }

std::string Narrow(const std::wstring& value) {
    if (value.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), needed, nullptr, nullptr);
    return out;
}

std::string Win32Message(DWORD error) {
    LPWSTR buffer = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = length && buffer ? std::wstring(buffer, length) : L"unknown error";
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    return Narrow(text) + " (" + std::to_string(error) + ")";
}

[[noreturn]] void Fail(const std::string& what) {
    throw std::runtime_error(what + ": " + Win32Message(GetLastError()));
}

class SecurityDescriptor {
public:
    SecurityDescriptor() {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                kSecuritySddl, SDDL_REVISION_1, &descriptor_, nullptr)) {
            Fail("ConvertStringSecurityDescriptorToSecurityDescriptorW");
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = descriptor_;
        attributes_.bInheritHandle = FALSE;
    }

    ~SecurityDescriptor() {
        if (descriptor_) LocalFree(descriptor_);
    }

    SecurityDescriptor(const SecurityDescriptor&) = delete;
    SecurityDescriptor& operator=(const SecurityDescriptor&) = delete;

    PSECURITY_ATTRIBUTES Get() { return &attributes_; }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

void CheckObjectOwner(HANDLE handle) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD error = GetSecurityInfo(handle, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
        nullptr, nullptr, nullptr, &descriptor);
    bool trusted = error == ERROR_SUCCESS && owner &&
        (IsWellKnownSid(owner, WinBuiltinAdministratorsSid) || IsWellKnownSid(owner, WinLocalSystemSid));
    if (descriptor) LocalFree(descriptor);
    if (!trusted) throw std::runtime_error("Refusing IPC object not owned by Administrators or SYSTEM.");
}

class OperationLock {
public:
    explicit OperationLock(const wchar_t* name = L"Global\\XiaojiHostOperation") {
        SecurityDescriptor security;
        handle_ = CreateMutexW(security.Get(), FALSE, name);
        if (!handle_) Fail("CreateMutexW");
        try {
            CheckObjectOwner(handle_);
            DWORD wait = WaitForSingleObject(handle_, 0);
            if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
                throw std::runtime_error("Another host operation is active, or the device lock is unavailable.");
        } catch (...) { CloseHandle(handle_); throw; }
    }
    ~OperationLock() { ReleaseMutex(handle_); CloseHandle(handle_); }
    OperationLock(const OperationLock&) = delete;
    OperationLock& operator=(const OperationLock&) = delete;
private:
    HANDLE handle_{};
};

class Section {
public:
    Section(const std::wstring& name, size_t size) {
        SecurityDescriptor security;
        handle_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, security.Get(), PAGE_READWRITE, 0,
            static_cast<DWORD>(size), name.c_str());
        if (!handle_) Fail("CreateFileMappingW(" + Narrow(name) + ")");
        view_ = MapViewOfFile(handle_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
        if (!view_) {
            DWORD error = GetLastError(); CloseHandle(handle_);
            throw std::runtime_error("MapViewOfFile: " + Win32Message(error));
        }
        try { CheckObjectOwner(handle_); }
        catch (...) { UnmapViewOfFile(view_); CloseHandle(handle_); throw; }
    }

    ~Section() {
        if (view_) UnmapViewOfFile(view_);
        if (handle_) CloseHandle(handle_);
    }

    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;

    void* Data() const { return view_; }

private:
    HANDLE handle_ = nullptr;
    void* view_ = nullptr;
};

class Event {
public:
    explicit Event(const std::wstring& name) {
        SecurityDescriptor security;
        handle_ = CreateEventW(security.Get(), FALSE, FALSE, name.c_str());
        if (!handle_) Fail("CreateEventW(" + Narrow(name) + ")");
        try { CheckObjectOwner(handle_); } catch (...) { CloseHandle(handle_); throw; }
    }

    ~Event() {
        if (handle_) CloseHandle(handle_);
    }

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    HANDLE Get() const { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

void SetBinaryValue(HKEY key, const wchar_t* name, const void* data, DWORD size) {
    LSTATUS error = RegSetValueExW(key, name, 0, REG_BINARY, static_cast<const BYTE*>(data), size);
    if (error != ERROR_SUCCESS) throw std::runtime_error("Write " + Narrow(name) + ": " + Win32Message(error));
}

void SetDwordValue(HKEY key, const wchar_t* name, DWORD value) {
    LSTATUS error = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    if (error != ERROR_SUCCESS) throw std::runtime_error("RegSetValueExW(" + Narrow(name) + "): " + Win32Message(error));
}

void SetStringValue(HKEY key, const wchar_t* name, const std::wstring& value) {
    DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    LSTATUS error = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    if (error != ERROR_SUCCESS) throw std::runtime_error("Write " + Narrow(name) + ": " + Win32Message(error));
}

std::wstring ControllerKey(int index) {
    return L"SOFTWARE\\Xiaoji\\Controller" + std::to_wstring(index);
}

void WriteCurrentLayout(HKEY key, int index) {
    const auto& layout = kCurrentLayouts[index];
    SetBinaryValue(key, L"ReportDescriptor", layout.descriptor, layout.size);
    SetDwordValue(key, L"InputReportByteLength", layout.inputBytes);
}

void WriteControllerConfig(int index, const std::wstring& productString) {
    HKEY key = nullptr;
    const std::wstring path = ControllerKey(index);
    DWORD disposition = 0;
    LSTATUS error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition);
    if (error != ERROR_SUCCESS) throw std::runtime_error("Open " + Narrow(path) + ": " + Win32Message(error));

    try {
        const wchar_t* instance = index == 0 ? kKeyboardInstance : kMouseInstance;
        if (disposition == REG_OPENED_EXISTING_KEY) {
            wchar_t owner[128]{};
            DWORD bytes = sizeof(owner);
            if (RegGetValueW(key, nullptr, L"OwnerInstance", RRF_RT_REG_SZ, nullptr, owner, &bytes) != ERROR_SUCCESS ||
                _wcsicmp(owner, instance) != 0) throw std::runtime_error("Controller configuration is not owned by this instance.");
        }
        SetStringValue(key, L"OwnerInstance", instance);
        WriteCurrentLayout(key, index);
        SetDwordValue(key, L"VendorId", 0x1209);
        SetDwordValue(key, L"ProductId", index + 1);
        SetDwordValue(key, L"VersionNumber", 0x0100);
        SetDwordValue(key, L"FunctionMode", 0);
        SetStringValue(key, L"ProductString", productString);
    } catch (...) {
        RegCloseKey(key);
        throw;
    }
    RegCloseKey(key);
}


void WriteDeviceControllerIndex(const std::wstring& instanceId, int controllerIndex) {
    const std::wstring parameters = L"SYSTEM\\CurrentControlSet\\Enum\\" + instanceId + L"\\Device Parameters";
    HKEY key = nullptr;
    auto rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, parameters.c_str(), 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) throw std::runtime_error("Open device parameters: " + Win32Message(rc));
    try {
        SetDwordValue(key, L"ControllerIndex", static_cast<DWORD>(controllerIndex));
    } catch (...) {
        RegCloseKey(key);
        throw;
    }
    RegCloseKey(key);
}

void CreateRootDevice(const std::wstring& instanceId, const std::wstring& description, int controllerIndex) {
    HDEVINFO devices = SetupDiCreateDeviceInfoList(&kHidClassGuid, nullptr);
    if (devices == INVALID_HANDLE_VALUE)
        Fail("SetupDiCreateDeviceInfoList");

    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    if (!SetupDiCreateDeviceInfoW(
            devices, instanceId.c_str(), &kHidClassGuid, description.c_str(),
            nullptr, 0, &info)) {
        DWORD error = GetLastError();
        SetupDiDestroyDeviceInfoList(devices);
        SetLastError(error);
        Fail("SetupDiCreateDeviceInfoW(" + Narrow(instanceId) + ")");
    }

    const wchar_t hardwareIds[] = L"ROOT\\XIAOJI\0\0";
    if (!SetupDiSetDeviceRegistryPropertyW(
            devices, &info, SPDRP_HARDWAREID,
            reinterpret_cast<const BYTE*>(hardwareIds), sizeof(hardwareIds))) {
        DWORD error = GetLastError();
        SetupDiDestroyDeviceInfoList(devices);
        SetLastError(error);
        Fail("SetupDiSetDeviceRegistryPropertyW");
    }

    // HIDMaestro reads this from the devnode's Device Parameters key while
    // EvtDeviceAdd runs. Write it before registration so a driver already
    // present in the store cannot start against the previous index.
    try { WriteDeviceControllerIndex(instanceId, controllerIndex); }
    catch (...) { SetupDiDestroyDeviceInfoList(devices); throw; }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devices, &info)) {
        DWORD error = GetLastError();
        SetupDiDestroyDeviceInfoList(devices);
        SetLastError(error);
        Fail("DIF_REGISTERDEVICE(" + Narrow(instanceId) + ")");
    }
    SetupDiDestroyDeviceInfoList(devices);
}

bool RemoveRootDevice(const std::wstring& instanceId) {
    HDEVINFO devices = SetupDiCreateDeviceInfoList(&kHidClassGuid, nullptr);
    if (devices == INVALID_HANDLE_VALUE) return false;

    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    bool removed = false;
    if (SetupDiOpenDeviceInfoW(devices, instanceId.c_str(), nullptr, 0, &info)) {
        removed = SetupDiCallClassInstaller(DIF_REMOVE, devices, &info) != FALSE;
        if (removed) {
            SP_DEVINSTALL_PARAMS_W params{};
            params.cbSize = sizeof(params);
            if (!SetupDiGetDeviceInstallParamsW(devices, &info, &params)) {
                auto error = GetLastError();
                SetupDiDestroyDeviceInfoList(devices);
                throw std::runtime_error("Check removal flags: " + Win32Message(error));
            }
            if ((params.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT)) != 0) {
                SetupDiDestroyDeviceInfoList(devices);
                throw std::runtime_error("Restart required to finish device removal. Retry uninstall afterwards.");
            }
        }
    }
    SetupDiDestroyDeviceInfoList(devices);

    if (removed) {
        // DIF_REMOVE can return before the devnode leaves the tree. A new
        // create with the same stable instance id waits for that teardown.
        for (int attempt = 0; attempt < 100; ++attempt) {
            DEVINST device = 0;
            CONFIGRET result = CM_Locate_DevNodeW(&device, const_cast<DEVINSTID_W>(instanceId.c_str()), 0);
            if (result == CR_NO_SUCH_DEVNODE) return true;
            if (result != CR_SUCCESS) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }
    return removed;
}

std::wstring StageDriverPackage(const std::wstring& infPath) {
    wchar_t published[MAX_PATH]{};
    auto source = std::filesystem::absolute(infPath);
    if (_wcsicmp(source.filename().c_str(), L"xiaoji.inf") != 0) throw std::runtime_error("Expected xiaoji.inf");
    if (!SetupCopyOEMInfW(source.c_str(), nullptr, SPOST_PATH, 0, published, MAX_PATH, nullptr, nullptr)) Fail("SetupCopyOEMInfW");
    return std::filesystem::path(CharLowerW(published)).filename().wstring();
}

struct KeyStroke {
    BYTE modifier;
    BYTE usage;
};

bool AsciiToKey(unsigned char character, KeyStroke& stroke) {
    stroke.modifier = 0;
    if (character >= 'a' && character <= 'z') {
        stroke.usage = static_cast<BYTE>(0x04 + character - 'a');
        return true;
    }
    if (character >= 'A' && character <= 'Z') {
        stroke.modifier = 0x02;
        stroke.usage = static_cast<BYTE>(0x04 + character - 'A');
        return true;
    }
    if (character >= '1' && character <= '9') {
        stroke.usage = static_cast<BYTE>(0x1E + character - '1');
        return true;
    }
    if (character == '0') {
        stroke.usage = 0x27;
        return true;
    }

    switch (character) {
    case '!': stroke = {0x02, 0x1E}; return true;
    case '@': stroke = {0x02, 0x1F}; return true;
    case '#': stroke = {0x02, 0x20}; return true;
    case '$': stroke = {0x02, 0x21}; return true;
    case '%': stroke = {0x02, 0x22}; return true;
    case '^': stroke = {0x02, 0x23}; return true;
    case '&': stroke = {0x02, 0x24}; return true;
    case '*': stroke = {0x02, 0x25}; return true;
    case '(': stroke = {0x02, 0x26}; return true;
    case ')': stroke = {0x02, 0x27}; return true;
    case ' ': stroke = {0x00, 0x2C}; return true;
    case '\n': stroke = {0x00, 0x28}; return true;
    case '\t': stroke = {0x00, 0x2B}; return true;
    case '-': stroke = {0x00, 0x2D}; return true;
    case '_': stroke = {0x02, 0x2D}; return true;
    case '=': stroke = {0x00, 0x2E}; return true;
    case '+': stroke = {0x02, 0x2E}; return true;
    case '[': stroke = {0x00, 0x2F}; return true;
    case '{': stroke = {0x02, 0x2F}; return true;
    case ']': stroke = {0x00, 0x30}; return true;
    case '}': stroke = {0x02, 0x30}; return true;
    case '\\': stroke = {0x00, 0x31}; return true;
    case '|': stroke = {0x02, 0x31}; return true;
    case ';': stroke = {0x00, 0x33}; return true;
    case ':': stroke = {0x02, 0x33}; return true;
    case '\'': stroke = {0x00, 0x34}; return true;
    case '"': stroke = {0x02, 0x34}; return true;
    case '`': stroke = {0x00, 0x35}; return true;
    case '~': stroke = {0x02, 0x35}; return true;
    case ',': stroke = {0x00, 0x36}; return true;
    case '<': stroke = {0x02, 0x36}; return true;
    case '.': stroke = {0x00, 0x37}; return true;
    case '>': stroke = {0x02, 0x37}; return true;
    case '/': stroke = {0x00, 0x38}; return true;
    case '?': stroke = {0x02, 0x38}; return true;
    default: return false;
    }
}

constexpr ULONG NextSequence(ULONG published, ULONG delivered) {
    ULONG last = published & ~1UL;
    if (last == 0 || (delivered != 0 && static_cast<LONG>(delivered - last) > 0)) last = delivered;
    ULONG next = last + 2;
    return next == 0 ? 2 : next;
}
static_assert(NextSequence(0, 0) == 2 && NextSequence(2, 2) == 4);
static_assert(NextSequence(0, 42) == 44 && NextSequence(45, 42) == 46);
static_assert(NextSequence(0xfffffffeUL, 0xfffffffcUL) == 2 && NextSequence(2, 0xfffffffeUL) == 4);

using Clock = std::chrono::steady_clock;

static DWORD PollInterval(Clock::time_point deadline, DWORD maximum) {
    auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<DWORD>(std::clamp<decltype(remaining)>(remaining, 0, maximum));
}

struct FrameReceipt {
    const char* failure{};
    DWORD error{};
};

class XiaojiDevice {
public:
    explicit XiaojiDevice(int index, const std::wstring& prefix = L"Global\\Xiaoji")
        : input_(prefix + L"Input" + std::to_wstring(index), sizeof(SharedInput)),
          ack_(prefix + L"InputAck" + std::to_wstring(index), sizeof(InputAck)),
          ackEvent_(prefix + L"InputAckEvent" + std::to_wstring(index)),
          inputEvent_(prefix + L"InputEvent" + std::to_wstring(index)),
          inputView_(static_cast<SharedInput*>(input_.Data())),
          ackView_(static_cast<InputAck*>(ack_.Data())) {}

    bool WaitForDriver(int timeoutMs, const std::function<bool(DWORD)>& stop = {}) const {
        return WaitForDriver(Clock::now() + std::chrono::milliseconds(timeoutMs), stop);
    }

    bool WaitForDriver(Clock::time_point deadline, const std::function<bool(DWORD)>& stop = {}) const {
        for (;;) {
            if ((ackView_->capabilities & kAckDelivered) != 0)
                return true;
            auto wait = PollInterval(deadline, 25);
            if (!wait) break;
            if (stop) { if (stop(wait)) return false; }
            else std::this_thread::sleep_for(std::chrono::milliseconds(wait));
        }
        throw std::runtime_error(
            "xiaoji driver did not open the input acknowledgement section; "
            "check device installation and signatures");
    }

    void WriteKeyboardReport(BYTE modifier, BYTE usage) {
        BYTE report[8]{};
        report[0] = modifier;
        report[2] = usage;
        WriteFrame(report, sizeof(report));
    }

    void TapKey(unsigned char character, int settleMs = 8) {
        KeyStroke stroke{};
        if (!AsciiToKey(character, stroke))
            throw std::runtime_error("unsupported ASCII character for the built-in US key map");
        TapStroke(stroke, settleMs);
    }

    void TapStroke(KeyStroke stroke, int settleMs = 8) {
        try { WriteKeyboardReport(stroke.modifier, stroke.usage); }
        catch (...) { WriteKeyboardReport(0, 0); throw; }
        std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
        WriteKeyboardReport(0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
    }

    void Type(const std::string& text, int settleMs = 8) {
        for (unsigned char character : text) TapKey(character, settleMs);
    }

    void MoveMouse(int dx, int dy, int wheel = 0) {
        while (dx != 0 || dy != 0 || wheel != 0) {
            const int stepX = std::clamp(dx, -127, 127);
            const int stepY = std::clamp(dy, -127, 127);
            const int stepWheel = std::clamp(wheel, -127, 127);
            BYTE report[4] = {
                0,
                static_cast<BYTE>(static_cast<int8_t>(stepX)),
                static_cast<BYTE>(static_cast<int8_t>(stepY)),
                static_cast<BYTE>(static_cast<int8_t>(stepWheel)),
            };
            WriteFrame(report, sizeof(report));
            dx -= stepX;
            dy -= stepY;
            wheel -= stepWheel;
        }
    }

    ULONG PublishFrame(const BYTE* data, ULONG size) {
        if (size > sizeof(SharedInput::data))
            throw std::runtime_error("report exceeds xiaoji shared input capacity");

        const ULONG sequence = NextSequence(inputView_->seqNo, static_cast<ULONG>(ackView_->deliveredSeqNo));
        inputView_->seqNo = sequence - 1;
        MemoryBarrier();
        inputView_->dataSize = size;
        std::memcpy(inputView_->data, data, size);
        inputView_->extendedReportSize = 0;
        MemoryBarrier();
        inputView_->seqNo = sequence;
        return sequence;
    }

    FrameReceipt SignalAndWait(ULONG sequence, DWORD ackMs) const noexcept {
        if (!SetEvent(inputEvent_.Get())) return {"SetEvent", GetLastError()};

        const auto deadline = Clock::now() + std::chrono::milliseconds(ackMs);
        while (static_cast<ULONG>(ackView_->deliveredSeqNo) != sequence) {
            auto wait = PollInterval(deadline, 10);
            if (!wait) return {"HID report acknowledgement timed out; delivery is uncertain."};
            if (WaitForSingleObject(ackEvent_.Get(), wait) == WAIT_FAILED) return {"Wait for HID acknowledgement", GetLastError()};
        }
        return {};
    }

private:
    void WriteFrame(const BYTE* data, ULONG size) {
        auto receipt = SignalAndWait(PublishFrame(data, size), 2000);
        if (receipt.error) throw std::runtime_error(std::string(receipt.failure) + ": " + Win32Message(receipt.error));
        if (receipt.failure) throw std::runtime_error(receipt.failure);
    }

    Section input_;
    Section ack_;
    Event ackEvent_;
    Event inputEvent_;
    SharedInput* inputView_;
    InputAck* ackView_;
};

LayoutVersion ReadStoredLayout(HKEY key, int index) {
    auto read = [&](const wchar_t* name, DWORD type, void* data, DWORD& bytes) {
        auto rc = RegGetValueW(key, nullptr, name, type, nullptr, data, &bytes);
        if (rc == ERROR_SUCCESS) return true;
        if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_MORE_DATA || rc == ERROR_UNSUPPORTED_TYPE ||
            rc == ERROR_DATATYPE_MISMATCH) return false;
        throw std::runtime_error("Read " + Narrow(name) + ": " + Win32Message(rc));
    };
    BYTE descriptor[std::max(kCurrentLayouts[0].size, kCurrentLayouts[1].size)]{};
    DWORD size = sizeof(descriptor), length = 0, bytes = sizeof(length);
    if (!read(L"ReportDescriptor", RRF_RT_REG_BINARY, descriptor, size) ||
        !read(L"InputReportByteLength", RRF_RT_REG_DWORD, &length, bytes) || bytes != sizeof(length))
        return LayoutVersion::unknown;
    return ClassifyConfig(index, {descriptor, size, length});
}

// Return false only for a positively identified slot predating OwnerInstance.
bool CheckSlotOwner(HKEY key, int index, bool allowLegacy) {
    wchar_t owner[128]{};
    DWORD size = sizeof(owner);
    auto rc = RegGetValueW(key, nullptr, L"OwnerInstance", RRF_RT_REG_SZ, nullptr, owner, &size);
    if (rc == ERROR_FILE_NOT_FOUND && allowLegacy) {
        auto matchesDword = [&](const wchar_t* name, DWORD expected) {
            DWORD value = 0, bytes = sizeof(value);
            return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS && value == expected;
        };
        if (ReadStoredLayout(key, index) == LayoutVersion::unknown || !matchesDword(L"VendorId", 0x1209) ||
            !matchesDword(L"ProductId", index + 1) || !matchesDword(L"FunctionMode", 0))
            throw IdentityMismatch("Legacy slot does not match a known xiaoji controller configuration.");
        return false;
    }
    if (rc == ERROR_ACCESS_DENIED) throw std::runtime_error("Cannot read controller owner: access denied");
    if (rc != ERROR_SUCCESS || _wcsicmp(owner, index == 0 ? kKeyboardInstance : kMouseInstance) != 0)
        throw IdentityMismatch("Controller slot has an unknown owner; not changing it.");
    return true;
}

using ControllerHandle = std::unique_ptr<HKEY__, decltype(&RegCloseKey)>;

bool OwnedConfig(int index, bool allowLegacy = false) {
    HKEY key{};
    auto rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ControllerKey(index).c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) return false;
    if (rc != ERROR_SUCCESS) throw std::runtime_error("Read controller configuration: " + Win32Message(rc));
    ControllerHandle handle(key, RegCloseKey);
    CheckSlotOwner(key, index, allowLegacy);
    return true;
}

bool UpgradeBoundConfig(int index) {
    HKEY key{};
    auto rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ControllerKey(index).c_str(), 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (rc != ERROR_SUCCESS) throw std::runtime_error("Open bound controller: " + Win32Message(rc));
    ControllerHandle handle(key, RegCloseKey);
    if (!CheckSlotOwner(key, index, true))
        SetStringValue(key, L"OwnerInstance", index == 0 ? kKeyboardInstance : kMouseInstance);
    if (!NeedsLayoutRewrite(ReadStoredLayout(key, index))) return false;
    WriteCurrentLayout(key, index);
    return true;
}

std::wstring BoundOem(int index, bool requireStarted, const std::wstring& allowedInfs, bool includeAbsent) {
    const std::wstring instance = index == 0 ? kKeyboardInstance : kMouseInstance;
    DEVINST node = 0;
    auto rc = CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(instance.c_str()), includeAbsent ? CM_LOCATE_DEVNODE_PHANTOM : CM_LOCATE_DEVNODE_NORMAL);
    if (rc == CR_NO_SUCH_DEVNODE) return {};
    if (rc != CR_SUCCESS) throw std::runtime_error("CM_Locate_DevNode failed: " + std::to_string(rc));
    wchar_t hardware[256]{};
    ULONG size = sizeof(hardware), type = 0;
    rc = CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_HARDWAREID, &type, hardware, &size, 0);
    if (rc != CR_SUCCESS && rc != CR_NO_SUCH_VALUE && rc != CR_BUFFER_SMALL) throw std::runtime_error("Cannot query hardware ID: " + std::to_string(rc));
    if (rc != CR_SUCCESS ||
        type != REG_MULTI_SZ || size < sizeof(kHardwareId) + sizeof(wchar_t) || hardware[ARRAYSIZE(kHardwareId)-1] != 0 ||
        _wcsicmp(hardware, kHardwareId) != 0 || !OwnedConfig(index, !allowedInfs.empty())) throw IdentityMismatch("Device identity mismatch: " + Narrow(instance));
    DWORD boundIndex = MAXDWORD, bytes = sizeof(boundIndex);
    std::wstring parameters = L"SYSTEM\\CurrentControlSet\\Enum\\" + instance + L"\\Device Parameters";
    auto read = RegGetValueW(HKEY_LOCAL_MACHINE, parameters.c_str(), L"ControllerIndex", RRF_RT_REG_DWORD, nullptr, &boundIndex, &bytes);
    if (read == ERROR_ACCESS_DENIED) throw std::runtime_error("Cannot read controller index: access denied");
    if (read != ERROR_SUCCESS || boundIndex != static_cast<DWORD>(index)) throw IdentityMismatch("Controller index mismatch.");
    const DEVPROPKEY driverInf = {{0xa8b865dd,0x2e3d,0x4094,{0xad,0x97,0xe5,0x93,0xa7,0x0c,0x75,0xd6}},5};
    wchar_t bound[MAX_PATH]{};
    size = sizeof(bound);
    DEVPROPTYPE propType = 0;
    rc = CM_Get_DevNode_PropertyW(node, &driverInf, &propType, reinterpret_cast<BYTE*>(bound), &size, 0);
    if (rc == CR_NO_SUCH_VALUE && !requireStarted && OwnedConfig(index)) return L"<unbound>";
    if (rc != CR_SUCCESS && rc != CR_NO_SUCH_VALUE && rc != CR_BUFFER_SMALL) throw std::runtime_error("Cannot query bound driver package: " + std::to_string(rc));
    if (rc != CR_SUCCESS || propType != DEVPROP_TYPE_STRING || size < 4 || size > sizeof(bound) || bound[size/2-1] != 0)
        throw IdentityMismatch("Cannot identify the bound driver package.");
    std::wstring oem = CharLowerW(bound);
    wchar_t storeInf[MAX_PATH]{};
    if (!SetupGetInfDriverStoreLocationW(oem.c_str(), nullptr, nullptr, storeInf, MAX_PATH, nullptr)) {
        auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND && error != ERROR_NOT_FOUND)
            throw std::runtime_error("Cannot resolve bound driver: " + Win32Message(error));
        throw IdentityMismatch("The bound driver package is missing.");
    }
    if (_wcsicmp(std::filesystem::path(storeInf).filename().c_str(), L"xiaoji.inf") != 0)
        throw IdentityMismatch("The fixed instance is bound to an unknown driver package.");
    if (!allowedInfs.empty() && allowedInfs.find(L"," + oem + L",") == std::wstring::npos)
        throw std::runtime_error("Bound OEM driver differs from the selected package: " + Narrow(oem));
    if (requireStarted) {
        ULONG flags = 0, problem = 0;
        rc = CM_Get_DevNode_Status(&flags, &problem, node, 0);
        if (rc != CR_SUCCESS) throw std::runtime_error("Cannot query devnode status: " + std::to_string(rc));
        if (!(flags & DN_STARTED) || problem != 0)
            throw NotStarted("Device not started: " + Narrow(instance) + ", problem=" + std::to_string(problem));
    }
    return oem;
}

void BindSelected(const std::wstring& instance, const std::wstring& oem) {
    wchar_t storeInf[MAX_PATH]{};
    if (!SetupGetInfDriverStoreLocationW(oem.c_str(), nullptr, nullptr, storeInf, MAX_PATH, nullptr)) Fail("Resolve selected OEM INF");
    HDEVINFO devices = SetupDiCreateDeviceInfoList(&kHidClassGuid, nullptr);
    if (devices == INVALID_HANDLE_VALUE) Fail("Open HID device list");
    SP_DEVINFO_DATA data{sizeof(SP_DEVINFO_DATA)};
    SP_DEVINSTALL_PARAMS_W params{sizeof(SP_DEVINSTALL_PARAMS_W)};
    SP_DRVINFO_DATA_W driver{sizeof(SP_DRVINFO_DATA_W)};
    BOOL reboot = FALSE;
    try {
        if (!SetupDiOpenDeviceInfoW(devices, instance.c_str(), nullptr, 0, &data) ||
            !SetupDiGetDeviceInstallParamsW(devices, &data, &params)) Fail("Open persistent xiaoji device");
        params.Flags |= DI_ENUMSINGLEINF;
        wcscpy_s(params.DriverPath, storeInf);
        if (!SetupDiSetDeviceInstallParamsW(devices, &data, &params) ||
            !SetupDiBuildDriverInfoList(devices, &data, SPDIT_COMPATDRIVER) ||
            !SetupDiEnumDriverInfoW(devices, &data, SPDIT_COMPATDRIVER, 0, &driver) ||
            !DiInstallDevice(nullptr, devices, &data, &driver, 0, &reboot)) Fail("Bind selected driver to fixed device");
        if (reboot) throw std::runtime_error("Restart required to finish binding the driver.");
    } catch (...) { SetupDiDestroyDeviceInfoList(devices); throw; }
    SetupDiDestroyDeviceInfoList(devices);
}

static std::wstring HidChildPath(int index) {
    auto check = [](CONFIGRET rc, const char* operation) {
        if (rc != CR_SUCCESS) throw std::runtime_error(std::string(operation) + " (CONFIGRET " + std::to_string(rc) + ")");
    };
    DEVINST root{}, child{};
    auto instance = index == 0 ? kKeyboardInstance : kMouseInstance;
    check(CM_Locate_DevNodeW(&root, const_cast<wchar_t*>(instance), CM_LOCATE_DEVNODE_NORMAL), "Locate fixed root");
    auto rc = CM_Get_Child(&child, root, 0);
    if (rc == CR_NO_SUCH_DEVNODE) throw std::runtime_error("No child device under fixed root");
    GUID hid{};
    HidD_GetHidGuid(&hid);
    std::wstring path;
    for (; rc != CR_NO_SUCH_DEVNODE; rc = CM_Get_Sibling(&child, child, 0)) {
        check(rc, "Enumerate child nodes");
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        check(CM_Get_Device_IDW(child, id, ARRAYSIZE(id), 0), "Read child instance ID");
        ULONG chars = 0;
        check(CM_Get_Device_Interface_List_SizeW(&chars, &hid, id, CM_GET_DEVICE_INTERFACE_LIST_PRESENT), "Size HID interface list");
        std::vector<wchar_t> paths(chars);
        check(CM_Get_Device_Interface_ListW(&hid, id, paths.data(), chars, CM_GET_DEVICE_INTERFACE_LIST_PRESENT), "Read HID interface list");
        for (auto entry = paths.data(); *entry; entry += wcslen(entry) + 1) {
            if (!path.empty()) throw std::runtime_error("Multiple active HID interfaces under fixed root");
            path = entry;
        }
    }
    if (path.empty()) throw std::runtime_error("No active HID interface on child nodes");
    return path;
}

RunningLayout QueryRunningLayout(int index) {
    try {
        auto path = HidChildPath(index);
        HANDLE raw = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (raw == INVALID_HANDLE_VALUE) Fail("Open HID interface for capabilities");
        std::unique_ptr<void, decltype(&CloseHandle)> file(raw, CloseHandle);
        PHIDP_PREPARSED_DATA data{};
        if (!HidD_GetPreparsedData(raw, &data)) Fail("HidD_GetPreparsedData");
        std::unique_ptr<_HIDP_PREPARSED_DATA, decltype(&HidD_FreePreparsedData)> preparsed(data, HidD_FreePreparsedData);
        HIDP_CAPS caps{};
        auto status = HidP_GetCaps(data, &caps);
        if (status != HIDP_STATUS_SUCCESS) throw std::runtime_error("HidP_GetCaps status " + std::to_string(status));
        if (caps.UsagePage != HID_USAGE_PAGE_GENERIC || caps.Usage != (index == 0 ? HID_USAGE_GENERIC_KEYBOARD : HID_USAGE_GENERIC_MOUSE))
            throw std::runtime_error("Unexpected HID top-level collection");
        // HIDP_CAPS includes the report ID placeholder, even for no-ID devices.
        if (index == 0) {
            if (caps.InputReportByteLength == kKeyboardBytes + 1) return {LayoutVersion::current, "nkro-28"};
            if (caps.InputReportByteLength == kLegacyKeyboardBytes + 1) return {LayoutVersion::legacy, "legacy-8"};
        } else if (caps.InputReportByteLength == kMouseBytes + 1) {
            USHORT count = caps.NumberInputButtonCaps;
            if (!count) throw std::runtime_error("No mouse input button capabilities");
            std::vector<HIDP_BUTTON_CAPS> buttons(count);
            status = HidP_GetButtonCaps(HidP_Input, buttons.data(), &count, data);
            if (status != HIDP_STATUS_SUCCESS) throw std::runtime_error("HidP_GetButtonCaps status " + std::to_string(status));
            const HIDP_BUTTON_CAPS* range = nullptr;
            for (USHORT pos = 0; pos < count; ++pos) {
                const auto& cap = buttons[pos];
                if (cap.UsagePage != HID_USAGE_PAGE_BUTTON) continue;
                if (range || !cap.IsRange || cap.Range.UsageMin != 1 || cap.ReportID != 0)
                    throw std::runtime_error("Unexpected mouse button capabilities");
                range = &cap;
            }
            if (!range) throw std::runtime_error("Mouse has no button-page range");
            auto maximum = range->Range.UsageMax;
            if (maximum == kMouseButtons) return {LayoutVersion::current, std::to_string(maximum)};
            if (maximum == kLegacyMouseButtons) return {LayoutVersion::legacy, std::to_string(maximum)};
            throw std::runtime_error("Unexpected mouse button UsageMax=" + std::to_string(maximum));
        }
        throw std::runtime_error("Unexpected HID InputReportByteLength=" + std::to_string(caps.InputReportByteLength));
    } catch (const std::exception& error) { return {LayoutVersion::unknown, error.what()}; }
}

static void RestartDevice(const std::wstring& instance) {
    HDEVINFO raw = SetupDiCreateDeviceInfoList(&kHidClassGuid, nullptr);
    if (raw == INVALID_HANDLE_VALUE) Fail("Create device restart list");
    std::unique_ptr<void, decltype(&SetupDiDestroyDeviceInfoList)> devices(raw, SetupDiDestroyDeviceInfoList);
    SP_DEVINFO_DATA info{sizeof(info)};
    if (!SetupDiOpenDeviceInfoW(raw, instance.c_str(), nullptr, 0, &info)) Fail("Open fixed device for restart");
    // Microsoft Learn: SP_PROPCHANGE_PARAMS / DICS_PROPCHANGE stops and restarts
    // the device. DIF_PROPERTYCHANGE targets this devnode, not its hardware ID.
    SP_PROPCHANGE_PARAMS change{{sizeof(SP_CLASSINSTALL_HEADER), DIF_PROPERTYCHANGE}, DICS_PROPCHANGE, DICS_FLAG_CONFIGSPECIFIC, 0};
    if (!SetupDiSetClassInstallParamsW(raw, &info, &change.ClassInstallHeader, sizeof(change)) ||
        !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, raw, &info)) Fail("Restart " + Narrow(instance));
    SP_DEVINSTALL_PARAMS_W params{sizeof(params)};
    if (!SetupDiGetDeviceInstallParamsW(raw, &info, &params)) Fail("Check device restart flags");
    if (params.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT))
        throw std::runtime_error("Computer restart required to finish layout upgrade: " + Narrow(instance));
}

static void ConfirmCurrentLayout(int index, const std::wstring& instance) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        auto layout = QueryRunningLayout(index);
        if (layout.version == LayoutVersion::current) return;
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("New layout not active on " + Narrow(instance) + ": " + layout.detail);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void EnsureDevices(const std::wstring& allowedInfs, const std::wstring& priorInfs) {
    auto target = allowedInfs.substr(1, allowedInfs.find(L',', 1) - 1);
    for (int index : {0, 1}) {
        std::wstring product = index == 0 ? L"xiaoji Keyboard" : L"xiaoji Mouse";
        const std::wstring instance = index == 0 ? kKeyboardInstance : kMouseInstance;
        auto current = BoundOem(index, false, allowedInfs + priorInfs);
        if (current.empty()) {
            WriteControllerConfig(index, product);
            CreateRootDevice(instance, product, index);
            BindSelected(instance, target);
        } else if (allowedInfs.find(L"," + current + L",") == std::wstring::npos) {
            BindSelected(instance, target);
        }
        if (BoundOem(index, true, allowedInfs).empty()) throw std::runtime_error("Installed device disappeared.");
        if (!current.empty()) {
            bool restart = UpgradeBoundConfig(index);
            if (!restart) {
                auto active = QueryRunningLayout(index).version;
                if (active == LayoutVersion::current) continue;
                // A prior install can have written the new config but failed to restart,
                // or the running device may be on an unknown layout.
                restart = active != LayoutVersion::current;
            }
            if (restart) RestartDevice(instance);
        }
        ConfirmCurrentLayout(index, instance);
        if (BoundOem(index, true, allowedInfs).empty()) throw std::runtime_error("Device disappeared during layout confirmation.");
    }
}

void RemoveXiaojiDevices(const std::wstring& allowedInfs) {
    bool present[2], owned[2];
    for (int index : {0, 1}) {
        present[index] = !BoundOem(index, false, allowedInfs, true).empty();
        owned[index] = present[index] || OwnedConfig(index);
    }
    for (int index : {0, 1}) {
        if (present[index] && !RemoveRootDevice(index == 0 ? kKeyboardInstance : kMouseInstance))
            throw std::runtime_error("Could not remove device; retry uninstall after closing consumers.");
        if (owned[index]) {
            auto rc = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, ControllerKey(index).c_str(), KEY_WOW64_64KEY, 0);
            if (rc != ERROR_SUCCESS) throw std::runtime_error("Remove controller configuration: " + Win32Message(rc));
        }
    }
}

void SendInstalled(bool keyboard, const std::wstring& allowedInfs, const std::string& text, int dx, int dy, int wheel,
                   const std::wstring& sourceInf = {}) {
    int index = keyboard ? 0 : 1;
    auto bound = BoundOem(index, true, allowedInfs);
    if (bound.empty()) throw std::runtime_error("xiaoji device is not installed. Install the driver first.");
    if (!sourceInf.empty()) {
        wchar_t store[MAX_PATH]{};
        if (!SetupGetInfDriverStoreLocationW(bound.c_str(), nullptr, nullptr, store, MAX_PATH, nullptr)) Fail("Resolve OEM package");
        auto source = std::filesystem::absolute(sourceInf);
        if (_wcsicmp(source.filename().c_str(), L"xiaoji.inf") || _wcsicmp(std::filesystem::path(store).filename().c_str(), L"xiaoji.inf"))
            throw std::runtime_error("Expected xiaoji.inf.");
        for (const auto* name : {L"xiaoji.inf", L"xiaoji.dll", L"xiaoji.cat"}) {
            std::ifstream left(source.parent_path()/name, std::ios::binary), right(std::filesystem::path(store).parent_path()/name, std::ios::binary);
            if (!left || !right || !std::equal(std::istreambuf_iterator<char>(left), {}, std::istreambuf_iterator<char>(right), std::istreambuf_iterator<char>()))
                throw std::runtime_error("Requested files do not match the installed package.");
        }
    }
    XiaojiDevice device(index);
    device.WaitForDriver(10000);
    if (keyboard) device.Type(text); else device.MoveMouse(dx, dy, wheel);
}

unsigned RepeatNumber(const char* text, unsigned min, unsigned max) {
    unsigned value = 0;
    const char* end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, value);
    if (result.ec != std::errc{} || result.ptr != end || value < min || value > max)
        throw std::invalid_argument("Invalid repeat parameter.");
    return value;
}

bool RepeatKeys(XiaojiDevice& device, BYTE usage, unsigned count, DWORD gapMs,
                const std::function<bool(DWORD)>& stop, const std::function<void(unsigned)>& progress) {
    if (!device.WaitForDriver(10000, stop)) return false;
    for (unsigned completed = 0; completed < count; ++completed) {
        if (stop(completed == 0 ? 0 : gapMs)) return false;
        bool stopping;
        try {
            device.WriteKeyboardReport(0, usage);
            stopping = stop(8);
        } catch (...) {
            device.WriteKeyboardReport(0, 0);
            throw;
        }
        device.WriteKeyboardReport(0, 0);
        progress(completed + 1);
        if (stopping && completed + 1 < count) return false;
    }
    return true;
}

void RepeatInstalled(const std::wstring& allowedInfs, char** args) {
    const unsigned usage = RepeatNumber(args[0], 0x04, 0x2c);
    if (usage > 0x27 && usage != 0x2c) throw std::invalid_argument("Unsupported repeat key.");
    const unsigned count = RepeatNumber(args[1], 1, 1000), gap = RepeatNumber(args[2], 10, 5000);
    const std::string token = args[3];
    if (token.size() != 32 || token.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("Invalid repeat event ID.");
    DWORD parentId = RepeatNumber(args[4], 1, MAXDWORD);
    using OwnedHandle = std::unique_ptr<void, decltype(&CloseHandle)>;
    auto name = L"Local\\XiaojiRepeatStop-" + std::wstring(token.begin(), token.end());
    OwnedHandle cancel(OpenEventW(SYNCHRONIZE, FALSE, name.c_str()), CloseHandle);
    if (!cancel) Fail("Open repeat stop event");
    OwnedHandle parent(OpenProcess(SYNCHRONIZE, FALSE, parentId), CloseHandle);
    if (!parent) Fail("Open repeat owner");
    HANDLE handles[] = {cancel.get(), parent.get()};
    auto stop = [&](DWORD ms) {
        DWORD result = WaitForMultipleObjects(2, handles, FALSE, ms);
        if (result == WAIT_FAILED) Fail("Wait for repeat stop");
        return result != WAIT_TIMEOUT;
    };
    unsigned confirmed = 0;
    bool finished = false;
    if (!stop(0)) {
        if (BoundOem(0, true, allowedInfs).empty()) throw std::runtime_error("Install the xiaoji keyboard first.");
        XiaojiDevice device(0);
        auto lastUpdate = std::chrono::steady_clock::now();
        try {
            finished = RepeatKeys(device, static_cast<BYTE>(usage), count, gap, stop, [&](unsigned completed) {
                confirmed = completed;
                auto now = std::chrono::steady_clock::now();
                if (now - lastUpdate >= std::chrono::milliseconds(250)) {
                    std::cout << "PROGRESS " << confirmed << std::endl;
                    lastUpdate = now;
                }
            });
        } catch (...) { std::cout << "PROGRESS " << confirmed << std::endl; throw; }
    }
    std::cout << (finished ? "FINISHED " : "STOPPED ") << confirmed << std::endl;
}

struct DescriptorFacts {
    unsigned inputBits{}, outputBits{}, leds{}, buttons{}, bitmapBits{};
};

static DescriptorFacts InspectDescriptor(const ReportLayout& layout) {
    DescriptorFacts facts{};
    unsigned size = 0, count = 0, page = 0, first = UINT_MAX, last = UINT_MAX;
    for (DWORD pos = 0; pos < layout.size;) {
        BYTE tag = layout.descriptor[pos++];
        unsigned width = tag & 3;
        if (width > 1 || pos + width > layout.size) throw std::runtime_error("Unexpected HID item in built-in descriptor");
        unsigned value = width ? layout.descriptor[pos++] : 0;
        switch (tag) {
        case 0x05: page = value; break;
        case 0x19: first = value; break;
        case 0x29: last = value; break;
        case 0x75: size = value; break;
        case 0x95: count = value; break;
        case 0x85: throw std::runtime_error("Built-in descriptor declares a report ID");
        case 0x81:
            if (value == 2 && size == 1 && first == 1 && last == count && page == HID_USAGE_PAGE_BUTTON && facts.inputBits == 0)
                facts.buttons = count;
            if (value == 2 && size == 1 && first == 0 && last + 1 == count && page == HID_USAGE_PAGE_KEYBOARD && facts.inputBits == 64)
                facts.bitmapBits = count;
            facts.inputBits += size * count;
            break;
        case 0x91:
            if (value == 2 && size == 1 && first == 1 && last == count && page == HID_USAGE_PAGE_LED && facts.outputBits == 0)
                facts.leds = count;
            facts.outputBits += size * count;
            break;
        }
        if ((tag & 0x0c) == 0) first = last = UINT_MAX;
    }
    return facts;
}

static void TestReportLayouts() {
    auto keyboard = InspectDescriptor(kCurrentLayouts[0]), mouse = InspectDescriptor(kCurrentLayouts[1]);
    if (keyboard.inputBits != 28 * 8 || keyboard.bitmapBits != 160 || keyboard.outputBits != 8 || keyboard.leds != 5)
        throw std::runtime_error("Unexpected hybrid keyboard input or LED output");
    if (mouse.inputBits != 4 * 8 || mouse.buttons != 5) throw std::runtime_error("Unexpected five-button mouse layout");
    for (int index : {0, 1}) {
        auto check = [&](ReportLayout config, LayoutVersion expected, bool rewrite) {
            auto version = ClassifyConfig(index, config);
            if (version != expected || NeedsLayoutRewrite(version) != rewrite)
                throw std::runtime_error("Stored layout recognition or migration decision failed");
        };
        for (auto version : {LayoutVersion::legacy, LayoutVersion::current}) {
            auto config = (version == LayoutVersion::legacy ? kLegacyLayouts : kCurrentLayouts)[index];
            config.inputBytes = index == 1 ? 4 : version == LayoutVersion::legacy ? 8 : 28;
            check(config, version, version == LayoutVersion::legacy);
            auto wrongLength = config;
            wrongLength.inputBytes = index == 1 ? 5 : version == LayoutVersion::legacy ? 28 : 8;
            check(wrongLength, LayoutVersion::unknown, true);
            std::vector<BYTE> modified(config.descriptor, config.descriptor + config.size);
            modified.back() ^= 1;
            config.descriptor = modified.data();
            check(config, LayoutVersion::unknown, true);
        }
    }
}

void SelfTest() {
    if (sizeof(SharedInput) != 362 || sizeof(InputAck) != 688)
        throw std::runtime_error("IPC layout mismatch");
    TestReportLayouts();

    const std::string sample = "xiaoji A1!\n";
    for (unsigned char character : sample) {
        KeyStroke stroke{};
        if (!AsciiToKey(character, stroke))
            throw std::runtime_error("key map rejected an expected character");
    }

    BYTE keyboard[8]{};
    keyboard[0] = 0x02;
    keyboard[2] = 0x04;
    if (keyboard[0] != 0x02 || keyboard[2] != 0x04)
        throw std::runtime_error("keyboard report packing failed");
    std::cout << "xiaoji host self-test: OK\n";
}

void ReconnectTest() {
    const std::wstring prefix = L"Local\\XiaojiReconnectTest-" + std::to_wstring(GetCurrentProcessId()) + L"-";
    Section published(prefix + L"Input0", sizeof(SharedInput)), ack(prefix + L"InputAck0", sizeof(InputAck));
    Event inputEvent(prefix + L"InputEvent0"), ackEvent(prefix + L"InputAckEvent0");
    auto frame = static_cast<SharedInput*>(published.Data());
    auto receipt = static_cast<InputAck*>(ack.Data());
    receipt->capabilities = kAckDelivered;
    frame->seqNo = 40;
    receipt->deliveredSeqNo = 40;
    auto reader = std::async(std::launch::async, [&] {
        for (ULONG num = 1; num <= 4; ++num) {
            if (WaitForSingleObject(inputEvent.Get(), 3000) != WAIT_OBJECT_0 || frame->seqNo != 40 + 2*num ||
                frame->dataSize != 8 || frame->data[2] != (num % 2 ? 0x1b : 0))
                throw std::runtime_error("IPC reopen lost the report, key release, or sequence.");
            InterlockedExchange(&receipt->deliveredSeqNo, frame->seqNo);
            if (!SetEvent(ackEvent.Get())) Fail("Acknowledge private IPC frame");
        }
    });
    for (int session = 0; session < 2; ++session) {
        XiaojiDevice writer(0, prefix);
        writer.WaitForDriver(500);
        writer.TapKey('x');
    }
    reader.get();
    std::cout << "Private IPC: two writer sessions, four acknowledged reports; no PnP device or desktop input.\n";
}

void RepeatTest() {
    const std::wstring prefix = L"Local\\XiaojiRepeatTest-" + std::to_wstring(GetCurrentProcessId()) + L"-";
    Section published(prefix + L"Input0", sizeof(SharedInput)), ack(prefix + L"InputAck0", sizeof(InputAck));
    Event inputEvent(prefix + L"InputEvent0"), ackEvent(prefix + L"InputAckEvent0");
    std::unique_ptr<void, decltype(&CloseHandle)> cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle);
    if (!cancel) Fail("Create private repeat stop event");
    auto frame = static_cast<SharedInput*>(published.Data());
    auto receipt = static_cast<InputAck*>(ack.Data());
    receipt->capabilities = kAckDelivered;
    XiaojiDevice writer(0, prefix);
    enum class When { never, beforeStart, duringPress, betweenTaps };
    for (auto when : {When::never, When::beforeStart, When::duringPress, When::betweenTaps}) {
        if (!ResetEvent(cancel.get())) Fail("Reset private stop event");
        if (when == When::beforeStart && !SetEvent(cancel.get())) Fail("Cancel before start");
        const unsigned expected = when == When::never ? 3 : when == When::beforeStart ? 0 : 1;
        ULONG start = frame->seqNo;
        auto reader = std::async(std::launch::async, [&] {
            for (unsigned num = 1; num <= 2 * expected; ++num) {
                if (WaitForSingleObject(inputEvent.Get(), 3000) != WAIT_OBJECT_0 || frame->dataSize != 8 ||
                    frame->seqNo != start + num * 2 || frame->data[0] != 0 || frame->data[2] != (num % 2 ? 0x1b : 0))
                    throw std::runtime_error("Unexpected repeat press/release sequence.");
                if (when == When::duringPress && num == 1 && !SetEvent(cancel.get())) Fail("Cancel during press");
                InterlockedExchange(&receipt->deliveredSeqNo, frame->seqNo);
                if (!SetEvent(ackEvent.Get())) Fail("Acknowledge repeat test frame");
            }
        });
        unsigned confirmed = 0;
        bool finished = RepeatKeys(writer, 0x1b, 3, 20, [&](DWORD ms) {
            if (when == When::betweenTaps && ms == 20 && !SetEvent(cancel.get())) Fail("Cancel between taps");
            auto result = WaitForSingleObject(cancel.get(), ms);
            if (result == WAIT_FAILED) Fail("Wait for private stop event");
            return result == WAIT_OBJECT_0;
        }, [&](unsigned done) {
            if (done != ++confirmed || frame->data[2] != 0 || static_cast<ULONG>(receipt->deliveredSeqNo) != frame->seqNo)
                throw std::runtime_error("Count advanced before confirmed release.");
        });
        reader.get();
        if (finished != (when == When::never) || confirmed != expected || frame->seqNo != start + 4 * expected)
            throw std::runtime_error("Repeat cancellation or count mismatch.");
    }
    std::cout << "Private IPC repeat: completion, pre-cancel, held-key release and gap cancellation OK; no desktop input.\n";
}

std::wstring Widen(const std::string& value) {
    if (value.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), needed);
    return out;
}

void Usage() {
    std::cout
        << "xiaoji-host.exe self-test\n"
        << "xiaoji-host.exe install [--check]   (embedded local-test package)\n"
        << "xiaoji-host.exe install <xiaoji.inf>\n"
        << "xiaoji-host.exe type <xiaoji.inf> <ascii-text>\n"
        << "xiaoji-host.exe mouse <xiaoji.inf> <dx> <dy> [wheel]\n"
        << "xiaoji-host.exe remove\n";
}

void WithOperationLock(const std::function<void()>& operation) {
    OperationLock lock;
    operation();
}

std::uint32_t StartedDevices() {
    std::uint32_t mask = 0;
    for (int index : {0, 1}) {
        if (!BoundOem(index, true, {}).empty()) mask |= 1u << index;
    }
    return mask;
}

struct InputEnvironment {
    const wchar_t* prefix;
    std::function<bool()> admin;
    std::function<bool(int)> bound;
    std::function<RunningLayout(int)> layout;

    std::wstring LockName() const { return std::wstring(prefix) + L"HostOperation"; }
};

static const InputEnvironment installedEnvironment{L"Global\\Xiaoji", [] { return IsUserAnAdmin() != FALSE; },
    [](int index) { return !BoundOem(index, true, {}).empty(); }, QueryRunningLayout};
#ifdef XIAOJI_SESSION_TEST
static thread_local const InputEnvironment* inputEnvironment = &installedEnvironment;
#else
static constexpr const InputEnvironment* inputEnvironment = &installedEnvironment;
#endif
static std::atomic<bool> sessionOpen{false};
static thread_local InputSession* ownedSession{};

void RequireNoSession() {
    if (sessionOpen.load()) throw std::runtime_error("An input session is already open in this process.");
}

struct KeyboardState {
    BYTE modifiers{};
    std::array<BYTE, kBitmapUsages / 8> bitmap{};
};

static std::array<BYTE, kKeyboardBytes> KeyboardReport(const KeyboardState& state) {
    std::array<BYTE, kKeyboardBytes> report{};
    report[0] = state.modifiers;
    std::copy(state.bitmap.begin(), state.bitmap.end(), report.begin() + kLegacyKeyboardBytes);
    return report;
}

static std::array<BYTE, kMouseBytes> MouseReport(BYTE buttons, int detents) {
    return {buttons, 0, 0, static_cast<BYTE>(detents)};
}

static BYTE SetBit(BYTE value, unsigned bit, bool down) {
    unsigned mask = 1u << bit;
    return static_cast<BYTE>(down ? value | mask : value & ~mask);
}

class InputSession {
public:
    InputSession(const InputEnvironment& env, uint32_t devices, uint32_t readyMs, uint32_t ackMs)
        : lock_(env.LockName().c_str()), ackMs_(ackMs) {
        for (int index : {0, 1}) {
            if (!(devices & (1u << index))) continue;
            if (!env.bound(index)) throw std::runtime_error("Requested xiaoji device is not installed.");
            auto layout = env.layout(index);
            if (layout.version == LayoutVersion::legacy)
                throw std::runtime_error("Legacy HID layout; run install with the new xiaoji.exe first.");
            if (layout.version != LayoutVersion::current)
                throw std::runtime_error("Cannot determine running HID layout: " + layout.detail);
            devices_[index] = std::make_unique<XiaojiDevice>(index, env.prefix);
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds(readyMs);
        for (const auto& device : devices_) if (device) device->WaitForDriver(deadline);
    }

    void Key(BYTE usage, bool down) {
        auto& device = Device(0);
        auto next = keyboard_;
        if (usage >= 0xe0 && usage <= 0xe7) next.modifiers = SetBit(next.modifiers, usage - 0xe0, down);
        else if (usage >= 4 && usage < kBitmapUsages) next.bitmap[usage / 8] = SetBit(next.bitmap[usage / 8], usage % 8, down);
        else throw std::invalid_argument("Expected keyboard usage 0x04..0x9F or 0xE0..0xE7.");
        Publish(device, KeyboardReport(next), keyboard_, next);
    }

    void Button(uint32_t button, bool down) {
        auto& device = Device(1);
        if (!button || button > kMouseButtons) throw std::invalid_argument("Expected mouse button 1..5.");
        auto next = SetBit(buttons_, button - 1, down);
        Publish(device, MouseReport(next, 0), buttons_, next);
    }

    void Wheel(int detents) {
        auto& device = Device(1);
        if (!detents || detents < -127 || detents > 127) throw std::invalid_argument("Expected nonzero wheel detents in -127..127.");
        Publish(device, MouseReport(buttons_, detents), buttons_, buttons_);
    }

private:
    XiaojiDevice& Device(int index) {
        if (!devices_[index]) throw std::invalid_argument("Requested device is not open in this session.");
        return *devices_[index];
    }

    template<size_t Size, typename State>
    void Publish(XiaojiDevice& device, const std::array<BYTE, Size>& report, State& published, const State& next) {
        auto sequence = device.PublishFrame(report.data(), static_cast<ULONG>(Size));
        published = next;
        auto receipt = device.SignalAndWait(sequence, ackMs_);
        if (receipt.error) throw Unconfirmed(std::string(receipt.failure) + ": " + Win32Message(receipt.error));
        if (receipt.failure) throw Unconfirmed(receipt.failure);
    }

    OperationLock lock_;
    DWORD ackMs_;
    std::unique_ptr<XiaojiDevice> devices_[2];
    KeyboardState keyboard_{};
    BYTE buttons_{};
};

InputSession* OpenSession(uint32_t devices, uint32_t readyMs, uint32_t ackMs) {
    const auto& env = *inputEnvironment;
    if (!env.admin()) throw std::runtime_error("Input requires an elevated process.");
    RequireNoSession();
    ownedSession = new InputSession(env, devices, readyMs, ackMs);
    sessionOpen.store(true);
    return ownedSession;
}

static InputSession& OwnedSession(InputSession* session) {
    if (!session) throw std::invalid_argument("Missing input session.");
    if (session != ownedSession) throw std::invalid_argument("Use the open session on its opening thread.");
    return *session;
}

void SessionKey(InputSession* session, uint8_t usage, bool down) { OwnedSession(session).Key(usage, down); }
void SessionButton(InputSession* session, uint32_t button, bool down) { OwnedSession(session).Button(button, down); }
void SessionWheel(InputSession* session, int32_t detents) { OwnedSession(session).Wheel(detents); }

void CloseSession(InputSession* session) {
    OwnedSession(session);
    ownedSession = nullptr;
    // Clear under the operation lock, never after another opener can acquire it.
    sessionOpen.store(false);
    delete session;
}

template <typename Operation>
static void WithInstalledDevice(int index, Operation operation) {
    const auto& env = *inputEnvironment;
    if (!env.admin()) throw std::runtime_error("Input requires an elevated process.");
    OperationLock lock(env.LockName().c_str());
    if (!env.bound(index)) throw std::runtime_error("Requested xiaoji device is not installed.");
    XiaojiDevice device(index, env.prefix);
    device.WaitForDriver(10000);
    operation(device);
}

void CheckInstalled(bool keyboard) {
    WithInstalledDevice(keyboard ? 0 : 1, [](XiaojiDevice&) {});
}

void TapInstalled(std::uint8_t modifiers, std::uint8_t usage) {
    WithInstalledDevice(0, [&](XiaojiDevice& device) { device.TapStroke({modifiers, usage}); });
}

void TypeInstalled(const std::string& text) {
    KeyStroke stroke{};
    for (unsigned char character : text) {
        if (!AsciiToKey(character, stroke)) throw std::invalid_argument("Unsupported byte for the US keyboard mapping.");
    }
    WithInstalledDevice(0, [&](XiaojiDevice& device) { device.Type(text); });
}

void MoveInstalled(int dx, int dy, int wheel) {
    WithInstalledDevice(1, [&](XiaojiDevice& device) { device.MoveMouse(dx, dy, wheel); });
}

#ifdef XIAOJI_SESSION_TEST
static void ExpectResult(int actual, int expected, const char* detail = "") {
    const std::string message = XiaojiLastError();
    if (actual != expected || (expected == 0 ? !message.empty() : message.empty()) || message.find(detail) == std::string::npos)
        throw std::runtime_error("Expected " + std::to_string(expected) + " (" + detail + "), got " + std::to_string(actual) + ": " + message);
}

class SessionPeer {
public:
    SessionPeer(const std::wstring& prefix, int index)
        : input_(prefix + L"Input" + std::to_wstring(index), sizeof(SharedInput)),
          ack_(prefix + L"InputAck" + std::to_wstring(index), sizeof(InputAck)),
          inputEvent_(prefix + L"InputEvent" + std::to_wstring(index)),
          ackEvent_(prefix + L"InputAckEvent" + std::to_wstring(index)) { Ready(true); }

    void Ready(bool ready) { Receipt()->capabilities = ready ? kAckDelivered : 0; }
    ULONG Sequence() const { return Frame()->seqNo; }

    template<size_t Size, typename Send>
    void Expect(const std::array<BYTE, Size>& report, Send send, bool acknowledge = true) {
        const ULONG sequence = Sequence() + 2;
        auto reader = std::async(std::launch::async, [&] {
            if (WaitForSingleObject(inputEvent_.Get(), 3000) != WAIT_OBJECT_0 || Sequence() != sequence ||
                Frame()->dataSize != Size || Frame()->extendedReportSize != 0 || std::memcmp(Frame()->data, report.data(), Size) != 0)
                throw std::runtime_error("Incorrect private report or sequence: " + std::to_string(sequence));
            if (acknowledge) {
                InterlockedExchange(&Receipt()->deliveredSeqNo, sequence);
                if (!SetEvent(ackEvent_.Get())) Fail("Acknowledge private session report");
            }
        });
        int result = send();
        reader.get();
        ExpectResult(result, acknowledge ? XIAOJI_OK : XIAOJI_UNCONFIRMED);
    }

private:
    SharedInput* Frame() const { return static_cast<SharedInput*>(input_.Data()); }
    InputAck* Receipt() const { return static_cast<InputAck*>(ack_.Data()); }
    Section input_, ack_;
    Event inputEvent_, ackEvent_;
};

static void SessionTest() {
    const std::wstring prefix = L"Local\\XiaojiSessionTest-" + std::to_wstring(GetCurrentProcessId()) + L"-";
    SessionPeer keyboard(prefix, 0), mouse(prefix, 1);
    bool admin = true, bound = true;
    auto layout = LayoutVersion::current;
    InputEnvironment env{prefix.c_str(), [&] { return admin; }, [&](int index) { return index == 0 || bound; },
        [&](int index) { return RunningLayout{index == 0 ? LayoutVersion::current : layout, "test layout"}; }};
    struct RestoreEnvironment {
        const InputEnvironment* previous = inputEnvironment;
        ~RestoreEnvironment() { inputEnvironment = previous; }
    } restore;
    inputEnvironment = &env;

    XiaojiSession* session{};
    auto open = [&](uint32_t devices, int expected, const char* detail = "") {
        ExpectResult(XiaojiSessionOpen(devices, 100, 400, &session), expected, detail);
        if ((session != nullptr) != (expected == 0)) throw std::runtime_error("Incorrect session output.");
    };
    auto unchanged = [&](ULONG keys, ULONG buttons) {
        if (keyboard.Sequence() != keys || mouse.Sequence() != buttons) throw std::runtime_error("Unexpected session publication.");
    };
    admin = false; open(3, 2, "elevated"); admin = true;
    bound = false; open(3, 2, "not installed"); bound = true;
    layout = LayoutVersion::legacy; open(3, 2, "install");
    layout = LayoutVersion::unknown; open(3, 2, "test layout"); layout = LayoutVersion::current;
    mouse.Ready(false); open(3, 2, "acknowledgement"); mouse.Ready(true);
    std::async(std::launch::async, [&] { OperationLock recovered(env.LockName().c_str()); }).get();
    open(3, 0);
    XiaojiSession* second = session;
    ExpectResult(XiaojiSessionOpen(1, 100, 400, &second), 2, "already open");
    if (second) throw std::runtime_error("Duplicate open returned a handle.");
    ExpectResult(XiaojiTapKey(0, 4), 2, "session");
    ExpectResult(XiaojiTypeAscii("x", 1), 2, "session");
    ExpectResult(XiaojiMoveMouse(0, 0, 1), 2, "session");
    ExpectResult(XiaojiCheckConnection(1), 2, "session");
    for (BYTE usage : std::array<BYTE, 5>{0, 3, 0xa0, 0xdf, 0xe8}) ExpectResult(XiaojiSessionKey(session, usage, 1), 1);
    for (auto button : {0u, 6u}) ExpectResult(XiaojiSessionButton(session, button, 1), 1);
    for (auto detents : {0, -128, 128}) ExpectResult(XiaojiSessionWheel(session, detents), 1);
    unchanged(0, 0);

    auto key = [&](BYTE usage, int down, BYTE mods, BYTE first, BYTE last, bool ack = true) {
        std::array<BYTE, 28> report{};
        report[0] = mods; report[8] = first; report[27] = last;
        keyboard.Expect(report, [&] { return XiaojiSessionKey(session, usage, down); }, ack);
    };
    key(0xe0, 1, 1, 0, 0);
    key(4, -1, 1, 0x10, 0);
    std::async(std::launch::async, [&] {
        ExpectResult(XiaojiSessionKey(session, 4, 0), 1, "thread");
        ExpectResult(XiaojiSessionButton(session, 3, 1), 1, "thread");
        ExpectResult(XiaojiSessionWheel(session, 1), 1, "thread");
        ExpectResult(XiaojiSessionClose(session), 1, "thread");
        ExpectResult(XiaojiCheckConnection(0), 2, "session");
        try { OperationLock contender(env.LockName().c_str()); }
        catch (const std::runtime_error& error) {
            if (std::string(error.what()).find("Another host operation") != std::string::npos) return;
            throw;
        }
        throw std::runtime_error("Session did not retain its operation lock.");
    }).get();
    unchanged(4, 0);
    key(0x9f, 1, 1, 0x10, 0x80, false);
    key(5, 1, 1, 0x30, 0x80);
    key(0xe7, 1, 0x81, 0x30, 0x80);
    key(4, 1, 0x81, 0x30, 0x80);
    key(4, 0, 0x81, 0x20, 0x80);
    key(4, 0, 0x81, 0x20, 0x80);
    key(0xe0, 0, 0x80, 0x20, 0x80);
    key(0xe7, 0, 0, 0x20, 0x80);
    key(5, 0, 0, 0, 0x80);
    key(0x9f, 0, 0, 0, 0);

    for (unsigned button = 1; button <= 5; ++button) {
        std::array<BYTE, 4> report{static_cast<BYTE>((1u << button) - 1), 0, 0, 0};
        mouse.Expect(report, [&] { return XiaojiSessionButton(session, button, 1); }, button != 5);
    }
    for (int detents : {-127, 127}) {
        std::array<BYTE, 4> report{31, 0, 0, static_cast<BYTE>(detents)};
        mouse.Expect(report, [&] { return XiaojiSessionWheel(session, detents); });
    }
    mouse.Expect(std::array<BYTE, 4>{29, 0, 0, 0}, [&] { return XiaojiSessionButton(session, 2, 0); });
    auto keys = keyboard.Sequence(), buttons = mouse.Sequence();
    ExpectResult(XiaojiSessionClose(session), 0);
    open(1, 0);
    ExpectResult(XiaojiSessionButton(session, 1, 1), 1);
    ExpectResult(XiaojiSessionWheel(session, 1), 1);
    ExpectResult(XiaojiSessionClose(session), 0);
    open(2, 0);
    ExpectResult(XiaojiSessionKey(session, 4, 1), 1);
    unchanged(keys, buttons);
    mouse.Expect(std::array<BYTE, 4>{0, 0, 0, 1}, [&] { return XiaojiSessionWheel(session, 1); });
    ExpectResult(XiaojiSessionClose(session), 0);
    unchanged(keys, buttons + 2);
    std::async(std::launch::async, [&] {
        inputEnvironment = &env;
        open(3, 0);
        ExpectResult(XiaojiSessionClose(session), 0);
    }).get();
    unchanged(keys, buttons + 2);
    std::cout << "Private IPC session: full reports, publication, timeout, ownership and reopen OK; no desktop input.\n";
}
#endif
}

int RunXiaojiHost(int argc, char** argv, int (*installEmbeddedPackage)(bool)) {
    using namespace xiaoji;
    try {
        if (argc < 2) {
            Usage();
            return 1;
        }
        const std::string command = argv[1];
        if (command == "self-test") {
            SelfTest();
            return 0;
        }
        if (command == "reconnect-test" && argc == 2) { ReconnectTest(); return 0; }
        if (command == "repeat-test" && argc == 2) { RepeatTest(); return 0; }
#ifdef XIAOJI_SESSION_TEST
        if (command == "session-test" && argc == 2) { SessionTest(); return 0; }
        if (command == "signing-test" && argc == 2) {
            std::wstring path(32768, L'\0');
            auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (!length || length >= path.size()) Fail("Locate signing-test package");
            path.resize(length);
            SigningTest(std::filesystem::path(path).parent_path());
            return 0;
        }
#endif
        if (command == "install" && (argc == 2 || (argc == 3 && std::string(argv[2]) == "--check")))
            return installEmbeddedPackage(argc == 3);
        OperationLock operation;
        if (command == "repeat-key" && argc == 8) {
            RepeatInstalled(L"," + Widen(argv[2]) + L",", argv + 3);
            return 0;
        }
        if (command == "inspect-devices" && argc == 3) {
            std::wstring allowed = L"," + Widen(argv[2]) + L",";
            for (int index : {0, 1}) {
                auto bound = BoundOem(index, true, allowed);
                std::cout << (index == 0 ? "keyboard: " : "mouse: ") << (bound.empty() ? "not installed" : Narrow(bound)) << "\n";
            }
            return 0;
        }
        if (command == "ensure-devices" && argc == 4) {
            EnsureDevices(L"," + Widen(argv[2]) + L",", L"," + Widen(argv[3]) + L",");
            return 0;
        }
        if (argc == 3 && (command == "remove-devices" || command == "send-key" || command == "send-mouse")) {
            std::wstring allowed = L"," + Widen(argv[2]) + L",";
            if (command == "remove-devices") RemoveXiaojiDevices(allowed);
            else SendInstalled(command == "send-key", allowed, "x", 12, 0, 0);
            return 0;
        }
        if (command == "install" && argc == 3) {
            EnsureDevices(L"," + StageDriverPackage(Widen(argv[2])) + L",", {});
            std::cout << "driver package staged and devices started\n";
            return 0;
        }
        if (command == "type" && argc == 4) {
            SendInstalled(true, {}, argv[3], 0, 0, 0, Widen(argv[2]));
            std::cout << "typed " << std::strlen(argv[3]) << " bytes\n";
            return 0;
        }
        if (command == "mouse" && (argc == 5 || argc == 6)) {
            SendInstalled(false, {}, {}, std::stoi(argv[3]), std::stoi(argv[4]), argc == 6 ? std::stoi(argv[5]) : 0, Widen(argv[2]));
            std::cout << "mouse report acknowledged\n";
            return 0;
        }
        if (command == "remove") {
            RemoveXiaojiDevices({});
            return 0;
        }
        Usage();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "xiaoji host: " << error.what() << "\n";
        return 2;
    }
}
