# x86recomp

Static recompiler for 32-bit x86 Windows games: every function of a PE image becomes C (`void f_<va>(Ctx*)`) built
against `runtime/runtime.h`. Shared by proto-re (Prototype) and spore-re (Spore).

A game project keeps its settings in `tools/recomp/game.py` (image path, CRT setjmp/longjmp addresses, inlined MSVC
helpers, native overrides) and its Ghidra export in `re/functions.csv`. Run from the project root:

    python3 -I <x86recomp>/tools/data_entries.py
    python3 -I <x86recomp>/tools/recomp.py --functions all --base <load address> --split 400 --out build/recomp/all

`--config` or `RECOMP_CONFIG` picks another config, `RECOMP_ROOT` another project root.
