#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <shlobj.h>
#include "host_entry.h"
#include "installed_input.h"
#include "pnp_management.h"

#include <algorithm>
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

const BYTE kKeyboardDescriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07,
    0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

const BYTE kMouseDescriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01,
    0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0xC0, 0xC0,
};

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
    OperationLock() {
        SecurityDescriptor security;
        handle_ = CreateMutexW(security.Get(), FALSE, L"Global\\XiaojiHostOperation");
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

void WriteControllerConfig(
    int index,
    const BYTE* reportDescriptor,
    DWORD reportDescriptorSize,
    DWORD inputReportByteLength,
    WORD vendorId,
    WORD productId,
    const std::wstring& productString) {
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
        SetBinaryValue(key, L"ReportDescriptor", reportDescriptor, reportDescriptorSize);
        SetDwordValue(key, L"VendorId", vendorId);
        SetDwordValue(key, L"ProductId", productId);
        SetDwordValue(key, L"VersionNumber", 0x0100);
        SetDwordValue(key, L"InputReportByteLength", inputReportByteLength);
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if ((ackView_->capabilities & kAckDelivered) != 0)
                return true;
            if (stop) { if (stop(25)) return false; }
            else std::this_thread::sleep_for(std::chrono::milliseconds(25));
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

private:
    void WriteFrame(const BYTE* data, ULONG size) {
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
        if (!SetEvent(inputEvent_.Get()))
            Fail("SetEvent");

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (static_cast<ULONG>(ackView_->deliveredSeqNo) != sequence) {
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("HID report acknowledgement timed out; delivery is uncertain.");
            if (WaitForSingleObject(ackEvent_.Get(), 10) == WAIT_FAILED) Fail("Wait for HID acknowledgement");
        }
    }

    Section input_;
    Section ack_;
    Event ackEvent_;
    Event inputEvent_;
    SharedInput* inputView_;
    InputAck* ackView_;
};

bool OwnedConfig(int index, bool allowLegacy = false) {
    wchar_t owner[128]{};
    DWORD size = sizeof(owner);
    auto path = ControllerKey(index);
    HKEY key{};
    auto rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) return false;
    if (rc != ERROR_SUCCESS) throw std::runtime_error("Read controller configuration: " + Win32Message(rc));
    rc = RegGetValueW(key, nullptr, L"OwnerInstance", RRF_RT_REG_SZ, nullptr, owner, &size);
    if (rc == ERROR_FILE_NOT_FOUND && allowLegacy) {
        BYTE descriptor[sizeof(kKeyboardDescriptor) + sizeof(kMouseDescriptor)]{};
        size = sizeof(descriptor);
        const auto expectedSize = index == 0 ? sizeof(kKeyboardDescriptor) : sizeof(kMouseDescriptor);
        bool match = RegGetValueW(key, nullptr, L"ReportDescriptor", RRF_RT_REG_BINARY, nullptr, descriptor, &size) == ERROR_SUCCESS &&
            size == expectedSize && std::memcmp(descriptor, index == 0 ? kKeyboardDescriptor : kMouseDescriptor, size) == 0;
        auto matchesDword = [&](const wchar_t* name, DWORD expected) {
            DWORD value = 0, bytes = sizeof(value);
            return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS && value == expected;
        };
        match = match && matchesDword(L"VendorId", 0x1209) && matchesDword(L"ProductId", index + 1) &&
            matchesDword(L"InputReportByteLength", index == 0 ? 8 : 4) && matchesDword(L"FunctionMode", 0);
        RegCloseKey(key);
        if (!match) throw std::runtime_error("Legacy slot does not match the original xiaoji controller configuration.");
        return true;
    }
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || _wcsicmp(owner, index == 0 ? kKeyboardInstance : kMouseInstance) != 0)
        throw std::runtime_error("Controller slot has an unknown owner; not changing it.");
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
    if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_HARDWAREID, &type, hardware, &size, 0) != CR_SUCCESS ||
        type != REG_MULTI_SZ || size < sizeof(kHardwareId) + sizeof(wchar_t) || hardware[ARRAYSIZE(kHardwareId)-1] != 0 ||
        _wcsicmp(hardware, kHardwareId) != 0 || !OwnedConfig(index, !allowedInfs.empty())) throw std::runtime_error("Device identity mismatch: " + Narrow(instance));
    DWORD boundIndex = MAXDWORD, bytes = sizeof(boundIndex);
    std::wstring parameters = L"SYSTEM\\CurrentControlSet\\Enum\\" + instance + L"\\Device Parameters";
    if (RegGetValueW(HKEY_LOCAL_MACHINE, parameters.c_str(), L"ControllerIndex", RRF_RT_REG_DWORD, nullptr, &boundIndex, &bytes) != ERROR_SUCCESS ||
        boundIndex != static_cast<DWORD>(index)) throw std::runtime_error("Controller index mismatch.");
    const DEVPROPKEY driverInf = {{0xa8b865dd,0x2e3d,0x4094,{0xad,0x97,0xe5,0x93,0xa7,0x0c,0x75,0xd6}},5};
    wchar_t bound[MAX_PATH]{};
    size = sizeof(bound);
    DEVPROPTYPE propType = 0;
    rc = CM_Get_DevNode_PropertyW(node, &driverInf, &propType, reinterpret_cast<BYTE*>(bound), &size, 0);
    if (rc == CR_NO_SUCH_VALUE && !requireStarted && OwnedConfig(index)) return L"<unbound>";
    if (rc != CR_SUCCESS || propType != DEVPROP_TYPE_STRING || size < 4 || size > sizeof(bound) || bound[size/2-1] != 0)
        throw std::runtime_error("Cannot identify the bound driver package.");
    std::wstring oem = CharLowerW(bound);
    wchar_t storeInf[MAX_PATH]{};
    if (!SetupGetInfDriverStoreLocationW(oem.c_str(), nullptr, nullptr, storeInf, MAX_PATH, nullptr) ||
        _wcsicmp(std::filesystem::path(storeInf).filename().c_str(), L"xiaoji.inf") != 0)
        throw std::runtime_error("The fixed instance is bound to an unknown driver package.");
    if (!allowedInfs.empty() && allowedInfs.find(L"," + oem + L",") == std::wstring::npos)
        throw std::runtime_error("Bound OEM driver differs from the selected package: " + Narrow(oem));
    if (requireStarted) {
        ULONG flags = 0, problem = 0;
        if (CM_Get_DevNode_Status(&flags, &problem, node, 0) != CR_SUCCESS || !(flags & DN_STARTED) || problem != 0)
            throw std::runtime_error("Device not started: " + Narrow(instance) + ", problem=" + std::to_string(problem));
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

void EnsureDevices(const std::wstring& allowedInfs, const std::wstring& priorInfs) {
    auto target = allowedInfs.substr(1, allowedInfs.find(L',', 1) - 1);
    for (int index : {0, 1}) {
        std::wstring product = index == 0 ? L"xiaoji Keyboard" : L"xiaoji Mouse";
        const std::wstring instance = index == 0 ? kKeyboardInstance : kMouseInstance;
        auto current = BoundOem(index, false, allowedInfs + priorInfs);
        if (current.empty()) {
            if (index == 0) WriteControllerConfig(index, kKeyboardDescriptor, sizeof(kKeyboardDescriptor), 8, 0x1209, 1, product);
            else WriteControllerConfig(index, kMouseDescriptor, sizeof(kMouseDescriptor), 4, 0x1209, 2, product);
            CreateRootDevice(instance, product, index);
            BindSelected(instance, target);
        } else if (allowedInfs.find(L"," + current + L",") == std::wstring::npos) {
            BindSelected(instance, target);
        }
        if (BoundOem(index, true, allowedInfs).empty()) throw std::runtime_error("Installed device disappeared.");
        HKEY key{};
        auto rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ControllerKey(index).c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
        if (rc != ERROR_SUCCESS) throw std::runtime_error("Open controller: " + Win32Message(rc));
        try { SetStringValue(key, L"OwnerInstance", instance); } catch (...) { RegCloseKey(key); throw; }
        RegCloseKey(key);
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

void SelfTest() {
    if (sizeof(SharedInput) != 362 || sizeof(InputAck) != 688)
        throw std::runtime_error("IPC layout mismatch");
    if (sizeof(kKeyboardDescriptor) == 0 || sizeof(kMouseDescriptor) == 0)
        throw std::runtime_error("descriptor missing");

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

template <typename Operation>
static void WithInstalledDevice(int index, Operation operation) {
    if (!IsUserAnAdmin()) throw std::runtime_error("Input requires an elevated process.");
    OperationLock lock;
    if (BoundOem(index, true, {}).empty()) throw std::runtime_error("Requested xiaoji device is not installed.");
    XiaojiDevice device(index);
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
