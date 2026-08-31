#!/usr/bin/env python3
# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT

"""Generate the versioned Homebrew formula shipped with a ckmux release."""

from __future__ import annotations

import argparse
import hashlib
import re
import tempfile
from pathlib import Path
from urllib.parse import urlparse


VERSION_PATTERN = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?")


def formula_text(version: str, source_url: str, source_sha256: str) -> str:
    if VERSION_PATTERN.fullmatch(version) is None:
        raise ValueError(f"invalid release version: {version!r}")
    parsed_url = urlparse(source_url)
    if parsed_url.scheme != "https" or not parsed_url.netloc:
        raise ValueError("the release source URL must be an absolute HTTPS URL")
    if re.fullmatch(r"[0-9a-f]{64}", source_sha256) is None:
        raise ValueError("the source SHA-256 must be 64 lowercase hexadecimal digits")

    return f'''# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# typed: strict
# frozen_string_literal: true

# Builds ckmux from an immutable GitHub release archive.
class Ckmux < Formula
  desc "Terminal multiplexer with a visible interface"
  homepage "https://github.com/cklukas/ckmux"
  url "{source_url}"
  version "{version}"
  sha256 "{source_sha256}"
  license "MIT"

  depends_on "cmake" => :build

  def install
    system "cmake", "-S", ".", "-B", "build",
           "-DCMAKE_BUILD_TYPE=Release",
           "-DCKMUX_BUILD_TESTING=OFF",
           "-DCKMUX_PREFER_CKVISION_SOURCE=OFF",
           *std_cmake_args
    system "cmake", "--build", "build", "--parallel"
    system "cmake", "--install", "build"
  end

  test do
    assert_match version.to_s, shell_output("#{{bin}}/ckmux --version")
  end
end
'''


def generate(version: str, source_url: str, archive: Path, output: Path) -> None:
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(formula_text(version, source_url, digest), encoding="utf-8")


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="ckmux-formula-") as temp_dir:
        root = Path(temp_dir)
        archive = root / "source.tar.gz"
        output = root / "ckmux.rb"
        archive.write_bytes(b"ckmux release source\n")
        generate("1.2.3", "https://example.invalid/ckmux-v1.2.3.tar.gz", archive, output)
        rendered = output.read_text(encoding="utf-8")
        expected_digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        assert 'version "1.2.3"' in rendered
        assert 'url "https://example.invalid/ckmux-v1.2.3.tar.gz"' in rendered
        assert f'sha256 "{expected_digest}"' in rendered
        assert 'shell_output("#{bin}/ckmux --version")' in rendered

    invalid_cases = (
        ("1.2", "https://example.invalid/source.tar.gz", "0" * 64),
        ("1.2.3", "http://example.invalid/source.tar.gz", "0" * 64),
        ("1.2.3", "https://example.invalid/source.tar.gz", "not-a-digest"),
    )
    for version, source_url, digest in invalid_cases:
        try:
            formula_text(version, source_url, digest)
        except ValueError:
            continue
        raise AssertionError("invalid formula input was accepted")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version")
    parser.add_argument("--source-url")
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return args
    missing = [
        name
        for name in ("version", "source_url", "archive", "output")
        if getattr(args, name) is None
    ]
    if missing:
        parser.error("the following arguments are required: " + ", ".join(missing))
    return args


def main() -> int:
    args = parse_args()
    if args.self_test:
        self_test()
    else:
        generate(args.version, args.source_url, args.archive, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
