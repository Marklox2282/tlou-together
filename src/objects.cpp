#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include "engine.hpp"
#include "log.hpp"
#include "objects.hpp"

using namespace engine;

namespace objects {

namespace {

constexpr ULONGLONG kTickMs = 250;
constexpr float kMovedM = 0.4f;    // сдвинули дальше - перестановка
constexpr float kStillM = 0.05f;   // за 0.25 с почти не сдвинулся - уже поставлен (не в руках)

struct Known {
	float sent[3];     // где он был, когда о нём знали оба (своё или от партнёра)
	float last[3];     // где был на прошлом шаге
	bool seen = false;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Placement> g_out, g_in;  // под g_lock
std::atomic<bool> g_reset{ false };

std::vector<uint64_t> g_sids;                 // только поток Tick
std::unordered_map<uint64_t, Known> g_known;  // только поток Tick
Chapter g_scannedFor;
ULONGLONG g_rescanAt = 0;
int g_rescansLeft = 0;

float Dist3(const float a[3], const float b[3]) {
	const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Rescan() {
	std::vector<std::string> names;
	g_sids = CarryableSids(names);
	for (auto it = g_known.begin(); it != g_known.end();) {
		bool keep = false;
		for (uint64_t s : g_sids) keep |= s == it->first;
		it = keep ? std::next(it) : g_known.erase(it);
	}
	std::string list;
	for (size_t i = 0; i < names.size() && i < 6; ++i) list += (i ? ", " : "") + names[i];
	Log("objects: %zu carryable planks/ladders%s%s", g_sids.size(), list.empty() ? "" : ": ", list.c_str());
}

}  // namespace

void Tick() {
	static ULONGLONG last = 0;
	const ULONGLONG now = GetTickCount64();
	if (now - last < kTickMs) return;
	last = now;
	if (g_reset.exchange(false)) g_known.clear();
	Locator me{};
	if (!PlayerLocator(me) || !g_dc.setObjectLocator) return;
	// Список - при смене главы/чекпоинта и ещё раз через 5 с (части уровня догружаются).
	const Chapter ch = CurrentChapter();
	if (!(ch == g_scannedFor)) {
		g_scannedFor = ch;
		g_rescanAt = now + 1500;
		g_rescansLeft = 2;
	}
	if (g_rescansLeft && now >= g_rescanAt) {
		Rescan();
		--g_rescansLeft;
		g_rescanAt = now + 5000;
	}
	std::vector<Placement> in;
	AcquireSRWLockExclusive(&g_lock);
	in.swap(g_in);
	ReleaseSRWLockExclusive(&g_lock);
	// От партнёра: поставить туда же. Предмет у нас в руках (рядом с нами и двигается) не трогаем.
	for (const Placement& p : in) {
		Known& k = g_known[p.object];
		Locator cur{};
		if (!NpcLocator(p.object, cur)) {
			Log("objects: %016llX from partner is not here", static_cast<unsigned long long>(p.object));
			continue;
		}
		if (k.seen && Dist3(cur.pos, me.pos) < 1.5f && Dist3(cur.pos, k.last) > kStillM) continue;
		if (const uint64_t frame = BoundFrame(p.pos, p.rot)) Call(g_dc.setObjectLocator, { p.object, frame });
		memcpy(k.sent, p.pos, sizeof(k.sent));
		memcpy(k.last, p.pos, sizeof(k.last));
		k.seen = true;
		Log("objects: partner moved %016llX to (%.1f %.1f %.1f)", static_cast<unsigned long long>(p.object), p.pos[0], p.pos[1], p.pos[2]);
	}
	// Свои: предмет стоит на новом месте (сдвинут и уже не движется) - сообщить партнёру.
	for (uint64_t sid : g_sids) {
		Locator cur{};
		if (!NpcLocator(sid, cur)) continue;
		Known& k = g_known[sid];
		if (!k.seen) {
			memcpy(k.sent, cur.pos, sizeof(k.sent));
			memcpy(k.last, cur.pos, sizeof(k.last));
			k.seen = true;
			continue;
		}
		const bool still = Dist3(cur.pos, k.last) < kStillM;
		memcpy(k.last, cur.pos, sizeof(k.last));
		if (still && Dist3(cur.pos, k.sent) > kMovedM) {
			memcpy(k.sent, cur.pos, sizeof(k.sent));
			Placement p{ sid, {}, {} };
			memcpy(p.pos, cur.pos, sizeof(p.pos));
			memcpy(p.rot, cur.rot, sizeof(p.rot));
			AcquireSRWLockExclusive(&g_lock);
			if (g_out.size() < 32) g_out.push_back(p);
			ReleaseSRWLockExclusive(&g_lock);
			Log("objects: moved %016llX to (%.1f %.1f %.1f) - telling partner", static_cast<unsigned long long>(sid), cur.pos[0], cur.pos[1], cur.pos[2]);
		}
	}
}

std::vector<Placement> TakeOutgoing() {
	std::vector<Placement> out;
	AcquireSRWLockExclusive(&g_lock);
	out.swap(g_out);
	ReleaseSRWLockExclusive(&g_lock);
	return out;
}

void Receive(const Placement& p) {
	AcquireSRWLockExclusive(&g_lock);
	if (g_in.size() < 32) g_in.push_back(p);
	ReleaseSRWLockExclusive(&g_lock);
}

void Reset() {
	g_reset = true;
	AcquireSRWLockExclusive(&g_lock);
	g_in.clear();
	g_out.clear();
	ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace objects
