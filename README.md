# LoadAccel

**Test version. Skyrim Special Edition 1.5.97.0, 1.6.640.0, 1.6.1170.0 and 1.7.104.0 (Steam) only.** On every other runtime the
plugin installs nothing and writes one line to its log.

An SKSE plugin that shortens the data load (launch to main menu) on very large load orders by taking two linear
searches out of the engine's load path. On the author's load order (about 3,700 plugins) the load went from
256 s to 196 s. Both costs grow with the number of plugins and overrides; a small load order gains little.

Binary: test archives with the licence, the third-party notices and the exact source as a second archive. 0.3.1
adds Skyrim SE 1.5.97 to the same two parts, 0.3.2 adds 1.7.104, 0.3.3 adds 1.6.640; nothing else changed. The 0.3.0 test build is on Nexus Mods as
"LoadAccel - Faster Data Load (Test Build)"; tag `v0.3.0-test-gpl` is its source under GPL-3.0-or-later, apart from
the plugin author string that DLL was built with (see Licence).

## What it does

**A. Source-file lists.** Every form keeps a pointer to a shared list of the plugin files that define or override
it. The engine keeps one global table of the distinct lists and, for every record a plugin adds or overrides,
searches that table from the start for an equal list (Address Library IDs 14580, 14569, 14570 on 1.6.640,
1.6.1170 and 1.7.104; 14423, 14415, 14416 on 1.5.97). With tens of
thousands of distinct lists and over a million lookups this is about 34 s here. LoadAccel keeps a hash index of
the table and answers "it is already entry N"; a list that is not in the table yet is added by the engine's own
function.

**B. Large references.** When an exterior reference of a non-master file is initialised
(`TESObjectREFR::InitItemImpl`, ID 19507, also on 1.6.640 and 1.7.104; 19105 on 1.5.97), the engine removes its FormID from the
worldspace's large-reference lists by walking every list of both maps (ID 18216 / 18242; 17804 / 17828 on 1.5.97), then lists
it again. For a reference that is in no
list the walk reads everything and changes nothing; that is about 37 s here. LoadAccel keeps, per worldspace, the
set of FormIDs that were ever listed and skips the walk for a FormID that is not in the set.

## Safety design

- **It never writes engine data.** It only decides whether the engine's own search has to run. Every insertion and
  every removal is still done by the engine's code.
- **It checks the code first.** At load each function it relies on is hashed and compared with the bytes that
  were analysed, and each call site must be the plain call that was analysed. If another plugin patched that
  code, or the executable differs, the target is not installed and the log says so.
- **It keeps checking itself.** The first 4096 answers of each target, then 1 in 64, are compared with what the
  engine's own function returns. One disagreement switches that target off for the rest of the session (the
  engine answers everything from then on) and is logged.
- **Audits.** At the end of the data load and at every new game, save and save load, every table entry is looked
  up through the index and every list is checked against the set. The result is `CLEAN` or `NOT CLEAN` in the log.
- **Threads.** The engine takes no lock around this table. LoadAccel serialises every access that goes through the
  seven wrapped call sites, which are all the ways into those functions (five on 1.5.97, where the `TESForm`
  constructor calls `TESForm::SetFile` instead of carrying its own copy of it).
- **Four runtimes, one table each.** Every Address Library ID, call offset and code hash exists once for 1.6.1170,
  once for 1.5.97, once for 1.6.640 and once for 1.7.104 (`src/FileLists.cpp`, `src/LargeRefs.cpp`); the runtime
  version picks one set at load. 1.6.640 and 1.7.104 keep the 1.6.1170 IDs and code; only the displacements inside
  the code and two globals moved (on 1.7.104 also one call offset).
- **After the main menu**, A stays active. B stops skipping at the end of the data load: in game the engine does
  every walk, and LoadAccel only tests 1 call in 64 and logs whether a skip would have been right.

Stages (built in with `-Stage N -StageB M`, or set in `SKSE\Plugins\LoadAccel.ini`): 0 off, 1 count only,
2 compare every call but let the engine answer, 3 replace. Stage 2 over a whole load with 0 mismatches and clean
audits is the gate before stage 3.

## Requirements

