#pragma once
// Автообновление: version.txt, winmm.dll и winmm.dll.sig из последнего релиза на GitHub.
// Файл ставится, только если подпись (ключ автора, см. update_key.hpp) верна для этой версии.
// Загруженную DLL Windows не даёт перезаписать, но даёт переименовать: старая уходит в
// winmm.dll.old, новая встаёт на её место и работает со следующего запуска игры.

#include <cstdint>
#include <string>

namespace update {

inline constexpr const wchar_t* kReleaseUrl = L"https://github.com/Marklox2282/tlou-together/releases/latest/download/";

enum class Result { UpToDate, Installed, Failed };
using LogFn = void (*)(const char* fmt, ...);

// Удаляет winmm.dll.old, оставшийся от прошлого обновления.
void CleanupOld(const std::wstring& dllPath);
Result Run(uint32_t currentVersion, const std::wstring& dllPath, const std::wstring& baseUrl, LogFn log);

}  // namespace update
