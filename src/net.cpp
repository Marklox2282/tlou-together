#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstring>
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
	const char* (*GetFriendPersonaName)(void*, uint64_t) = nullptr;
	uint32_t (*GetAppID)(void*) = nullptr;
	void (*IdentSet)(void*, uint64_t) = nullptr;
	uint64_t (*IdentGet)(const void*) = nullptr;
	int (*SendToUser)(void*, const void*, const void*, uint32_t, int, int) = nullptr;
	int (*RecvOnChannel)(void*, int, void**, int) = nullptr;
	bool (*CloseSession)(void*, const void*) = nullptr;
	void (*MsgRelease)(void*) = nullptr;
	uint32_t appId = 0;
} g_steam;

struct Ident { uint8_t raw[136]; };  // SteamNetworkingIdentity
struct FriendGameInfo { uint64_t gameId; uint32_t ip; uint16_t gamePort, queryPort; uint64_t lobby; };
constexpr int kFriendFlagImmediate = 0x04;
constexpr int kSendFlags = 1 | 32;  // UnreliableNoNagle | AutoRestartBrokenSession
constexpr int kChannel = 7;

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
	Sym(m, "SteamAPI_SteamUtils_v010", utils);
	Sym(m, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004", nutils);
	Sym(m, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess", initRelay);
	Sym(m, "SteamAPI_ISteamFriends_GetPersonaName", s.GetPersonaName);
	Sym(m, "SteamAPI_ISteamUser_GetSteamID", s.GetSteamID);
	Sym(m, "SteamAPI_ISteamFriends_GetFriendCount", s.GetFriendCount);
	Sym(m, "SteamAPI_ISteamFriends_GetFriendByIndex", s.GetFriendByIndex);
	Sym(m, "SteamAPI_ISteamFriends_GetFriendGamePlayed", s.GetFriendGamePlayed);
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
	if (nutils && initRelay)
		if (void* nu = nutils()) initRelay(nu);
	s.ok = true;
	log("net: Steam ready, appid %u, me %s", s.appId, s.GetPersonaName(s.friends));
	return true;
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

bool Valid(const Packet& p, int len) {
	return len == static_cast<int>(sizeof(Packet)) && p.magic == kMagic && p.version == kVersion;
}

}  // namespace

bool Link::Start(const Config& cfg, uint64_t nowMs) {
	Stop();
	steam_ = cfg.peerIp.empty();
	if (steam_) {
		if (!SteamInit(log_)) {
			log_("net: Steam is unavailable; set peer_ip in tlou1-coop.ini for LAN play");
			return false;
		}
	} else {
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa)) return false;
		in_addr a{};
		if (inet_pton(AF_INET, cfg.peerIp.c_str(), &a) != 1) {
			log_("net: bad peer_ip \"%s\"", cfg.peerIp.c_str());
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
			return false;
		}
		sock_ = s;
		peerAddr_ = a.s_addr;
		peerPort_ = htons(cfg.peerPort);
		peerName_ = cfg.peerIp;
	}
	state_ = State::Searching;
	lastHello_ = 0;
	lastHeard_ = nowMs;
	log_("net: searching (%s)", steam_ ? "Steam friends in this game" : cfg.peerIp.c_str());
	return true;
}

void Link::Stop() {
	if (state_ == State::Connected) {
		Packet bye;
		bye.type = kBye;
		Send(bye);
	}
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
	lastSeq_ = 0;
}

void Link::SendTo(uint64_t steamId, const Packet& p) {
	if (steam_) {
		const Ident id = MakeIdent(steamId);
		g_steam.SendToUser(g_steam.msgs, id.raw, &p, sizeof(p), kSendFlags, kChannel);
	} else {
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = peerAddr_;
		to.sin_port = peerPort_;
		sendto(static_cast<SOCKET>(sock_), reinterpret_cast<const char*>(&p), sizeof(p), 0,
			reinterpret_cast<sockaddr*>(&to), sizeof(to));
	}
}

void Link::Send(const Packet& p) {
	if (!steam_ || peerSteam_) SendTo(peerSteam_, p);
}

