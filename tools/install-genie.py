"""Compile mods/halo-genie.wh.cpp the way Windhawk does and install it into a
portable Windhawk. The engine watches AppData/Engine/Mods and loads the newest
DLL, so this applies live, no admin needed.

    python tools/install-genie.py [--windhawk C:/Users/me/Windhawk] [--include '*'] [--off] [--log]

--off disables the mod (Disabled=1) without compiling. --include limits which
processes get it (default *; use e.g. charmap.exe to trial it on one app).
"""
import argparse
import random
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MOD_ID = "local@halo-genie"


def meta(source, key):
    return re.search(rf"^// @{key}\s+(.*)$", source, re.M).group(1).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windhawk", default=str(Path.home() / "Windhawk"))
    ap.add_argument("--include", default="*")
    ap.add_argument("--off", action="store_true")
    ap.add_argument("--log", action="store_true", help="enable Windhawk logging (frame-time stats)")
    args = ap.parse_args()
    wh = Path(args.windhawk)
    mods = wh / "AppData" / "Engine" / "Mods"
    ini = mods / f"{MOD_ID}.ini"
    source = (REPO / "mods" / "halo-genie.wh.cpp").read_text(encoding="utf-8")
    version = meta(source, "version")

    if args.off:
        if not ini.exists():
            sys.exit("not installed")
        text = ini.read_text(encoding="utf-16")
        text = re.sub(r"^Disabled=.*$", "Disabled=1", text, flags=re.M)
        text = re.sub(r"^SettingsChangeTime=.*$", f"SettingsChangeTime={int(time.time())}", text, flags=re.M)
        ini.write_text(text, encoding="utf-16", newline="")
        print(f"{MOD_ID}: disabled")
        return

    # Same recipe as the Windhawk editor (extension.js compileModInternal).
    engine = next((wh / "Engine").iterdir())  # e.g. Engine/1.7.3
    out_dir = mods / "64"
    dll = f"{MOD_ID}_{version}_{random.randint(100000, 999999)}.dll"
    options = meta(source, "compilerOptions").split()
    cmd = [str(wh / "Compiler" / "bin" / "clang++.exe"),
           "-std=c++23", "-O2", "-shared", "-DUNICODE", "-D_UNICODE",
           "-DWINVER=0x0A00", "-D_WIN32_WINNT=0x0A00", "-D_WIN32_IE=0x0A00", "-DNTDDI_VERSION=0x0A000008",
           "-D__USE_MINGW_ANSI_STDIO=0", "-DWH_MOD", f'-DWH_MOD_ID=L"{MOD_ID}"', f'-DWH_MOD_VERSION=L"{version}"',
           str(engine / "64" / "windhawk.lib"), "-x", "c++", "-", "-include", "windhawk_api.h",
           "-target", "x86_64-w64-mingw32", "-Wl,--export-all-symbols", "-o", str(out_dir / dll), *options]
    out_dir.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(cmd, input=source.encode("utf-8"), cwd=wh / "Compiler", capture_output=True)
    sys.stdout.write(r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace"))
    if r.returncode:
        sys.exit(f"compile failed ({r.returncode})")

    fields = {"LibraryFileName": dll, "Disabled": 0, "Include": args.include,
              "Exclude": "halo.exe|guardian.exe", "Architecture": "x86-64", "Version": version,
              "SettingsChangeTime": int(time.time())}
    if args.log:
        fields["LoggingEnabled"] = 1
    text = "\r\n".join(["[Mod]"] + [f"{k}={v}" for k, v in fields.items()] + ["", "[Settings]", ""])
    ini.write_text(text, encoding="utf-16", newline="")  # UTF-16 with BOM
    print(f"{MOD_ID}: installed {dll}, Include={args.include}")


if __name__ == "__main__":
    main()
