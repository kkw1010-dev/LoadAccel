r"""Verdict for one SKSE\LoadAccel.log, and one ledger line per session (ledger.csv).

    python judge_log.py LoadAccel.log [--who NAME] [--dll SHA256-or-note] [--order HASH-or-note] [--plugins N]
                        [--load-on SECONDS] [--load-off SECONDS] [--note TEXT] [--ledger] [--date YYYY-MM-DD]

Prints PASS / FAIL / INERT, the reasons, and what the session did not exercise. The verdict is about LoadAccel's
own checks, nothing else. It is for comparing sessions (the author's game and testers' logs in one table), not a
gate. With --ledger the line is appended to ledger.csv next to this repository's README.

  PASS   both targets installed, every summary block has MISMATCHES 0 and a CLEAN audit, no fallback, no error
  FAIL   a MISMATCH, FELL BACK, NOT CLEAN or [error] line (each is printed with its line number)
  INERT  nothing installed: wrong runtime, a stage set to 0, or the analysed code was not found
The "not exercised" list names what the log cannot speak for: no new game, no save load, no save, no calls from
other threads (so nothing ran after the menu), part B never called after the data load.

--who, --dll, --order, --plugins, --load-on / --load-off are what the log does not contain (README.txt of the
tester package asks testers for them); they are copied into the ledger as given.
"""
import csv, datetime, os, re, sys

FIELDS = ["date", "who", "dll", "order", "plugins", "version", "runtime", "stages", "summaries", "events",
          "mismatches", "fell_back", "audits_not_clean", "errors", "a_calls", "a_other_thread_calls", "a_overlaps",
          "a_lock_waits", "a_seconds", "b_calls_load", "b_skipped", "b_calls_after_load", "b_seconds",
          "session_seconds", "load_on_s", "load_off_s", "verdict", "not_exercised", "note"]


def opt(name, default=""):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def stamp(line):
    m = re.match(r"\[(\d\d):(\d\d):(\d\d)\.(\d+)\]", line)
    return int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1000 if m else None


