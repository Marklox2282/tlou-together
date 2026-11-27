// TLOU Part I co-op: загрузка (прокси winmm), хуки, клавиши, сеть, журнал падений, обновление.
// Основан на TLOU2-Coop (GPL-3.0).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include <safetyhook.hpp>

#include "anim.hpp"
#include "enemies.hpp"
#include "engine.hpp"
#include "clipboard.hpp"
#include "log.hpp"
#include "menu.hpp"
#include "net.hpp"
#include "puppet.hpp"
#include "update.hpp"
#include "winmm-proxy.hpp"

namespace {

// Номер версии мода: увеличивать при каждом выпуске (tools/release.py берёт его отсюда).
// Игроки с разными версиями не соединяются.
constexpr uint32_t kModVersion = 31;

using engine::Locator;

safetyhook::MidHook g_frameHook, g_scriptHook;

bool KeyPressed(int vk, bool& wasDown) {
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000);
	const bool pressed = down && !wasDown;
	wasDown = down;
	return pressed;
}

// ---- сеть ----------------------------------------------------------------------------------
bool g_storySync = true;  // ini story_sync=off - не подтягиваться к чекпоинту партнёра
bool g_sharedDeath = true;  // ini shared_death=off - смерть партнёра не перезагружает нас
bool g_animSync = false;
ULONGLONG g_lastStoryJoin = 0;  // последняя загрузка главы партнёра (общая пауза для всех причин)  // ini anim_sync=on - экспериментальный перенос анимаций игрока (по умолчанию выкл)
// tlou-together.ini рядом с игрой; без peer_ip связь идёт через Steam.
net::Config LoadConfig(std::string& name) {
	net::Config cfg;
	g_storySync = true;
	g_sharedDeath = true;
	g_animSync = false;
	const std::wstring path = GameDirFile(L"tlou-together.ini");
	// Мод назывался tlou1-coop: настройки из старого файла переносим один раз.
	if (const std::wstring old = GameDirFile(L"tlou1-coop.ini"); !path.empty() && !old.empty())
		MoveFileExW(old.c_str(), path.c_str(), 0);  // не затирает новый, если он уже есть
	if (path.empty()) return cfg;
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"r") || !f) {
		if (!_wfopen_s(&f, path.c_str(), L"w") && f) {
			fputs("; tlou-together: co-op for The Last of Us Part I. Lines starting with ; are comments.\n"
				"; name      = shown to your partner; empty = your Steam name.\n"
				"; peer_ip   = empty: F8 connects to a Steam friend who is in this game and also pressed F8.\n"
				";             An IP (LAN): connect directly over UDP instead of Steam.\n"
				"; port      = your UDP port for peer_ip mode; peer_port = your friend's.\n"
				"; character = how your partner sees you: auto (default: the chapter's hero, like them),\n"
				";             companion (the chapter's companion: Tess, Ellie...), or joel, ellie, sarah, tess,\n"
				";             tommy, bill, henry, sam, riley, marlene, maria.\n"
				"; story_sync = on (default): when your partner reaches a new checkpoint (cutscene, car...),\n"
				";             you load it too and stand next to them. off: each plays the story alone.\n"
				"; partner_outline = on (default): your partner is outlined through walls when more than 12 m away.\n"
				"; enemies_see_partner = on (default): enemies notice and attack your partner's character too.\n"
				"; shared_death = on (default): when one of you dies, the other reloads the checkpoint too.\n"
				"; enemy_sync = soft (default): enemies keep their AI, moved to the host's spot off-screen.\n"
				";             full (experimental): the guest's enemies copy the host's moves; they don't attack yet.\n"
				"; anim_sync  = off (default) / on (experimental): direct bone animation replication for partner.\n"
				"name=\npeer_ip=\nport=7777\npeer_port=7777\ncharacter=auto\nanim_sync=off\n", f);
			fclose(f);
		}
		return cfg;
	}
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char* eq = strchr(line, '=');
		if (line[0] == ';' || !eq) continue;
		*eq = 0;
		std::string key = line, value = eq + 1;
		while (!value.empty() && strchr(" \t\r\n", value.back())) value.pop_back();
		while (!value.empty() && strchr(" \t", value.front())) value.erase(0, 1);
		while (!key.empty() && strchr(" \t", key.back())) key.pop_back();
		if (key == "peer_ip") cfg.peerIp = value;
		else if (key == "name") name = value;
		else if (key == "port") cfg.port = static_cast<uint16_t>(atoi(value.c_str()));
		else if (key == "peer_port") cfg.peerPort = static_cast<uint16_t>(atoi(value.c_str()));
		else if (key == "enemy_sync") enemies::SetFullSync(value == "full");
		else if (key == "story_sync") g_storySync = value != "off";
		else if (key == "shared_death") g_sharedDeath = value != "off";
		else if (key == "enemies_see_partner") puppet::SetEnemiesSeePartner(value != "off");
		else if (key == "partner_outline") puppet::SetPartnerOutline(value != "off");
		else if (key == "character") puppet::SetMyCharacter(puppet::CharacterFromName(value));
		else if (key == "anim_sync") g_animSync = (value == "on" || value == "1" || value == "true");
	}
	fclose(f);
	puppet::SetAnimSync(g_animSync);
	return cfg;
}

