# Provenance of `ship/debug/*` and `fast/debug/*`

mm3d-pc v0.1.9 (`fc4d8f6`) does not contain these files: its `.gitignore` (`[Dd]ebug/`) hid them, so the
runtime could not be built from the public tree. They were added here to build it.

Source: the TriAevum-Switch clone used by this project (`ricachii/mm3d-engine`, an fork of
https://github.com/JohnUzoka/TriAevum-Switch, "TriAevum v0.6.0-alpha.1 public source snapshot", commit
`034820ca`, 2026-09-07; `CrashHandler.cpp` from `0184a147`, 2026-10-13). mm3d-pc is based on TriAevum
`7875f7d1` (see `SOURCE_ARCHIVE_MANIFEST.json`); the headers that include these files are identical or
differ by one line between the two trees (`ship/window/gui/ConsoleWindow.h`, `fast/Fast3dWindow.h`,
`ship/Context.h`: identical; `fast/interpreter.h`: 1 line), so these are the closest version available.
The upstream revision `7875f7d1` itself could not be fetched (`not our ref`).

| File | SHA-256 (first 12) |
|---|---|
| include/ship/debug/Console.h | ceab771b4f6a |
| include/ship/debug/CrashHandler.h | 6ec4c8cf0d3f |
| include/fast/debug/GfxDebugger.h | c61dc39ffddc |
| src/ship/debug/Console.cpp | edf1cdc5d692 |
| src/ship/debug/CrashHandler.cpp | 3590e3c14fed |
| src/fast/debug/GfxDebugger.cpp | 50438733153e |

These files derive from libultraship (https://github.com/Kenix3/libultraship, MIT) as modified in TriAevum
(GPL-3.0-or-later); credit to their authors and to Volpe34 for mm3d-pc.

## Also hidden: `log/` (`.gitignore` line 95, `[Ll]og/`)

| File | SHA-256 (first 12) | Source commit |
|---|---|---|
| include/three_ds_recomp/legacy/log/luslog.h | f30abace04a5 | TriAevum-Switch snapshot `034820ca` |
| src/legacy_compat/log/luslog.cpp | c8df7460ab57 | TriAevum-Switch snapshot `034820ca` |
