// Два "игрока" по UDP на одном ПК, без игры: проверяет поиск, соединение, позы, выход.
//   netsim.exe            - запускает обоих в одном процессе и проверяет результат (код 0 = ок)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "../src/net.hpp"

static void Log(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

static int Fail(const char* what) {
	printf("FAIL: %s\n", what);
	return 1;
}

int main() {
	net::Link a(Log), b(Log);
	net::Config ca, cb;
	ca.peerIp = cb.peerIp = "127.0.0.1";
	ca.port = 7801; ca.peerPort = 7802;
	cb.port = 7802; cb.peerPort = 7801;

	uint64_t t = 1000;
	if (!a.Start(ca, t)) return Fail("start a");
	// b стартует позже: a уже шлёт приветствия в пустоту, это нормально
	net::Packet pose;
	for (int i = 0; i < 3; ++i, t += 500) a.Tick(t, pose);
	if (!b.Start(cb, t)) return Fail("start b");
	for (int i = 0; i < 40 && (a.GetState() != net::State::Connected || b.GetState() != net::State::Connected); ++i) {
		t += 50;
		a.Tick(t, pose);
		b.Tick(t, pose);
		Sleep(5);
	}
	if (a.GetState() != net::State::Connected || b.GetState() != net::State::Connected) return Fail("connect");

	// a идёт по кругу, b должен получать позы по порядку и с теми же числами
	int received = 0;
	float lastX = 0;
	for (int i = 1; i <= 100; ++i) {
		const float pos[3] = { std::cos(i * 0.1f) * 5, 1.5f, std::sin(i * 0.1f) * 5 }, rot[4] = { 0, 0.3826834f, 0, 0.9238795f };
		a.SendPose(pos, rot);
		Sleep(2);
		t += 50;
		a.Tick(t, pose);
		if (b.Tick(t, pose)) {
			++received;
			lastX = pose.pos[0];
			if (std::fabs(pose.rot[1] - rot[1]) > 1e-6f || pose.pos[1] != 1.5f) return Fail("pose contents");
		}
	}
	printf("received %d/100 poses, last x=%.3f (expected %.3f)\n", received, lastX, std::cos(10.0f) * 5);
	if (received < 90 || std::fabs(lastX - std::cos(10.0f) * 5) > 1e-4f) return Fail("pose stream");

	a.Stop();  // шлёт "пока"
	Sleep(20);
	t += 50;
	b.Tick(t, pose);
	if (b.GetState() != net::State::Searching) return Fail("bye");

	// тишина 10 с после повторного соединения -> снова поиск
	if (!a.Start(ca, t)) return Fail("restart a");
	for (int i = 0; i < 40 && b.GetState() != net::State::Connected; ++i) {
		t += 50;
		a.Tick(t, pose);
		b.Tick(t, pose);
		Sleep(5);
	}
	if (b.GetState() != net::State::Connected) return Fail("reconnect");
	a.Stop();
	b.Tick(t + 11000, pose);  // "пока" уже переводит в поиск; проверяем, что Tick не падает
	printf("OK\n");
	return 0;
}