net::Link g_link(Log, kModVersion);

// Лобби в меню игры: состояние для его текста (меню читает из своего потока) и "Да".
std::atomic<int> g_menuAction{ 0 };  // menu::Action из меню игры
bool g_codeError = false;            // "войти по коду", а в буфере обмена не код

SRWLOCK g_lobbyLock = SRWLOCK_INIT;
menu::Lobby g_lobbyState;

void PublishLobby(ULONGLONG now) {
	static ULONGLONG lastFriends = 0;
	static std::vector<menu::Lobby::Friend> friends;
	if (!lastFriends || now - lastFriends >= 10000) {  // список друзей Steam - раз в 10 с (перебор всех друзей)
		lastFriends = now;
		friends.clear();
		for (const net::Friend& f : g_link.Friends(now)) friends.push_back({ f.name, static_cast<int>(f.status) });
	}
	menu::Lobby s;
	s.friends = friends;
	s.lobby = g_link.Lobby() != 0;
	s.host = g_link.LobbyHost();
	s.busy = g_link.LobbyBusy();
	if (s.lobby && !s.host) s.owner = g_link.LobbyOwnerName();
	s.code = g_link.Code();
	s.codeError = g_codeError;
	s.failure = g_link.LobbyFailure();
	s.version = kModVersion;
	s.myCharacter = puppet::CharacterName(puppet::MyCharacter());
	s.partnerCharacter = puppet::CharacterName(puppet::PartnerCharacter());
	s.heroName = puppet::HeroCharacter();
	s.companionName = puppet::CompanionCharacter();
	s.authority = g_link.Lobby() ? g_link.LobbyHost() : g_link.Authority();
	s.myReady = g_link.MyReady();
	s.partnerReady = g_link.PeerReady();
	s.on = g_link.GetState() != net::State::Off;
	s.connected = g_link.GetState() == net::State::Connected;
	if (s.connected) s.partner = g_link.PeerName();
	AcquireSRWLockExclusive(&g_lobbyLock);
	g_lobbyState = s;
	ReleaseSRWLockExclusive(&g_lobbyLock);
}

menu::Lobby ReadLobby() {
	AcquireSRWLockShared(&g_lobbyLock);
	menu::Lobby s = g_lobbyState;
	ReleaseSRWLockShared(&g_lobbyLock);
	return s;
}

