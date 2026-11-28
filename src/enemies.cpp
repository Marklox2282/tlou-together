#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "engine.hpp"
#include "enemies.hpp"
#include "log.hpp"

using namespace engine;

namespace enemies {

namespace {

constexpr ULONGLONG kScanMs = 100;
constexpr uint64_t kMaxEnemies = 256;

// Что мы знали о каждом враге: отправляются только настоящие изменения, а полученное от
// партнёра не уходит обратно как своё.
struct Known { float health; bool dead; };
std::unordered_map<uint64_t, Known> g_known;  // только поток Tick
ULONGLONG g_lastScan = 0;
std::atomic<bool> g_reset{ false };
std::atomic<bool> g_reload{ false };
std::atomic<bool> g_resendDead{ false };
std::atomic<ULONGLONG> g_copyShotAt{ 0 }, g_ownShotAt{ 0 };
constexpr ULONGLONG kShotWindowMs = 400;

SRWLOCK g_lock = SRWLOCK_INIT;  // очереди между сетью и скриптовым потоком
std::vector<Change> g_out, g_in;
std::vector<Position> g_posOut;
std::atomic<bool> g_sendPositions{ false };
struct Remote {
	Position p;
	ULONGLONG at, snappedAt;
	float speed = 0;           // скорость врага у хозяина, м/с
	bool aiOff = false;        // полная синхронизация: ИИ уже выключен
	int mode = -1;             // последняя команда npc-move-to
	ULONGLONG cmdAt = 0;
};
std::atomic<bool> g_fullSync{ false }, g_cutscene{ false };
std::unordered_map<uint64_t, Remote> g_remote;  // гость: позиции хозяина (под g_lock)
constexpr ULONGLONG kPosMs = 200, kSnapEveryMs = 3000;
constexpr float kSnapDist = 6.0f;
constexpr size_t kMaxSent = 12;


void Push(std::vector<Change>& q, const Change& c) {
	AcquireSRWLockExclusive(&g_lock);
	if (q.size() < 1024) q.push_back(c);
	ReleaseSRWLockExclusive(&g_lock);
}

float Health(uint64_t npc) {
	const uint32_t bits = static_cast<uint32_t>(Call(g_dc.getHealth, { npc }));
	float h;
	memcpy(&h, &bits, 4);
	return h;
}

bool Dead(uint64_t npc) { return Call(g_dc.isDead, { npc }) & 1; }
bool Exists(uint64_t npc) { return Call(g_dc.entityExists, { npc }) & 1; }

// Имена живых врагов на загруженных уровнях (darray 'all-enemy-npcs из скриптов игры).
std::vector<uint64_t> AllEnemies() {
	std::vector<uint64_t> out;
	const uint64_t list = Sid("all-enemy-npcs");
	const uint64_t n = (std::min)(Call(g_dc.darrayCount, { list }), kMaxEnemies);
	for (uint64_t i = 0; i < n; ++i) {
		uint64_t npc = 0;
		if (const uint64_t at = Call(g_dc.darrayAt, { list, i }); at && Read(at, npc) && npc) out.push_back(npc);
	}
	return out;
}

// Спутники главы (Тесс, Элли, Билл...): у каждого игрока свой со своим ИИ и скриптами уровня.
// Их позицию хозяин шлёт вместе с врагами, у гостя вне кадра переносим туда же (мягко, ИИ не
// трогаем: скрипты уровня ими управляют, выключение ИИ спутника роняло игру).
std::vector<uint64_t> AllBuddies() {
	std::vector<uint64_t> out;
	const uint64_t list = Sid("all-buddy-npcs");
	const uint64_t n = (std::min)(Call(g_dc.darrayCount, { list }), uint64_t{ 8 });
	for (uint64_t i = 0; i < n; ++i) {
		uint64_t npc = 0;
		if (const uint64_t at = Call(g_dc.darrayAt, { list, i }); at && Read(at, npc) && npc) out.push_back(npc);
	}
	return out;
}

float Dist2(const float* a, const float* b) {
	const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz;
}

// Хозяин: ближайшие к игроку живые враги.
void SendPositions(uint64_t puppet) {
	Locator me{};
	if (!PlayerLocator(me)) return;
	std::vector<std::pair<float, Position>> closest;
	// Спутники - всегда (расстояние -1: в начале списка), потом ближайшие враги.
	// NpcLocator быстрее entity-exists?, поэтому сначала читаем координаты и фильтруем мёртвых.
	for (uint64_t npc : AllBuddies()) {
		Locator l{};
		if (npc == puppet || !NpcLocator(npc, l) || Dead(npc)) continue;
		Position p{ npc, {}, {} };
		memcpy(p.pos, l.pos, sizeof(p.pos));
		memcpy(p.rot, l.rot, sizeof(p.rot));
		closest.push_back({ -1.0f, p });
	}
	for (uint64_t npc : AllEnemies()) {
		Locator l{};
		if (npc == puppet || !NpcLocator(npc, l) || Dead(npc)) continue;
		Position p{ npc, {}, {} };
		memcpy(p.pos, l.pos, sizeof(p.pos));
		memcpy(p.rot, l.rot, sizeof(p.rot));
		closest.push_back({ Dist2(l.pos, me.pos), p });
	}
	std::sort(closest.begin(), closest.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	AcquireSRWLockExclusive(&g_lock);
	g_posOut.clear();
	for (size_t i = 0; i < closest.size() && i < kMaxSent; ++i) g_posOut.push_back(closest[i].second);
	ReleaseSRWLockExclusive(&g_lock);
}

// Полная синхронизация отключена (враги без ИИ не атаковали); оставляем только мягкую синхронизацию.

// Гость: враг ушёл далеко от позиции у хозяина - перенести (ИИ не трогаем), но только если ни
// он сейчас, ни точка переноса не видны в кадре; иначе ждём, пока игрок отвернётся.
void CorrectPositions(uint64_t puppet) {
	const ULONGLONG now = GetTickCount64();
	CameraView view{};
	bool viewRead = false;
	std::vector<Position> snap;
	AcquireSRWLockExclusive(&g_lock);
	for (auto it = g_remote.begin(); it != g_remote.end();) {
		if (now - it->second.at > 2000) { it = g_remote.erase(it); continue; }  // хозяин его больше не шлёт
		if (now - it->second.snappedAt >= kSnapEveryMs) snap.push_back(it->second.p);
		++it;
	}
	ReleaseSRWLockExclusive(&g_lock);
	for (const Position& p : snap) {
		if (p.npc == puppet || !Exists(p.npc) || Dead(p.npc)) continue;
		Locator cur{};
		if (!NpcLocator(p.npc, cur) || Dist2(cur.pos, p.pos) < kSnapDist * kSnapDist) continue;
		if (!viewRead) {
			view = CurrentView();
			viewRead = true;
		}
		if (Seen(view, cur.pos) || Seen(view, p.pos)) continue;  // на глазах - не переносим
		if (const uint64_t frame = BoundFrame(p.pos, p.rot)) PlaceNpc(p.npc, frame);
		AcquireSRWLockExclusive(&g_lock);
		if (auto it = g_remote.find(p.npc); it != g_remote.end()) it->second.snappedAt = now;
		ReleaseSRWLockExclusive(&g_lock);
		Log("[enemies] %016llX moved to the host's position (%.1f m off)", static_cast<unsigned long long>(p.npc),
			std::sqrt(Dist2(cur.pos, p.pos)));
	}
}

void Scan(uint64_t puppet) {
	std::vector<uint64_t> seen = AllEnemies();
	for (uint64_t npc : seen) {
		if (npc == puppet || !Exists(npc)) continue;
		const float h = Health(npc);
		const bool dead = Dead(npc);
		auto it = g_known.find(npc);
		if (it == g_known.end()) {  // впервые видим
			g_known[npc] = { h, dead };
			continue;
		}
		Known& k = it->second;
		const bool newlyDead = dead && !k.dead;
		const bool hurt = !dead && std::isfinite(h) && h < k.health - 0.01f;
		// Пуля копии партнёра: урон этого выстрела партнёр пришлёт сам - здоровье назад, не отправляем.
		const ULONGLONG nowMs = GetTickCount64();
		const bool copyShot = nowMs - g_copyShotAt.load() < kShotWindowMs && nowMs - g_ownShotAt.load() >= kShotWindowMs;
		if (hurt && copyShot) {
			Call(g_dc.setHealth, { npc, F(k.health) });
			Log("[enemies] copy's bullet hit %016llX: %.1f -> %.1f undone", static_cast<unsigned long long>(npc), k.health, h);
			continue;
		}
		if (newlyDead || hurt) {
			Push(g_out, { npc, dead ? 0.0f : h, dead });
			Log("[enemies] local %s %016llX: %.1f -> %.1f", newlyDead ? "kill" : "hit",
				static_cast<unsigned long long>(npc), k.health, h);
		}
		k = { h, dead };
	}
	// Убитый враг сразу пропадает из списка и мёртвым там обычно не виден: каждого, кто был
	// жив и пропал, проверяем отдельно. Нет совсем - убран скриптом, это не убийство.
	std::sort(seen.begin(), seen.end());
	for (auto it = g_known.begin(); it != g_known.end();) {
		const uint64_t npc = it->first;
		Known& k = it->second;
		if (k.dead || std::binary_search(seen.begin(), seen.end(), npc)) {
			++it;
			continue;
		}
		if (!Exists(npc)) {
			// Убран скриптом не мёртвым (ушёл со сцены): удаляем из кэша, чтобы не опрашивать дальше.
			it = g_known.erase(it);
			continue;
		}
		if (Dead(npc) || Health(npc) <= 0.0f) {
			Push(g_out, { npc, 0.0f, true });
			Log("[enemies] local kill %016llX (left the enemy list dead)", static_cast<unsigned long long>(npc));
			k = { 0.0f, true };
		}
		++it;
	}
}

void Apply(const Change& c, uint64_t puppet) {
	if (!c.npc || c.npc == puppet || !Exists(c.npc)) return;
	Known& k = g_known.try_emplace(c.npc, Known{ Health(c.npc), false }).first->second;
	if (Dead(c.npc)) {
		k.dead = true;
		return;
	}
	if (c.dead) {
		k = { 0.0f, true };  // сначала запомнить: свой Scan не должен отправить это обратно
		Call(g_dc.npcDie, { c.npc, 0, 0, 0, 0 });
		const bool died = Dead(c.npc);
		if (!died) Call(g_dc.setHealth, { c.npc, F(0.0f) });
		Log("[enemies] partner killed %016llX -> %s", static_cast<unsigned long long>(c.npc),
			died ? "npc-die" : "health set to 0");
		return;
	}
	const float local = Health(c.npc);
	if (std::isfinite(c.health) && c.health >= 0.0f && c.health < local - 0.01f) {
		Call(g_dc.setHealth, { c.npc, F(c.health) });
		Log("[enemies] partner hit %016llX: %.1f -> %.1f", static_cast<unsigned long long>(c.npc), local, c.health);
		k.health = c.health;
	} else {
		k.health = (std::min)(k.health, local);
	}
}

}  // namespace

void Tick(uint64_t puppet) {
	if (g_reset.exchange(false)) {
		g_known.clear();
		AcquireSRWLockExclusive(&g_lock);
		g_out.clear();
		g_in.clear();
		g_posOut.clear();
		g_remote.clear();
		ReleaseSRWLockExclusive(&g_lock);
	}
	if (g_reload.exchange(false)) {
		g_known.clear();
		AcquireSRWLockExclusive(&g_lock);
		g_posOut.clear();
		g_remote.clear();
		ReleaseSRWLockExclusive(&g_lock);
	}
	if (g_resendDead.exchange(false)) {
		int n = 0;
		for (const auto& [npc, k] : g_known) {
			if (!k.dead || npc == puppet) continue;
			Push(g_out, { npc, 0.0f, true });
			++n;
		}
		if (n) Log("[enemies] synced %d already-dead enemies to partner", n);
	}
	std::vector<Change> in;
	AcquireSRWLockExclusive(&g_lock);
	in.swap(g_in);
	ReleaseSRWLockExclusive(&g_lock);
	for (const Change& c : in) Apply(c, puppet);

	const ULONGLONG now = GetTickCount64();
	static ULONGLONG lastPos = 0;
	if (now - lastPos >= kPosMs) {
		lastPos = now;
		const int64_t p0 = QpcNow();
		if (g_sendPositions) SendPositions(puppet);
		else if (!g_cutscene) CorrectPositions(puppet);  // в катсцене никого не двигаем
		ProfileAdd("[enemy pos]", MicrosecondsSince(p0));
	}
	if (now - g_lastScan < kScanMs) return;
	g_lastScan = now;
	const int64_t s = QpcNow();
	Scan(puppet);
	ProfileAdd("[enemies]", MicrosecondsSince(s));
}

std::vector<Change> TakeOutgoing() {
	std::vector<Change> out;
	AcquireSRWLockExclusive(&g_lock);
	out.swap(g_out);
	ReleaseSRWLockExclusive(&g_lock);
	return out;
}

void Receive(const Change& c) { Push(g_in, c); }

void SetSendPositions(bool on) { g_sendPositions = on; }
void SetFullSync(bool) { g_fullSync = false; }
void SetCutscene(bool on) { g_cutscene = on; }

std::vector<Position> TakePositions() {
	std::vector<Position> out;
	AcquireSRWLockExclusive(&g_lock);
	out.swap(g_posOut);
	ReleaseSRWLockExclusive(&g_lock);
	return out;
}

void ReceivePosition(const Position& p) {
	if (!p.npc || !std::isfinite(p.pos[0]) || !std::isfinite(p.pos[1]) || !std::isfinite(p.pos[2])) return;
	AcquireSRWLockExclusive(&g_lock);
	Remote& r = g_remote[p.npc];
	const ULONGLONG now = GetTickCount64();
	if (r.at && now > r.at && now - r.at < 1000) {  // скорость по двум последним позициям (по горизонтали)
		const float dx = p.pos[0] - r.p.pos[0], dz = p.pos[2] - r.p.pos[2];
		r.speed = r.speed * 0.5f + std::sqrt(dx * dx + dz * dz) / ((now - r.at) / 1000.0f) * 0.5f;
	}
	r.p = p;
	r.at = now;
	ReleaseSRWLockExclusive(&g_lock);
}
void Reset() { g_reset = true; }
void OnReload() { g_reload = true; }
void ResendDead() { g_resendDead = true; }
void NoteCopyShot() { g_copyShotAt = GetTickCount64(); }
void NoteOwnShot() { g_ownShotAt = GetTickCount64(); }

}  // namespace enemies
