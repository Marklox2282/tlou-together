# tlou1-coop

Experimental two-player co-op for **The Last of Us Part I** (PC, Steam). Unofficial fan project, not
affiliated with Sony Interactive Entertainment or Naughty Dog. You need your own copy of the game.
No game files are in this repository.

Based on the reverse-engineering and approach of [TLOU2-Coop](https://github.com/cheese123113/TLOU2-Coop)
(GPL-3.0), which builds on [Twelve Weeks](https://github.com/kelliex3/Twelve-Weeks). Licensed GPL-3.0.

**Status: early test builds.** Works only with Steam build 18995818 of the game.

## What works

- Each player sees the other as an NPC that walks, runs and sprints where the partner is
  (snaps to them over jumps and drops).
- Connection through Steam friends (no port forwarding) or a LAN IP. Players with different mod
  versions are told so and not connected; a partner in another part of the story is not spawned.
- The mod updates itself from this repository's releases; every update is signed and checked.

Not yet: shared enemies, doors, items, chapter sync, cutscenes, real partner animations and look.

## Install

1. Download `winmm.dll` from the [latest release](../../releases/latest).
2. Steam > Library > The Last of Us Part I > Manage > Browse local files. Put `winmm.dll` there.
3. Later versions download themselves at game start and take effect on the next launch.

Uninstall: delete `winmm.dll` (and `tlou1-coop.*` files) from the game folder.

## Play

Both players load the same chapter, then both press **F8**; a beep means connected. **F6** disconnects, **F7** teleports you to your partner.
For LAN play without Steam set `peer_ip` in `tlou1-coop.ini` (created next to the game on first F8).
Log: `tlou1-coop.log` in the game folder.

Test keys without a partner: **F10** spawns a test NPC, **F9** makes it follow you with a 1 s delay.

## Build

Needs CMake, Ninja, LLVM (clang-cl) and MSVC build tools with the Windows SDK.

```powershell
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build
build\netsim.exe
```

`build\netsim.exe` tests the network layer (two peers over UDP on one PC). Releases are made with
`python tools/release.py` after raising `kModVersion` in `src/main.cpp`; it signs with the
maintainer's private key, which is never in this repository.
