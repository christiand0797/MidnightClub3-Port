#!/usr/bin/env python3
"""Exercise the native tuning regression with every vehicle tune from a user's own extracted disc archives.

No game files are modified. Only numeric suspension parameters enter a temporary fixture, deleted after use.
Example: python audit_vehicle_tuning.py --assets templates/game/assets --test-exe out/tests/tuning_test.exe
The executable must include MC3_TEST_REAL_WHEEL_SOLVER (generated sources available when it was built).
"""
from __future__ import annotations

import argparse
import math
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "texture_pack"))
from dave import Archive


def value(text: str, key: str, default: float | None = None) -> float:
    match = re.search(r"\b" + re.escape(key) + r"\s+([-+0-9.eE]+)", text)
    if not match:
        if default is not None:
            return default
        raise ValueError(f"Missing {key}")
    out = float(match[1])
    if not math.isfinite(out):
        raise ValueError(f"Non-finite {key}")
    return out


def collect(assets: Path) -> list[str]:
    texts: dict[str, str] = {}
    for path in sorted(assets.glob("assets_x.dat*")):
        if not path.is_file() or not re.fullmatch(r"assets_x\.dat(?:_[0-9]+)?", path.name):
            continue
        archive = Archive(path)
        for entry in archive.entries:
            if entry.name.startswith("tune/vehicle/") and entry.name.endswith((".vehsim", ".vehmodel")):
                texts[entry.name] = archive.read(entry).decode("latin1")
    rows = []
    for name, text in sorted(texts.items()):
        if not name.endswith(".vehsim"):
            continue
        stem = re.sub(r"_(?:base|mods)\.vehsim$", "", name)
        model = texts.get(stem + ".vehmodel")
        if model is None:
            print(f"{name}: no standalone model tune; arch limits remain a runtime geometry check.", file=sys.stderr)
            model = ""
        fields = []
        for axle, model_key in (("WheelFront", "SuspensionLimitFront"), ("WheelBack", "SuspensionLimitRear")):
            block = re.search(r"\b" + axle + r"\s*\{([^}]+)\}", text)
            if not block:
                raise ValueError(f"No {axle} in {name}")
            # vehWheel tune constructor 0xA69C0 / stores 0xA6ABC..0xA6AC6: .5/.18/.5/.2.
            # Numerous shipped files intentionally omit TravelLimit and TravelExtent.
            fields.extend(value(block[1], key, default) for key, default in
                          (("TravelLimit", .5), ("TravelExtent", .18), ("SuspensionLimit", .5), ("SuspensionExtent", .2)))
            if re.search(r"\b" + model_key + r"\s", model):
                fields.append(value(model, model_key))
            else:
                # Aprilia's model file is empty: the runtime derives this limit from its bundled geometry.
                # Its actual wheel tune is still covered; a zero arch query only exercises the generic bypass.
                print(f"{name}: {model_key} comes from runtime geometry; using zero only for the fit-query fixture.",
                      file=sys.stderr)
                fields.append(0.0)
        rows.append(name + " " + " ".join(format(v, ".9g") for v in fields))
    if not rows:
        raise ValueError("No vehicle tunes found in the supplied original assets")
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--test-exe", type=Path, required=True)
    args = parser.parse_args()
    rows = collect(args.assets.resolve())
    with tempfile.TemporaryDirectory(prefix="mc3_vehicle_tuning_") as temporary:
        fixture = Path(temporary) / "suspension.txt"
        fixture.write_text("\n".join(rows) + "\n", encoding="utf8")
        return subprocess.run([str(args.test_exe.resolve()), str(fixture)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