// Все сетевые вызовы - из игрового кадра (основной поток): Link не потокобезопасен.
void NetTick(ULONGLONG now, bool havePlayer, const Locator& me) {
	static bool f8Down = false, f6Down = false;
	static ULONGLONG lastSend = 0;
	static bool wasOn = false;
	const int action = g_menuAction.exchange(0);
	const bool off = g_link.GetState() == net::State::Off;
	std::string name;
	if (KeyPressed(VK_F8, f8Down) && off) {  // F8: без лобби, к любому другу, который тоже нажал F8
		const net::Config cfg = LoadConfig(name);
		g_link.SetName(name);
		g_link.Start(cfg, now);
	} else if ((KeyPressed(VK_F6, f6Down) && !off) || action == menu::Leave) {
		g_link.LeaveLobby();  // и отключиться
	}
	if (action == menu::CreateLobby) {
		LoadConfig(name);
		g_link.SetName(name);
		g_link.CreateLobby(now);
	}
	if (action == menu::Invite) g_link.InviteToLobby();
	if (action == menu::GoToPartner) puppet::RequestTeleport();  // как F7
	if (action == menu::ToggleReady) g_link.SetReady(!g_link.MyReady());
	if (action == menu::StartGame) g_link.SetReady(true);
	if (action == menu::HostLaunch) {
		g_link.SetReady(true);
		g_link.SendStart();  // мгновенный сигнал гостю на Продолжить / Новая игра
	}
	if (puppet::TakeMyDeath()) g_link.SendDeath();
	// Оружие в руках: при смене сразу и раз в 2 с (пакет мог потеряться).
	static ULONGLONG lastWeapon = 0;
	static uint64_t sentWeapon = 0;
	if (g_link.GetState() == net::State::Connected && (puppet::MyWeapon() != sentWeapon || now - lastWeapon >= 2000)) {
		lastWeapon = now;
		sentWeapon = puppet::MyWeapon();
		g_link.SendWeapon(sentWeapon);
	}
	// Сложность: ведущая сторона (хозяин лобби) шлёт свою раз в 3 с, гость играет на ней.
	static ULONGLONG lastSettings = 0;
	const bool linked = g_link.GetState() == net::State::Connected;
	if (linked && g_link.Authority() && now - lastSettings >= 3000 && puppet::MyDifficulty() >= 0) {
		lastSettings = now;
		g_link.SendSettings(static_cast<uint32_t>(puppet::MyDifficulty()));
	}
	puppet::FollowDifficulty(linked && !g_link.Authority() ? g_link.PeerDifficulty() : -1);
	if (g_link.TakePartnerDeath()) {
		Log("death: %s died", g_link.PeerName().c_str());
		menu::Notify(g_link.PeerName() + " died - restarting the checkpoint together",
			g_link.PeerName() + " погиб — перезагружаем чекпоинт вместе");
		if (g_sharedDeath) puppet::PartnerDied();
	}
	if (action == menu::JoinByCode) {
		const std::string text = clipboard::Text();
		const net::Link::LobbyKey key = net::Link::LobbyFromCode(text);
		const uint64_t lobby = key.lobby;
		g_codeError = !lobby;
		Log("net: join by code -> %s", lobby ? "lobby found" : "the clipboard holds no lobby code");  // сам буфер не пишем
		if (lobby) {
			LoadConfig(name);
			g_link.SetName(name);
			g_link.JoinLobby(key, now);
		}
	}
	if (action == menu::CreateLobby) g_codeError = false;
	// Своё новое лобби: код сразу в буфер обмена - остаётся вставить другу в чат.
	static uint64_t copiedLobby = 0;
	if (g_link.Lobby() && g_link.LobbyHost() && g_link.Lobby() != copiedLobby) {
		copiedLobby = g_link.Lobby();
		clipboard::Set(g_link.Code());
		Log("net: lobby code copied to the clipboard");  // сам код в журнал не пишем: в нём секрет
	}
	// Персонаж: кнопка в лобби перебирает варианты (auto, спутник, joel, ellie...).
	if (action == menu::SwapRoles) puppet::SetMyCharacter((puppet::MyCharacter() + 1) % puppet::kCharacterCount);
	if (const uint64_t invite = net::Link::TakeInvite()) {  // приняли приглашение в Steam
		LoadConfig(name);
		g_link.SetName(name);
		g_link.JoinLobby({ invite, 0 }, now);  // приглашение Steam - от друга, секрет не нужен
	}

	net::Packet pose;
	const bool gotPose = g_link.Tick(now, pose);  // и ответы Steam по лобби, даже когда сеть выключена
	const bool on = g_link.GetState() != net::State::Off;
	// Сообщения на экране о подключении и отключении партнёра.
	static bool wasConnected = false;
	const bool connectedNote = g_link.GetState() == net::State::Connected;
	static bool prevHavePlayer = false;
	if (prevHavePlayer && !havePlayer) g_link.SetReady(false);
	prevHavePlayer = havePlayer;
	if (connectedNote != wasConnected) {
		wasConnected = connectedNote;
		menu::Notify(connectedNote ? g_link.PeerName() + " joined" : std::string("Partner disconnected"),
			connectedNote ? g_link.PeerName() + " присоединился" : std::string("Напарник отключился"));
		// В главном меню (игрока ещё нет): показать лобби/готовность заново.
		if (connectedNote && !havePlayer) menu::ShowLobbyAgain();
	}
	static bool wasPeerReady = false;
	const bool peerReadyNow = connectedNote && g_link.PeerReady();
	if (peerReadyNow && !wasPeerReady && !havePlayer) {
		menu::Notify(g_link.PeerName() + " is READY!", g_link.PeerName() + " ГОТОВ!");
		if (g_link.Authority()) menu::ShowLobbyAgain();
	}
	wasPeerReady = peerReadyNow;
	if (on != wasOn) {  // сеть включилась или выключилась (клавиши, меню, лобби)
		wasOn = on;
		if (on) enemies::Reset();
		if (on) menu::Notify("Multiplayer: searching for your partner", "Мультиплеер: поиск напарника");
		puppet::SetNetActive(on);
		if (!on) {
			anim::SetTxEnabled(false);
			puppet::SetPartnerChapter({});
			puppet::UseNetwork(false);
		}
	}
	PublishLobby(now);
	if (!on) return;

	const engine::Chapter mine = puppet::MyChapter();
	g_link.SetChapter({ mine.node, mine.task });
	g_link.SetInCutscene(puppet::MeInCutscene());
	if (gotPose) {
		puppet::SetPartnerFiring((pose.flags & net::kFlagFiring) != 0);
		puppet::SetPartnerWeapon(g_link.PeerWeapon());
		puppet::SetPartnerStance((pose.flags & net::kFlagCrouch) != 0, (pose.flags & net::kFlagAim) != 0);
		puppet::SetPartnerFlashlight((pose.flags & net::kFlagFlashlight) != 0);
		if (std::isfinite(pose.health)) puppet::SetPartnerAimPitch(pose.health);
		puppet::SetPartnerCharacter(puppet::CharacterFromSid(pose.target));
		char look[sizeof(pose.name) + 1]{};
		memcpy(look, pose.name, sizeof(pose.name));
		puppet::SetPartnerLook(look);
	}
	if (gotPose) {
		if (!puppet::UsingNetwork()) {  // первая поза друга: марионетка теперь его
			puppet::UseNetwork(true);
			puppet::SetWanted(true);
			puppet::SetFollow(true);
			Log("net: driving the puppet from %s", g_link.PeerName().c_str());
		}
		Locator l{};
		memcpy(l.pos, pose.pos, sizeof(pose.pos));
		memcpy(l.rot, pose.rot, sizeof(pose.rot));
		puppet::Record(now, l, pose.modVersion);
	}
	const bool connected = g_link.GetState() == net::State::Connected;
	if (connected && g_animSync) {
		for (const auto& frame : g_link.TakeAnimFrames())
			anim::PushRx(frame.data(), frame.size(), now);
		static std::vector<uint8_t> animTx;
		if (havePlayer && anim::PollTx(animTx))
			g_link.SendAnim(animTx.data(), animTx.size());
	} else {
		g_link.TakeAnimFrames();
	}
	const engine::Chapter theirs = connected ? engine::Chapter{ g_link.PeerChapter().first, g_link.PeerChapter().second } : engine::Chapter{};
	puppet::SetPartnerChapter(theirs);
	// Хозяин нажал "Продолжить" / "Новая игра": у гостя само открывается меню сюжета, он жмёт
	// "Продолжить". Когда оба в игре (от хоста пошли позы - их шлют только из игры, не из меню),
	// гость один раз грузит главу хоста. Из главного меню ничего не грузим.
	static bool joinHost = false;
	static ULONGLONG hostStartedAt = 0, lastPoseAt = 0;
	if (gotPose) lastPoseAt = now;
	if (g_link.TakePartnerStarted() && !havePlayer && !g_link.Authority()) {
		const bool opened = menu::StartGameFromPartner();
		Log("net: %s started the game - %s", g_link.PeerName().c_str(), opened ? "opening the story menu" : "open the story yourself");
		menu::Notify(g_link.PeerName() + " started - choose Continue", g_link.PeerName() + " начал игру - выберите «Продолжить»");
		joinHost = true;
		hostStartedAt = now;
	}
	if (!connected || g_link.Authority() || (joinHost && now - hostStartedAt > 300000)) joinHost = false;
	if (joinHost && havePlayer && mine && theirs && lastPoseAt > hostStartedAt && now - lastPoseAt < 1000) {
		joinHost = false;
		if (!(theirs == mine) && now - g_lastStoryJoin > 5000) {
			g_lastStoryJoin = now;
			Log("net: both in the game - loading %s's chapter", g_link.PeerName().c_str());
			puppet::RequestJoin(false);
		}
	}
	// Друг в другой главе - подсказка в журнал при каждой смене.
	static engine::Chapter lastHint;
	if (theirs && mine && !(theirs == mine) && !(theirs == lastHint)) {
		lastHint = theirs;
		Log("net: %s is in another chapter or checkpoint; F5 loads it", g_link.PeerName().c_str());
	}
	// Общие враги: свои удары - партнёру (без подключения просто в журнал), его - в игру.
	for (const enemies::Change& c : enemies::TakeOutgoing()) g_link.SendEnemy(c.npc, c.health, c.dead);
	enemies::SetSendPositions(connected && g_link.Authority());
	for (const enemies::Position& p : enemies::TakePositions()) g_link.SendEnemyPos(p.npc, p.pos, p.rot);
	for (const net::Packet& p : g_link.TakeEnemyEvents()) {
		if (p.type == net::kEnemyPos) {
			enemies::Position e{ p.target, {}, {} };
			memcpy(e.pos, p.pos, sizeof(e.pos));
			memcpy(e.rot, p.rot, sizeof(e.rot));
			enemies::ReceivePosition(e);
		} else {
			enemies::Receive({ p.target, p.health, (p.flags & net::kFlagDead) != 0 });
		}
	}
	// Общий сюжет: партнёр продвинулся (новый чекпоинт, катсцена, пропуск заставки) - синхронизируем
	// чекпоинт быстрее при катсцене (600 мс вместо 2 с) и без долгого 20-секундного блока (6 с).
	// Если оба сменили главу почти одновременно — не-хост следует за хостом.
	static engine::Chapter lastTheirs;
	static ULONGLONG followAt = 0;
	static bool wasSameChapter = false;
	const bool meCut = puppet::MeInCutscene();
	const bool partnerCut = g_link.PeerInCutscene();
	const bool sameChapterNow = connected && theirs && mine && (theirs == mine);
	if (sameChapterNow && !wasSameChapter && g_link.Authority()) enemies::ResendDead();
	wasSameChapter = sameChapterNow;
	if (connected && theirs && mine) {
		if (!(theirs == lastTheirs)) {
			if (lastTheirs && !(theirs == mine)) {
				const ULONGLONG waitMs = (partnerCut || meCut) ? 600 : 2000;
				followAt = now + waitMs;
			}
			lastTheirs = theirs;
		}
		if (followAt && now >= followAt && havePlayer) {  // из главного меню не грузим
			followAt = 0;
			constexpr ULONGLONG kJoinCooldownMs = 20000;
			if (g_storySync && !(theirs == mine) && now - g_lastStoryJoin > kJoinCooldownMs) {
				g_lastStoryJoin = now;
				Log("net: %s reached a new checkpoint%s; loading it to stay together", g_link.PeerName().c_str(),
					partnerCut ? " (cutscene)" : (meCut && !partnerCut ? " (skipped cutscene)" : ""));
				puppet::RequestJoin(false);
			}
		}
	} else if (!connected) {
		lastTheirs = {};
		followAt = 0;
	}
	if (havePlayer && now - lastSend >= 30) {  // ~33 раза в секунду
		lastSend = now;
		g_link.SendPose(me.pos, me.rot, puppet::GetRole() == puppet::Role::Hero, puppet::MeFiring(),
			puppet::CharacterSid(puppet::MyCharacter()), puppet::MyLook(),
			static_cast<uint16_t>((puppet::MeCrouched() ? net::kFlagCrouch : 0) |
				(puppet::MeAiming() ? net::kFlagAim : 0) |
				(puppet::MeFlashlight() ? net::kFlagFlashlight : 0)),
			now, puppet::MeAimPitch());
	}
}

