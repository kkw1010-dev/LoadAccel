r"""Licence files and archives of a LoadAccel test release (GPL-3.0-or-later since 2026-10-03, user decision D49).

    python tools\make_release.py --notices
    python tools\make_release.py --version 0.3.0 --dll build\dist\tester-0.3.0\LoadAccel.dll --expect-sha256 <hash>
                                 [--author <AUTHOR string the DLL was built with>] [--out <folder>]

Why GPL: CommonLibSSE-NG (alandtse/CommonLibVR, branch ng), linked statically, was relicensed from MIT to
GPL-3.0-or-later (with a Modding Exception and a GPL-3.0 Linking Exception) in July and August 2026. A build made
with it is distributed under GPL-3.0-or-later with its source.

Order: --notices writes public\THIRD-PARTY-NOTICES.txt from the licence files of the build (build\work); then
tools\export_public.py writes and checks the public copy (commit it there); then the second form makes:
  "LoadAccel <version> (test).7z"           the DLL (checked against --expect-sha256), LoadAccel.ini, and at the
                                            root "LoadAccel README.txt" (package\README.txt, BOM + CRLF),
                                            "LoadAccel LICENSE.txt" and "LoadAccel THIRD-PARTY-NOTICES.txt"
  "LoadAccel <version> (test) - source.7z"  the public copy without .git; with --author the AUTHOR line of
                                            CMakeLists.txt is set back to the string the DLL was built with, so
                                            the archive is the source of that very binary
The DLL is never rebuilt here. No file of either archive may hold this machine's user name or profile path.
"""
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SEVEN_ZIP = os.environ.get("SEVEN_ZIP") or shutil.which("7z") or os.path.join(os.environ.get("ProgramFiles", ""), "7-Zip", "7z.exe")
# Code compiled into LoadAccel.dll besides its own, checked against the DLL's symbols (PDB of build\work) for 0.3.0:
# fmt, spdlog and rapidcsv functions are present; DirectXTK, SimpleIni and xbyak contribute nothing.
VCPKG_IN_DLL = ("fmt", "spdlog", "rapidcsv")


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def commonlib_dir():
    path = os.environ.get("COMMONLIB_DIR")
    if not path:
        local = os.path.join(HERE, "local.ps1")
        if os.path.isfile(local):
            m = re.search(r"COMMONLIB_DIR\s*=\s*'([^']+)'", open(local, encoding="utf-8").read())
            path = m.group(1) if m else None
    if not path or not os.path.isfile(os.path.join(path, "EXCEPTIONS.md")):
        raise SystemExit("set COMMONLIB_DIR to the CommonLibSSE-NG checkout the DLL was built with")
    return path


