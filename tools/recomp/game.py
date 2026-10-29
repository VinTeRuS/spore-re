# Recompiler settings for Spore (GOG 3.1.0.22, SporebinEP1/SporeApp.exe; sha256 in re/target.sha256), read by
# third_party/x86recomp. Addresses at the exe's preferred base 0x400000; helpers found with find_helpers.py against
# an already configured game and cross-checked with Ghidra's names (__allshl, __alloca_probe*, __aullshr, __allmul).
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE = os.environ.get("SPORERE_EXE", "~/spore-re-data/game/SporebinEP1/SporeApp.exe")
FUNCTIONS_CSV = os.path.join(ROOT, "re", "decomp", "functions.csv")
EXTRA_FUNCTIONS = os.path.join(ROOT, "re", "decomp", "extra_functions.txt")
# MSVCR90 _setjmp3/longjmp: import thunk and IAT slots.
SETJMP_TARGETS = {0x011e0cec}
SETJMP_IAT = {0x013cc440}
LONGJMP_IAT = {0x013cc430}
INLINE_HELPERS = {0x011e0720: "allshl", 0x011e0740: "chkstk", 0x011e0886: "ftol2", 0x011e0910: "aullshr",
                  0x011e0ad0: "allmul", 0x011e0cc0: "chkstk16", 0x011e0cd6: "chkstk8"}
# MSVCR90 memset/memcpy/memmove IAT slots: done in place at the call sites (the game calls them through the import
# thunks a few hundred thousand times a second while loading).
INLINE_IMPORTS = {0x013cc478: "memset", 0x013cc47c: "memcpy", 0x013cc480: "memmove"}
NATIVE_OVERRIDES = set()