void FrameTick(safetyhook::Context&) {
	static ULONGLONG lastLog = 0;
	static unsigned frames = 0;
	static bool f10Down = false, f9Down = false, f7Down = false, f5Down = false;
	static uint64_t frameUs = 0;
	LARGE_INTEGER start;
	QueryPerformanceCounter(&start);
	++frames;
	puppet::OnFrame();
	menu::OnFrame();
	const ULONGLONG now = GetTickCount64();

	Locator me{};
	const bool havePlayer = engine::PlayerLocator(me);
	NetTick(now, havePlayer, me);
	if (havePlayer && !puppet::UsingNetwork()) {
		puppet::Record(now, me);  // локальная проверка (F10)
		std::vector<uint8_t> soloAnim;
		if (puppet::Wanted() && anim::PollTx(soloAnim)) puppet::RecordLocalAnim(now, std::move(soloAnim));
	}
	// F10: включить/выключить тестовую марионетку (выключение её прячет).
	// Shift+F10 без подключения: сменить свою роль (герой/напарник) для проверки.
	if (KeyPressed(VK_F10, f10Down)) {
		if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
			if (g_link.GetState() != net::State::Connected)
				puppet::SetMyCharacter((puppet::MyCharacter() + 1) % puppet::kCharacterCount);
		} else {
			puppet::SetWanted(!puppet::Wanted());
			Log("F10: puppet %s", puppet::Wanted() ? "on" : "off");
		}
	}
	// F9: марионетка идёт по следу (иначе стоит, где появилась).
	if (KeyPressed(VK_F9, f9Down)) {
		puppet::SetFollow(!puppet::Follow());
		Log("F9: follow %s", puppet::Follow() ? "on" : "off");
	}
	if (KeyPressed(VK_F7, f7Down)) puppet::RequestTeleport();
	// F5: загрузить главу партнёра и встать рядом; Shift+F5 - перезагрузить свою (проверка).
	if (KeyPressed(VK_F5, f5Down)) puppet::RequestJoin((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);

	if (now - lastLog >= 2000) {
		// Сколько мод тратит на кадр: в основном потоке и в скриптовом (среднее за 2 с).
		const uint64_t scriptUs = puppet::TakeWorkMicroseconds();
		const std::string profile = engine::TakeProfile();
		if (havePlayer && frames)
			Log("player pos=(%.2f %.2f %.2f) fps=%.0f mod=%llu+%llu us/frame%s%s", me.pos[0], me.pos[1], me.pos[2],
				frames * 1000.0 / (now - lastLog), frameUs / frames, scriptUs / frames, profile.empty() ? "" : " | ",
				profile.c_str());
		lastLog = now;
		frames = 0;
		frameUs = 0;
	}
	LARGE_INTEGER end, freq;
	QueryPerformanceCounter(&end);
	QueryPerformanceFrequency(&freq);
	frameUs += static_cast<uint64_t>((end.QuadPart - start.QuadPart) * 1000000 / freq.QuadPart);
}

