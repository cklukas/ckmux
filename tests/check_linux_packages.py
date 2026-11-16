# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import pathlib
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--package", required=True, type=pathlib.Path)
    parser.add_argument("--scratch", required=True, type=pathlib.Path)
    parser.add_argument("--version", required=True)
    arguments = parser.parse_args()
    arguments.scratch.mkdir()
    package = arguments.package.resolve()
    checker = arguments.source.resolve() / "packaging/linux/check-deb.sh"
    architecture = subprocess.check_output(
        ["dpkg", "--print-architecture"], text=True
    ).strip()
    for case in ("checksum", "version", "architecture", "modified-package"):
        root = arguments.scratch / case
        root.mkdir()
        candidate = root / package.name
        shutil.copyfile(package, candidate)
        digest = hashlib.sha256(candidate.read_bytes()).hexdigest()
        if case == "checksum":
            digest = "0" * 64
        if case == "modified-package":
            with candidate.open("ab") as output:
                output.write(b"corrupted after checksum generation")
        candidate.with_name(candidate.name + ".sha256").write_text(
            f"{digest}  {candidate.name}\n", encoding="ascii"
        )
        expected_version = "0.0.0" if case == "version" else arguments.version
        expected_architecture = (
            "amd64" if architecture == "arm64" else "arm64"
        ) if case == "architecture" else architecture
        result = subprocess.run(
            ["bash", str(checker), str(candidate), str(root / "check"),
             expected_version, expected_architecture],
            capture_output=True, text=True, timeout=30,
        )
        expected_error = "checksum" if case == "modified-package" else case
        if result.returncode == 0 or expected_error not in result.stdout + result.stderr:
            raise RuntimeError(f"{case} was not rejected correctly: {result}")
        if (root / "check").exists():
            raise RuntimeError(f"{case} reached extraction or execution")
        print(f"PASS: reject {case} before extraction or execution")


if __name__ == "__main__":
    main()
