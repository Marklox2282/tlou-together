#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <vector>

#include "net.hpp"

namespace net {

namespace {

// ---- Steam: те же вызовы, что в TLOU2-Coop, через steam_api64.dll самой игры ----------------
struct Steam {
	bool tried = false, ok = false;
	void *friends = nullptr, *user = nullptr, *msgs = nullptr, *utils = nullptr;
	const char* (*GetPersonaName)(void*) = nullptr;
	uint64_t (*GetSteamID)(void*) = nullptr;
	int (*GetFriendCount)(void*, int) = nullptr;
	uint64_t (*GetFriendByIndex)(void*, int, int) = nullptr;
	bool (*GetFriendGamePlayed)(void*, uint64_t, void*) = nullptr;
	int (*GetFriendRelationship)(void*, uint64_t) = nullptr;
	const char* (*GetFriendPersonaName)(void*, uint64_t) = nullptr;
	uint32_t (*GetAppID)(void*) = nullptr;
	void (*IdentSet)(void*, uint64_t) = nullptr;
	uint64_t (*IdentGet)(const void*) = nullptr;
	int (*SendToUser)(void*, const void*, const void*, uint32_t, int, int) = nullptr;
	int (*RecvOnChannel)(void*, int, void**, int) = nullptr;
	bool (*CloseSession)(void*, const void*) = nullptr;
	void (*MsgRelease)(void*) = nullptr;
	void* mm = nullptr;  // ISteamMatchmaking
	uint64_t (*CreateLobby)(void*, int, int) = nullptr;
	uint64_t (*JoinLobby)(void*, uint64_t) = nullptr;
	void (*LeaveLobby)(void*, uint64_t) = nullptr;
	uint64_t (*GetLobbyOwner)(void*, uint64_t) = nullptr;
	int (*GetNumLobbyMembers)(void*, uint64_t) = nullptr;
	uint64_t (*GetLobbyMember)(void*, uint64_t, int) = nullptr;
	bool (*SetLobbyData)(void*, uint64_t, const char*, const char*) = nullptr;
	void (*InviteDialog)(void*, uint64_t) = nullptr;
	bool (*CallDone)(void*, uint64_t, bool*) = nullptr;
	bool (*CallResult)(void*, uint64_t, void*, int, int, bool*) = nullptr;
	void (*RegisterCallback)(void*, int) = nullptr;
	uint32_t appId = 0;
} g_steam;

struct Ident { uint8_t raw[136]; };  // SteamNetworkingIdentity
struct FriendGameInfo { uint64_t gameId; uint32_t ip; uint16_t gamePort, queryPort; uint64_t lobby; };
constexpr int kFriendFlagImmediate = 0x04;
constexpr int kSendFlags = 1 | 32;  // UnreliableNoNagle | AutoRestartBrokenSession
constexpr int kSendReliable = 8 | 32;  // Reliable | AutoRestartBrokenSession
constexpr int kChannel = 7;

// ---- приглашение в лобби, принятое в Steam (GameLobbyJoinRequested_t, 333) -------------------
// Объект с раскладкой CCallbackBase из steamworks (виртуальные Run/Run/GetCallbackSizeBytes,
// флаги и номер события); SteamAPI_RunCallbacks игры вызывает Run в своём потоке.
std::atomic<uint64_t> g_invite{ 0 };

class CallbackBase {
public:
	virtual void Run(void* param) = 0;
	virtual void Run(void* param, bool ioFailure, uint64_t call) = 0;
	virtual int GetCallbackSizeBytes() = 0;
	uint8_t flags = 0;
	int callback = 0;
};

struct LobbyJoinRequested { uint64_t lobby, friendId; };
constexpr int kLobbyJoinRequested = 333, kLobbyCreated = 513, kLobbyEnter = 504;

class InviteCallback final : public CallbackBase {
public:
	void Run(void* param) override { g_invite = static_cast<const LobbyJoinRequested*>(param)->lobby; }
	void Run(void* param, bool, uint64_t) override { Run(param); }
	int GetCallbackSizeBytes() override { return sizeof(LobbyJoinRequested); }
} g_inviteCallback;

void RegisterInviteCallback();

template <typename T>
void Sym(HMODULE m, const char* name, T& out) { out = reinterpret_cast<T>(GetProcAddress(m, name)); }

bool SteamInit(Link::LogFn log) {
	Steam& s = g_steam;
	if (s.tried) return s.ok;
	s.tried = true;
	HMODULE m = GetModuleHandleW(L"steam_api64.dll");
	if (!m) return false;
	void* (*friends)() = nullptr, *(*user)() = nullptr, *(*msgs)() = nullptr, *(*utils)() = nullptr, *(*nutils)() = nullptr;
	void (*initRelay)(void*) = nullptr;
	Sym(m, "SteamAPI_SteamFriends_v017", friends);
	Sym(m, "SteamAPI_SteamUser_v021", user);  // в Part I v021, в Part II v023; GetSteamID одинаков
	Sym(m, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002", msgs);
	void* (*mm)() = nullptr;
	Sym(m, "SteamAPI_SteamMatchmaking_v009", mm);
	Sym(m, "SteamAPI_ISteamMatchmaking_CreateLobby", s.CreateLobby);
	Sym(m, "SteamAPI_ISteamMatchmaking_JoinLobby", s.JoinLobby);
	Sym(m, "SteamAPI_ISteamMatchmaking_LeaveLobby", s.LeaveLobby);
	Sym(m, "SteamAPI_ISteamMatchmaking_GetLobbyOwner", s.GetLobbyOwner);
	Sym(m, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers", s.GetNumLobbyMembers);
	Sym(m, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex", s.GetLobbyMember);
	Sym(m, "SteamAPI_ISteamMatchmaking_SetLobbyData", s.SetLobbyData);
	Sym(m, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog", s.InviteDialog);
	Sym(m, "SteamAPI_ISteamUtils_IsAPICallCompleted", s.CallDone);
	Sym(m, "SteamAPI_ISteamUtils_GetAPICallResult", s.CallResult);
	Sym(m, "SteamAPI_RegisterCallback", s.RegisterCallback);
	Sym(m, "SteamAPI_SteamUtils_v010", utils);
	Sym(m, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004", nutils);
	Sym(m, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess", initRelay);
	Sym(m, "SteamAPI_ISteamFriends_GetPersonaName", s.GetPersonaName);
	Sym(m, "SteamAPI_ISteamUser_GetSteamID", s.GetSteamID);
	Sym(m, "SteamAPI_ISteamFriends_GetFriendCount", s.GetFriendCount);
	Sym(m, "SteamAPI_ISteamFriends_GetFriendByIndex", s.GetFriendByIndex);
	Sym(m, "SteamAPI_ISteamFriends_GetFriendGamePlayed", s.GetFriendGamePlayed);
	Sym(m, "SteamAPI_ISteamFriends_GetFriendRelationship", s.GetFriendRelationship);
	Sym(m, "SteamAPI_ISteamFriends_GetFriendPersonaName", s.GetFriendPersonaName);
	Sym(m, "SteamAPI_ISteamUtils_GetAppID", s.GetAppID);
	Sym(m, "SteamAPI_SteamNetworkingIdentity_SetSteamID64", s.IdentSet);
	Sym(m, "SteamAPI_SteamNetworkingIdentity_GetSteamID64", s.IdentGet);
	Sym(m, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser", s.SendToUser);
	Sym(m, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel", s.RecvOnChannel);
	Sym(m, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser", s.CloseSession);
	Sym(m, "SteamAPI_SteamNetworkingMessage_t_Release", s.MsgRelease);
	if (!friends || !user || !msgs || !utils || !s.GetPersonaName || !s.GetSteamID || !s.GetFriendCount ||
		!s.GetFriendByIndex || !s.GetFriendGamePlayed || !s.GetFriendPersonaName || !s.GetAppID || !s.IdentSet ||
		!s.IdentGet || !s.SendToUser || !s.RecvOnChannel || !s.CloseSession || !s.MsgRelease) {
		log("net: steam_api64 is missing exports");
		return false;
	}
	s.friends = friends();
	s.user = user();
	s.msgs = msgs();
	s.utils = utils();
	if (!s.friends || !s.user || !s.msgs || !s.utils) {
		log("net: Steam interfaces unavailable");
		return false;
	}
	s.appId = s.GetAppID(s.utils);
	if (mm && s.CreateLobby && s.JoinLobby && s.LeaveLobby && s.GetLobbyOwner && s.GetNumLobbyMembers && s.GetLobbyMember &&
		s.SetLobbyData && s.InviteDialog &&
		s.CallDone && s.CallResult)
		s.mm = mm();
	if (!s.mm) log("net: Steam lobbies unavailable");
	RegisterInviteCallback();
	if (nutils && initRelay)
		if (void* nu = nutils()) initRelay(nu);
	s.ok = true;
	log("net: Steam ready, appid %u, me %s", s.appId, s.GetPersonaName(s.friends));
	return true;
}

void RegisterInviteCallback() {
	static bool done = false;
	if (done || !g_steam.RegisterCallback) return;
	done = true;
	g_inviteCallback.callback = kLobbyJoinRequested;
	g_steam.RegisterCallback(&g_inviteCallback, kLobbyJoinRequested);
}

// Steam запускает игру по приглашению с "+connect_lobby <id>" в командной строке.
uint64_t LobbyFromCommandLine() {
	const char* cmd = GetCommandLineA();
	const char* p = cmd ? strstr(cmd, "+connect_lobby") : nullptr;
	return p ? _strtoui64(p + strlen("+connect_lobby"), nullptr, 10) : 0;
}

Ident MakeIdent(uint64_t id) {
	Ident i{};
	g_steam.IdentSet(i.raw, id);
	return i;
}

std::vector<uint64_t> FriendsInThisGame() {
	std::vector<uint64_t> out;
	const Steam& s = g_steam;
	const int n = s.GetFriendCount(s.friends, kFriendFlagImmediate);
	for (int i = 0; i < n && i < 2000; ++i) {
		const uint64_t id = s.GetFriendByIndex(s.friends, i, kFriendFlagImmediate);
		FriendGameInfo info{};
		if (id && s.GetFriendGamePlayed(s.friends, id, &info) && (info.gameId & 0xFFFFFF) == s.appId) out.push_back(id);
	}
	return out;
}

// Пакет нашего мода, но другого протокола (например, v7 без рукопожатия): сообщаем один раз.
enum class Kind { Ok, OkAnim, OtherProtocol, Junk };

Kind Classify(const void* data, int len) {
	uint32_t magic = 0;
	if (!data || len < 5) return Kind::Junk;
	memcpy(&magic, data, 4);
	if (magic != kMagic) return Kind::Junk;
	const auto* b = static_cast<const uint8_t*>(data);
	if (b[4] != kProtocol) return Kind::OtherProtocol;
	if (len >= 8 && b[5] == kAnim && len <= 4096) {
		uint16_t payload = 0;
		memcpy(&payload, b + 6, 2);
		return payload >= 20 && 8 + static_cast<int>(payload) == len ? Kind::OkAnim : Kind::Junk;
	}
	return len == static_cast<int>(sizeof(Packet)) ? Kind::Ok : Kind::OtherProtocol;
}

}  // namespace

bool Link::Start(const Config& cfg, uint64_t nowMs) {
	Stop();
	steam_ = cfg.peerIp.empty();
	if (steam_) {
		if (!SteamInit(log_)) {
			log_("net: Steam is unavailable; set peer_ip in tlou-together.ini for LAN play");
			return false;
		}
		if (name_.empty()) name_ = g_steam.GetPersonaName(g_steam.friends);
	} else {
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa)) return false;
		in_addr a{};
		if (inet_pton(AF_INET, cfg.peerIp.c_str(), &a) != 1) {
			log_("net: bad peer_ip \"%s\"", cfg.peerIp.c_str());
			WSACleanup();
			return false;
		}
		const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in local{};
		local.sin_family = AF_INET;
		local.sin_port = htons(cfg.port);
		u_long nonBlocking = 1;
		if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) ||
			ioctlsocket(s, FIONBIO, &nonBlocking)) {
			log_("net: can't open UDP port %u (error %d)", cfg.port, WSAGetLastError());
			if (s != INVALID_SOCKET) closesocket(s);
			WSACleanup();
			return false;
		}
		sock_ = s;
		port_ = cfg.port;
		peerAddr_ = a.s_addr;
		peerPort_ = htons(cfg.peerPort);
	}
	state_ = State::Searching;
	lastHello_ = 0;
	lastHeard_ = nowMs;
	oldProtocolLogged_ = false;
	log_("net: searching (%s), I am \"%s\", mod v%u", steam_ ? "Steam friends in this game" : cfg.peerIp.c_str(),
		name_.c_str(), modVersion_);
	return true;
}

void Link::Stop() {
	if (state_ == State::Connected) Send(Make(kBye));
	if (steam_ && peerSteam_ && g_steam.ok) {
		const Ident id = MakeIdent(peerSteam_);
		g_steam.CloseSession(g_steam.msgs, id.raw);
	}
	if (sock_ != ~uintptr_t(0)) {
		closesocket(static_cast<SOCKET>(sock_));
		WSACleanup();
		sock_ = ~uintptr_t(0);
	}
	if (state_ != State::Off) log_("net: off");
	state_ = State::Off;
	peerSteam_ = 0;
	peerChapter_ = {};
	lastSeq_ = 0;
	peerDifficulty_ = -1;
	peerWeapon_ = 0;
	partnerStarted_ = false;
	ping_ = -1;
	myReady_ = false;
	peerReady_ = false;
	myCutscene_ = false;
	peerCutscene_ = false;
	lastReadySent_ = 0;
	lastSentChapter_ = {};
	enemyEvents_.clear();
	animFrames_.clear();
}

Packet Link::Make(uint8_t type) const {
	Packet p;
	p.type = type;
	p.modVersion = modVersion_;
	p.chapterNode = chapter_.first;
	p.chapterTask = chapter_.second;
	if (myReady_) p.flags |= kFlagReady;
	if (myCutscene_) p.flags |= kFlagCutscene;
	if (type == kHello) {
		strncpy_s(p.name, name_.c_str(), _TRUNCATE);
		p.target = joinSecret_;  // гость лобби: секрет из кода
	}
	return p;
}

void Link::SendTo(uint64_t steamId, const Packet& p, bool reliable) {
	if (steam_) {
		const Ident id = MakeIdent(steamId);
		g_steam.SendToUser(g_steam.msgs, id.raw, &p, sizeof(p), reliable ? kSendReliable : kSendFlags, kChannel);
	} else {
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = peerAddr_;
		to.sin_port = peerPort_;
		sendto(static_cast<SOCKET>(sock_), reinterpret_cast<const char*>(&p), sizeof(p), 0,
			reinterpret_cast<sockaddr*>(&to), sizeof(to));
	}
}

void Link::Send(const Packet& p, bool reliable) {
	if (!steam_ || peerSteam_) SendTo(peerSteam_, p, reliable);
}

void Link::SendRawBytes(const void* data, size_t len, bool reliable) {
	if (!data || !len) return;
	if (steam_) {
		if (!peerSteam_) return;
		const Ident id = MakeIdent(peerSteam_);
		g_steam.SendToUser(g_steam.msgs, id.raw, data, static_cast<int>(len), reliable ? kSendReliable : kSendFlags, kChannel);
	} else {
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = peerAddr_;
		to.sin_port = peerPort_;
		sendto(static_cast<SOCKET>(sock_), static_cast<const char*>(data), static_cast<int>(len), 0,
			reinterpret_cast<sockaddr*>(&to), sizeof(to));
	}
}

bool Link::Receive(Packet& p, std::vector<uint8_t>& animOut, bool& isAnim, uint64_t& fromSteam) {
	for (;;) {
		Kind kind = Kind::Junk;
		fromSteam = 0;
		isAnim = false;
		if (steam_) {
			void* msg = nullptr;
			if (g_steam.RecvOnChannel(g_steam.msgs, kChannel, &msg, 1) != 1 || !msg) return false;
			const auto* m = static_cast<const uint8_t*>(msg);
			const void* data = *reinterpret_cast<const void* const*>(m + 0x00);  // m_pData
			const int len = *reinterpret_cast<const int*>(m + 0x08);              // m_cbSize
			fromSteam = g_steam.IdentGet(m + 0x10);                               // m_identityPeer
			kind = Classify(data, len);
			if (kind == Kind::Ok) memcpy(&p, data, sizeof(p));
			else if (kind == Kind::OkAnim) {
				const auto* b = static_cast<const uint8_t*>(data);
				animOut.assign(b + 8, b + len);
				isAnim = true;
			}
			g_steam.MsgRelease(msg);
		} else {
			char buf[4096];
			sockaddr_in from{};
			int fromLen = sizeof(from);
			const int n = recvfrom(static_cast<SOCKET>(sock_), buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
			if (n == SOCKET_ERROR) return false;  // WSAEWOULDBLOCK - пусто; ICMP-ошибки тоже пропускаем
			if (from.sin_addr.s_addr != peerAddr_ || from.sin_port != peerPort_) continue;
			kind = Classify(buf, n);
			if (kind == Kind::Ok) memcpy(&p, buf, sizeof(p));
			else if (kind == Kind::OkAnim) {
				animOut.assign(reinterpret_cast<const uint8_t*>(buf) + 8, reinterpret_cast<const uint8_t*>(buf) + n);
				isAnim = true;
			}
		}
		if (kind == Kind::Ok || kind == Kind::OkAnim) return true;
		if (kind == Kind::OtherProtocol && !oldProtocolLogged_) {
			oldProtocolLogged_ = true;
			log_("net: got a packet from another version of the mod; both players need the latest version");
		}
	}
}

void Link::OnHello(const Packet& p, uint64_t from, uint64_t nowMs) {
	char name[sizeof(p.name) + 1]{};
	memcpy(name, p.name, sizeof(p.name));
	if (from) {
		bool known = false;
		for (Heard& h : heard_)
			if (h.steamId == from) h = { from, p.modVersion, nowMs }, known = true;
		if (!known && heard_.size() < 64) heard_.push_back({ from, p.modVersion, nowMs });
	}
	// Своё лобби: незнакомцев, нашедших его поиском, не подключаем.
	if (steam_ && lobby_ && lobbyHost_ && state_ != State::Connected) {
		const bool isFriend = g_steam.GetFriendRelationship && g_steam.GetFriendRelationship(g_steam.friends, from) == 3;
		if (!Admit(secret_, p.target, isFriend)) {
			if (!lastRejectLog_ || nowMs - lastRejectLog_ >= 10000) {
				lastRejectLog_ = nowMs;
				log_("net: %s is in the lobby without the code's secret and is not a Steam friend - not connecting", name);
			}
			return;
		}
	}
	if (p.modVersion != modVersion_) {
		// Разные версии мода не соединяем: структуры и смещения могут отличаться.
		if (!lastMismatchLog_ || nowMs - lastMismatchLog_ >= 10000) {
			lastMismatchLog_ = nowMs;
			log_("net: %s has mod v%u, I have v%u - not connecting until both have the same version",
				name, p.modVersion, modVersion_);
		}
		return;
	}
	if (state_ == State::Connected) {
		if (!steam_ || from == peerSteam_) Send(Make(kHello));  // второй ещё ищет: наш ответ потерялся
		return;
	}
	peerSteam_ = from;
	peerName_ = name[0] ? name : (steam_ ? g_steam.GetFriendPersonaName(g_steam.friends, from) : "partner");
	peerChapter_ = { p.chapterNode, p.chapterTask };
	peerReady_ = (p.flags & kFlagReady) != 0;
	peerCutscene_ = (p.flags & kFlagCutscene) != 0;
	state_ = State::Connected;
	lastHeard_ = nowMs;
	lastSeq_ = 0;
	lastReadySent_ = 0;
	lastSentChapter_ = {};
	log_("net: connected to %s", peerName_.c_str());
	if (beep_) MessageBeep(MB_OK);
	Send(Make(kHello));  // чтобы второй тоже сразу узнал
}

void Link::Disconnected(const char* why) {
	log_("net: %s %s, searching again", peerName_.c_str(), why);
	state_ = State::Searching;
	peerSteam_ = 0;
	peerChapter_ = {};
	peerReady_ = false;
	peerCutscene_ = false;
	lastReadySent_ = 0;
	lastSentChapter_ = {};
	lastHello_ = 0;
	ping_ = -1;
}

bool Link::Tick(uint64_t nowMs, Packet& pose) {
	PollLobby(nowMs);
	if (state_ == State::Off) return false;
	// Приветствие раз в секунду, пока не нашли друг друга. Отправка в Steam открывает сессию,
	// поэтому оба игрока жмут F8: каждый шлёт другому, и сессии принимаются без запроса.
	if (state_ == State::Searching && nowMs - lastHello_ >= 1000) {
		lastHello_ = nowMs;
		const Packet hello = Make(kHello);
		if (steam_ && target_)
			SendTo(target_, hello);  // гость лобби: только хозяину
		else if (steam_ && lobby_ && lobbyHost_) {
			// Хозяин - всем в лобби: гость может быть не другом в Steam, а сообщения от незнакомых
			// Steam принимает, только если мы написали им первыми.
			const uint64_t me = g_steam.GetSteamID(g_steam.user);
			const int n = g_steam.GetNumLobbyMembers(g_steam.mm, lobby_);
			for (int i = 0; i < n && i < 4; ++i)
				if (const uint64_t id = g_steam.GetLobbyMember(g_steam.mm, lobby_, i); id && id != me) SendTo(id, hello);
		}
		else if (steam_)
			for (uint64_t id : FriendsInThisGame()) SendTo(id, hello);
		else
			SendTo(0, hello);
	}
	bool gotPose = false;
	Packet p;
	std::vector<uint8_t> animPayload;
	bool isAnim = false;
	uint64_t from = 0;
	while (Receive(p, animPayload, isAnim, from)) {
		if (!isAnim && p.type == kHello) {
			OnHello(p, from, nowMs);
			if (state_ == State::Connected && (!steam_ || from == peerSteam_)) lastHeard_ = nowMs;
			continue;
		}
		if (state_ != State::Connected || (steam_ && from != peerSteam_)) continue;
		lastHeard_ = nowMs;
		if (isAnim) {
			if (animFrames_.size() >= 16) animFrames_.erase(animFrames_.begin());
			animFrames_.push_back(std::move(animPayload));
			animPayload.clear();
			continue;
		}
		if (p.type == kBye) {
			// Хозяин чужого лобби сам ушёл: ждать в пустом лобби незачем - выходим и мы (после разбора
			// пакетов). Просто пропавшая связь (тишина 10 с) - не выход: остаёмся и переподключаемся.
			if (lobby_ && !lobbyHost_ && from && from == target_) leaveAfterTick_ = true;
			Disconnected("left");
			continue;
		}
		if (p.type == kPing) {  // эхо: время отправителя обратно
			Packet r = Make(kPong);
			r.target = p.target;
			Send(r);
			continue;
		}
		if (p.type == kPong) {
			if (p.target && p.target <= nowMs && nowMs - p.target < 10000) {
				const int rtt = static_cast<int>(nowMs - p.target);
				ping_ = ping_ < 0 ? rtt : (ping_ * 3 + rtt) / 4;
			}
			continue;
		}
		if (p.type == kStart) {
			if (p.chapterNode && p.chapterTask) peerChapter_ = { p.chapterNode, p.chapterTask };
			partnerStarted_ = true;
			continue;
		}
		if (p.type == kReady) {
			peerReady_ = (p.flags & kFlagReady) != 0;
			peerCutscene_ = (p.flags & kFlagCutscene) != 0;
			peerChapter_ = { p.chapterNode, p.chapterTask };
			continue;
		}
		if (p.type == kWeapon) {
			peerWeapon_ = p.target;
			continue;
		}
		if (p.type == kSettings) {
			peerDifficulty_ = static_cast<int>(p.target);
			continue;
		}
		if (p.type == kDeath) {
			partnerDied_ = true;
			continue;
		}
		if (p.type == kEnemy || p.type == kEnemyPos) {
			if (enemyEvents_.size() < 1024) enemyEvents_.push_back(p);
			continue;
		}
		// Без надёжной доставки пакеты могут прийти не по порядку: старые позы отбрасываем.
		if (p.type == kPose && static_cast<int32_t>(p.seq - lastSeq_) > 0) {
			lastSeq_ = p.seq;
			peerChapter_ = { p.chapterNode, p.chapterTask };
			peerReady_ = (p.flags & kFlagReady) != 0;
			peerCutscene_ = (p.flags & kFlagCutscene) != 0;
			pose = p;
			gotPose = true;
		}
	}
	// Периодический обмен готовностью и ранним номером главы (чтобы при загрузке "Продолжить"
	// у хоста партнёр узнал главу и начал загрузку одновременно, не дожидаясь спавна).
	if (state_ == State::Connected && (chapter_ != lastSentChapter_ || nowMs - lastReadySent_ >= 250)) {
		lastSentChapter_ = chapter_;
		lastReadySent_ = nowMs;
		Send(Make(kReady));
	}
	if (state_ == State::Connected && nowMs - lastPing_ >= 1000) {  // пинг раз в секунду
		lastPing_ = nowMs;
		Packet p = Make(kPing);
		p.target = nowMs;
		Send(p);
	}
	if (state_ == State::Connected && nowMs - lastHeard_ > 10000) Disconnected("sent nothing for 10 s");
	if (leaveAfterTick_) {
		leaveAfterTick_ = false;
		log_("net: the lobby host left, leaving the lobby too");
		LeaveLobby();
	}
	return gotPose;
}

void Link::SendPose(const float pos[3], const float rot[4], bool hero, bool firing, uint64_t character, const std::string& look,
	uint16_t stance, uint64_t nowMs, float aimPitch) {
	if (state_ != State::Connected) return;
	Packet p = Make(kPose);
	p.target = character;
	strncpy_s(p.name, look.c_str(), _TRUNCATE);
	p.flags = static_cast<uint16_t>((hero ? kFlagHero : 0) | (firing ? kFlagFiring : 0) |
		(stance & (kFlagCrouch | kFlagAim | kFlagFlashlight)) | (myReady_ ? kFlagReady : 0) | (myCutscene_ ? kFlagCutscene : 0));
	p.modVersion = static_cast<uint32_t>(nowMs ? nowMs : GetTickCount64());
	p.health = aimPitch;
	p.seq = ++seq_;
	memcpy(p.pos, pos, sizeof(p.pos));
	memcpy(p.rot, rot, sizeof(p.rot));
	Send(p);
}

void Link::SendAnim(const void* data, size_t len) {
	if (state_ != State::Connected || !data || len < 20 || len > 4000) return;
	std::vector<uint8_t> buf(8 + len);
	memcpy(buf.data(), &kMagic, 4);
	buf[4] = kProtocol;
	buf[5] = kAnim;
	const uint16_t sz = static_cast<uint16_t>(len);
	memcpy(buf.data() + 6, &sz, 2);
	memcpy(buf.data() + 8, data, len);
	SendRawBytes(buf.data(), buf.size(), false);
}

std::vector<std::vector<uint8_t>> Link::TakeAnimFrames() {
	std::vector<std::vector<uint8_t>> out;
	out.swap(animFrames_);
	return out;
}

void Link::SendEnemy(uint64_t npc, float health, bool dead) {
	if (state_ != State::Connected) return;
	Packet p = Make(kEnemy);
	p.target = npc;
	p.health = health;
	p.flags = dead ? kFlagDead : 0;
	Send(p, true);
}

void Link::SendEnemyPos(uint64_t npc, const float pos[3], const float rot[4]) {
	if (state_ != State::Connected) return;
	Packet p = Make(kEnemyPos);
	p.target = npc;
	memcpy(p.pos, pos, sizeof(p.pos));
	memcpy(p.rot, rot, sizeof(p.rot));
	Send(p);
}

void Link::SendDeath() {
	if (state_ == State::Connected) Send(Make(kDeath), true);
}

void Link::SendSettings(uint32_t difficulty) {
	if (state_ != State::Connected) return;
	Packet p = Make(kSettings);
	p.target = difficulty;
	Send(p);
}

void Link::SendStart() {
	if (state_ == State::Connected) Send(Make(kStart), true);
}

void Link::SetReady(bool ready) {
	if (myReady_ == ready) return;
	myReady_ = ready;
	if (state_ == State::Connected) Send(Make(kReady), true);
}

void Link::SetInCutscene(bool on) {
	if (myCutscene_ == on) return;
	myCutscene_ = on;
	if (state_ == State::Connected) Send(Make(kReady), true);
}

bool Link::TakePartnerStarted() {
	const bool s = partnerStarted_;
	partnerStarted_ = false;
	return s;
}

void Link::SendWeapon(uint64_t weapon) {
	if (state_ != State::Connected) return;
	Packet p = Make(kWeapon);
	p.target = weapon;
	Send(p);
}

bool Link::TakePartnerDeath() {
	const bool d = partnerDied_;
	partnerDied_ = false;
	return d;
}

std::vector<Packet> Link::TakeEnemyEvents() {
	std::vector<Packet> out;
	out.swap(enemyEvents_);
	return out;
}

std::vector<Friend> Link::Friends(uint64_t nowMs) {
	std::vector<Friend> out;
	if (!SteamInit(log_)) return out;
	for (uint64_t id : FriendsInThisGame()) {
		Friend f;
		f.name = g_steam.GetFriendPersonaName(g_steam.friends, id);
		for (const Heard& h : heard_)
			if (h.steamId == id && nowMs - h.atMs < 3000)
				f.status = h.modVersion == modVersion_ ? Friend::Ready : Friend::OtherVersion;
		out.push_back(f);
	}
	return out;
}

bool Link::Authority() const {
	if (lobby_) return lobbyHost_;
	if (steam_) return g_steam.ok && g_steam.GetSteamID(g_steam.user) > peerSteam_;
	return port_ < ntohs(peerPort_);
}

// ---- лобби ---------------------------------------------------------------------------------
// Id лобби Steam: 0x01860000 в старшей половине, номер - в младшей.
constexpr uint64_t kLobbyHigh = 0x0186000000000000ull;
constexpr char kCodeAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // base32 Крокфорда

// 7 символов номера + 4 символа секрета (20 бит): "0D7C-RFW-K3P9".
std::string Link::LobbyCode(const LobbyKey& key) {
	if ((key.lobby & 0xFFFFFFFF00000000ull) != kLobbyHigh) return {};
	uint64_t v = ((key.lobby & 0xFFFFFFFFull) << 20) | (key.secret & 0xFFFFF);
	char c[11];
	for (int i = 10; i >= 0; --i, v >>= 5) c[i] = kCodeAlphabet[v & 31];
	return std::string(c, 4) + "-" + std::string(c + 4, 3) + "-" + std::string(c + 7, 4);
}

Link::LobbyKey Link::LobbyFromCode(const std::string& code) {
	uint64_t v = 0;
	int n = 0;
	for (char ch : code) {
		ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
		if (ch == 'O') ch = '0';
		if (ch == 'I' || ch == 'L') ch = '1';
		const char* p = ch ? strchr(kCodeAlphabet, ch) : nullptr;
		if (!p) {
			if (ch == '-' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
			return {};  // чужой текст в буфере обмена
		}
		v = (v << 5) | static_cast<uint64_t>(p - kCodeAlphabet);
		if (++n > 11) return {};
	}
	if (n != 11) return {};  // и старые коды без секрета (7 символов) - не подходят
	const uint64_t lobby = v >> 20;
	if (!lobby || lobby > 0xFFFFFFFFull) return {};
	return { kLobbyHigh | lobby, static_cast<uint32_t>(v & 0xFFFFF) };
}

uint64_t Link::TakeInvite() {
	static bool commandLineChecked = false;
	if (!commandLineChecked) {
		commandLineChecked = true;
		if (const uint64_t lobby = LobbyFromCommandLine()) return lobby;
	}
	return g_invite.exchange(0);
}

void Link::CreateLobby(uint64_t nowMs) {
	if (!SteamInit(log_) || !g_steam.mm || lobby_ || lobbyCall_) return;
	if (state_ == State::Off) Start(Config{}, nowMs);
	lobbyCall_ = g_steam.CreateLobby(g_steam.mm, 2 /* публичное: войти по коду может и не друг */, 2);
	// Секрет кода - из системного генератора случайных чисел.
	if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&secret_), sizeof(secret_), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		secret_ = static_cast<uint32_t>(GetTickCount64() * 2654435761u);
	secret_ = (secret_ & 0xFFFFF) | 1;  // 20 бит, не 0 (0 - "секрета нет")
	lobbyFailure_ = 0;
	lobbyCallKind_ = 1;
	log_("net: creating a lobby");
}

void Link::JoinLobby(LobbyKey key, uint64_t nowMs) {
	const uint64_t lobby = key.lobby;
	if (!lobby || !SteamInit(log_) || !g_steam.mm || lobby == lobby_) return;
	LeaveLobby();
	Start(Config{}, nowMs);
	joinSecret_ = key.secret;
	lobbyCall_ = g_steam.JoinLobby(g_steam.mm, lobby);
	lobbyFailure_ = 0;
	lobbyCallKind_ = 2;
	log_("net: joining lobby %llu", static_cast<unsigned long long>(lobby));
}

void Link::LeaveLobby() {
	if (lobby_ && g_steam.mm) g_steam.LeaveLobby(g_steam.mm, lobby_);
	if (lobby_) log_("net: left the lobby");
	lobby_ = lobbyCall_ = target_ = 0;
	lobbyHost_ = false;
	Stop();
}

void Link::InviteToLobby() {
	if (lobby_ && g_steam.mm) g_steam.InviteDialog(g_steam.friends, lobby_);
}

std::string Link::LobbyOwnerName() const {
	if (!lobby_ || !g_steam.mm) return {};
	const uint64_t owner = g_steam.GetLobbyOwner(g_steam.mm, lobby_);
	return owner ? g_steam.GetFriendPersonaName(g_steam.friends, owner) : std::string();
}

void Link::PollLobby(uint64_t nowMs) {
	if (!lobbyCall_ || !g_steam.mm) return;
	bool failed = false;
	if (!g_steam.CallDone(g_steam.utils, lobbyCall_, &failed)) return;
	const uint64_t call = lobbyCall_;
	lobbyCall_ = 0;
	if (lobbyCallKind_ == 1) {
		struct { int32_t result; uint64_t lobby; } r{};  // LobbyCreated_t
		if (!g_steam.CallResult(g_steam.utils, call, &r, sizeof(r), kLobbyCreated, &failed) || failed || r.result != 1 || !r.lobby) {
			log_("net: could not create a lobby (result %d)", r.result);
			lobbyFailure_ = 1;
			Stop();
			return;
		}
		lobby_ = r.lobby;
		lobbyHost_ = true;
		g_steam.SetLobbyData(g_steam.mm, lobby_, "tlou1coop", std::to_string(modVersion_).c_str());
		log_("net: lobby %llu created; opening the Steam invite window", static_cast<unsigned long long>(lobby_));
		InviteToLobby();
	} else {
		struct { uint64_t lobby; uint32_t permissions; bool locked; uint32_t response; } r{};  // LobbyEnter_t
		if (!g_steam.CallResult(g_steam.utils, call, &r, sizeof(r), kLobbyEnter, &failed) || failed || r.response != 1) {
			log_("net: could not join the lobby (response %u)", r.response);
			lobbyFailure_ = 2;
			Stop();
			return;
		}
		lobby_ = r.lobby;
		lobbyHost_ = false;
		target_ = g_steam.GetLobbyOwner(g_steam.mm, lobby_);
		if (state_ == State::Off) Start(Config{}, nowMs);
		lastHello_ = 0;  // сразу поприветствовать хозяина
		log_("net: joined %s's lobby", LobbyOwnerName().c_str());
	}
}

}  // namespace net