void ScriptInvoke(safetyhook::Context&) {
	puppet::ScriptTick();
	menu::ScriptTick();
}

// ---- журнал падений ------------------------------------------------------------------------
// Windows пишет дамп не на каждое падение, поэтому при ошибке доступа мод сам пишет в
// tlou-together-crash.log адрес, регистры и адреса возврата со стека. Обработку ошибки не меняет.
// Только WriteFile и snprintf: блокировки CRT в момент падения могут быть заняты.
HANDLE g_crashFile = INVALID_HANDLE_VALUE;
std::atomic<int> g_crashCount{ 0 };

void CrashWrite(const char* fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	DWORD written = 0;
	if (n > 0) WriteFile(g_crashFile, buf, static_cast<DWORD>((std::min)(n, static_cast<int>(sizeof(buf)) - 1)), &written, nullptr);
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* e) {
	if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || g_crashCount++ >= 3)
		return EXCEPTION_CONTINUE_SEARCH;
	const uintptr_t base = engine::g_base;
	const CONTEXT& c = *e->ContextRecord;
	auto rva = [base](uint64_t a) { return a >= base && a < base + 0x9000000 ? a - base : 0; };
	CrashWrite("\r\naccess violation v%u tid=%lu rip=tlou+%llX (%llX) %s %llX\r\n", kModVersion, GetCurrentThreadId(), rva(c.Rip),
		c.Rip, e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", e->ExceptionRecord->ExceptionInformation[1]);
	CrashWrite("rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX r8=%llX r9=%llX rsp=%llX\r\n",
		c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.R8, c.R9, c.Rsp);
	CrashWrite("puppet=%016llX wanted=%d follow=%d stack:", puppet::Current(), puppet::Wanted() ? 1 : 0, puppet::Follow() ? 1 : 0);
	const HMODULE self = GetModuleHandleW(L"winmm.dll");
	for (uint64_t p = c.Rsp, found = 0; p < c.Rsp + 0x3000 && found < 40; p += 8) {
		uint64_t v = 0;
		if (!engine::Read(p, v)) break;
		if (rva(v)) { CrashWrite(" tlou+%llX", rva(v)); ++found; }
		else if (self && v >= reinterpret_cast<uint64_t>(self) && v < reinterpret_cast<uint64_t>(self) + 0x100000) {
			CrashWrite(" MOD+%llX", v - reinterpret_cast<uint64_t>(self));
			++found;
		}
	}
	CrashWrite("\r\n");
	return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashLog() {
	const std::wstring path = GameDirFile(L"tlou-together-crash.log");
	if (path.empty()) return;
	g_crashFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (g_crashFile != INVALID_HANDLE_VALUE && AddVectoredExceptionHandler(1, OnException)) Log("crash log: on");
}

// ---- автообновление ------------------------------------------------------------------------
std::wstring SelfPath() {
	HMODULE self = nullptr;
	wchar_t path[MAX_PATH]{};
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&SelfPath), &self);
	const DWORD n = self ? GetModuleFileNameW(self, path, MAX_PATH) : 0;
	return n && n < MAX_PATH ? path : L"";
}

