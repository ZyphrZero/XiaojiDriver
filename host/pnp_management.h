#pragma once

#include <functional>
#include <string>
#include <stdexcept>

namespace xiaoji {
struct IdentityMismatch : std::runtime_error { using std::runtime_error::runtime_error; };
struct NotStarted : std::runtime_error { using std::runtime_error::runtime_error; };
enum class LayoutVersion { current, legacy, unknown };
struct RunningLayout {
    LayoutVersion version;
    std::string detail;
};
RunningLayout QueryRunningLayout(int index);
std::wstring StageDriverPackage(const std::wstring& infPath);
std::wstring BoundOem(int index, bool requireStarted, const std::wstring& allowedInfs, bool includeAbsent = false);
void EnsureDevices(const std::wstring& allowedInfs, const std::wstring& priorInfs);
void RemoveXiaojiDevices(const std::wstring& allowedInfs);
void WithOperationLock(const std::function<void()>& operation);
}
