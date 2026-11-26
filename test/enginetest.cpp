// Части engine, которые не требуют игры: безопасное чтение и разбор имён. Код 0 = ок.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../src/engine.hpp"

static int g_failed = 0;
static void Check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failed;
}

int main() {
	using namespace engine;
	setvbuf(stdout, nullptr, _IONBF, 0);
	uint64_t v = 0x1234;
	Check(!SafeCopy(&v, 0x10, 8) && v == 0x1234, "address below 64K refused");

	// Страница, за которой ничего нет: чтение через границу не должно ронять процесс.
	auto* page = static_cast<char*>(VirtualAlloc(nullptr, 0x2000, MEM_RESERVE, PAGE_NOACCESS));
	VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE);
	const uintptr_t edge = reinterpret_cast<uintptr_t>(page) + 0x1000;
	memcpy(page + 0x1000 - 4, "abc", 4);
	Check(ReadName(edge - 4) == "abc", "name ending right at a page edge");
	memcpy(page + 0x1000 - 3, "xyz", 3);  // без нуля: строка упирается в недоступную страницу
	Check(ReadName(edge - 3) == "xyz", "unterminated name stops at the edge");
	Check(!Read(edge - 4, v), "read across into an inaccessible page fails");
	Check(ReadPtr(reinterpret_cast<uintptr_t>(page)) == 0, "zero pointer reads as 0");
	Check(Read(edge - 8, v), "readable page is readable");
	VirtualFree(page, 0x1000, MEM_DECOMMIT);  // игра освободила память, а область ещё в кэше
	Sleep(250);
	Check(!Read(edge - 8, v), "freed memory is not read once the cache expires");

	Check(HeroName("t1x-joel-costume-01") == "joel", "hero from costume art");
	Check(HeroName("t1x-joel") == "joel", "hero from short art");
	Check(HeroName("survivor-m-02").empty(), "generic NPC has no hero name");
	Check(SpawnerRank("t1x-tool-clump-cleaning-rod", "joel") == 0, "item with t1x- art is not a character");
	Check(SpawnerRank("t1x-joel-costume-01", "joel") == 0, "never a copy of the hero being played");
	Check(SpawnerRank("t1x-tommy-costume-01", "joel") > SpawnerRank("survivor-m-02", "joel"), "male story NPC first for Joel");
	Check(SpawnerRank("survivor-m-02", "joel") > SpawnerRank("t1x-marlene-costume-01", "joel"), "then a generic man, not Marlene");
	Check(SpawnerRank("invisible-cube", "joel") == 0 && SpawnerRank("arrow-tail", "") == 0, "non-people refused");
	Check(SpawnerRank("military-f-01", "ellie") > SpawnerRank("military-m-01", "ellie"), "woman for Ellie");
	static_assert(Sid("player") == 0x4580FAB03B7EB9C0ull);
	Check(Sid("a") == 0xAF63DC4C8601EC8Cull, "FNV-1a 64 of \"a\"");

	printf(g_failed ? "%d FAILED\n" : "OK\n", g_failed);
	return g_failed ? 1 : 0;
}