bool Link::Receive(Packet& p, uint64_t& fromSteam) {
	fromSteam = 0;
	if (steam_) {
		void* msg = nullptr;
		if (g_steam.RecvOnChannel(g_steam.msgs, kChannel, &msg, 1) != 1 || !msg) return false;
		const auto* m = static_cast<const uint8_t*>(msg);
		const void* data = *reinterpret_cast<const void* const*>(m + 0x00);  // m_pData
		const int len = *reinterpret_cast<const int*>(m + 0x08);              // m_cbSize
		fromSteam = g_steam.IdentGet(m + 0x10);                               // m_identityPeer
		const bool ok = data && len == static_cast<int>(sizeof(Packet));
		if (ok) memcpy(&p, data, sizeof(p));
		g_steam.MsgRelease(msg);
		return ok ? Valid(p, len) : Receive(p, fromSteam);
	}
	sockaddr_in from{};
	int fromLen = sizeof(from);
	for (;;) {
		const int n = recvfrom(static_cast<SOCKET>(sock_), reinterpret_cast<char*>(&p), sizeof(p), 0,
			reinterpret_cast<sockaddr*>(&from), &fromLen);
		if (n == SOCKET_ERROR) return false;  // WSAEWOULDBLOCK - пусто; ICMP-ошибки тоже пропускаем
		if (from.sin_addr.s_addr == peerAddr_ && from.sin_port == peerPort_ && Valid(p, n)) return true;
	}
}

void Link::OnConnected(uint64_t steamId, uint64_t nowMs) {
	peerSteam_ = steamId;
	if (steam_) peerName_ = g_steam.GetFriendPersonaName(g_steam.friends, steamId);
	state_ = State::Connected;
	lastHeard_ = nowMs;
	lastSeq_ = 0;
	log_("net: connected to %s", peerName_.c_str());
	MessageBeep(MB_OK);
}

bool Link::Tick(uint64_t nowMs, Packet& pose) {
	if (state_ == State::Off) return false;
	// Приветствие раз в секунду, пока не нашли друг друга. Отправка в Steam открывает сессию,
	// поэтому оба игрока жмут F8: каждый шлёт другому, и сессии принимаются без запроса.
	if (state_ == State::Searching && nowMs - lastHello_ >= 1000) {
		lastHello_ = nowMs;
		Packet hello;
		hello.type = kHello;
		if (steam_)
			for (uint64_t id : FriendsInThisGame()) SendTo(id, hello);
		else
			SendTo(0, hello);
	}
	bool gotPose = false;
	Packet p;
	uint64_t from = 0;
	while (Receive(p, from)) {
		if (state_ == State::Searching) {
			if (p.type == kBye) continue;
			OnConnected(from, nowMs);
			Packet hello;
			hello.type = kHello;
			Send(hello);  // чтобы второй тоже сразу узнал
		}
		if (steam_ && from != peerSteam_) continue;
		lastHeard_ = nowMs;
		if (p.type == kBye) {
			log_("net: %s left", peerName_.c_str());
			state_ = State::Searching;
			peerSteam_ = 0;
			continue;
		}
		// Без надёжной доставки пакеты могут прийти не по порядку: старые позы отбрасываем.
		if (p.type == kPose && static_cast<int32_t>(p.seq - lastSeq_) > 0) {
			lastSeq_ = p.seq;
			pose = p;
			gotPose = true;
		}
	}
	if (state_ == State::Connected && nowMs - lastHeard_ > 10000) {
		log_("net: no data from %s for 10 s, searching again", peerName_.c_str());
		state_ = State::Searching;
		peerSteam_ = 0;
	}
	return gotPose;
}

void Link::SendPose(const float pos[3], const float rot[4]) {
	if (state_ != State::Connected) return;
	Packet p;
	p.type = kPose;
	p.seq = ++seq_;
	memcpy(p.pos, pos, sizeof(p.pos));
	memcpy(p.rot, rot, sizeof(p.rot));
	Send(p);
}

}  // namespace net
