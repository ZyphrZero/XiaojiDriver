#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

int InstallEmbeddedPackage(bool check) {
    const bool elevated = IsUserAnAdmin() != FALSE;
    if (!check && !elevated) throw std::runtime_error("Local signing requires an elevated administrator terminal.");
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(elevated ? FOLDERID_ProgramFiles : FOLDERID_LocalAppData,
            0, nullptr, &folder))) throw std::runtime_error("Cannot locate setup working directory.");
    fs::path parent(folder);
    CoTaskMemFree(folder);
    GUID guid{};
    wchar_t id[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, id, ARRAYSIZE(id)))
        throw std::runtime_error("Cannot allocate setup directory name.");
    auto dir = parent / (std::wstring(L"xiaoji-bootstrap-") + id);
    auto exe = dir / L"xiaoji-setup.exe";

    auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA);
    auto size = resource ? SizeofResource(nullptr, resource) : 0;
    auto data = size ? LockResource(LoadResource(nullptr, resource)) : nullptr;
    if (!data) throw std::runtime_error("Embedded xiaoji setup payload is missing.");
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (elevated && !ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr))
        throw std::system_error(GetLastError(), std::system_category(), "setup directory security");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const BOOL created = CreateDirectoryW(dir.c_str(), elevated ? &security : nullptr);
    const DWORD createError = GetLastError();
    if (descriptor) LocalFree(descriptor);
    if (!created) throw std::system_error(createError, std::system_category(), "CreateDirectoryW");
    struct Cleanup {
        const fs::path &exe, &dir;
        ~Cleanup() { DeleteFileW(exe.c_str()); RemoveDirectoryW(dir.c_str()); }
    } cleanup{exe, dir};
    std::ofstream stream;
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.open(exe, std::ios::binary);
    stream.write(static_cast<const char*>(data), size);
    stream.close();

    std::wstring args = L"\"" + exe.wstring() + (check ? L"\" --check" : L"\" install");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), args.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup, &process))
        throw std::system_error(GetLastError(), std::system_category(), "CreateProcessW(xiaoji-setup)");
    CloseHandle(process.hThread);
    DWORD code = 1;
    const bool ok = WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (!ok) throw std::runtime_error("Cannot determine the result of local setup.");
    return static_cast<int>(code);
}
