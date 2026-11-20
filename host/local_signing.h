#pragma once

#include <filesystem>
#include <functional>

namespace xiaoji {
using PackageCheck = void (*)(const std::filesystem::path& inf);
using Progress = std::function<void(const char* step, const char* text)>;
std::filesystem::path PrepareLocalPackage(const std::filesystem::path& source, PackageCheck identity,
                                          PackageCheck verify, const Progress& progress);
void RemoveLocalTrust();
bool IsMissingPublishedInf(unsigned long error);
#ifdef XIAOJI_SESSION_TEST
void SigningTest(const std::filesystem::path& source);
#endif
}
