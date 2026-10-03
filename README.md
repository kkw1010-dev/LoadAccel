# LoadAccel

**Test version. Skyrim Special Edition 1.6.1170.0 (Steam) only.** On every other runtime the plugin installs
nothing and writes one line to its log.

An SKSE plugin that shortens the data load (launch to main menu) on very large load orders by taking two linear
searches out of the engine's load path. On the author's load order (about 3,700 plugins) the load went from
256 s to 196 s. Both costs grow with the number of plugins and overrides; a small load order gains little.

There is no binary release.

## What it does

**A. Source-file lists.** Every form keeps a pointer to a shared list of the plugin files that define or override
it. The engine keeps one global table of the distinct lists and, for every record a plugin adds or overrides,
searches that table from the start for an equal list (Address Library IDs 14580, 14569, 14570). With tens of
thousands of distinct lists and over a million lookups this is about 34 s here. LoadAccel keeps a hash index of
the table and answers "it is already entry N"; a list that is not in the table yet is added by the engine's own
function.

**B. Large references.** When an exterior reference of a non-master file is initialised
(`TESObjectREFR::InitItemImpl`, ID 19507), the engine removes its FormID from the worldspace's large-reference
lists by walking every list of both maps (ID 18216 / 18242), then lists it again. For a reference that is in no
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
  seven wrapped call sites, which are all the ways into those functions.
- **After the main menu**, A stays active. B stops skipping at the end of the data load: in game the engine does
  every walk, and LoadAccel only tests 1 call in 64 and logs whether a skip would have been right.

Stages (built in with `-Stage N -StageB M`, or set in `SKSE\Plugins\LoadAccel.ini`): 0 off, 1 count only,
2 compare every call but let the engine answer, 3 replace. Stage 2 over a whole load with 0 mismatches and clean
audits is the gate before stage 3.

## Requirements

SKSE64 for 1.6.1170 and Address Library for SKSE Plugins. No ESP, no scripts, nothing stored in saves.

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
source-file lists: stage 3 (replace: ...); 7 call sites wrapped ...
large refs: stage 3 (replace: ...)
SUMMARY at kDataLoaded: stage 3, calls ..., MISMATCHES 0, time spent ...
  audit of the whole table: ... : CLEAN
LARGE REFS SUMMARY at kDataLoaded: stage 3, ..., MISMATCHES 0 ...
  audit of every list: CLEAN ...
```

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
The log's "engine time saved" is an estimate from sampled engine time, not the wall-clock gain (about 70 s
estimated was 60 s measured on the author's game).

Not verified:

- part B after the data load: the engine made no such call in those sessions (B does not skip there anyway);
- long sessions, many cell loads;
- any other load order, in particular one that uses DynDOLOD DLL NG, which rewrites the large-reference lists
  itself after the main menu;
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
