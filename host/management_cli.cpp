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
#include <fcntl.h>
#include <io.h>
#include "machine_protocol.h"
#include "local_signing.h"
#include "pnp_management.h"

namespace fs = std::filesystem;

namespace {
void Fail(const char* operation, DWORD code = GetLastError()) {
    throw std::system_error(static_cast<int>(code), std::system_category(), operation);
}

fs::path StoreInf(const std::wstring& published, bool allowMissing = false) {
    wchar_t path[MAX_PATH]{};
    if (!SetupGetInfDriverStoreLocationW(published.c_str(), nullptr, nullptr, path, MAX_PATH, nullptr)) {
        auto error = GetLastError();
        if (allowMissing && xiaoji::IsMissingPublishedInf(error)) return {};
        Fail("Resolve published INF", error);
    }
    return path;
}

void CheckIdentity(const fs::path& inf) {
    if (_wcsicmp(inf.filename().c_str(), L"xiaoji.inf")) throw xiaoji::IdentityMismatch("Expected xiaoji.inf");
    HINF raw = SetupOpenInfFileW(inf.c_str(), L"HIDClass", INF_STYLE_WIN4, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_CLASS_MISMATCH) throw xiaoji::IdentityMismatch("Bound INF is not HIDClass");
        Fail("Open xiaoji HIDClass INF");
    }
    std::unique_ptr<void, decltype(&SetupCloseInfFile)> handle(raw, SetupCloseInfFile);
    auto requireField = [&](const wchar_t* section, const wchar_t* key, DWORD field, const wchar_t* expected) {
        INFCONTEXT context{};
        wchar_t value[MAX_PATH]{};
        if (!SetupFindFirstLineW(raw, section, key, &context) ||
            !SetupGetStringFieldW(&context, field, value, MAX_PATH, nullptr) || _wcsicmp(value, expected))
            throw xiaoji::IdentityMismatch("Unexpected INF identity; refusing to manage this package.");
    };
    requireField(L"Version", L"Provider", 1, L"xiaoji");
    requireField(L"Version", L"CatalogFile", 1, L"xiaoji.cat");
    requireField(L"Standard.NTamd64", nullptr, 1, L"xiaoji_Install");
    requireField(L"Standard.NTamd64", nullptr, 2, L"ROOT\\XIAOJI");
    requireField(L"xiaoji_Install.NT.Wdf", L"UmdfService", 1, L"xiaoji");
}

void VerifyPackage(const fs::path& inf, bool offline = false) {
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
        if (offline) trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
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

void Manage(const std::function<void()>& operation, bool signingLock = false) {
    if (!IsUserAnAdmin()) throw std::runtime_error("Run this command from an elevated administrator terminal.");
    PWSTR path = nullptr;
    HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &path);
    if (FAILED(result)) Fail("Locate Program Files", result);
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(path, CoTaskMemFree);
    auto acquire = [&](const wchar_t* name) {
        HANDLE raw = CreateFileW((fs::path(path)/name).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (raw == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_SHARING_VIOLATION) throw std::runtime_error("Driver management lock busy; stop active driver sessions first.");
            Fail("Acquire xiaoji management lock");
        }
        return std::unique_ptr<void, decltype(&CloseHandle)>(raw, CloseHandle);
    };
    auto lock = acquire(L"xiaoji-device-lifecycle.lock");
    auto signing = signingLock ? acquire(L"xiaoji-umdf2-install.lock") : decltype(lock)(nullptr, CloseHandle);
    xiaoji::WithOperationLock(operation);
}

void Install(const fs::path& inf, const xiaoji::Progress& progress = {}) {
    VerifyPackage(inf, static_cast<bool>(progress));
    auto prior = BoundPackages();
    auto name = xiaoji::StageDriverPackage(inf.wstring());
    auto staged = StoreInf(name);
    VerifyPackage(staged, static_cast<bool>(progress));
    bool exact = true;
    for (auto file : {L"xiaoji.inf", L"xiaoji.dll", L"xiaoji.cat"}) {
        std::ifstream source(inf.parent_path()/file, std::ios::binary), actual(staged.parent_path()/file, std::ios::binary);
        if (!source || !actual || !std::equal(std::istreambuf_iterator<char>(source), {}, std::istreambuf_iterator<char>(actual), std::istreambuf_iterator<char>())) {
            exact = false;
            break;
        }
    }
    if (!exact && !(progress && xiaoji::SameSignedBuild(inf.parent_path(), staged.parent_path())))
        throw std::runtime_error("Windows staged a different build; devices were not rebound. Check DriverVer.");
    if (progress) progress("devices", "Configure the fixed keyboard and mouse");
    xiaoji::EnsureDevices(L"," + name + L",", Allowed(prior));
    if (!progress) std::wcout << L"Installed: " << name << L"; keyboard and mouse started.\n";
}

