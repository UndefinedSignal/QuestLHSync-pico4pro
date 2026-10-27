"""Builds the PICO 4 Pro Magisk module: out/QuestLHSync-pico-module-<version>.zip

  lhsyncd       ../src/headset/lhsyncd.c (shared) + src/pico.c, a static arm64 binary: Zig's clang for aarch64-linux-musl
                (pip install ziglang), or the Android NDK's clang when ANDROID_NDK_HOME is set
  lhsight.js    src/lhsight.js with ../magisk/src/lhsight.c inlined (Frida CModule), no time limit
  frida-inject  Frida 17.10.0 android-arm64, the Quest module's (../magisk/build_module.py), checked by hash
  cammap.txt    the camera id of each of pvrtrackingservice's four camera rings, in buffer order

pico.c uses no Bionic-only API, so the musl build needs no Android sysroot.
"""
import os
import re
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "out")
HEADSET = os.path.join(ROOT, "src", "headset")
sys.path.insert(0, os.path.join(ROOT, "magisk"))
import build_module as quest  # noqa: E402  (frida_inject(), lf())

with open(os.path.join(ROOT, "src", "common", "qlhs_status.h"), encoding="utf-8") as _h:
    VERSION = "v" + re.search(r'#define QLHS_RELEASE "([^"]+)"', _h.read()).group(1) + "-pico3"
CODE = 3


def compile_daemon(out):
    srcs = [os.path.join(HEADSET, "lhsyncd.c"), os.path.join(HERE, "src", "pico.c")]
    flags = ["-O2", "-Wall", "-Wno-unused-result", f'-DMODULE_VERSION="{VERSION}"', f"-I{HEADSET}", *srcs, "-lm", "-s",
             "-o", out]
    ndk = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK_ROOT")
    if ndk:
        host = "windows-x86_64" if os.name == "nt" else "linux-x86_64"
        clang = os.path.join(ndk, "toolchains", "llvm", "prebuilt", host, "bin", "clang" + (".exe" if os.name == "nt" else ""))
        cmd = [clang, "--target=aarch64-linux-android29", "-fPIE", "-pie", *flags]
    else:
        try:
            import ziglang  # noqa: F401
        except ImportError:
            sys.exit("no compiler: pip install ziglang, or set ANDROID_NDK_HOME")
        cmd = [sys.executable, "-m", "ziglang", "cc", "-target", "aarch64-linux-musl", "-static", *flags]
    subprocess.run(cmd, check=True)


def main():
    build = os.path.join(HERE, "build")
    os.makedirs(build, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    daemon = os.path.join(build, "lhsyncd")
    compile_daemon(daemon)
    js = open(os.path.join(HERE, "src", "lhsight.js"), encoding="utf-8").read()
    c = open(os.path.join(ROOT, "magisk", "src", "lhsight.c"), encoding="utf-8").read()
    js = js.replace("@CSRC@", c).replace("@DUR@", "0")
    prop = open(os.path.join(HERE, "module", "module.prop.in"), encoding="utf-8").read()
    prop = prop.replace("@VERSION@", VERSION).replace("@CODE@", str(CODE))
    zpath = os.path.join(OUT, f"QuestLHSync-pico-module-{VERSION.lstrip('v')}.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        def add(name, data, mode=0o644):
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 27, 0, 0, 0))
            info.external_attr = (0o100000 | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
        m = os.path.join(HERE, "module")
        for rel in ("META-INF/com/google/android/update-binary", "META-INF/com/google/android/updater-script",
                    "service.sh", "customize.sh", "uninstall.sh", "cammap.txt"):
            add(rel, quest.lf(open(os.path.join(m, rel), encoding="utf-8").read()),
                0o755 if rel.endswith((".sh", "binary")) else 0o644)
        add("module.prop", quest.lf(prop))
        add("lhsight.js", quest.lf(js))
        add("lhsyncd", open(daemon, "rb").read(), 0o755)
        add("frida-inject", quest.frida_inject(), 0o755)
    print(f"built {zpath} ({os.path.getsize(zpath) / 1e6:.1f} MB)")
    return zpath


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
