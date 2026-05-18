#!/usr/bin/env python3
"""Classify upstream OpenJPEG nonregression inputs as expected-pass vs expected-fail.

Reads two sources from a checked-out upstream OpenJPEG tree:

  - tests/nonregression/CMakeLists.txt — `set(BLACKLIST_JPEG2000 ...)` and
    `set(BLACKLIST_JPEG2000_TMP ...)`. Files in BLACKLIST_JPEG2000 (which
    includes _TMP via variable interpolation) are flagged WILL_FAIL on the
    upstream dump test. We treat all of them as expected_fail.

  - tests/nonregression/test_suite.ctest.in — lines starting with `!` mean
    "this opj_compress/opj_decompress invocation is expected to fail."
    Extracts the input filename (token after `-i`) from each such line.

Union goes to stdout as JSON. Source mtimes are recorded so a downstream
cache can decide whether to regenerate.

Usage:
  scripts/classify_nonregression.py --openjpeg-source PATH > out.json
"""
import argparse
import json
import os
import re
import sys


def parse_blacklist_set(text: str, name: str) -> set:
    """Extract bare filenames from a `set(NAME ...)` block in cmake text.

    Skips `#`-prefixed lines and inline comments; ignores `${VAR}`
    references (BLACKLIST_JPEG2000 includes ${BLACKLIST_JPEG2000_TMP},
    so we resolve that by calling this function twice and unioning).
    """
    m = re.search(rf"set\({re.escape(name)}\b(.*?)\)", text, re.S)
    if not m:
        return set()
    out = set()
    for line in m.group(1).splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        # strip inline comment
        s = re.sub(r"\s*#.*$", "", s).strip()
        if not s or s.startswith("${"):
            continue
        out.add(s)
    return out


def parse_ctest_in(path: str) -> set:
    """Extract filenames from `!`-prefixed opj_decompress lines that test
    *file-level* failures (bare -i/-o invocation).

    `!`-lines mean "this command should exit non-zero." That's a mix of
    two things upstream:

    1. File-level failures: the file itself can't be decoded. The command
       is just `opj_decompress -i FILE -o OUT.png`. Counts.

    2. Option-level failures: the file decodes fine, but the *flags*
       are bad (out-of-range tile/component/ROI). Doesn't count for our
       classifier because the bench decodes with default options.

    Heuristic: only accept !-lines whose opj_decompress invocation has
    no decoder-behavior flags (-d/-t/-c/-r/-l/...). The presence of any
    such flag means upstream is testing option handling, not the input.
    """
    out = set()
    if not os.path.exists(path):
        return out
    # File-level failure: !opj_decompress -i FILE -o OUTPUT (only).
    # Flags that change decode behavior (and thus turn a !-line into an
    # option test rather than a file test):
    behavior_flags = {"-d", "-t", "-c", "-r", "-l", "-tn", "-comps", "-allow-partial",
                       "-allow-fractional-resolutions", "-quiet", "-OutFor"}
    pat_input = re.compile(
        r"-i\s+\S*[/@]([^/\s]+\.(?:jp2|j2k|jpc|j2c|jph|jhc))",
        re.IGNORECASE,
    )
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            s = raw.lstrip()
            if not s.startswith("!"):
                continue
            # Only opj_decompress lines are about decode failures.
            if "opj_decompress" not in s:
                continue
            tokens = s.split()
            if any(t in behavior_flags for t in tokens):
                continue
            m = pat_input.search(s)
            if not m:
                continue
            name = m.group(1)
            if "@" in name:
                continue
            out.add(name)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--openjpeg-source", required=True,
                    help="Path to a checked-out openjpeg (or fork) repo.")
    ap.add_argument("--out", default="-",
                    help="Output path; '-' for stdout.")
    ap.add_argument("--format", choices=("json", "txt"), default="json",
                    help="json: full classification with source provenance. "
                         "txt: one expected-fail basename per line, suitable "
                         "for the bench's --classification flag.")
    args = ap.parse_args()

    src = args.openjpeg_source
    cmake_path = os.path.join(src, "tests", "nonregression", "CMakeLists.txt")
    ctest_path = os.path.join(src, "tests", "nonregression", "test_suite.ctest.in")

    if not os.path.exists(cmake_path):
        sys.exit(f"missing {cmake_path}")

    cmake_text = open(cmake_path).read()
    tmp = parse_blacklist_set(cmake_text, "BLACKLIST_JPEG2000_TMP")
    full = parse_blacklist_set(cmake_text, "BLACKLIST_JPEG2000") | tmp
    ctest_fails = parse_ctest_in(ctest_path)

    payload = {
        "generated_from": {
            "cmake": os.path.relpath(cmake_path, src),
            "ctest_in": os.path.relpath(ctest_path, src) if os.path.exists(ctest_path) else None,
            "source_root": os.path.abspath(src),
        },
        "source_mtime": {
            "cmake": os.path.getmtime(cmake_path),
            "ctest_in": os.path.getmtime(ctest_path) if os.path.exists(ctest_path) else None,
        },
        "expected_fail_blacklist": sorted(full),
        "expected_fail_ctest": sorted(ctest_fails),
        "expected_fail": sorted(full | ctest_fails),
    }

    if args.format == "txt":
        body = "# expected-fail nonregression inputs (basenames)\n"
        body += "# generated from %s + %s\n" % (
            payload["generated_from"]["cmake"],
            payload["generated_from"]["ctest_in"] or "(no ctest.in)",
        )
        body += "\n".join(payload["expected_fail"]) + "\n"
        if args.out == "-":
            sys.stdout.write(body)
        else:
            open(args.out, "w").write(body)
        return

    if args.out == "-":
        json.dump(payload, sys.stdout, indent=2)
        sys.stdout.write("\n")
    else:
        with open(args.out, "w") as f:
            json.dump(payload, f, indent=2)
            f.write("\n")


if __name__ == "__main__":
    main()