SKSE64 for your runtime (2.0.20 for 1.5.97, 2.2.3 for 1.6.640, 2.2.x for 1.6.1170, 2.3.1 or later for 1.7.104) and
Address Library for SKSE Plugins (the SE file `version-1-5-97-0.bin` for 1.5.97, the 1.6.x "all in one" for 1.6.640
and 1.6.1170, v13 or later with `versionlib-1-7-104-0.bin` for 1.7.104). No ESP, no scripts, nothing stored in saves.

## Build

Windows, Visual Studio 2022 or newer C++ build tools, CMake 3.25+, Ninja, [vcpkg](https://github.com/microsoft/vcpkg),
and a checkout of CommonLibSSE-NG ([alandtse/CommonLibVR](https://github.com/alandtse/CommonLibVR), branch `ng`).

```powershell
$env:VCPKG_ROOT    = 'D:\src\vcpkg'
$env:COMMONLIB_DIR = 'D:\src\CommonLibVR'
powershell -ExecutionPolicy Bypass -File tools\Build.ps1 -Stage 3 -StageB 3
```

The script runs the offline tests first and builds nothing if they fail, then builds
`build\dist\A3-B3\LoadAccel.dll`. Install it as `Data\SKSE\Plugins\LoadAccel.dll`, optionally with
`package\LoadAccel.ini` next to it.

`tools\Test.ps1` alone builds and runs the tests (no game, no CommonLib): models of the engine functions receive
the same random operations directly and through the index, in every stage, and both sides must end in the same
state; then faults are injected (entries appended behind the index's back, changed in place, duplicated, removed;
re-entrant calls; four threads; unseen list changes). What the tests cannot show is that the models match the
engine. That is what stage 2 measures in the game.

## Reading the log

`Documents\My Games\Skyrim Special Edition\SKSE\LoadAccel.log`, rewritten at every launch.

Healthy:

```
source-file lists: stage 3 (replace: ...); 7 call sites wrapped ...        (5 call sites on 1.5.97)
large refs: stage 3 (replace: ...)
data load finished: kDataLoaded +N s after plugin load
SUMMARY at kDataLoaded: stage 3, calls ..., MISMATCHES 0, time spent ...
  audit of the whole table: ... : CLEAN
LARGE REFS SUMMARY at kDataLoaded: stage 3, ..., MISMATCHES 0 ...
  audit of every list: CLEAN ...
```

The `data load finished` line (since 0.3.1) is written at every stage, both parts off included, so the time to the
end of the data load can be compared with LoadAccel on and off.

Not healthy: `[error] ... is not the code that was analysed` (nothing installed), `MISMATCH #n`,
`FELL BACK TO THE ENGINE`, `NOT CLEAN`.

`python tools\judge_log.py LoadAccel.log` prints PASS / FAIL / INERT, the failing lines, and what that session
did not exercise (new game, save load, save, calls from other threads, part B after the data load).

## Verified and not verified

Verified on the author's game (1.6.1170, about 3,700 plugins, about 350 SKSE plugins):

- twelve launches to the main menu, among them two with every answer of A and three with every call of B
  compared against the engine: 0 mismatches, every audit clean;
- one session with a new game and five saves, two sessions that loaded a save: 0 mismatches, every audit clean,
  no fallback; several thousand calls of A from other threads, none overlapping.

One outside tester (about 1,200 plugins, DynDOLOD): one launch to the main menu, 0 mismatches, audits clean.
0.3.1 has the same 1.6.1170 code, addresses and hashes; with the 0.3.1 DLL, one launch to the main menu on the
author's game: 0 mismatches, audits clean, the same estimated saving as 0.3.0 (about 35 s for A, 34 s for B).
The log's "engine time saved" is an estimate from sampled engine time, not the wall-clock gain (about 70 s
estimated was 60 s measured on the author's game).

Verified for 1.5.97 (0.3.1), on a separate install with SKSE 2.0.20:

- before any launch: the 1.5.97 counterpart of every function and call site was read off the executable, and the
  engine's own 1.5.97 code was run against the models the index relies on (part A 20,000 random cases and 52,401
  calls in `SetFile` sequences, part B 20,000 walks: 0 differences);
- the base game and DLC: every call of A compared against the engine, 0 mismatches, audits clean;
- 430 plugins of an older 1.5.97 load order: every call compared (403,747 of A, 4,415 of B in 9 worldspaces), then
  both parts on: 0 mismatches, audits clean, no fallback. That order is small, so the gain is small (about 0.3 s
  estimated); the gain grows with the load order as on 1.6.1170.

Verified for 1.7.104 (0.3.2), on the Steam base game with SKSE 2.3.1:

- before any launch: the 16 functions LoadAccel uses were compared instruction by instruction with their 1.6.1170
  twins. They keep their Address Library IDs, and 15 differ only in displacements that reach the same places; the
  worldspace destructor gained a lock elsewhere, which moved the call LoadAccel watches from +967 to +994 (same
  argument). The table globals moved by +0xA7C80;
- six launches to the main menu: every call of A compared against the engine (195,272 calls, 0 mismatches, audits
  clean), then both parts on twice (0 mismatches, audits clean, no fallback). The base game is small: the load took
  the same time with LoadAccel on and off (about 5.8 s to the end of the data load).

Verified for 1.6.640 (0.3.3), on a separate vanilla install made from the Steam 1.6.640 files, with SKSE 2.2.3:

- before any launch: the same 16 functions were compared instruction by instruction with their 1.6.1170 twins. They
  keep their Address Library IDs and call offsets, and the code LoadAccel relies on differs only in displacements
  that reach the same places. The table globals moved by -0x19E1F0;
- four launches to the end of the data load: every call of A compared against the engine (194,808 calls,
  0 mismatches, audits clean), then both parts on (0 mismatches, audits clean, no fallback). The base game is small:
  the load took about the same time with LoadAccel on and off (about 4.8 s to the end of the data load).

Not verified:

- part B after the data load: the engine made no such call in those sessions (B does not skip there anyway);
- long sessions, many cell loads;
- any other load order, in particular one that uses DynDOLOD DLL NG, which rewrites the large-reference lists
  itself after the main menu;
- on 1.5.97: anything after the main menu (new game, save load, saves), a large 1.5.97 load order, and the 1.5.97
  builds of common SKSE plugins (if one patches the same code, that part installs nothing and the log says so);
- on 1.7.104: anything after the main menu, a large 1.7.104 load order (part B's skip path gets no call in the base
  game), and 1.7.104 builds of other SKSE plugins;
- on 1.6.640: the same as on 1.7.104 (after the main menu, a large load order, other SKSE plugins built for it);
- any other runtime. Supporting one means reading the same functions again on that executable.

The large-reference lists differ between two launches of the unpatched game as well (order inside lists, a few
references on cell borders), so "identical lists" cannot be shown byte for byte; patched launches differ from
unpatched ones only in the ways unpatched launches differ from each other.

## Licence

LoadAccel is licensed under the GNU General Public License v3.0 or later (`LICENSE`). It is built with
CommonLibSSE-NG, which is licensed under GPL-3.0-or-later with a Modding Exception and a GPL-3.0 Linking Exception
(it was MIT until mid-2026); the notices of all code compiled into the DLL are in `THIRD-PARTY-NOTICES.txt`. This
repository contains no code or data of the game.

The 0.3.0 test build (DLL SHA-256 `378E613AF142F38675FDC60611010968411CB541EC12870EF2C1D10237A620CB`) was built
from this source with an earlier plugin author string (`AUTHOR` in `CMakeLists.txt`; `LoadAccel` from the next
version on); its source archive carries that line as it was built.

The 0.3.1 test build (DLL SHA-256 `E8C480125DFD7D772DD646CCAB78624C8DC6BB8B270759667D74582AA9ABAB0B`) was built from
the 0.3.1 source as it was.

The 0.3.2 test build (DLL SHA-256 `52F80C6361B6CA0E11FBE1D5B6E95295526A6F49FCFB621F59136F651BBB43CF`) was built from
the 0.3.2 source as it was.

The 0.3.3 test build (DLL SHA-256 `09BB5E695F9B0885C1FCE28BF1283B5E7085F0AD996EF59F5A75B4BA5098D31C`) is built from
this source as it is.
