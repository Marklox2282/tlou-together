#pragma once
// Версия мода: MAJOR.MINOR.PATCH. Внутри - одно число (MAJOR*10000 + MINOR*100 + PATCH): его
// сравнивают обновление (version.txt) и сеть (у обоих должна быть одна версия). Старые версии
// были просто номерами 7..39 - они меньше любого нового числа, поэтому обновляются сами.
#include <cstdint>
#include <cstdio>
#include <string>

namespace version {

constexpr uint32_t kMajor = 0, kMinor = 43, kPatch = 0;
constexpr uint32_t kCode = kMajor * 10000 + kMinor * 100 + kPatch;

// "0.40.0"; старые номера (до 100) - "v39".
inline std::string Name(uint32_t code) {
	char s[32];
	if (code < 100) snprintf(s, sizeof(s), "v%u", code);
	else snprintf(s, sizeof(s), "%u.%u.%u", code / 10000, code / 100 % 100, code % 100);
	return s;
}

}  // namespace version
