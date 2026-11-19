#!/usr/bin/env python3
"""Fetch the board's 3D models from EasyEDA/LCSC with easyeda2kicad, and check them.

The models aren't in the repo (EasyEDA's redistribution terms are unclear), but every
one comes from an LCSC part, so this rebuilds pcb/3d and pcb/EASYEDA_MODELS:

    pip install easyeda2kicad
    python tools/fetch_3d_models.py

Each file is compared with pcb/models.sha256, the models the b43 board was made with.
A mismatch usually just means EasyEDA has since updated that model; KiCad will still
show it. Only the 3D view uses these; the footprints and gerbers don't.
"""
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

PCB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pcb")

# file under pcb/  ->  LCSC part whose EasyEDA 3D model it is
MODELS = {
    "3d/ESP32-C6-WROOM-1.STEP": "C5366877",
    "EASYEDA_MODELS/BATTERY-SMD_18650-1S-L77.1-W20.7-1.step": "C7525438",
    "EASYEDA_MODELS/BUZ-SMD_5P-L15.0-W15.0-P13.7-BL.step": "C482341",
    "EASYEDA_MODELS/CONN-TH_PH2.00-LI-2P.step": "C131337",
    "EASYEDA_MODELS/HDR-TH_3P-P2.54-V-M_PZ254V-11-03P.step": "C2937625",
    "EASYEDA_MODELS/LED-SEG-TH_0.56_4p.step": "C46908",
    "EASYEDA_MODELS/LED-SMD_4P-L5.0-W5.0-H1.57-TL_WS2812B-B.step": "C52941391",
    "EASYEDA_MODELS/LED0603-R-RD_WHITE.step": "C2290",
    "EASYEDA_MODELS/LED0603_L1.6-W0.8-H0.6.step": "C2286",
    "EASYEDA_MODELS/SOP-16_L9.9-W3.9-P1.27-LS6.1-BL.step": "C427456",
    "EASYEDA_MODELS/SOP-8_L4.9-W3.9-P1.27-LS6.0-BL-1.step": "C2875838",
    "EASYEDA_MODELS/SOT-23-3P_L2.9-W1.3-H1.0-LS2.4-P0.95.step": "C20917",
    "EASYEDA_MODELS/SOT-23-6_L2.9-W1.6-H1.5-LS2.8-P0.95.step": "C12044",
    "EASYEDA_MODELS/SW-SMD_4P-L5.1-W5.1-P3.70-LS6.5-TL_H1.5.step": "C318884",
    "EASYEDA_MODELS/USB-C-SMD_TYPE-C16PIN.step": "C393939",
    "EASYEDA_MODELS/USP-6C_L2.0-W1.8-P0.50-BL-EP.step": "C3013657",
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def expected():
    out = {}
    with open(os.path.join(PCB, "models.sha256")) as f:
        for line in f:
            digest, name = line.split(None, 1)
            out[name.strip().lstrip("*")] = digest
    return out


def main():
    want = expected()
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for rel, lcsc in MODELS.items():
            dest = os.path.join(PCB, rel)
            if not os.path.exists(dest):
                out = os.path.join(tmp, lcsc, "lib")
                r = subprocess.run([sys.executable, "-m", "easyeda2kicad", "--3d", "--lcsc_id", lcsc,
                                    "--output", out], capture_output=True, text=True)
                steps = glob.glob(os.path.join(tmp, lcsc, "**", "*.step"), recursive=True)
                if r.returncode or not steps:
                    print(f"FAILED  {rel} ({lcsc})\n{r.stdout}{r.stderr}")
                    bad += 1
                    continue
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                shutil.copyfile(steps[0], dest)
            ok = sha256(dest) == want.get(rel)
            bad += not ok
            print(f"{'ok     ' if ok else 'DIFFERS'} {rel} ({lcsc})")
    print("All models match." if not bad else f"{bad} model(s) failed or differ (see above).")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
