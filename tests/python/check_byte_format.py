#!/usr/bin/env python3
"""Checks that liteanno writes JSON exactly like Python's json module.

Format spec 13.5: every JSON file this program writes is byte-identical to
json.dumps(value, ensure_ascii=False, indent=2) encoded as UTF-8. X-AnyLabeling writes its
files the same way, so for the same value both tools produce the same text (apart from the
CRLF line endings upstream gets on Windows).

Usage: check_byte_format.py <json_reformat executable> <fixture directory> <work directory>

Every *.json file directly in the fixture directory is checked, plus generated inputs that
cover number formatting, string escaping and nesting.
"""

import json
import math
import pathlib
import random
import struct
import subprocess
import sys


def python_bytes(data: bytes) -> bytes:
    value = json.loads(data.decode("utf-8-sig"))
    return json.dumps(value, ensure_ascii=False, indent=2).encode("utf-8")


def random_double(rng: random.Random) -> float:
    while True:
        value = struct.unpack("<d", struct.pack("<Q", rng.getrandbits(64)))[0]
        if math.isfinite(value):
            return value


def generated_inputs(rng: random.Random):
    # Every exponent range, subnormals included.
    yield "random_doubles", [random_double(rng) for _ in range(20000)]
    # Pixel coordinates as annotation tools write them.
    yield "coordinates", [
        [round(rng.uniform(-50.0, 9000.0), rng.randint(0, 6)), rng.uniform(0.0, 4096.0)] for _ in range(5000)
    ]
    # Around the switch between fixed and exponent notation, and other edge values.
    edges = [m * 10.0 ** e for e in range(-325, 309) for m in (1.0, 1.5, 2.5, 7.0, 9.999999999999998)]
    edges += [0.0, -0.0, 5e-324, 2.2250738585072014e-308, 1.7976931348623157e308, 2.0 ** 53, 2.0 ** 53 + 2,
              1e15, 1e16, 9999999999999998.0, 0.0001, 0.00001, 0.1 + 0.2, 123456789.0]
    yield "float_edges", [value for value in edges if math.isfinite(value)]
    yield "integers", [0, -1, 1, 2 ** 63 - 1, -(2 ** 63), 2 ** 64 - 1, 10 ** 15, -(10 ** 18)]
    # Control characters, quotes, backslashes and non-ASCII text, in values and in keys.
    alphabet = [chr(c) for c in range(0x80)] + list("划伤样本\u00e9e\u0301€😀\u2028\u2029\ufeff")
    strings = ["".join(rng.choice(alphabet) for _ in range(rng.randint(0, 24))) for _ in range(2000)]
    yield "strings", strings
    yield "keys", {text: index for index, text in enumerate(strings[:500])}
    yield "nesting", {"a": [], "b": {}, "c": [[], [{}], {"d": [1, [2, [3, {}]]]}], "": "empty key", "e": None,
                      "f": [True, False, None, 0.5, "x"]}


def compare(tool: str, source: pathlib.Path, work: pathlib.Path) -> bool:
    output = work / (source.stem + ".out.json")
    run = subprocess.run([tool, str(source), str(output)], capture_output=True, text=True)
    if run.returncode != 0:
        print(f"FAIL {source.name}: json_reformat failed: {run.stderr.strip()}")
        return False
    actual = output.read_bytes()
    expected = python_bytes(source.read_bytes())
    if actual == expected:
        return True
    offset = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b), min(len(actual), len(expected)))
    print(f"FAIL {source.name}: first difference at byte {offset}")
    print(f"  liteanno: {actual[max(0, offset - 40):offset + 40]!r}")
    print(f"  python:   {expected[max(0, offset - 40):offset + 40]!r}")
    return False


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    tool = sys.argv[1]
    fixtures = pathlib.Path(sys.argv[2])
    work = pathlib.Path(sys.argv[3])
    work.mkdir(parents=True, exist_ok=True)

    sources = sorted(fixtures.glob("*.json"))
    if not sources:
        print(f"no fixtures in {fixtures}")
        return 1

    rng = random.Random(20261020)
    for name, value in generated_inputs(rng):
        # Compact input, so the output layout comes from the writer and not from the input.
        path = work / f"generated_{name}.json"
        path.write_bytes(json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
        sources.append(path)

    failures = sum(not compare(tool, source, work) for source in sources)
    print(f"{len(sources) - failures} of {len(sources)} files match Python's json.dumps byte for byte")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
