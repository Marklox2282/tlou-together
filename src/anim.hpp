#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace anim {

using LogFn = void(*)(const char*, ...);

bool Init(uintptr_t gameBase, LogFn logFn);
void Shutdown();

// Обновляет целевые объекты (локальный игрок для TX, марионетка партнёра для RX)
// из их указателей Entity* (определяются через engine::PlayerEntity / engine::ResolveEntity).
void SetTargets(uintptr_t playerEntity, uintptr_t puppetEntity);
void SetTxEnabled(bool enabled);
void SetRxEnabled(bool enabled);

// TX (игровой поток): переводит захваченный кадр AnimCmdList в сетевой формат и отдаёт.
bool PollTx(std::vector<uint8_t>& out);

// RX: кладёт пришедший по сети кадр AnimCmdList в джиттер-буфер марионетки.
void PushRx(const uint8_t* data, size_t len, uint64_t nowMs);

// RX (скриптовый/игровой поток): резолвит клипы и feather-маски на безопасном потоке
// и публикует готовый кадр для фонового хука CreateAnimCmds.
void PrepareRx(uint64_t nowMs);

// Реально ли сейчас применяются подготовленные кадры анимации партнёра.
bool HasActiveRx(uint64_t nowMs);

// Сколько кадров подряд игра строит копии только bindpose без клипов (Т-поза); 0 - норма.
uint32_t PuppetTposeStreak();
// Хук CreateAnimCmds стоит (иначе ни переноса анимаций, ни сторожа Т-позы).
bool HooksOk();

// Диагностика / статистика.
struct Stats {
	uint64_t txFrames = 0;
	uint64_t rxFrames = 0;
	uint64_t rxApplied = 0;
	uint64_t rxDropped = 0;
	uint64_t rxMissClips = 0;
	uint64_t rxRetargeted = 0;
	bool hookOk = false;
};
Stats GetStats();

}  // namespace anim