void Uninstall(bool keepPackage, bool dryRun, const xiaoji::Progress& progress = {}) {
    auto packages = BoundPackages();
    if (!progress) for (const auto& entry : packages) std::wcout << L"Bound package: " << entry.first << L" -> " << entry.second.native() << L"\n";
    if (dryRun) {
        std::wcout << L"Plan: remove the fixed xiaoji keyboard/mouse and their owned configuration, if present.\n"
                   << L"Only the bound packages listed above are candidates for non-forced deletion.\nDry run: no changes.\n";
        return;
    }
    if (progress) progress("devices", "Remove the fixed devices and owned configuration");
    xiaoji::RemoveXiaojiDevices(Allowed(packages));
    if (!keepPackage) {
        if (progress) progress("stage", "Remove bound packages without force");
        for (const auto& entry : packages) {
            if (StoreInf(entry.first) != entry.second) throw std::runtime_error("Published package changed during uninstall.");
            if (!SetupUninstallOEMInfW(entry.first.c_str(), 0, nullptr)) {
                DWORD error = GetLastError();
                if (progress) throw std::runtime_error("Devices removed; package " + entry.second.filename().u8string() +
                    " (" + fs::path(entry.first).u8string() + ") remains; local trust retained. Windows error " + std::to_string(error));
                std::wcerr << L"Device removal finished, but the package was not deleted: " << entry.first << L"\n";
                Fail("Remove bound package (not forced)", error);
            }
            if (!progress) std::wcout << L"Removed package: " << entry.first << L"\n";
        }
    }
    if (!progress) std::wcout << L"Fixed devices and their configuration removed; "
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

struct Observation { xiaoji::DeviceState state; std::string detail; };
Observation Inspect(int index) {
    using State = xiaoji::DeviceState;
    try {
        auto name = xiaoji::BoundOem(index, false, {}, true);
        if (name.empty()) return {State::absent, "absent"};
        if (name == L"<unbound>") return {State::not_started, "no bound package"};
        CheckIdentity(StoreInf(name));
        auto started = xiaoji::BoundOem(index, true, {});
        if (started.empty()) return {State::not_started, "registered but not present"};
        if (started != name) throw std::runtime_error("Binding changed during query");
        auto layout = xiaoji::QueryRunningLayout(index);
        return {layout.version == xiaoji::LayoutVersion::current ? State::current :
                layout.version == xiaoji::LayoutVersion::legacy ? State::legacy : State::unknown, layout.detail};
    } catch (const xiaoji::IdentityMismatch& err) { return {State::unknown, err.what()}; }
      catch (const xiaoji::NotStarted& err) { return {State::not_started, err.what()}; }
}

void ConfirmNoPackages() {
    wchar_t windows[MAX_PATH]{};
    auto length = GetSystemWindowsDirectoryW(windows, MAX_PATH);
    if (!length || length >= MAX_PATH) Fail("Locate Windows INF directory");
    for (const auto& file : fs::directory_iterator(fs::path(windows)/L"INF")) {
        auto name = file.path().filename().wstring();
        if (name.size() < 8 || _wcsnicmp(name.c_str(), L"oem", 3) || _wcsicmp(name.c_str()+name.size()-4, L".inf") ||
            name.find_first_not_of(L"0123456789", 3) != name.size()-4) continue;
        auto stored = StoreInf(name, true);
        if (stored.empty()) continue;
        if (!_wcsicmp(stored.filename().c_str(), L"xiaoji.inf"))
            throw std::runtime_error("Published xiaoji package remains: " + file.path().filename().u8string() + "; trust and caches retained");
    }
}

std::string ErrorText(const std::exception& err) {
    std::string message = err.what();
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, message.c_str(), -1, nullptr, 0)) return message;
    int size = MultiByteToWideChar(CP_ACP, 0, message.c_str(), -1, nullptr, 0);
    std::wstring wide(size, L'\0');
    if (!size || !MultiByteToWideChar(CP_ACP, 0, message.c_str(), -1, wide.data(), size)) return "Cannot decode Windows error message";
    size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    if (!size || !WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), size, nullptr, nullptr)) return "Cannot encode Windows error message";
    result.pop_back(); return result;
}
int Machine(int argc, wchar_t** argv) {
    using xiaoji::JsonString;
    _setmode(_fileno(stdout), _O_BINARY);
    const std::wstring_view command = argv[1];
    const char* step = command == L"status" ? nullptr : "verify";
    auto progress = [&](const char* next, const char* text) {
        step = next;
        std::cout << "{\"type\":\"progress\",\"step\":" << JsonString(step) << ",\"text\":" << JsonString(text) << "}" << std::endl;
    };
    try {
        if (command == L"status" && argc == 3) {
            auto keyboard = Inspect(0), mouse = Inspect(1);
            std::cout << "{\"type\":\"status\",\"state\":" << JsonString(xiaoji::InstallationState(keyboard.state, mouse.state))
                << ",\"keyboard\":" << JsonString(xiaoji::StateName(keyboard.state)) << ",\"mouse\":" << JsonString(xiaoji::StateName(mouse.state))
                << ",\"elevated\":" << (IsUserAnAdmin() ? "true" : "false") << ",\"detail\":"
                << JsonString("keyboard: " + keyboard.detail + "; mouse: " + mouse.detail) << "}\n";
        } else if (command == L"install") {
            if (argc != 6 || std::wstring_view(argv[2]) != L"--local-sign" || std::wstring_view(argv[4]) != L"--trust-local-certificate" ||
                std::wstring_view(argv[5]) != L"--json") throw std::runtime_error("Expected install --local-sign <directory> --trust-local-certificate --json; explicit machine trust consent required");
            Manage([&] {
                progress("verify", "Check package identity and file hashes");
                auto inf = xiaoji::PrepareLocalPackage(fs::absolute(argv[3]), CheckIdentity,
                    [](const fs::path& signedInf) { VerifyPackage(signedInf, true); }, progress);
                progress("stage", "Stage and compare the published package"); Install(inf, progress);
                progress("confirm", "Confirm both active layouts");
                for (int index : {0, 1}) { auto device = Inspect(index); if (device.state != xiaoji::DeviceState::current)
                    throw std::runtime_error("Device layout not confirmed: " + device.detail); }
            }, true);
            std::cout << "{\"type\":\"result\",\"state\":\"ready\",\"detail\":\"Both devices are running the current layout\"}\n";
        } else if (command == L"uninstall" && (argc == 3 || (argc == 4 &&
            (std::wstring_view(argv[2]) == L"--remove-local-trust" || std::wstring_view(argv[3]) == L"--remove-local-trust")))) {
            Manage([&] {
                progress("verify", "Identify bound packages"); Uninstall(false, false, progress);
                progress("confirm", "Confirm fixed device removal");
                for (int index : {0, 1}) if (!xiaoji::BoundOem(index, false, {}, true).empty()) throw std::runtime_error("Fixed device remains; trust retained");
                if (argc == 4) {
                    ConfirmNoPackages(); progress("certificate", "Remove recorded caches, certificate and private key");
                    xiaoji::RemoveLocalTrust();
                }
            }, argc == 4);
            std::cout << "{\"type\":\"result\",\"state\":\"not_installed\",\"detail\":\"Fixed devices and bound packages removed\"}\n";
        } else throw std::runtime_error("Invalid JSON command arguments");
        return 0;
    } catch (const std::exception& err) {
        std::cout << "{\"type\":\"error\"";
        if (step) std::cout << ",\"step\":" << JsonString(step);
        std::cout << ",\"detail\":" << JsonString(ErrorText(err)) << "}\n";
        return 2;
    }
}

