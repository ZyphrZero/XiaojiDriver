#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <shlobj.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <system_error>
#include "pnp_management.h"

namespace fs = std::filesystem;

namespace {
void Fail(const char* operation, DWORD code = GetLastError()) {
    throw std::system_error(static_cast<int>(code), std::system_category(), operation);
}

fs::path StoreInf(const std::wstring& published) {
    wchar_t path[MAX_PATH]{};
    if (!SetupGetInfDriverStoreLocationW(published.c_str(), nullptr, nullptr, path, MAX_PATH, nullptr)) Fail("Resolve published INF");
    return path;
}

void CheckIdentity(const fs::path& inf) {
    if (_wcsicmp(inf.filename().c_str(), L"xiaoji.inf")) throw std::runtime_error("Expected xiaoji.inf");
    HINF raw = SetupOpenInfFileW(inf.c_str(), L"HIDClass", INF_STYLE_WIN4, nullptr);
    if (raw == INVALID_HANDLE_VALUE) Fail("Open xiaoji HIDClass INF");
    std::unique_ptr<void, decltype(&SetupCloseInfFile)> handle(raw, SetupCloseInfFile);
    auto requireField = [&](const wchar_t* section, const wchar_t* key, DWORD field, const wchar_t* expected) {
        INFCONTEXT context{};
        wchar_t value[MAX_PATH]{};
        if (!SetupFindFirstLineW(raw, section, key, &context) ||
            !SetupGetStringFieldW(&context, field, value, MAX_PATH, nullptr) || _wcsicmp(value, expected))
            throw std::runtime_error("Unexpected INF identity; refusing to manage this package.");
    };
    requireField(L"Version", L"Provider", 1, L"xiaoji");
    requireField(L"Version", L"CatalogFile", 1, L"xiaoji.cat");
    requireField(L"Standard.NTamd64", nullptr, 1, L"xiaoji_Install");
    requireField(L"Standard.NTamd64", nullptr, 2, L"ROOT\\XIAOJI");
    requireField(L"xiaoji_Install.NT.Wdf", L"UmdfService", 1, L"xiaoji");
}

void VerifyPackage(const fs::path& inf) {
    CheckIdentity(inf);
    const auto cat = inf.parent_path() / L"xiaoji.cat";
    HCATADMIN rawAdmin{};
    if (!CryptCATAdminAcquireContext2(&rawAdmin, nullptr, L"SHA256", nullptr, 0)) Fail("Open SHA256 catalog context");
    const auto release = [](HCATADMIN admin) { CryptCATAdminReleaseContext(admin, 0); };
    std::unique_ptr<void, decltype(release)> admin(rawAdmin, release);
    for (auto name : {L"xiaoji.inf", L"xiaoji.dll"}) {
        const auto path = inf.parent_path() / name;
        HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (raw == INVALID_HANDLE_VALUE) Fail("Open catalog member");
        std::unique_ptr<void, decltype(&CloseHandle)> file(raw, CloseHandle);
        std::array<BYTE, 32> hash{};
        DWORD size = static_cast<DWORD>(hash.size());
        if (!CryptCATAdminCalcHashFromFileHandle2(rawAdmin, raw, &size, hash.data(), 0)) Fail("Hash catalog member");
        if (size != hash.size()) throw std::runtime_error("Unexpected SHA256 hash size");
        std::wstring tag;
        constexpr wchar_t digits[] = L"0123456789ABCDEF";
        for (auto byte : hash) { tag += digits[byte >> 4]; tag += digits[byte & 15]; }
        WINTRUST_CATALOG_INFO member{};
        member.cbStruct = sizeof(member);
        member.pcwszCatalogFilePath = cat.c_str();
        member.pcwszMemberFilePath = path.c_str();
        member.pcwszMemberTag = tag.c_str();
        member.hMemberFile = raw;
        member.hCatAdmin = rawAdmin;
        WINTRUST_DATA trust{};
        trust.cbStruct = sizeof(trust);
        trust.dwUIChoice = WTD_UI_NONE;
        trust.dwUnionChoice = WTD_CHOICE_CATALOG;
        trust.pCatalog = &member;
        trust.dwStateAction = WTD_STATEACTION_VERIFY;
        GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        HWND noWindow = static_cast<HWND>(INVALID_HANDLE_VALUE);
        LONG result = WinVerifyTrust(noWindow, &policy, &trust);
        trust.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(noWindow, &policy, &trust);
        if (result != ERROR_SUCCESS) Fail("Package catalog is untrusted or does not cover the supplied INF/DLL", result);
    }
}

using Packages = std::map<std::wstring, fs::path>;

Packages BoundPackages() {
    Packages packages;
    for (int index : {0, 1}) {
        auto name = xiaoji::BoundOem(index, false, {}, true);
        if (name.empty() || name == L"<unbound>" || packages.count(name)) continue;
        auto path = StoreInf(name);
        CheckIdentity(path);
        packages.emplace(name, path);
    }
    return packages;
}

std::wstring Allowed(const Packages& packages) {
    std::wstring names = L",";
    for (const auto& entry : packages) names += entry.first + L",";
    return names;
}

void Manage(const std::function<void()>& operation) {
    if (!IsUserAnAdmin()) throw std::runtime_error("Run this command from an elevated administrator terminal.");
    PWSTR path = nullptr;
    HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &path);
    if (FAILED(result)) Fail("Locate Program Files", result);
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(path, CoTaskMemFree);
    auto lockPath = fs::path(path) / L"xiaoji-device-lifecycle.lock";
    HANDLE raw = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) Fail("Acquire xiaoji lifecycle lock");
    std::unique_ptr<void, decltype(&CloseHandle)> lock(raw, CloseHandle);
    xiaoji::WithOperationLock(operation);
}

