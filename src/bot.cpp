#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "anim.hpp"
#include "bot.hpp"
#include "puppet.hpp"

namespace bot {

namespace {

constexpr uint16_t kPlayerPort = 17777;
constexpr uint16_t kBotPort = 17778;
constexpr uint64_t kPhaseMs = 4000;
constexpr uint64_t kCycleMs = kPhaseMs * 4;

net::Link* g_bot = nullptr;
net::Link::LogFn g_log = nullptr;
bool g_active = false;
uint64_t g_startedAt = 0;
uint64_t g_lastPoseAt = 0;
uint64_t g_lastReportAt = 0;
uint64_t g_spawnedAt = 0;
uint64_t g_weaponSid = 0;
uint64_t g_sentWeapon = ~0ull;
float g_anchor[3]{};
float g_maxErrM = 0.0f;
uint32_t g_tposeEvents = 0;
uint32_t g_doorEvents = 0;
uint32_t g_enemyEvents = 0;
int g_lastPhase = -1;

void YawQuat(float yaw, float q[4]) {
	q[0] = 0.0f;
	q[1] = std::sin(yaw * 0.5f);
	q[2] = 0.0f;
	q[3] = std::cos(yaw * 0.5f);
}

void BotLog(const char* fmt, ...) {
	if (!g_log) return;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_log("[TestBot] %s", buf);
}

void PrintSummary(const char* reason, uint64_t nowMs) {
	if (!g_log) return;
	const uint64_t sec = (nowMs >= g_startedAt) ? (nowMs - g_startedAt) / 1000 : 0;
	const uint64_t spawnMs = (g_spawnedAt >= g_startedAt && g_spawnedAt) ? (g_spawnedAt - g_startedAt) : 0;
	const anim::Stats st = anim::GetStats();
	g_log("bot: %s (%llu s) | spawn=%llu ms maxErr=%.2f m tpose=%u doorsRx=%u enemiesRx=%u anim(app=%llu drop=%llu miss=%llu)",
		reason, static_cast<unsigned long long>(sec), static_cast<unsigned long long>(spawnMs),
		g_maxErrM, g_tposeEvents, g_doorEvents, g_enemyEvents,
		static_cast<unsigned long long>(st.rxApplied), static_cast<unsigned long long>(st.rxDropped),
		static_cast<unsigned long long>(st.rxMissClips));
}

}  // namespace

bool Active() { return g_active; }

void Stop(net::Link& playerLink) {
	if (!g_active) return;
	PrintSummary("stopped", GetTickCount64());
	g_active = false;
	puppet::SetWanted(false);
	puppet::SetFollow(false);
	if (g_bot) g_bot->Stop();
	if (playerLink.GetState() != net::State::Off) playerLink.Stop();
}

void Toggle(uint64_t nowMs, uint32_t modVersion, net::Link::LogFn log, net::Link& playerLink,
	bool havePlayer, const engine::Locator& me, const engine::Chapter& ch) {
	g_log = log;
	if (g_active) {
		Stop(playerLink);
		return;
	}
	if (playerLink.GetState() != net::State::Off) {
		if (g_log) g_log("bot: disconnect current session first (F6) before starting F12 bot");
		return;
	}
	if (!havePlayer) {
		if (g_log) g_log("bot: load into a level first before pressing F12");
		return;
	}
	if (!g_bot) g_bot = new net::Link(BotLog, modVersion);
	g_bot->SetName("TestBot");
	g_bot->SetChapter({ ch.node, ch.task });
	g_bot->SetReady(true);

	const net::Config playerCfg{ "127.0.0.1", kPlayerPort, kBotPort };
	const net::Config botCfg{ "127.0.0.1", kBotPort, kPlayerPort };
	if (!playerLink.Start(playerCfg, nowMs) || !g_bot->Start(botCfg, nowMs)) {
		playerLink.Stop();
		g_bot->Stop();
		if (g_log) g_log("bot: failed to open local UDP ports %u/%u", kPlayerPort, kBotPort);
		return;
	}

	g_active = true;
	g_startedAt = g_lastReportAt = nowMs;
	g_lastPoseAt = 0;
	g_spawnedAt = 0;
	g_maxErrM = 0.0f;
	g_tposeEvents = 0;
	g_doorEvents = 0;
	g_enemyEvents = 0;
	g_lastPhase = -1;
	g_sentWeapon = ~0ull;
	memcpy(g_anchor, me.pos, sizeof(g_anchor));
	g_weaponSid = puppet::MyWeapon() ? puppet::MyWeapon() : engine::Sid("t1x-pistol-9mm-joel");
	if (g_log) g_log("bot: F12 emulator started around (%.1f %.1f %.1f) - watch puppet & press F12 to stop",
		g_anchor[0], g_anchor[1], g_anchor[2]);
}

void Tick(uint64_t nowMs, bool havePlayer, const engine::Chapter& ch, net::Link& playerLink) {
	if (!g_active || !g_bot) return;
	if (playerLink.GetState() == net::State::Off) {
		Stop(playerLink);
		return;
	}
	if (const uint64_t w = puppet::MyWeapon()) g_weaponSid = w;
	if (ch) g_bot->SetChapter({ ch.node, ch.task });
	g_bot->SetReady(true);

	net::Packet unusedPose;
	g_bot->Tick(nowMs, unusedPose);
	g_bot->TakeAnimFrames();
	g_bot->TakeThrows();
	g_bot->TakePartnerStarted();
	for (const net::Packet& ev : g_bot->TakeEnemyEvents()) {
		if (ev.type == net::kDoorOpen) {
			++g_doorEvents;
			if (g_log) g_log("bot: verified door %016llX delivered over UDP", static_cast<unsigned long long>(ev.target));
		} else if (ev.type == net::kEnemy) {
			++g_enemyEvents;
			if (g_log) g_log("bot: verified enemy %016llX event (hp=%.1f dead=%d) delivered over UDP",
				static_cast<unsigned long long>(ev.target), ev.health, (ev.flags & net::kFlagDead) ? 1 : 0);
		}
	}

	if (g_bot->GetState() == net::State::Connected && havePlayer && nowMs - g_lastPoseAt >= 30) {
		g_lastPoseAt = nowMs;
		const uint64_t elapsed = nowMs - g_startedAt;
		const uint64_t inCycle = elapsed % kCycleMs;
		const int phase = static_cast<int>(inCycle / kPhaseMs);
		const float tSec = static_cast<float>(elapsed) / 1000.0f;
		const float pSec = static_cast<float>(inCycle % kPhaseMs) / 1000.0f;

		if (phase != g_lastPhase) {
			g_lastPhase = phase;
			const char* names[4] = { "walk unarmed", "sprint+crouch+flashlight", "weapon+aim+fire+holster", "idle+heal" };
			if (g_log) g_log("bot: phase %d (%s)", phase, names[phase]);
		}

		float pos[3]{ g_anchor[0], g_anchor[1], g_anchor[2] };
		float rot[4]{ 0, 0, 0, 1 };
		uint16_t stance = 0;
		bool firing = false;
		float aimPitch = 0.0f;
		uint64_t wantWeapon = 0;

		if (phase == 0) {
			// Круг R=2.8 м со скоростью ~1.8 м/с (w=0.65 рад/с), пустые руки.
			const float a = tSec * 0.65f;
			pos[0] += std::cos(a) * 2.8f;
			pos[2] += std::sin(a) * 2.8f;
			YawQuat(std::atan2(-std::sin(a), std::cos(a)), rot);
		} else if (phase == 1) {
			// Быстрый полукруг R=4.6 м (~3.6 м/с), присед во 2-й половине + фонарик.
			const float a = tSec * 0.8f;
			pos[0] += std::cos(a) * 4.6f;
			pos[2] += std::sin(a) * 4.6f;
			YawQuat(std::atan2(-std::sin(a), std::cos(a)), rot);
			stance |= net::kFlagFlashlight;
			if (pSec >= 1.5f) stance |= net::kFlagCrouch;
		} else if (phase == 2) {
			// Достать оружие, прицелиться и выстрелить очередью, в конце убрать в кобуру.
			pos[0] += 2.6f;
			pos[2] += 1.2f;
			YawQuat(-1.2f + std::sin(pSec * 2.0f) * 0.4f, rot);
			if (pSec < 3.0f) {
				wantWeapon = g_weaponSid;
				stance |= net::kFlagAim;
				aimPitch = std::sin(pSec * 3.0f) * 0.35f;
				const int subMs = static_cast<int>(inCycle % 800);
				if (pSec > 0.6f && subMs < 180) firing = true;
			}
		} else {
			// Стойка на точке + жест лечения.
			pos[0] += 2.2f;
			pos[2] -= 1.0f;
			YawQuat(0.5f, rot);
			if (pSec >= 1.0f && pSec < 2.6f) stance |= net::kFlagHealing;
		}

		if (wantWeapon != g_sentWeapon) {
			g_sentWeapon = wantWeapon;
			g_bot->SendWeapon(wantWeapon);
		}
		g_bot->SendPose(pos, rot, true, firing, 0, {}, stance, nowMs, aimPitch);
	}

	if (!g_spawnedAt && puppet::Current()) {
		g_spawnedAt = nowMs;
		if (g_log) g_log("bot: puppet %016llX spawned in %llu ms",
			static_cast<unsigned long long>(puppet::Current()),
			static_cast<unsigned long long>(g_spawnedAt - g_startedAt));
	}
	const float err = puppet::LastErrorMeters();
	if (err > g_maxErrM && err < 200.0f) g_maxErrM = err;
	if (anim::PuppetTposeStreak() >= 3) ++g_tposeEvents;

	if (nowMs - g_lastReportAt >= 4000) {
		g_lastReportAt = nowMs;
		if (g_log) g_log("bot: status link=%d puppet=%016llX err=%.2f m (max %.2f m) tpose=%u",
			static_cast<int>(g_bot->GetState()), static_cast<unsigned long long>(puppet::Current()),
			err, g_maxErrM, g_tposeEvents);
	}
}

}  // namespace bot