void Help() {
    std::cout << "xiaoji status                         Show the two fixed devices\n"
                 "xiaoji verify [path\\xiaoji.inf]       Verify installed devices or an external signed package\n"
                 "xiaoji install path\\xiaoji.inf        Install a trusted signed package (administrator)\n"
                 "xiaoji uninstall [--keep-package]      Remove owned devices and their bound packages (administrator)\n"
                 "xiaoji uninstall --dry-run             Inspect bound package targets without changes\n"
                 "xiaoji status --json | uninstall --json [--remove-local-trust]\n"
                 "xiaoji install --local-sign <folder> --trust-local-certificate --json\n"
                 "Exit: 0 success, 1 usage, 2 error, 3 missing device(s). No automatic signing or elevation.\n";
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        SetupSetNonInteractiveMode(TRUE);
        for (int index = 1; index < argc; ++index) if (std::wstring_view(argv[index]) == L"--json") return Machine(argc, argv);
        const std::wstring command = argc > 1 ? argv[1] : L"--help";
        if (argc <= 2 && (command == L"--help" || command == L"help" || command == L"-h")) { Help(); return 0; }
        if (argc == 2 && (command == L"status" || command == L"verify")) return Status(command == L"verify");
        if (argc == 3 && command == L"verify") { VerifyPackage(fs::absolute(argv[2])); std::cout << "Package verified.\n"; return 0; }
        if (argc >= 3 && command == L"install" && std::wstring_view(argv[2]) == L"--local-sign")
            throw std::runtime_error("Local signing requires --trust-local-certificate and --json");
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