void Install(const fs::path& inf) {
    VerifyPackage(inf);
    auto prior = BoundPackages();
    auto name = xiaoji::StageDriverPackage(inf.wstring());
    auto staged = StoreInf(name);
    VerifyPackage(staged);
    for (auto file : {L"xiaoji.inf", L"xiaoji.dll", L"xiaoji.cat"}) {
        std::ifstream source(inf.parent_path()/file, std::ios::binary), actual(staged.parent_path()/file, std::ios::binary);
        if (!source || !actual || !std::equal(std::istreambuf_iterator<char>(source), {}, std::istreambuf_iterator<char>(actual), std::istreambuf_iterator<char>()))
            throw std::runtime_error("Windows staged a different build; devices were not rebound. Check DriverVer.");
    }
    xiaoji::EnsureDevices(L"," + name + L",", Allowed(prior));
    std::wcout << L"Installed: " << name << L"; keyboard and mouse started.\n";
}

void Uninstall(bool keepPackage, bool dryRun) {
    auto packages = BoundPackages();
    for (const auto& entry : packages) std::wcout << L"Bound package: " << entry.first << L" -> " << entry.second.native() << L"\n";
    if (dryRun) {
        std::wcout << L"Plan: remove the fixed xiaoji keyboard/mouse and their owned configuration, if present.\n"
                   << L"Only the bound packages listed above are candidates for non-forced deletion.\nDry run: no changes.\n";
        return;
    }
    xiaoji::RemoveXiaojiDevices(Allowed(packages));
    if (!keepPackage) {
        for (const auto& entry : packages) {
            if (StoreInf(entry.first) != entry.second) throw std::runtime_error("Published package changed during uninstall.");
            if (!SetupUninstallOEMInfW(entry.first.c_str(), 0, nullptr)) {
                DWORD error = GetLastError();
                std::wcerr << L"Device removal finished, but the package was not deleted: " << entry.first << L"\n";
                Fail("Remove bound package (not forced)", error);
            }
            std::wcout << L"Removed package: " << entry.first << L"\n";
        }
    }
    std::wcout << L"Fixed devices and their configuration removed; "
               << (keepPackage ? L"packages kept." : L"no unbound or historical packages targeted.") << L"\n";
}

int Status(bool verify) {
    int ready = 0;
    bool failed = false;
    Packages checked;
    for (int index : {0, 1}) {
        const char* label = index == 0 ? "keyboard" : "mouse";
        try {
            auto name = xiaoji::BoundOem(index, true, {});
            const char* field = index == 0 ? ", layout: " : ", buttons: ";
            if (name.empty()) { std::cout << label << ": not present\n"; continue; }
            if (verify && !checked.count(name)) {
                auto path = StoreInf(name);
                VerifyPackage(path);
                checked.emplace(name, path);
            }
            auto layout = xiaoji::QueryRunningLayout(index);
            std::cout << label << ": started, " << fs::path(name).string() << (verify ? " (catalog verified)" : "") << field;
            if (layout.version == xiaoji::LayoutVersion::unknown) {
                std::cout << "unknown (" << layout.detail << ")\n";
                failed = true;
            } else {
                std::cout << layout.detail << (layout.version == xiaoji::LayoutVersion::legacy ? " (run install to upgrade)" : "") << '\n';
            }
            ++ready;
        } catch (const std::exception& error) { std::cerr << label << ": " << error.what() << '\n'; failed = true; }
    }
    return failed ? 2 : ready == 2 ? 0 : 3;
}

void Help() {
    std::cout << "xiaoji status                         Show the two fixed devices\n"
                 "xiaoji verify [path\\xiaoji.inf]       Verify installed devices or an external signed package\n"
                 "xiaoji install path\\xiaoji.inf        Install a trusted signed package (administrator)\n"
                 "xiaoji uninstall [--keep-package]      Remove owned devices and their bound packages (administrator)\n"
                 "xiaoji uninstall --dry-run             Inspect bound package targets without changes\n"
                 "Exit: 0 success, 1 usage, 2 error, 3 missing device(s). No automatic signing or elevation.\n";
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        SetupSetNonInteractiveMode(TRUE);
        const std::wstring command = argc > 1 ? argv[1] : L"--help";
        if (argc <= 2 && (command == L"--help" || command == L"help" || command == L"-h")) { Help(); return 0; }
        if (argc == 2 && (command == L"status" || command == L"verify")) return Status(command == L"verify");
        if (argc == 3 && command == L"verify") { VerifyPackage(fs::absolute(argv[2])); std::cout << "Package verified.\n"; return 0; }
        if (argc == 3 && command == L"install") { Manage([&] { Install(fs::absolute(argv[2])); }); return 0; }
        if (command == L"uninstall") {
            if (argc == 3 && std::wstring(argv[2]) == L"--dry-run") { Uninstall(false, true); return 0; }
            if (argc == 2 || (argc == 3 && std::wstring(argv[2]) == L"--keep-package")) {
                Manage([&] { Uninstall(argc == 3, false); }); return 0;
            }
        }
        Help(); return 1;
    } catch (const std::exception& error) { std::cerr << "xiaoji: " << error.what() << '\n'; return 2; }
}
