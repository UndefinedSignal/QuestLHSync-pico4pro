"""Packs a PICO release into out/release-pico-<version>/ from what build.bat and pico/build_module.py built:

  QuestLHSync-pico-steamvr-<version>.zip   QuestLHSync-pico/: the SteamVR driver folder questlhsync/ (driver,
                                            dashboard app, openvr_api.dll), register_driver.ps1, INSTALL.txt
  QuestLHSync-pico-module-<version>.zip    the PICO's Magisk module, as built
  SHA256SUMS.txt

Each package carries LICENSE and THIRD_PARTY_NOTICES.md. Refuses when a build is older than its sources.
"""
import hashlib
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from build_module import VERSION  # noqa: E402

V = VERSION.lstrip("v")
DRIVER = os.path.join(ROOT, "driver", "questlhsync")
MODULE = os.path.join(ROOT, "out", f"QuestLHSync-pico-module-{V}.zip")
NOTICES = ("LICENSE", "THIRD_PARTY_NOTICES.md")
FILES = ("driver.vrdrivermanifest", "resources/settings/default.vrsettings", "bin/win64/driver_questlhsync.dll",
         "bin/win64/QuestLHSync.exe", "bin/win64/openvr_api.dll")


def newest(*dirs):
    t = 0
    for d in dirs:
        for root, _, files in os.walk(os.path.join(ROOT, d)):
            t = max([t] + [os.path.getmtime(os.path.join(root, f)) for f in files])
    return t


def main():
    checks = [(os.path.join(DRIVER, "bin", "win64", "driver_questlhsync.dll"), newest("src/driver", "src/common"),
               "build.bat"),
              (os.path.join(DRIVER, "bin", "win64", "QuestLHSync.exe"), newest("src/overlay", "src/common"), "build.bat"),
              (MODULE, newest("pico/src", "pico/module", "magisk/src", "src/headset"), "python pico/build_module.py")]
    for path, src, how in checks:
        if not os.path.exists(path):
            sys.exit(f"{path} missing: run {how}")
        if os.path.getmtime(path) < src:
            sys.exit(f"{path} is older than its sources: run {how}")
    out = os.path.join(ROOT, "out", f"release-pico-{V}")
    os.makedirs(out, exist_ok=True)
    drv = os.path.join(out, f"QuestLHSync-pico-steamvr-{V}.zip")
    with zipfile.ZipFile(drv, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in FILES:
            z.write(os.path.join(DRIVER, rel), "QuestLHSync-pico/questlhsync/" + rel)
        z.write(os.path.join(HERE, "register_driver.ps1"), "QuestLHSync-pico/register_driver.ps1")
        z.write(os.path.join(HERE, "INSTALL.txt"), "QuestLHSync-pico/INSTALL.txt")
        for f in NOTICES:
            z.write(os.path.join(ROOT, f), "QuestLHSync-pico/" + f)
    mod = os.path.join(out, os.path.basename(MODULE))
    with zipfile.ZipFile(MODULE) as src, zipfile.ZipFile(mod, "w", zipfile.ZIP_DEFLATED) as z:
        for info in src.infolist():  # as built: Magisk reads its META-INF and module.prop from it
            z.writestr(info, src.read(info))
        for f in NOTICES:
            z.write(os.path.join(ROOT, f), f)
    with open(os.path.join(out, "SHA256SUMS.txt"), "w", newline="\n") as sums:
        for p in (drv, mod):
            with open(p, "rb") as fh:
                sums.write(f"{hashlib.sha256(fh.read()).hexdigest()}  {os.path.basename(p)}\n")
    print(f"packed {out}")


if __name__ == "__main__":
    main()
