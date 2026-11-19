#pragma once

#include <functional>
#include <string>

namespace xiaoji {
std::wstring StageDriverPackage(const std::wstring& infPath);
std::wstring BoundOem(int index, bool requireStarted, const std::wstring& allowedInfs, bool includeAbsent = false);
void EnsureDevices(const std::wstring& allowedInfs, const std::wstring& priorInfs);
void RemoveXiaojiDevices(const std::wstring& allowedInfs);
void WithOperationLock(const std::function<void()>& operation);
}
