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
std::atomic<bool> g_fullSync{ false };
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

float Dist2(const float* a, const float* b) {
	const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz;
}

// Хозяин: ближайшие к игроку живые враги.
void SendPositions(uint64_t puppet) {
	Locator me{};
	if (!PlayerLocator(me)) return;
	std::vector<std::pair<float, Position>> closest;
	for (uint64_t npc : AllEnemies()) {
		if (npc == puppet || !Exists(npc) || Dead(npc)) continue;
		Locator l{};
		if (!NpcLocator(npc, l) || !std::isfinite(l.pos[0])) continue;
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

// Точку видно с камеры: ближе 25 м и в конусе ~70° от взгляда (с запасом на широкий FOV).
// Без камеры считаем, что видно: лучше не перенести, чем перенести на глазах.
struct View { bool ok; Locator cam; float fwd[3]; };

View CurrentView() {
	View v{};
	v.ok = CameraLocator(v.cam);
	const float* q = v.cam.rot;  // (0,0,1), повёрнутый кватернионом камеры
	v.fwd[0] = 2.0f * (q[0] * q[2] + q[3] * q[1]);
	v.fwd[1] = 2.0f * (q[1] * q[2] - q[3] * q[0]);
	v.fwd[2] = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
	return v;
}

bool Seen(const View& v, const float p[3]) {
	if (!v.ok) return true;
	const float d[3] = { p[0] - v.cam.pos[0], p[1] - v.cam.pos[1], p[2] - v.cam.pos[2] };
	const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (len > 25.0f) return false;
	if (len < 2.0f) return true;
	return (d[0] * v.fwd[0] + d[1] * v.fwd[1] + d[2] * v.fwd[2]) > len * 0.34f;  // cos 70°
}

// Полная синхронизация: враг у гостя без ИИ идёт за врагом хозяина (как марионетка: шаг/бег/
// спринт по скорости, телепорт при отставании больше 4 м).
void FollowHost(uint64_t puppet) {
	const ULONGLONG now = GetTickCount64();
	std::vector<std::pair<uint64_t, Remote>> list;
	AcquireSRWLockExclusive(&g_lock);
	for (auto it = g_remote.begin(); it != g_remote.end();) {
		if (now - it->second.at > 2000) { it = g_remote.erase(it); continue; }
		list.push_back({ it->first, it->second });
		++it;
	}
	ReleaseSRWLockExclusive(&g_lock);
	for (auto& [npc, r] : list) {
		if (npc == puppet || !Exists(npc) || Dead(npc)) continue;
		if (!r.aiOff) {
			Call(g_dc.scriptMode, { npc, 2, 1 });
			Call(g_dc.setBlind, { npc, 1 });
			Call(g_dc.setDeaf, { npc, 1 });
			r.aiOff = true;
			Log("[enemies] %016llX follows the host's copy (AI off)", static_cast<unsigned long long>(npc));
		}
		Locator cur{};
		if (!NpcLocator(npc, cur)) continue;
		const float err = std::sqrt(Dist2(cur.pos, r.p.pos));
		const uint64_t frame = BoundFrame(r.p.pos, r.p.rot);
		if (!frame) continue;
		if (err > 4.0f) {
			Call(g_dc.teleport, { npc, frame });
			r.mode = -1;
		} else if (r.speed > 0.4f || err > 1.0f) {
			const float speed = (std::max)(r.speed, err * 0.8f);
			const int mode = speed < 1.8f ? 0 : (speed < 4.2f ? 1 : 2);
			if (mode != r.mode || now - r.cmdAt > 600) {
				Call(g_dc.moveTo, { npc, frame, static_cast<uint64_t>(mode), 0, 0, 0, 0, 0 });
				r.mode = mode;
				r.cmdAt = now;
			}
		}
		AcquireSRWLockExclusive(&g_lock);
		if (auto it = g_remote.find(npc); it != g_remote.end()) {
			it->second.aiOff = r.aiOff;
			it->second.mode = r.mode;
			it->second.cmdAt = r.cmdAt;
		}
		ReleaseSRWLockExclusive(&g_lock);
	}
}

// Гость: враг ушёл далеко от позиции у хозяина - перенести (ИИ не трогаем), но только если ни
// он сейчас, ни точка переноса не видны в кадре; иначе ждём, пока игрок отвернётся.
void CorrectPositions(uint64_t puppet) {
	const ULONGLONG now = GetTickCount64();
	View view{};
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
		if (const uint64_t frame = BoundFrame(p.pos, p.rot)) Call(g_dc.teleport, { p.npc, frame });
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
	for (auto& [npc, k] : g_known) {
		if (k.dead || std::binary_search(seen.begin(), seen.end(), npc) || !Exists(npc)) continue;
		if (Dead(npc) || Health(npc) <= 0.0f) {
			Push(g_out, { npc, 0.0f, true });
			Log("[enemies] local kill %016llX (left the enemy list dead)", static_cast<unsigned long long>(npc));
			k = { 0.0f, true };
		}
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
		ReleaseSRWLockExclusive(&g_lock);
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
		else if (g_fullSync) FollowHost(puppet);
		else CorrectPositions(puppet);
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
void SetFullSync(bool on) { g_fullSync = on; }

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

}  // namespace enemies
