// Связь двух "игроков" по UDP на одном ПК, без игры. Код 0 = все проверки прошли.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/net.hpp"

static std::vector<std::string> g_log;

static void Log(const char* fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("  %s\n", buf);
	g_log.push_back(buf);
}

static bool Logged(const char* part) {
	for (const auto& l : g_log)
		if (l.find(part) != std::string::npos) return true;
	return false;
}

static int g_failed = 0;
static void Check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failed;
}

static uint64_t t = 1000;
static net::Packet pose;

static void Run(net::Link& a, net::Link& b, int steps) {
	for (int i = 0; i < steps; ++i) {
		t += 50;
		a.Tick(t, pose);
		b.Tick(t, pose);
		Sleep(3);
	}
}

static net::Config Cfg(uint16_t port, uint16_t peer) {
	net::Config c;
	c.peerIp = "127.0.0.1";
	c.port = port;
	c.peerPort = peer;
	return c;
}

// Сырой UDP-сокет на порту одного из игроков: имитирует чужие пакеты.
static void SendRaw(uint16_t fromPort, uint16_t toPort, const void* data, int len) {
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in local{}, to{};
	local.sin_family = to.sin_family = AF_INET;
	local.sin_port = htons(fromPort);
	to.sin_port = htons(toPort);
	inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
	bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local));
	sendto(s, static_cast<const char*>(data), len, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
	closesocket(s);
	WSACleanup();
}

int main() {
	const net::Link::Chapter kPrologue{ 0x1111, 0xA }, kBoston{ 0x2222, 0xB };

	puts("== connect, names and chapters");
	{
		net::Link a(Log, 8), b(Log, 8);
		a.SetBeep(false);
		b.SetBeep(false);
		a.SetName("Alice");
		b.SetName("Bob");
		a.SetChapter(kPrologue);
		b.SetChapter(kBoston);
		a.Start(Cfg(7801, 7802), t);
		Run(a, b, 3);  // b ещё не запущен: приветствия a уходят в пустоту
		b.Start(Cfg(7802, 7801), t);
		Run(a, b, 20);
		Check(a.GetState() == net::State::Connected && b.GetState() == net::State::Connected, "both connected");
		Check(a.PeerName() == "Bob" && b.PeerName() == "Alice", "names exchanged");
		Check(a.PeerChapter() == kBoston && b.PeerChapter() == kPrologue, "chapters from hello");

		puts("== pose stream");
		int received = 0;
		float lastX = 0;
		bool contents = true;
		b.SetChapter(kPrologue);
		for (int i = 1; i <= 100; ++i) {
			const float pos[3] = { std::cos(i * 0.1f) * 5, 1.5f, std::sin(i * 0.1f) * 5 }, rot[4] = { 0, 0.3826834f, 0, 0.9238795f };
			a.SendPose(pos, rot);
			b.SendPose(pos, rot);
			Sleep(2);
			t += 50;
			a.Tick(t, pose);
			if (b.Tick(t, pose)) {
				++received;
				lastX = pose.pos[0];
				contents = contents && std::fabs(pose.rot[1] - rot[1]) < 1e-6f && pose.pos[1] == 1.5f;
			}
		}
		Check(received >= 90 && std::fabs(lastX - std::cos(10.0f) * 5) < 1e-4f && contents, "100 poses arrive in order");
		Check(a.PeerChapter() == kPrologue, "chapter change carried by poses");

		puts("== enemy hits and kills");
		a.SendEnemy(0xABCDEF0123456789ull, 42.5f, false);
		a.SendEnemy(0x1111222233334444ull, 0.0f, true);
		Sleep(10);
		Run(a, b, 2);
		const std::vector<net::Packet> ev = b.TakeEnemyEvents();
		Check(ev.size() == 2 && ev[0].target == 0xABCDEF0123456789ull && ev[0].health == 42.5f &&
			!(ev[0].flags & net::kFlagDead) && ev[1].target == 0x1111222233334444ull && (ev[1].flags & net::kFlagDead),
			"enemy events arrive with name, health and death");
		Check(b.TakeEnemyEvents().empty() && a.TakeEnemyEvents().empty(), "events taken once, not echoed");

		puts("== roles");
		Check(a.Authority() != b.Authority(), "exactly one side decides roles");
		b.SendSwap();
		Sleep(10);
		Run(a, b, 2);
		Check(a.TakeSwapRequest() && !a.TakeSwapRequest() && !b.TakeSwapRequest(), "swap request arrives once");
		a.SendPose(pose.pos, pose.rot, false);
		Sleep(10);
		net::Packet got;
		bool seen = false;
		for (int i = 0; i < 3 && !seen; ++i) {
			t += 50;
			seen = b.Tick(t, got);
			Sleep(3);
		}
		Check(seen && !(got.flags & net::kFlagHero), "pose carries the sender's role");

		puts("== bye");
		a.Stop();
		Sleep(10);
		Run(b, b, 1);
		Check(b.GetState() == net::State::Searching, "partner leaving -> searching");
	}

	puts("== different mod versions do not connect");
	{
		g_log.clear();
		net::Link a(Log, 8), b(Log, 9);
		a.SetBeep(false);
		b.SetBeep(false);
		a.Start(Cfg(7811, 7812), t);
		b.Start(Cfg(7812, 7811), t);
		Run(a, b, 40);
		Check(a.GetState() == net::State::Searching && b.GetState() == net::State::Searching, "stay searching");
		Check(Logged("has mod v9, I have v8") && Logged("has mod v8, I have v9"), "mismatch logged on both sides");
	}

	puts("== packet from the old protocol (v7)");
	{
		g_log.clear();
		net::Link b(Log, 8);
		b.SetBeep(false);
		b.Start(Cfg(7822, 7821), t);
		uint8_t old[40]{};
		const uint32_t magic = net::kMagic;
		memcpy(old, &magic, 4);
		old[4] = 1;  // протокол 1
		old[5] = net::kHello;
		SendRaw(7821, 7822, old, sizeof(old));
		Sleep(10);
		Run(b, b, 2);
		Check(b.GetState() == net::State::Searching, "old packet does not connect");
		Check(Logged("another version of the mod"), "old version logged");
	}

	puts("== reply lost: one side connected, the other still searching");
	{
		g_log.clear();
		net::Link a(Log, 8);
		a.SetBeep(false);
		a.Start(Cfg(7831, 7832), t);
		net::Packet hello;
		hello.type = net::kHello;
		hello.modVersion = 8;
		strcpy_s(hello.name, "Bob");
		SendRaw(7832, 7831, &hello, sizeof(hello));  // a подключается, ответ уходит на закрытый порт
		Sleep(10);
		Run(a, a, 1);
		Check(a.GetState() == net::State::Connected, "a connected from a single hello");
		net::Link b(Log, 8);
		b.SetBeep(false);
		b.Start(Cfg(7832, 7831), t);
		Run(a, b, 40);
		Check(b.GetState() == net::State::Connected, "b connects anyway");
	}

	puts("== silence for 10 s -> searching");
	{
		net::Link a(Log, 8), b(Log, 8);
		a.SetBeep(false);
		b.SetBeep(false);
		a.Start(Cfg(7841, 7842), t);
		b.Start(Cfg(7842, 7841), t);
		Run(a, b, 20);
		t += 11000;
		b.Tick(t, pose);
		Check(b.GetState() == net::State::Searching, "timeout");
	}

	printf(g_failed ? "%d FAILED\n" : "OK\n", g_failed);
	return g_failed ? 1 : 0;
}
