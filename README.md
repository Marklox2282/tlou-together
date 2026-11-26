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
  versions are told so and not connected; a partner more than 300 m away is not spawned.
- Shared enemies: an enemy you hurt or kill is hurt or killed in your partner's game too
  (each game still runs its own enemy AI).
- The mod updates itself from this repository's releases; every update is signed and checked.

Not yet: shared enemy movement, doors, items, automatic chapter sync, cutscenes, real partner animations and look.

## Install

1. Download `winmm.dll` from the [latest release](../../releases/latest).
2. Steam > Library > The Last of Us Part I > Manage > Browse local files. Put `winmm.dll` there.
3. Later versions download themselves at game start and take effect on the next launch.

Uninstall: delete `winmm.dll` (and `tlou1-coop.*` files) from the game folder.

## Play

Main menu > **MULTIPLAYER** > **Create lobby**: the Steam invite window opens; pick your friend.
Your friend accepts the invite in Steam (the game starts if it was closed) and joins your lobby;
a beep means connected. Then both load the same chapter, or one of you presses **F5**.
Without the menu: both press **F8** to connect to any friend who also pressed it. **F6** disconnects, **F7** teleports you to your partner,
**F5** loads your partner's chapter and checkpoint and puts you next to them (Shift+F5 reloads your own, to test it).
For LAN play without Steam set `peer_ip` in `tlou1-coop.ini` (created next to the game on first F8).
Log: `tlou1-coop.log` in the game folder.

Roles: the lobby host is the **hero** of the chapter (Sarah, Joel, Ellie); the friend is the
**companion** (Tommy, Tess, Ellie, Bill, Henry...). The hero sees the friend as the real companion
NPC (its AI is switched off); the companion player takes the companion's look and their own AI
companion is hidden. **Swap roles** in the lobby once connected.

Test keys without a partner: **F10** spawns a test NPC, **F9** makes it follow you with a 1 s delay
(as the hero, the chapter's companion becomes that test partner); **Shift+F10** switches your own
role between hero and companion.

## Build

Needs CMake, Ninja, LLVM (clang-cl) and MSVC build tools with the Windows SDK.

```powershell
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build
build\netsim.exe
build\enginetest.exe
```

`netsim` tests the network layer (two peers over UDP on one PC), `enginetest` the game-free parts of
the engine layer (guarded memory reads, name parsing). Releases are made with
`python tools/release.py` after raising `kModVersion` in `src/main.cpp`; it signs with the
maintainer's private key, which is never in this repository.
