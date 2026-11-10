# spore-re

[Русский](README.ru.md) | English

Native Android port of Spore (with Galactic Adventures). Not an emulator and not Wine: `SporeApp.exe` is statically
recompiled from x86 to C, built for arm64 and run on top of my own Win32/D3D9 layer (SDL2, DXVK).

> **The project is at a very early stage.** Expect a lot of small, frequent commits, quick fixes, things breaking
> and getting rewritten. Nothing here is stable yet and there are no releases.

If you like the port, you can support me on Patreon: **[patreon.com/VinTeRuS](https://www.patreon.com/cw/VinTeRuS/membership)**.

## What's in the repo

Only my own code: the Win32/D3D9 host, the Android app (Java, touch controls, settings), build scripts and the
recompiler (`third_party/x86recomp`). There are **no APKs, no game files and no recompiled game code** here, and there
won't be. The recompiled code is generated from your own copy of the game at build time.

## Status

- Runs on a Pixel 6 (Mali-G78): menus, sound, galaxy, Cell stage, editors, other stages via the cheat console.
- Touch controls: tap/drag = left mouse, long press = right click, two fingers = camera, pinch = zoom, WASD stick.
- 20-60 fps in gameplay, stage loading is slow. Performance work is ongoing.
- Known problems: C++ exceptions aren't supported yet (the game dies if it throws one), only ARMv8.2+ CPUs, only
  tested on one phone.

## Game version

GOG Spore Collection 3.1.0.22, `SporebinEP1/SporeApp.exe` (sha256 in `re/target.sha256`). Other builds won't work.

## Building (rough)

Linux, Python 3, Android NDK, the game installed somewhere.

1. `python3 -I third_party/x86recomp/tools/recomp.py ...` with `tools/recomp/game.py` generates C into `build/`.
2. `make -j3` in `src/native` for a desktop (Linux) build, `make -j2 PLATFORM=android` for arm64.
3. `android/build-deps.sh`, then `android/build-apk.sh` gives `build/apk/spore.apk`.

Instructions will get better once things settle down.

## Legal

You need your own bought copy of Spore. Don't ask for game files or APKs. Spore is a trademark of Electronic Arts;
this project isn't affiliated with EA or Maxis.
