#!/usr/bin/env python3
"""Download only the pinned Unleashed 084 IR sources for the host simulator."""
import argparse
import hashlib
from pathlib import Path
import tarfile
import tempfile
import urllib.request

URL = "https://github.com/DarkFlippers/unleashed-firmware/archive/refs/tags/unlshd-084.tar.gz"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ir-sdk-download-") as folder:
        archive = Path(folder) / "sdk.tar.gz"
        urllib.request.urlretrieve(URL, archive)
        with tarfile.open(archive) as tar:
            count = 0
            for member in tar:
                relative = Path(*Path(member.name).parts[1:])
                if not member.isfile() or relative.parts[:2] != ("lib", "infrared"):
                    continue
                if ".." in relative.parts or relative.is_absolute():
                    raise ValueError("invalid archive path")
                target = args.destination / relative
                if target.exists():
                    raise FileExistsError(f"refusing to overwrite existing source: {target}")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(tar.extractfile(member).read())
                count += 1
        print(f"Extracted {count} IR source files from Unleashed 084")
        print(f"Archive SHA-256: {hashlib.sha256(archive.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
