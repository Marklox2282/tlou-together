#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>

#include "clipboard.hpp"

namespace clipboard {

std::string Text() {
	std::string out;
	if (!OpenClipboard(nullptr)) return out;
	if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
		if (const auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
			for (size_t i = 0; w[i] && i < 64; ++i) out += w[i] < 128 ? static_cast<char>(w[i]) : '?';
			GlobalUnlock(h);
		}
	CloseClipboard();
	return out;
}

void Set(const std::string& text) {
	if (!OpenClipboard(nullptr)) return;
	EmptyClipboard();
	if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
		memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
		GlobalUnlock(h);
		if (!SetClipboardData(CF_TEXT, h)) GlobalFree(h);
	}
	CloseClipboard();
}

}  // namespace clipboard
