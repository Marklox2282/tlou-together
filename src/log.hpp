#pragma once
#include <string>

// Лог мода: tlou1-coop.log рядом с игрой.
void Log(const char* fmt, ...);
void OpenLog();
// Полный путь к файлу рядом с tlou-i.exe (пусто, если путь не получить).
std::wstring GameDirFile(const wchar_t* name);