void StartUpdateCheck() {
	static std::wstring self = SelfPath();
	if (self.empty()) return;
	update::CleanupOld(self);
	// Фоновый поток: сеть не трогаем в DllMain и не задерживаем запуск игры.
	if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
			Sleep(5000);
			update::Run(kModVersion, self, update::kReleaseUrl, Log);
			return 0;
		}, nullptr, 0, nullptr))
		CloseHandle(t);
}

void Init() {
	engine::g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	wchar_t path[MAX_PATH]{};
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	const wchar_t* slash = wcsrchr(path, L'\\');
	if (!slash || _wcsicmp(slash + 1, L"tlou-i.exe")) return;
	OpenLog();
	const uint32_t stamp = engine::BuildTimestamp();
	Log("tlou-together v%u, base=%p build=%08X", kModVersion, reinterpret_cast<void*>(engine::g_base), stamp);
	StartUpdateCheck();  // и на неподдерживаемой сборке игры: обновление может её поддержать
	if (stamp != engine::kBuildTimestamp) {
		Log("unsupported game build, mod is off");
		return;
	}
	if (!engine::InitNatives()) {
		Log("natives: MISSING, mod is off");
		return;
	}
	Log("natives: ok");
	auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(engine::g_base + engine::kFrameTickRva), FrameTick);
	if (hook) g_frameHook = std::move(*hook);
	Log("frame hook: %s", g_frameHook ? "ok" : "FAILED");
	auto script = safetyhook::MidHook::create(reinterpret_cast<void*>(engine::g_base + engine::kScriptInvokeRva), ScriptInvoke);
	if (script) g_scriptHook = std::move(*script);
	Log("script hook: %s", g_scriptHook ? "ok" : "FAILED");
	{
		std::string name;
		LoadConfig(name);  // персонаж, anim_sync и режим врагов - сразу, до установки хуков
	}
	if (g_animSync) anim::Init(engine::g_base, Log);
	else Log("anim: off (set anim_sync=on in tlou-together.ini to test)");
	// Код лобби в буфере обмена (для окна "войти по коду"): "0D7C-RFW" или пусто.
	menu::Install(ReadLobby, [](menu::Action a) { g_menuAction = a; },
		[] { return net::Link::LobbyCode(net::Link::LobbyFromCode(clipboard::Text())); });  // проверенный код, не сам буфер
	InstallCrashLog();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(module);
		winmm_proxy::ResolveReal();
		Init();
	}
	return TRUE;
}