def notices(build_dir):
    nl, rule = "\n", "=" * 78
    lib = commonlib_dir()
    commit = subprocess.run(["git", "-C", lib, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    parts = [
        "LoadAccel is licensed under the GNU General Public License v3.0 or later (LoadAccel LICENSE.txt in the" + nl,
        "test archive, LICENSE in the source). Its source is published with the archive and on GitHub." + nl + nl,
        "LoadAccel.dll contains code from the projects below. Their licences and notices follow." + nl + nl,
        "- CommonLibSSE-NG (https://github.com/alandtse/CommonLibVR, branch ng, commit %s):" % (commit or "unknown") + nl,
        "  GPL-3.0-or-later (the same licence text) with the exceptions quoted below, and the MIT notice of" + nl,
        "  the original CommonLibSSE code it keeps." + nl,
    ]
    sections = [("CommonLibSSE-NG: licence exceptions (EXCEPTIONS.md)", os.path.join(lib, "EXCEPTIONS.md")),
                ("CommonLibSSE-NG: notice of the original CommonLibSSE code (licenses/LICENSE-MIT.txt)",
                 os.path.join(lib, "licenses", "LICENSE-MIT.txt")),
                ("Hacker Disassembler Engine 64 (hde64, compiled into CommonLibSSE-NG; from MinHook)",
                 os.path.join(lib, "licenses", "LICENSE-hde64.txt"))]
    share = os.path.join(build_dir, "vcpkg_installed", "x64-windows-static-md", "share")
    info = os.path.join(build_dir, "vcpkg_installed", "vcpkg", "info")
    for name in VCPKG_IN_DLL:
        path = os.path.join(share, name, "copyright")
        if not os.path.isfile(path):
            raise SystemExit("no licence file for %s under %s" % (name, share))
        listed = glob.glob(os.path.join(info, name + "_*_x64-windows-static-md.list"))
        ver = os.path.basename(listed[0]).split("_")[1] if listed else "?"
        parts.append("- %s %s" % (name, ver) + nl)
        sections.append(("%s %s" % (name, ver), path))
    parts.append("- hde64 (Hacker Disassembler Engine 64 C, from MinHook): BSD-2-Clause" + nl)
    for title, path in sections:
        with open(path, encoding="utf-8-sig", errors="replace") as f:
            parts.append(nl + rule + nl + title + nl + rule + nl + nl + f.read().strip() + nl)
    return "".join(parts)


def check_private(paths, root):
    profile = os.path.expanduser("~")
    user = os.path.basename(profile).lower()
    words = {profile.lower(), profile.replace("\\", "/").lower()}
    for p in paths:
        data = open(p, "rb").read().lower()
        for w in words:
            for enc in (w.encode("utf-8"), w.encode("utf-16-le")):
                if enc in data:
                    raise SystemExit("%s holds this machine's profile path" % os.path.relpath(p, root))
        # The bare user name is a prefix of the public GitHub account (kkw1010-dev): that one is allowed.
        for enc, tail in ((user.encode("utf-8"), b"10-dev"), (user.encode("utf-16-le"), "10-dev".encode("utf-16-le"))):
            at = data.find(enc)
            while at != -1:
                if not data.startswith(tail, at + len(enc)):
                    raise SystemExit("%s holds this machine's user name" % os.path.relpath(p, root))
                at = data.find(enc, at + 1)


def seven_zip(folder, archive):
    if os.path.exists(archive):
        os.remove(archive)
    result = subprocess.run([SEVEN_ZIP, "a", "-t7z", "-mx=9", archive, "*"], cwd=folder, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit("7-Zip failed:\n" + result.stdout + result.stderr)


def sha256(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest().upper()


def files_of(root):
    return sorted(os.path.join(r, f) for r, _, fs in os.walk(root) for f in fs)


def main():
    build_dir = os.path.join(REPO, "build", "work")
    public_notices = os.path.join(REPO, "public", "THIRD-PARTY-NOTICES.txt")
    if "--notices" in sys.argv:
        with open(public_notices, "w", encoding="utf-8", newline="\n") as f:
            f.write(notices(build_dir))
        print("written:", public_notices)
        return
    version, dll, expect = arg("--version"), arg("--dll"), arg("--expect-sha256")
    if not (version and dll and expect):
        raise SystemExit(__doc__)
    if sha256(dll) != expect.upper():
        raise SystemExit("%s is not the expected build (SHA-256 %s)" % (dll, sha256(dll)))
    out_root = arg("--out", os.path.join(os.path.expanduser("~"), "Downloads"))
    source = arg("--source", os.path.join(os.path.dirname(REPO), "LoadAccel-public"))
    name = "LoadAccel %s (test)" % version
    text = notices(build_dir)
    for copy in (public_notices, os.path.join(source, "THIRD-PARTY-NOTICES.txt")):
        if not os.path.isfile(copy) or open(copy, encoding="utf-8").read() != text:
            raise SystemExit("%s differs from the notices of the build: run --notices, then export_public.py" % copy)

    out = os.path.join(REPO, "build", "package", name)
    if os.path.isdir(out):
        shutil.rmtree(out)
    plugins = os.path.join(out, "SKSE", "Plugins")
    os.makedirs(plugins)
    shutil.copyfile(dll, os.path.join(plugins, "LoadAccel.dll"))
    ini = open(os.path.join(REPO, "package", "LoadAccel.ini"), encoding="utf-8").read().replace("\r\n", "\n")
    open(os.path.join(plugins, "LoadAccel.ini"), "w", encoding="utf-8", newline="\r\n").write(ini)
    readme = open(os.path.join(REPO, "package", "README.txt"), encoding="utf-8").read().replace("\r\n", "\n")
    readme = readme.replace("@VERSION@", version).replace("@SHA256@", expect.upper())
    open(os.path.join(out, "LoadAccel README.txt"), "w", encoding="utf-8-sig", newline="\r\n").write(readme)
    shutil.copyfile(os.path.join(REPO, "public", "LICENSE"), os.path.join(out, "LoadAccel LICENSE.txt"))
    open(os.path.join(out, "LoadAccel THIRD-PARTY-NOTICES.txt"), "w", encoding="utf-8", newline="\r\n").write(text)
    check_private(files_of(out), out)
    archive = os.path.join(out_root, name + ".7z")
    seven_zip(out, archive)
    print("PASS test archive: %s  (%d bytes, SHA-256 %s)" % (archive, os.path.getsize(archive), sha256(archive)))
    for p in files_of(out):
        print("    %-40s %9d bytes" % (os.path.relpath(p, out), os.path.getsize(p)))

    if not os.path.isfile(os.path.join(source, "LICENSE")):
        raise SystemExit("no public copy at %s: run tools\\export_public.py first" % source)
    src_out = os.path.join(REPO, "build", "package", name + " - source")
    if os.path.isdir(src_out):
        shutil.rmtree(src_out)
    shutil.copytree(source, src_out, ignore=shutil.ignore_patterns(".git", "build", "__pycache__"))
    author = arg("--author")
    if author:
        cmake = os.path.join(src_out, "CMakeLists.txt")
        body = open(cmake, encoding="utf-8").read()
        body, n = re.subn(r'AUTHOR "[^"]*"', 'AUTHOR "%s"' % author, body)
        if n != 1:
            raise SystemExit("CMakeLists.txt has %d AUTHOR lines" % n)
        open(cmake, "w", encoding="utf-8", newline="\n").write(body)
        print("    source: AUTHOR set to the string the DLL was built with")
    check_private(files_of(src_out), src_out)
    src_archive = os.path.join(out_root, name + " - source.7z")
    seven_zip(src_out, src_archive)
    print("PASS source archive: %s  (%d files, %d bytes, SHA-256 %s)" % (src_archive, len(files_of(src_out)),
                                                                        os.path.getsize(src_archive), sha256(src_archive)))


if __name__ == "__main__":
    main()
