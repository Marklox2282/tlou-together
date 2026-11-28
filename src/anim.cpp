#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <safetyhook.hpp>

#include "anim.hpp"

namespace anim {

namespace {

// Проверенные RVA для tlou-i.exe (build 685BEBE1).
constexpr uintptr_t kRvaCreateAnimCmds     = 0x1EF4864;
constexpr uintptr_t kRvaLookupAnimBySid    = 0x1F5F79C;
constexpr uintptr_t kRvaLookupAnimEx       = 0x20DA09C;
constexpr uintptr_t kRvaFindRetarget       = 0x1FD8E2C;
constexpr uintptr_t kRvaFeatherGetId       = 0x1FCF664;
constexpr uintptr_t kRvaFeatherLogin       = 0x1FCFB38;
constexpr uintptr_t kRvaFeatherChannel     = 0x1FCEAD4;
constexpr uintptr_t kRvaAnimDbPtr          = 0x48D7318;
constexpr uintptr_t kRvaFeatherTable       = 0x6614190;

// Смещения в структурах NdGameObject / FgAnimData / AnimControl / AnimCmdList / AnimClip.
constexpr uintptr_t kGoAnimControl         = 0x238;
constexpr uintptr_t kGoFgAnim              = 0x240;
constexpr uintptr_t kAcFgAnim              = 0x18;
constexpr uintptr_t kFgArtSkelPrimary      = 0x1B0;
constexpr uintptr_t kFgArtSkelFallback     = 0xD8;
constexpr uintptr_t kArtSkelId             = 0x34;
constexpr uintptr_t kArtSkelHierarchy      = 0x50;
constexpr uintptr_t kHierSkelId            = 0x04;
constexpr uintptr_t kClipSkeleton          = 0x38;
constexpr uintptr_t kClipId32              = 0x50;
constexpr uintptr_t kClipSkelId            = 0x04;

constexpr uintptr_t kAnimDbBuckets         = 0x40A0;
constexpr uintptr_t kAnimDbBucketCount     = 0x4130;

// Опкоды AnimCmd (16-бит opcode | (wordCount << 16)).
constexpr uint16_t kOpHeader               = 0x0003;  // 2 dwords
constexpr uint16_t kOpTerm                 = 0x0004;  // 2 dwords
constexpr uint16_t kOpClip                 = 0x0007;  // 6 dwords local, 7 dwords wire
constexpr uint16_t kOpBlend                = 0x0008;  // 6 dwords
constexpr uint16_t kOpFeather              = 0x0009;  // 12 dwords
constexpr uint16_t kOpSlotA                = 0x000A;  // 2 dwords
constexpr uint16_t kOpMirror               = 0x000B;  // 2 dwords
constexpr uint16_t kOpBindpose             = 0x000D;  // 2 dwords
constexpr uint16_t kOpMarkLayer            = 0x001B;  // 4 dwords (debug)

constexpr uint8_t  kWireVersion            = 1;
constexpr size_t   kMaxCmdDwords           = 1024;
constexpr uint32_t kMaxOutDwords           = 900;
constexpr uint32_t kMaxListCursor          = 16384;
constexpr uint64_t kRxHoldMs               = 350;
constexpr uint64_t kAnimJitterMs           = 20;
constexpr size_t   kMaxRxQueue             = 12;

struct AnimCmdList {
	uint32_t* buf;           // +0x00
	uint64_t  unk08;         // +0x08
	uint32_t  cursor;        // +0x0C
	uint32_t* headerSlotPtr; // +0x10 (points at OP_HEADER[1])
	uint32_t  maxSlot;       // +0x18
};

struct LookupAnimResult {
	uint32_t type;
	uint32_t pad;
	uint64_t sid;
	uintptr_t clip;
};
static_assert(sizeof(LookupAnimResult) == 24);

struct BufferedRx {
	uint32_t senderMs = 0;
	uint64_t arrivalMs = 0;
	std::vector<uint8_t> bytes;
};

struct PreparedFrame {
	bool valid = false;
	uint32_t targetHierSkelId = 0;
	uint64_t readyAt = 0;
	std::vector<uint32_t> words;
};

uintptr_t g_base = 0;
LogFn g_log = nullptr;
safetyhook::InlineHook g_hookCreateAnimCmds;

std::atomic<uintptr_t> g_playerAc{ 0 };
std::atomic<uintptr_t> g_puppetAc{ 0 };
std::atomic<bool> g_txEnabled{ false };
std::atomic<bool> g_rxEnabled{ true };
std::atomic<bool> g_hookOk{ false };
std::atomic<uint64_t> g_frameNowMs{ 0 };
std::atomic<uint64_t> g_lastAppliedMs{ 0 };

std::atomic<uint64_t> g_txCount{ 0 };
std::atomic<uint64_t> g_hookCalls{ 0 }, g_playerCalls{ 0 }, g_puppetCalls{ 0 }, g_rawTxCount{ 0 };
std::atomic<uintptr_t> g_sampleAc{ 0 };
std::atomic<uint64_t> g_rxCount{ 0 };
std::atomic<uint64_t> g_rxApplied{ 0 };
std::atomic<uint64_t> g_rxDropped{ 0 };
std::atomic<uint64_t> g_rxMissClips{ 0 };
std::atomic<uint64_t> g_rxRetargeted{ 0 };
std::atomic<uint32_t> g_puppetTposeStreak{ 0 };  // кадров подряд копия в Т-позе (только bindpose, ни одного клипа)

// Сырой буфер TX: пишет фоновый поток CreateAnimCmds (только memcpy), читает игровой поток (PollTx).
SRWLOCK g_rawTxLock = SRWLOCK_INIT;
std::vector<uint32_t> g_rawTx;
uint32_t g_rawTxMs = 0;
bool g_hasRawTx = false;

// Входящая сетевая очередь и подготовленный кадр RX.
SRWLOCK g_rxQueueLock = SRWLOCK_INIT;
std::deque<BufferedRx> g_rxQueue;
uint64_t g_rxBaseSenderMs = 0;
uint64_t g_rxBaseLocalMs = 0;

SRWLOCK g_preparedLock = SRWLOCK_INIT;
PreparedFrame g_prepared;

// Кэши резолва клипов: используются ТОЛЬКО на основном/скриптовом потоке (PollTx / PrepareRx).
std::unordered_map<uintptr_t, uint64_t> g_clipPtrToSid;
std::unordered_map<uint64_t, uintptr_t> g_sidToClipPtr;
std::unordered_set<uint16_t> g_loggedUnknownOps;
uint64_t g_lastRebuildMs = 0;

template <typename T>
bool SafeRead(uintptr_t addr, T& out) {
	if (addr < 0x10000) return false;
	__try {
		out = *reinterpret_cast<const T*>(addr);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

uintptr_t SafeReadPtr(uintptr_t addr) {
	uintptr_t v = 0;
	return SafeRead(addr, v) ? v : 0;
}

bool SafeCopyWords(void* dst, uintptr_t src, uint32_t wordCount) {
	if (!dst || src < 0x10000 || !wordCount || wordCount > kMaxCmdDwords) return false;
	__try {
		memcpy(dst, reinterpret_cast<const void*>(src), static_cast<size_t>(wordCount) * 4);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SafeWriteCmds(AnimCmdList* cmdList, uint32_t startCursor, const uint32_t* words, uint32_t wordCount) {
	if (!cmdList || !words || !wordCount || startCursor > kMaxListCursor || wordCount > kMaxOutDwords) return false;
	__try {
		uint32_t* buf = cmdList->buf;
		if (reinterpret_cast<uintptr_t>(buf) < 0x10000) return false;
		memcpy(buf + startCursor, words, static_cast<size_t>(wordCount) * 4);
		cmdList->cursor = startCursor + wordCount;
		cmdList->headerSlotPtr = nullptr;
		cmdList->maxSlot = 0;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

uint32_t Hdr(uint16_t op, uint16_t wc) {
	return static_cast<uint32_t>(op) | (static_cast<uint32_t>(wc) << 16);
}

bool ReadAnimControlSkeleton(uintptr_t animCtrl, uintptr_t& outArtSkel, uint32_t& outHierSkelId) {
	outArtSkel = 0;
	outHierSkelId = 0;
	if (animCtrl < 0x10000) return false;
	const uintptr_t fgAnim = SafeReadPtr(animCtrl + kAcFgAnim);
	if (!fgAnim) return false;
	uintptr_t artSkel = SafeReadPtr(fgAnim + kFgArtSkelPrimary);
	if (!artSkel) artSkel = SafeReadPtr(fgAnim + kFgArtSkelFallback);
	outArtSkel = artSkel;
	const uintptr_t hier = artSkel ? SafeReadPtr(artSkel + kArtSkelHierarchy) : 0;
	if (hier) SafeRead(hier + kHierSkelId, outHierSkelId);
	return outHierSkelId != 0;
}

// Индексирует [kRvaAnimDbPtr] -> (AnimClip* -> sid) только на игровом потоке.
void RebuildClipIndex(uint64_t nowMs) {
	if (nowMs && g_lastRebuildMs && nowMs - g_lastRebuildMs < 2000) return;
	g_lastRebuildMs = nowMs;
	const uintptr_t db = SafeReadPtr(g_base + kRvaAnimDbPtr);
	if (!db) return;
	const uintptr_t bucketsPtr = SafeReadPtr(db + kAnimDbBuckets);
	uint32_t nBuckets = 0;
	if (!bucketsPtr || !SafeRead(db + kAnimDbBucketCount, nBuckets) || nBuckets == 0 || nBuckets > 2000000) return;
	uintptr_t sentinel = 0;
	SafeRead(bucketsPtr + static_cast<size_t>(nBuckets) * 8, sentinel);

	for (uint32_t i = 0; i < nBuckets; ++i) {
		uintptr_t node = 0;
		if (!SafeRead(bucketsPtr + static_cast<size_t>(i) * 8, node)) continue;
		for (int guard = 0; node && node != sentinel && guard < 64; ++guard) {
			uint64_t sid = 0;
			uintptr_t clip = 0;
			uintptr_t next = 0;
			if (!SafeRead(node + 0x10, sid) || !SafeRead(node + 0x18, clip) || !SafeRead(node + 0x00, next)) break;
			if (clip && sid) g_clipPtrToSid[clip] = sid;
			node = next;
		}
	}
}

uint64_t ResolveClipSid(uintptr_t clip, uint32_t& outId32, uint64_t nowMs) {
	outId32 = 0;
	if (!clip) return 0;
	SafeRead(clip + kClipId32, outId32);
	if (auto it = g_clipPtrToSid.find(clip); it != g_clipPtrToSid.end()) return it->second;
	RebuildClipIndex(nowMs);
	if (auto it = g_clipPtrToSid.find(clip); it != g_clipPtrToSid.end()) return it->second;
	return 0;
}

bool HasRetarget(uint32_t srcSkelId, uint32_t dstSkelId) {
	if (!srcSkelId || !dstSkelId) return false;
	if (srcSkelId == dstSkelId) return true;
	using FindRetargetFn = uintptr_t(__fastcall*)(uint32_t srcSkelId, uint32_t dstSkelId);
	auto fn = reinterpret_cast<FindRetargetFn>(g_base + kRvaFindRetarget);
	__try {
		return fn(srcSkelId, dstSkelId) != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

uintptr_t LookupSidRaw(uintptr_t fnAddr, uint64_t sid) {
	using LookupSidFn = LookupAnimResult*(__fastcall*)(LookupAnimResult* out, uint64_t sid);
	LookupAnimResult res{};
	__try {
		reinterpret_cast<LookupSidFn>(fnAddr)(&res, sid);
		return res.clip;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

uintptr_t LookupExRaw(uintptr_t fnAddr, uint32_t animId, uint32_t skelId) {
	using LookupExFn = LookupAnimResult*(__fastcall*)(LookupAnimResult* out, uint32_t animId, uint32_t skelId, const char* name, bool exact);
	LookupAnimResult res{};
	__try {
		reinterpret_cast<LookupExFn>(fnAddr)(&res, animId, skelId, nullptr, false);
		return res.clip;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

int FeatherBlendRaw(uintptr_t fnLogin, uintptr_t fnChan, void* featherTable, uint64_t fbSid, const void* artSkel, float w2,
	void** outMasks, void** outFactors) {
	using FeatherLoginFn = int(__fastcall*)(void* tbl, uint64_t fbSid, const void* artSkel);
	using FeatherChanFn = float(__fastcall*)(void* unused, const void* artSkel, int featherId, float w2,
		void** outMasks, void** outFactors, float w1);
	__try {
		const int newId = reinterpret_cast<FeatherLoginFn>(fnLogin)(featherTable, fbSid, artSkel);
		if (newId >= 0) {
			reinterpret_cast<FeatherChanFn>(fnChan)(nullptr, artSkel, newId, w2, outMasks, outFactors, 1.0f);
		}
		return newId;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		*outMasks = nullptr;
		*outFactors = nullptr;
		return -1;
	}
}

uintptr_t ResolveClipOnRx(uint64_t sid, uint32_t fallbackId32, uintptr_t artSkel, uint32_t targetHierSkelId) {
	uint32_t targetArtSkelId = 0;
	if (artSkel) SafeRead(artSkel + kArtSkelId, targetArtSkelId);

	const uint64_t cacheKey = sid ^ (static_cast<uint64_t>(targetHierSkelId) << 32) ^ (static_cast<uint64_t>(fallbackId32) << 1);
	if (auto it = g_sidToClipPtr.find(cacheKey); it != g_sidToClipPtr.end()) return it->second;

	uintptr_t resolved = 0;
	if (sid) {
		const uintptr_t clip = LookupSidRaw(g_base + kRvaLookupAnimBySid, sid);
		if (clip) {
			const uintptr_t clipSkel = SafeReadPtr(clip + kClipSkeleton);
			uint32_t clipHierId = 0, clipId32 = 0;
			if (clipSkel) SafeRead(clipSkel + kClipSkelId, clipHierId);
			SafeRead(clip + kClipId32, clipId32);
			if (!targetHierSkelId || clipHierId == targetHierSkelId || (targetArtSkelId && clipId32 == targetArtSkelId) ||
				HasRetarget(clipHierId, targetHierSkelId)) {
				resolved = clip;
				if (clipHierId != targetHierSkelId) g_rxRetargeted.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}

	if (!resolved && fallbackId32 && targetHierSkelId) {
		if (const uintptr_t clip = LookupExRaw(g_base + kRvaLookupAnimEx, fallbackId32, targetHierSkelId)) {
			resolved = clip;
			g_rxRetargeted.fetch_add(1, std::memory_order_relaxed);
		}
	}

	if (resolved) {
		if (g_sidToClipPtr.size() > 8192) g_sidToClipPtr.clear();
		g_sidToClipPtr[cacheKey] = resolved;
	} else {
		g_rxMissClips.fetch_add(1, std::memory_order_relaxed);
	}
	return resolved;
}

uint64_t LookupFeatherSid(int featherId) {
	if (featherId < 0) return 0;
	using GetIdFn = uint64_t*(__fastcall*)(void* dummy, uint64_t* outSid, int id);
	auto fn = reinterpret_cast<GetIdFn>(g_base + kRvaFeatherGetId);
	uint64_t sid = 0;
	__try {
		fn(nullptr, &sid, featherId);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		sid = 0;
	}
	return sid;
}

bool SerializeTxCmds(const uint32_t* cmds, uint32_t countDwords, uint32_t senderMs, std::vector<uint8_t>& outBytes) {
	if (!cmds || countDwords < 2 || countDwords > kMaxCmdDwords) return false;
	const uint64_t nowMs = GetTickCount64();

	std::vector<uint32_t> outWords;
	outWords.reserve(countDwords + 8);
	outWords.push_back(static_cast<uint32_t>(kWireVersion));
	outWords.push_back(senderMs);
	outWords.push_back(0);  // maxSlot

	uint32_t maxSlot = 0;
	uint32_t clipCount = 0;
	for (uint32_t i = 0; i < countDwords;) {
		const uint32_t hdr = cmds[i];
		const uint16_t op = static_cast<uint16_t>(hdr & 0xFFFFu);
		const uint16_t wc = static_cast<uint16_t>(hdr >> 16);
		if (wc == 0 || i + wc > countDwords) return false;

		if (op == kOpTerm) {
			outWords.push_back(Hdr(kOpTerm, 2));
			outWords.push_back(0);
			break;
		} else if (op == kOpHeader && wc >= 2) {
			outWords.push_back(Hdr(kOpHeader, 2));
			outWords.push_back(cmds[i + 1]);
		} else if ((op == kOpSlotA || op == kOpMirror || op == kOpBindpose) && wc >= 2) {
			outWords.push_back(Hdr(op, 2));
			outWords.push_back(cmds[i + 1]);
			if (op == kOpBindpose) maxSlot = (std::max)(maxSlot, cmds[i + 1]);
		} else if (op == kOpBlend && wc >= 6) {
			for (uint32_t k = 0; k < 6; ++k) outWords.push_back(cmds[i + k]);
			maxSlot = (std::max)({ maxSlot, cmds[i + 1], cmds[i + 2], cmds[i + 3] });
		} else if (op == kOpClip && wc >= 6) {
			uint64_t clipPtr = 0;
			memcpy(&clipPtr, &cmds[i + 2], 8);
			uint32_t id32 = 0;
			const uint64_t sid = ResolveClipSid(clipPtr, id32, nowMs);
			if (!sid && !id32) return false;  // без неразрешимого клипа: не шлём дырявый граф
			outWords.push_back(Hdr(kOpClip, 7));
			outWords.push_back(cmds[i + 1]);
			const uint32_t* sidParts = reinterpret_cast<const uint32_t*>(&sid);
			outWords.push_back(sidParts[0]);
			outWords.push_back(sidParts[1]);
			outWords.push_back(cmds[i + 4]);
			outWords.push_back(cmds[i + 5]);
			outWords.push_back(id32);
			maxSlot = (std::max)(maxSlot, cmds[i + 1]);
			++clipCount;
		} else if (op == kOpFeather && wc >= 12) {
			const int featherId = static_cast<int>(cmds[i + 10]);
			const uint64_t fbSid = LookupFeatherSid(featherId);
			for (uint32_t k = 0; k < 6; ++k) outWords.push_back(cmds[i + k]);
			const uint32_t* fbParts = reinterpret_cast<const uint32_t*>(&fbSid);
			outWords.push_back(fbParts[0]);
			outWords.push_back(fbParts[1]);
			outWords.push_back(0);
			outWords.push_back(0);
			outWords.push_back(cmds[i + 10]);
			outWords.push_back(cmds[i + 11]);
			maxSlot = (std::max)({ maxSlot, cmds[i + 1], cmds[i + 2], cmds[i + 3] });
		} else if (op == kOpMarkLayer) {
			// Отладочный маркер не нужен.
		} else {
			if (g_loggedUnknownOps.insert(op).second && g_log)
				g_log("anim: skipped frame with unknown opcode 0x%x (wc=%u)", op, wc);
			return false;  // неизвестный опкод может сдвинуть слоты или прятать указатель
		}
		i += wc;
	}

	if (!clipCount || outWords.size() <= 5) return false;
	if ((outWords.back() & 0xFFFFu) != kOpTerm && outWords[outWords.size() - 2] != Hdr(kOpTerm, 2)) {
		outWords.push_back(Hdr(kOpTerm, 2));
		outWords.push_back(0);
	}
	outWords[2] = maxSlot;
	outBytes.resize(outWords.size() * 4);
	memcpy(outBytes.data(), outWords.data(), outBytes.size());
	g_txCount.fetch_add(1, std::memory_order_relaxed);
	return true;
}

bool SelectRxFrame(uint64_t nowMs, std::vector<uint8_t>& outBytes) {
	AcquireSRWLockExclusive(&g_rxQueueLock);
	if (g_rxQueue.empty()) {
		ReleaseSRWLockExclusive(&g_rxQueueLock);
		return false;
	}
	while (g_rxQueue.size() > 1) {
		const auto& front = g_rxQueue.front();
		const auto& second = g_rxQueue[1];
		const uint64_t targetSender = g_rxBaseSenderMs + (nowMs > g_rxBaseLocalMs ? (nowMs - g_rxBaseLocalMs) : 0);
		if (second.senderMs <= targetSender || nowMs - front.arrivalMs > 120) {
			g_rxQueue.pop_front();
		} else {
			break;
		}
	}
	outBytes = g_rxQueue.front().bytes;
	ReleaseSRWLockExclusive(&g_rxQueueLock);
	return !outBytes.empty();
}

bool BuildPreparedRx(uintptr_t animCtrl, const std::vector<uint8_t>& wire, PreparedFrame& outFrame) {
	if (!animCtrl || wire.size() < 20 || (wire.size() & 3) != 0) return false;
	const uint32_t nWords = static_cast<uint32_t>(wire.size() / 4);
	const uint32_t* w = reinterpret_cast<const uint32_t*>(wire.data());
	if ((w[0] & 0xFFu) != kWireVersion || nWords < 5) return false;

	uintptr_t artSkel = 0;
	uint32_t targetHierSkelId = 0;
	if (!ReadAnimControlSkeleton(animCtrl, artSkel, targetHierSkelId)) return false;

	std::vector<uint32_t> out;
	out.reserve(nWords + 8);
	uint32_t maxSlotSeen = 0;
	bool slotValid[64]{};
	auto markValid = [&](uint32_t s) { if (s < 64) slotValid[s] = true; };
	auto isValid = [&](uint32_t s) { return s < 64 && slotValid[s]; };

	out.push_back(Hdr(kOpHeader, 2));
	out.push_back(1);

	void* const featherTable = reinterpret_cast<void*>(g_base + kRvaFeatherTable);
	uint32_t clipCount = 0;
	for (uint32_t i = 3; i < nWords && out.size() + 16 < kMaxOutDwords;) {
		const uint32_t hdr = w[i];
		const uint16_t op = static_cast<uint16_t>(hdr & 0xFFFFu);
		const uint16_t wc = static_cast<uint16_t>(hdr >> 16);
		if (wc == 0 || i + wc > nWords) return false;

		if (op == kOpTerm) {
			break;
		} else if (op == kOpHeader) {
			// Заголовок уже добавлен.
		} else if ((op == kOpSlotA || op == kOpMirror || op == kOpBindpose) && wc >= 2) {
			out.push_back(Hdr(op, 2));
			out.push_back(w[i + 1]);
			if (op == kOpBindpose) {
				if (w[i + 1] >= 64) return false;
				markValid(w[i + 1]);
				maxSlotSeen = (std::max)(maxSlotSeen, w[i + 1]);
			}
		} else if (op == kOpClip && wc >= 7) {
			const uint32_t slot = w[i + 1];
			if (slot >= 64) return false;
			uint64_t sid = 0;
			memcpy(&sid, &w[i + 2], 8);
			const uint32_t flags = w[i + 4];
			const uint32_t frameBits = w[i + 5];
			const uint32_t id32 = w[i + 6];
			const uintptr_t clip = ResolveClipOnRx(sid, id32, artSkel, targetHierSkelId);
			if (!clip) return false;  // целый граф: при пропуске клипа не портим кадр
			out.push_back(Hdr(kOpClip, 6));
			out.push_back(slot);
			const uint32_t* p = reinterpret_cast<const uint32_t*>(&clip);
			out.push_back(p[0]);
			out.push_back(p[1]);
			out.push_back(flags);
			out.push_back(frameBits);
			markValid(slot);
			maxSlotSeen = (std::max)(maxSlotSeen, slot);
			++clipCount;
		} else if (op == kOpBlend && wc >= 6) {
			const uint32_t dst = w[i + 1], srcA = w[i + 2], srcB = w[i + 3];
			if (dst >= 64 || !isValid(srcA) || !isValid(srcB)) return false;
			for (uint32_t k = 0; k < 6; ++k) out.push_back(w[i + k]);
			markValid(dst);
			maxSlotSeen = (std::max)({ maxSlotSeen, dst, srcA, srcB });
		} else if (op == kOpFeather && wc >= 12) {
			const uint32_t dst = w[i + 1], srcA = w[i + 2], srcB = w[i + 3];
			if (dst >= 64 || !isValid(srcA) || !isValid(srcB)) return false;
			uint64_t fbSid = 0;
			memcpy(&fbSid, &w[i + 6], 8);
			float w2 = 0.0f;
			memcpy(&w2, &w[i + 11], 4);
			int newId = -1;
			void* masks = nullptr;
			void* factors = nullptr;
			if (fbSid && artSkel) {
				newId = FeatherBlendRaw(g_base + kRvaFeatherLogin, g_base + kRvaFeatherChannel,
					featherTable, fbSid, reinterpret_cast<const void*>(artSkel), w2, &masks, &factors);
			}
			if (newId >= 0 && masks && factors) {
				out.push_back(Hdr(kOpFeather, 12));
				out.push_back(dst);
				out.push_back(srcA);
				out.push_back(srcB);
				out.push_back(w[i + 4]);
				out.push_back(w[i + 5]);
				const uint32_t* mp = reinterpret_cast<const uint32_t*>(&masks);
				const uint32_t* fp = reinterpret_cast<const uint32_t*>(&factors);
				out.push_back(mp[0]);
				out.push_back(mp[1]);
				out.push_back(fp[0]);
				out.push_back(fp[1]);
				out.push_back(static_cast<uint32_t>(newId));
				out.push_back(w[i + 11]);
			} else {
				out.push_back(Hdr(kOpBlend, 6));
				out.push_back(dst);
				out.push_back(srcA);
				out.push_back(srcB);
				out.push_back(w[i + 4]);
				out.push_back(w[i + 5]);
			}
			markValid(dst);
			maxSlotSeen = (std::max)({ maxSlotSeen, dst, srcA, srcB });
		} else {
			return false;
		}
		i += wc;
	}

	if (!clipCount) return false;
	out.push_back(Hdr(kOpTerm, 2));
	out.push_back(0);
	out[1] = maxSlotSeen + 1;

	outFrame.valid = true;
	outFrame.targetHierSkelId = targetHierSkelId;
	outFrame.words = std::move(out);
	return true;
}

int64_t __fastcall Hook_CreateAnimCmds(uintptr_t animCtrl, void* a2, AnimCmdList* cmdList) {
	const uintptr_t playerAc = g_playerAc.load(std::memory_order_acquire);
	const uintptr_t puppetAc = g_puppetAc.load(std::memory_order_acquire);
	if ((g_hookCalls.fetch_add(1, std::memory_order_relaxed) & 0x3FF) == 0) g_sampleAc.store(animCtrl, std::memory_order_relaxed);
	if (playerAc && animCtrl == playerAc) g_playerCalls.fetch_add(1, std::memory_order_relaxed);
	if (puppetAc && animCtrl == puppetAc) g_puppetCalls.fetch_add(1, std::memory_order_relaxed);
	uint32_t startCursor = 0;
	uintptr_t bufAddr = 0;
	if (cmdList) {
		SafeRead(reinterpret_cast<uintptr_t>(&cmdList->cursor), startCursor);
		SafeRead(reinterpret_cast<uintptr_t>(&cmdList->buf), bufAddr);
	}

	// Фоновый поток анимаций: НИКАКИХ вызовов движка, только копирование уже готового кадра.
	if (puppetAc && animCtrl == puppetAc && g_rxEnabled.load(std::memory_order_acquire) && cmdList && bufAddr) {
		uintptr_t artSkel = 0;
		uint32_t targetHierId = 0;
		if (ReadAnimControlSkeleton(animCtrl, artSkel, targetHierId)) {
			const uint64_t nowMs = g_frameNowMs.load(std::memory_order_relaxed);
			bool applied = false;
			AcquireSRWLockShared(&g_preparedLock);
			if (g_prepared.valid && g_prepared.targetHierSkelId == targetHierId &&
				nowMs >= g_prepared.readyAt && nowMs - g_prepared.readyAt <= kRxHoldMs &&
				!g_prepared.words.empty()) {
				applied = SafeWriteCmds(cmdList, startCursor, g_prepared.words.data(),
					static_cast<uint32_t>(g_prepared.words.size()));
			}
			ReleaseSRWLockShared(&g_preparedLock);
			if (applied) {
				g_lastAppliedMs.store(nowMs, std::memory_order_release);
				g_rxApplied.fetch_add(1, std::memory_order_relaxed);
				g_puppetTposeStreak.store(0, std::memory_order_relaxed);
				return 0;
			}
		}
	}

	const int64_t ret = g_hookCreateAnimCmds.call<int64_t>(animCtrl, a2, cmdList);

	uint32_t endCursor = 0;
	if (cmdList && bufAddr) SafeRead(reinterpret_cast<uintptr_t>(&cmdList->cursor), endCursor);

	if (puppetAc && animCtrl == puppetAc && cmdList && bufAddr && endCursor > startCursor &&
		endCursor - startCursor <= kMaxCmdDwords) {
		uint32_t buf[kMaxCmdDwords];
		const uint32_t n = endCursor - startCursor;
		if (SafeCopyWords(buf, bufAddr + static_cast<size_t>(startCursor) * 4, n)) {
			uint32_t clips = 0, binds = 0;
			for (uint32_t i = 0; i < n;) {
				const uint16_t op = static_cast<uint16_t>(buf[i] & 0xFFFFu);
				const uint16_t wc = static_cast<uint16_t>(buf[i] >> 16);
				if (wc == 0 || i + wc > n || op == kOpTerm) break;
				if (op == kOpClip) ++clips;
				else if (op == kOpBindpose) ++binds;
				i += wc;
			}
			if (binds && !clips) g_puppetTposeStreak.fetch_add(1, std::memory_order_relaxed);
			else g_puppetTposeStreak.store(0, std::memory_order_relaxed);
		}
	}

	if (playerAc && animCtrl == playerAc && g_txEnabled.load(std::memory_order_acquire) && cmdList && bufAddr &&
		endCursor > startCursor && endCursor - startCursor <= kMaxCmdDwords) {
		uint32_t raw[kMaxCmdDwords];
		const uint32_t n = endCursor - startCursor;
		if (SafeCopyWords(raw, bufAddr + static_cast<size_t>(startCursor) * 4, n)) {
			const uint32_t stampMs = static_cast<uint32_t>(g_frameNowMs.load(std::memory_order_relaxed));
			AcquireSRWLockExclusive(&g_rawTxLock);
			g_rawTx.assign(raw, raw + n);
			g_rawTxMs = stampMs ? stampMs : static_cast<uint32_t>(GetTickCount64());
			g_hasRawTx = true;
			ReleaseSRWLockExclusive(&g_rawTxLock);
			g_rawTxCount.fetch_add(1, std::memory_order_relaxed);
		}
	}
	return ret;
}

}  // namespace

void Uninstall() { g_hookCreateAnimCmds = {}; }

bool Init(uintptr_t gameBase, LogFn logFn) {
	if (!gameBase) return false;
	g_base = gameBase;
	g_log = logFn;
	auto res = safetyhook::InlineHook::create(
		reinterpret_cast<void*>(gameBase + kRvaCreateAnimCmds),
		reinterpret_cast<void*>(&Hook_CreateAnimCmds));
	if (!res) {
		if (g_log) g_log("anim: SafetyHook failed at RVA 0x%llX (err=%d)",
			static_cast<unsigned long long>(kRvaCreateAnimCmds), static_cast<int>(res.error().type));
		return false;
	}
	g_hookCreateAnimCmds = std::move(*res);
	g_hookOk.store(true, std::memory_order_release);
	if (g_log) g_log("anim: hooked CreateAnimCmds at 0x%llX (game-thread pipeline)", static_cast<unsigned long long>(gameBase + kRvaCreateAnimCmds));
	return true;
}

void Shutdown() {
	g_hookOk.store(false, std::memory_order_release);
	g_playerAc.store(0, std::memory_order_release);
	g_puppetAc.store(0, std::memory_order_release);
	g_hookCreateAnimCmds = {};
}

void SetTargets(uintptr_t playerEntity, uintptr_t puppetEntity) {
	uintptr_t pAc = playerEntity ? SafeReadPtr(playerEntity + kGoAnimControl) : 0;
	uintptr_t nAc = puppetEntity ? SafeReadPtr(puppetEntity + kGoAnimControl) : 0;
	if (!pAc && playerEntity) {
		if (const uintptr_t fg = SafeReadPtr(playerEntity + kGoFgAnim)) SafeRead(fg + 0x10, pAc);
	}
	if (!nAc && puppetEntity) {
		if (const uintptr_t fg = SafeReadPtr(puppetEntity + kGoFgAnim)) SafeRead(fg + 0x10, nAc);
	}
	g_playerAc.store(pAc, std::memory_order_release);
	const uintptr_t prevPuppetAc = g_puppetAc.exchange(nAc, std::memory_order_acq_rel);
	if (prevPuppetAc != nAc) {
		g_puppetTposeStreak.store(0, std::memory_order_relaxed);
		g_lastAppliedMs.store(0, std::memory_order_release);
		AcquireSRWLockExclusive(&g_preparedLock);
		g_prepared.valid = false;
		g_prepared.words.clear();
		ReleaseSRWLockExclusive(&g_preparedLock);
		if (!nAc) {
			AcquireSRWLockExclusive(&g_rxQueueLock);
			g_rxQueue.clear();
			ReleaseSRWLockExclusive(&g_rxQueueLock);
		}
	}
}

void SetTxEnabled(bool enabled) {
	g_txEnabled.store(enabled, std::memory_order_release);
	if (!enabled) {
		AcquireSRWLockExclusive(&g_rawTxLock);
		g_rawTx.clear();
		g_hasRawTx = false;
		ReleaseSRWLockExclusive(&g_rawTxLock);
	}
}

void SetRxEnabled(bool enabled) {
	g_rxEnabled.store(enabled, std::memory_order_release);
	if (!enabled) {
		g_lastAppliedMs.store(0, std::memory_order_release);
		AcquireSRWLockExclusive(&g_preparedLock);
		g_prepared.valid = false;
		ReleaseSRWLockExclusive(&g_preparedLock);
	}
}

bool PollTx(std::vector<uint8_t>& out) {
	std::vector<uint32_t> raw;
	uint32_t stampMs = 0;
	AcquireSRWLockExclusive(&g_rawTxLock);
	if (!g_hasRawTx || g_rawTx.empty()) {
		ReleaseSRWLockExclusive(&g_rawTxLock);
		return false;
	}
	raw.swap(g_rawTx);
	stampMs = g_rawTxMs;
	g_hasRawTx = false;
	ReleaseSRWLockExclusive(&g_rawTxLock);
	return SerializeTxCmds(raw.data(), static_cast<uint32_t>(raw.size()), stampMs, out);
}

void PushRx(const uint8_t* data, size_t len, uint64_t nowMs) {
	if (!data || len < 20 || (len & 3) != 0) return;
	uint32_t verWord = 0, senderMs = 0;
	memcpy(&verWord, data, 4);
	memcpy(&senderMs, data + 4, 4);
	if ((verWord & 0xFFu) != kWireVersion) return;

	g_rxCount.fetch_add(1, std::memory_order_relaxed);
	AcquireSRWLockExclusive(&g_rxQueueLock);
	if (g_rxQueue.empty() || nowMs - g_rxQueue.back().arrivalMs > 500 || senderMs < g_rxBaseSenderMs) {
		g_rxBaseSenderMs = senderMs;
		g_rxBaseLocalMs = nowMs + kAnimJitterMs;
	}
	if (!g_rxQueue.empty() && senderMs < g_rxQueue.back().senderMs) {
		ReleaseSRWLockExclusive(&g_rxQueueLock);
		return;
	}
	while (g_rxQueue.size() >= kMaxRxQueue) g_rxQueue.pop_front();
	BufferedRx b;
	b.senderMs = senderMs;
	b.arrivalMs = nowMs;
	b.bytes.assign(data, data + len);
	g_rxQueue.push_back(std::move(b));
	ReleaseSRWLockExclusive(&g_rxQueueLock);
}

void PrepareRx(uint64_t nowMs) {
	g_frameNowMs.store(nowMs, std::memory_order_relaxed);
	const uintptr_t puppetAc = g_puppetAc.load(std::memory_order_acquire);
	if (!puppetAc || !g_rxEnabled.load(std::memory_order_acquire)) return;

	std::vector<uint8_t> wire;
	if (!SelectRxFrame(nowMs, wire)) return;

	PreparedFrame next;
	if (!BuildPreparedRx(puppetAc, wire, next)) {
		g_rxDropped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	next.readyAt = nowMs;
	AcquireSRWLockExclusive(&g_preparedLock);
	g_prepared = std::move(next);
	ReleaseSRWLockExclusive(&g_preparedLock);
}

uint32_t PuppetTposeStreak() { return g_puppetTposeStreak.load(std::memory_order_relaxed); }
bool HooksOk() { return g_hookOk.load(std::memory_order_acquire); }

bool HasActiveRx(uint64_t nowMs) {
	const uint64_t last = g_lastAppliedMs.load(std::memory_order_acquire);
	return last != 0 && nowMs >= last && nowMs - last <= kRxHoldMs;
}

Stats GetStats() {
	return {
		g_txCount.load(std::memory_order_relaxed),
		g_rxCount.load(std::memory_order_relaxed),
		g_rxApplied.load(std::memory_order_relaxed),
		g_rxDropped.load(std::memory_order_relaxed),
		g_rxMissClips.load(std::memory_order_relaxed),
		g_rxRetargeted.load(std::memory_order_relaxed),
		g_hookCalls.load(std::memory_order_relaxed),
		g_playerCalls.load(std::memory_order_relaxed),
		g_puppetCalls.load(std::memory_order_relaxed),
		g_rawTxCount.load(std::memory_order_relaxed),
		g_playerAc.load(std::memory_order_relaxed),
		g_puppetAc.load(std::memory_order_relaxed),
		g_sampleAc.load(std::memory_order_relaxed),
		g_hookOk.load(std::memory_order_acquire),
	};
}

}  // namespace anim
