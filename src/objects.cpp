#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#include <safetyhook.hpp>

#include "engine.hpp"
#include "log.hpp"
#include "objects.hpp"

using namespace engine;

namespace objects {

namespace {

constexpr ULONGLONG kTickMs = 250;
constexpr float kMovedM = 0.4f;    // сдвинули дальше - перестановка
constexpr float kStillM = 0.05f;   // за 0.25 с почти не сдвинулся - уже поставлен (не в руках)
constexpr uintptr_t kDoorOnEventRva = 0x128ED74;  // Door::OnEvent (AnalogDoor vtable+0x70, Steam 685BEBE1)
constexpr uintptr_t kDoorProcessOffset = 0xD8;    // *(door + 0xD8) -> ProcessInfo, +0x10 = uint64_t sid
constexpr ULONGLONG kDoorCandidateTtlMs = 2000;
constexpr ULONGLONG kDoorEchoSuppressMs = 4000;
constexpr ULONGLONG kDoorSentCooldownMs = 2500;
constexpr ULONGLONG kDoorRetryWaitMs = 4000;
constexpr float kDoorMaxDistM = 5.0f;

struct Known {
	float sent[3];     // где он был, когда о нём знали оба (своё или от партнёра)
	float last[3];     // где был на прошлом шаге
	bool seen = false;
};

struct DoorCandidate { uint64_t sid, eventSid; ULONGLONG at; };
struct PendingDoor { uint64_t sid; ULONGLONG until; };

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Placement> g_out, g_in;        // под g_lock
std::vector<DoorCandidate> g_doorEvents;   // под g_lock
std::vector<uint64_t> g_doorsOut, g_doorsIn; // под g_lock
std::unordered_map<uint64_t, ULONGLONG> g_remoteDoors;  // под g_lock: недавно открыто по сети
std::atomic<bool> g_reset{ false };
std::atomic<bool> g_reload{ false };
std::atomic<bool> g_resendDoors{ false };
safetyhook::InlineHook g_doorHook;

std::vector<uint64_t> g_sids;                 // только поток Tick
std::unordered_map<uint64_t, Known> g_known;  // только поток Tick
std::unordered_map<uint64_t, ULONGLONG> g_sentDoors;  // только поток Tick
std::vector<PendingDoor> g_pendingDoorsIn;            // только поток Tick
std::vector<uint64_t> g_openedChapterDoors;           // только поток Tick: двери текущей попытки (до 32)
Chapter g_scannedFor;
ULONGLONG g_rescanAt = 0;
int g_rescansLeft = 0;

bool IsPlayerDoorEvent(uint64_t ev) {
	return ev == Sid("on-door-open") || ev == Sid("open-door") || ev == Sid("break-door") || ev == Sid("set-door-open");
}

void PruneDoorMap(std::unordered_map<uint64_t, ULONGLONG>& m, ULONGLONG now, ULONGLONG ttl) {
	for (auto it = m.begin(); it != m.end();)
		it = (now - it->second > ttl) ? m.erase(it) : std::next(it);
}

bool RecentRemoteDoorLocked(uint64_t sid, ULONGLONG now) {
	const auto it = g_remoteDoors.find(sid);
	return it != g_remoteDoors.end() && now - it->second <= kDoorEchoSuppressMs;
}

uintptr_t __fastcall Hook_DoorOnEvent(uintptr_t door, uintptr_t eventData, uintptr_t a3, uintptr_t a4) {
	uint64_t eventSid = 0;
	if (door && eventData && Read(eventData, eventSid) && IsPlayerDoorEvent(eventSid)) {
		const uintptr_t proc = ReadPtr(door + kDoorProcessOffset);
		uint64_t doorSid = 0;
		if (proc && Read(proc + 0x10, doorSid) && doorSid) {
			const ULONGLONG now = GetTickCount64();
			AcquireSRWLockExclusive(&g_lock);
			PruneDoorMap(g_remoteDoors, now, kDoorEchoSuppressMs);
			if (!RecentRemoteDoorLocked(doorSid, now) && g_doorEvents.size() < 32) {
				const bool dup = !g_doorEvents.empty() && g_doorEvents.back().sid == doorSid &&
					g_doorEvents.back().eventSid == eventSid && now - g_doorEvents.back().at < 500;
				if (!dup) g_doorEvents.push_back({ doorSid, eventSid, now });
			}
			ReleaseSRWLockExclusive(&g_lock);
		}
	}
	return g_doorHook.call<uintptr_t>(door, eventData, a3, a4);
}

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

bool HasOpenedDoor(uint64_t sid) {
	return std::find(g_openedChapterDoors.begin(), g_openedChapterDoors.end(), sid) != g_openedChapterDoors.end();
}

void RememberDoor(uint64_t sid) {
	if (!sid || HasOpenedDoor(sid)) return;
	if (g_openedChapterDoors.size() >= 32) g_openedChapterDoors.erase(g_openedChapterDoors.begin());
	g_openedChapterDoors.push_back(sid);
}

void TickDoors(ULONGLONG now) {
	if (g_resendDoors.exchange(false) && !g_openedChapterDoors.empty()) {
		AcquireSRWLockExclusive(&g_lock);
		int sent = 0;
		for (uint64_t sid : g_openedChapterDoors) {
			if (g_doorsOut.size() >= 32) break;
			if (std::find(g_doorsOut.begin(), g_doorsOut.end(), sid) == g_doorsOut.end()) {
				g_doorsOut.push_back(sid);
				++sent;
			}
		}
		ReleaseSRWLockExclusive(&g_lock);
		if (sent) Log("objects: synced %d already-open doors to partner", sent);
	}
	std::vector<uint64_t> doorsIn;
	std::vector<DoorCandidate> candidates;
	AcquireSRWLockExclusive(&g_lock);
	doorsIn.swap(g_doorsIn);
	candidates.swap(g_doorEvents);
	PruneDoorMap(g_remoteDoors, now, kDoorEchoSuppressMs);
	ReleaseSRWLockExclusive(&g_lock);
	PruneDoorMap(g_sentDoors, now, kDoorSentCooldownMs);

	for (uint64_t sid : doorsIn) {
		if (!sid || HasOpenedDoor(sid)) continue;
		const bool already = std::any_of(g_pendingDoorsIn.begin(), g_pendingDoorsIn.end(),
			[sid](const PendingDoor& d) { return d.sid == sid; });
		if (!already && g_pendingDoorsIn.size() < 16)
			g_pendingDoorsIn.push_back({ sid, now + kDoorRetryWaitMs });
	}
	if (!g_pendingDoorsIn.empty() && g_dc.sendEvent) {
		std::vector<PendingDoor> keep;
		for (const PendingDoor& d : g_pendingDoorsIn) {
			if (HasOpenedDoor(d.sid)) continue;
			if (g_dc.entityExists && !(Call(g_dc.entityExists, { d.sid }) & 1)) {
				if (now < d.until) keep.push_back(d);
				else Log("objects: partner door %016llX is not here", static_cast<unsigned long long>(d.sid));
				continue;
			}
			AcquireSRWLockExclusive(&g_lock);
			g_remoteDoors[d.sid] = now;
			ReleaseSRWLockExclusive(&g_lock);
			g_sentDoors[d.sid] = now;
			RememberDoor(d.sid);
			Call(g_dc.sendEvent, { Sid("open-door"), d.sid });
			Log("objects: partner opened door %016llX", static_cast<unsigned long long>(d.sid));
		}
		g_pendingDoorsIn = std::move(keep);
	}

	if (candidates.empty() || !g_dc.sendEvent) return;
	Locator me{};
	if (!PlayerLocator(me)) return;
	bool checkedCutscene = false, inCutscene = false;
	for (const DoorCandidate& c : candidates) {
		if (now - c.at > kDoorCandidateTtlMs) continue;
		if (g_sentDoors.find(c.sid) != g_sentDoors.end()) continue;
		bool remote = false;
		AcquireSRWLockShared(&g_lock);
		remote = RecentRemoteDoorLocked(c.sid, now);
		ReleaseSRWLockShared(&g_lock);
		if (remote) continue;
		// on-door-open шлётся только при нажатии игроком; прочие события (open-door/break-door/set-door-open) фильтруем от катсцен.
		if (c.eventSid != Sid("on-door-open")) {
			if (!checkedCutscene) {
				checkedCutscene = true;
				inCutscene = InCutscene();
			}
			if (inCutscene) continue;
			Locator cur{};
			if (!NpcLocator(c.sid, cur) || Dist3(cur.pos, me.pos) > kDoorMaxDistM) continue;
		}
		g_sentDoors[c.sid] = now;
		RememberDoor(c.sid);
		AcquireSRWLockExclusive(&g_lock);
		if (g_doorsOut.size() < 32) g_doorsOut.push_back(c.sid);
		ReleaseSRWLockExclusive(&g_lock);
		Log("objects: opened door %016llX (ev=%016llX) - telling partner",
			static_cast<unsigned long long>(c.sid), static_cast<unsigned long long>(c.eventSid));
	}
}

}  // namespace

void UninstallDoors() { g_doorHook = {}; }

bool InitDoors() {
	if (!g_base || BuildTimestamp() != kBuildTimestamp) return false;
	auto res = safetyhook::InlineHook::create(
		reinterpret_cast<void*>(g_base + kDoorOnEventRva),
		reinterpret_cast<void*>(&Hook_DoorOnEvent));
	if (!res) {
		Log("objects: door hook failed at RVA 0x%llX (err=%d)",
			static_cast<unsigned long long>(kDoorOnEventRva), static_cast<int>(res.error().type));
		return false;
	}
	g_doorHook = std::move(*res);
	Log("objects: door hook ok at RVA 0x%llX", static_cast<unsigned long long>(kDoorOnEventRva));
	return true;
}

void Tick() {
	const ULONGLONG now = GetTickCount64();
	if (g_reset.exchange(false)) {
		g_known.clear();
		g_sentDoors.clear();
		g_pendingDoorsIn.clear();
		g_openedChapterDoors.clear();
	}
	if (g_reload.exchange(false)) {
		g_known.clear();
		g_sentDoors.clear();
		g_pendingDoorsIn.clear();
		g_openedChapterDoors.clear();
		AcquireSRWLockExclusive(&g_lock);
		g_doorEvents.clear();
		g_remoteDoors.clear();
		ReleaseSRWLockExclusive(&g_lock);
	}
	TickDoors(now);
	static ULONGLONG last = 0;
	if (now - last < kTickMs) return;
	last = now;
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

std::vector<uint64_t> TakeDoors() {
	std::vector<uint64_t> out;
	AcquireSRWLockExclusive(&g_lock);
	out.swap(g_doorsOut);
	ReleaseSRWLockExclusive(&g_lock);
	return out;
}

void ReceiveDoor(uint64_t door) {
	if (!door) return;
	AcquireSRWLockExclusive(&g_lock);
	if (g_doorsIn.size() < 32) g_doorsIn.push_back(door);
	ReleaseSRWLockExclusive(&g_lock);
}

void ResendDoors() { g_resendDoors = true; }
void OnReload() { g_reload = true; }

void Reset() {
	g_reset = true;
	AcquireSRWLockExclusive(&g_lock);
	g_in.clear();
	g_out.clear();
	g_doorEvents.clear();
	g_doorsIn.clear();
	g_doorsOut.clear();
	g_remoteDoors.clear();
	ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace objects