def judge(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    r = {k: "" for k in FIELDS}
    bad, events, target = [], [], None
    a = {"calls": 0, "other": 0, "overlap": 0, "waits": 0, "sec": ""}
    b = {"first": None, "last": 0, "skipped": 0, "sec": ""}
    mism = fell = unclean = errors = summaries = 0
    installed = {"A": None, "B": None}
    for n, line in enumerate(lines, 1):
        text = line.split("] ", 2)[-1] if line.startswith("[") else line
        m = re.search(r"LoadAccel (\S+) \(built for stages (\d) / (\d)\), runtime (\S+)", text)
        if m:
            r["version"], r["runtime"] = m[1], m[4]
        if "nothing installed" in text:
            if text.startswith("runtime is not"):
                installed = {"A": "runtime", "B": "runtime"}
            elif text.startswith("source-file lists"):
                installed["A"] = "off" if "= off" in text else "refused"
            elif text.startswith("large refs"):
                installed["B"] = "off" if "= off" in text else "refused"
        m = re.match(r"source-file lists: stage (\d) \(", text)
        if m:
            installed["A"] = int(m[1])
        m = re.match(r"large refs: stage (\d) \(", text)
        if m:
            installed["B"] = int(m[1])
        if "[error]" in line:
            errors += 1
            bad.append(f"line {n}: {text[:160]}")
        elif re.search(r"MISMATCH #\d", text):
            bad.append(f"line {n}: {text[:160]}")
        m = re.match(r"(LARGE REFS )?SUMMARY at (\w+):(.*)", text)
        if m:
            target = "B" if m[1] else "A"
            summaries += 1
            if target == "A":
                events.append(m[2])
            body = m[3]
            if "FELL BACK" in body:
                fell += 1
                bad.append(f"line {n}: {target} fell back to the engine ({m[2]})")
            mm = re.search(r"MISMATCHES (\d+)", body)
            if mm and int(mm[1]):
                mism = max(mism, int(mm[1])) if target == "A" else mism + 0
                bad.append(f"line {n}: {target} MISMATCHES {mm[1]} ({m[2]})")
            calls = int(re.search(r"calls (\d+)", body)[1])
            sec = re.search(r"time (?:spent|in the engine[^,]*?) ([\d.]+) s", body)
            if target == "A":
                a["calls"], a["sec"] = calls, sec[1] if sec else ""
            else:
                b["first"] = calls if b["first"] is None else b["first"]
                b["last"] = calls
                sk = re.search(r"engine walks skipped (\d+)", body)
                b["skipped"], b["sec"] = int(sk[1]) if sk else 0, sec[1] if sec else ""
            continue
        if "audit" in text and "NOT CLEAN" in text:
            unclean += 1
            bad.append(f"line {n}: {text.strip()[:160]}")
        m = re.search(r"threads: .*calls from other threads (\d+).*overlapped another call (\d+).*waited for the \w+ lock (\d+)", text)
        if m and target == "A":
            a["other"], a["overlap"], a["waits"] = int(m[1]), int(m[2]), int(m[3])
    times = [t for t in map(stamp, lines) if t is not None]
    span = (times[-1] - times[0]) % 86400 if times else 0
    r.update(stages=f"{installed['A']}/{installed['B']}", summaries=summaries, events=" ".join(events),
             mismatches=sum(1 for x in bad if "MISMATCH" in x), fell_back=fell, audits_not_clean=unclean, errors=errors,
             a_calls=a["calls"], a_other_thread_calls=a["other"], a_overlaps=a["overlap"], a_lock_waits=a["waits"],
             a_seconds=a["sec"], b_calls_load=b["first"] or 0, b_skipped=b["skipped"],
             b_calls_after_load=(b["last"] - b["first"]) if b["first"] is not None else 0, b_seconds=b["sec"],
             session_seconds=round(span))
    missing = []
    if "kDataLoaded" not in events:
        missing.append("data load did not finish (no kDataLoaded summary)")
    for ev, what in (("kNewGame", "new game"), ("kPostLoadGame", "save load"), ("kSaveGame", "save")):
        if ev not in events:
            missing.append(what)
    if not a["other"]:
        missing.append("part A from other threads (nothing after the menu)")
    if not r["b_calls_after_load"]:
        missing.append("part B after the data load (no call)")
    if bad:
        verdict = "FAIL"
    elif installed["A"] != 3 and installed["B"] != 3:
        verdict = "INERT"
    elif "kDataLoaded" not in events:
        verdict = "FAIL"
        bad.append("no kDataLoaded summary: the session ended during the load (crash or closed), or the log is cut")
    else:
        verdict = "PASS"
    r["verdict"], r["not_exercised"] = verdict, "; ".join(missing)
    return r, bad


if __name__ == "__main__":
    log = sys.argv[1]
    r, bad = judge(log)
    r.update(date=opt("--date", datetime.date.fromtimestamp(os.path.getmtime(log)).isoformat()), who=opt("--who", "?"),
             dll=opt("--dll", "?"), order=opt("--order", "?"), plugins=opt("--plugins"), load_on_s=opt("--load-on"),
             load_off_s=opt("--load-off"), note=opt("--note"))
    print(f"{r['verdict']}  LoadAccel {r['version']} on runtime {r['runtime']}, stages A/B {r['stages']}, "
          f"{r['summaries']} summary blocks ({r['events']}), session {r['session_seconds']} s")
    print(f"  A: {r['a_calls']} calls, {r['a_seconds']} s, other threads {r['a_other_thread_calls']}, overlaps "
          f"{r['a_overlaps']}, lock waits {r['a_lock_waits']}")
    print(f"  B: {r['b_calls_load']} calls in the load ({r['b_skipped']} walks skipped, {r['b_seconds']} s), "
          f"{r['b_calls_after_load']} after it")
    for x in bad:
        print("  FAIL:", x)
    print("  not exercised:", r["not_exercised"] or "nothing (new game, save load, save, other threads and part B after the load all seen)")
    if "--ledger" in sys.argv:
        ledger = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ledger.csv")
        new = not os.path.isfile(ledger)
        with open(ledger, "a", encoding="utf-8", newline="") as fh:
            w = csv.DictWriter(fh, FIELDS)
            if new:
                w.writeheader()
            w.writerow(r)
        print("  ledger:", ledger)
    sys.exit({"PASS": 0, "INERT": 3}.get(r["verdict"], 1))
