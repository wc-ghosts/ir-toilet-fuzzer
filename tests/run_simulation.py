#!/usr/bin/env python3
"""Build/run the actual app with hardware stubs and real Unleashed 084 IR codecs."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-source", type=Path, required=True, help="source tree containing lib/infrared/encoder_decoder")
    parser.add_argument("--report", type=Path, default=Path("simulation-report.txt"))
    parser.add_argument("--interactive", action="store_true", help="drive the real app controller from the terminal")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    codec = args.sdk_source / "lib/infrared/encoder_decoder"
    if not (codec / "infrared.c").is_file():
        parser.error("SDK source must contain the Unleashed encoder/decoder C sources (the headers-only SDK zip is insufficient)")
    with tempfile.TemporaryDirectory(prefix="ir-lab-sim-build-") as directory:
        exe = Path(directory) / "simulate_app"
        flags = ["gcc", "-std=c11", "-g", "-Wall", "-Wextra", "-Wno-misleading-indentation", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-fno-strict-aliasing", "-I" + str(root / "tests/shims"), "-I" + str(codec), "-I" + str(root)]
        # The ARM SDK accesses byte-packed decoder buffers as uint32_t. Build
        # those upstream codecs with alignment/shift-base checks disabled: they
        # also assemble high-bit bytes via signed integer shifts. Retain ASan
        # and other UBSan checks there; all app UBSan checks stay enabled.
        objects = []
        for index, source in enumerate(sorted(codec.rglob("*.c"))):
            obj = Path(directory) / f"codec-{index}.o"
            subprocess.run(flags + ["-fno-sanitize=alignment,shift-base", "-c", str(source), "-o", str(obj)], check=True)
            objects.append(str(obj))
        source = root / "tests/simulate_app.c"
        command = flags + [str(source), str(root / "ir_lab_core.c"), str(root / "ir_lab_protocol.c"), *objects, "-o", str(exe)]
        subprocess.run(command, check=True)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(exe), "--interactive" if args.interactive else str(args.report.resolve())], check=True, cwd=root)


if __name__ == "__main__":
    main()
