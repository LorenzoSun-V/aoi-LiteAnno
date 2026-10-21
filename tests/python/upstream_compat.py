"""Format spec 11: round trips with X-AnyLabeling's own code.

Runs with a Python that has the pinned X-AnyLabeling release installed
(pip install x-anylabeling-cvhub==4.2.0). For every sample:

1. X-AnyLabeling reads the file (the sample must be valid for it to begin with).
2. liteanno_compat_roundtrip reads it like the editing window: unedited, it must not need
   writing; then it relabels the first shape, adds a rectangle and an image-level flag,
   confirms the image and writes the result.
3. X-AnyLabeling reads that result: every original shape and field must be what it was,
   apart from the edits.
4. X-AnyLabeling saves the result again the way its window does, and this program reads
   that file back without problems.

Samples: files written by X-AnyLabeling 4.2.0's LabelFile.save (made here), the upstream
repository's example files written by 2.4.0 (downloaded at a pinned tag and checked by
digest), the repository's own upstream-shaped fixtures, and optionally a folder of real
field files (--field-dir, images next to them).
"""

import argparse
import copy
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request

PINNED_UPSTREAM = "4.2.0"
EXAMPLES_TAG = "v4.2.0"
EXAMPLES = {
    # path in the upstream repository: sha256 of the file
    "examples/classification/shape-level/sources/multi-label/worker.json":
        "2655d346d5c0e1de8d359dd86ffeac2ff8ac812856763e13b24c686b8e6ecc05",
    "examples/classification/shape-level/sources/multi-task/elon-musk-tesla.json":
        "f1ca1f3bea606eef3136635bd64cb9e6c1f2ea03b875918c6e712032d45b591a",
}
FIXTURES = ["upstream_v2_legacy.json", "upstream_v4.json"]
ADDED_LABEL = "liteanno_added"
ADDED_FLAG = "liteanno_flag"
# Top-level keys this program writes on an edited save (format spec 4.1, 4.5).
WRITTEN_KEYS = {"version", "checked", "flags", "shapes", "imagePath", "imageData",
                "imageHeight", "imageWidth", "liteanno"}

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PIL import Image  # noqa: E402
from PyQt6.QtCore import QCoreApplication  # noqa: E402
from anylabeling.views.labeling.label_file import LabelFile  # noqa: E402
from anylabeling.views.labeling.shape import Shape  # noqa: E402

_app = QCoreApplication.instance() or QCoreApplication([])
failures = []


def fail(sample, message):
    failures.append(f"{sample}: {message}")
    print(f"FAIL {sample}: {message}", flush=True)


def check_upstream_version():
    from importlib.metadata import version
    found = version("x-anylabeling-cvhub")
    if found != PINNED_UPSTREAM:
        sys.exit(f"X-AnyLabeling {PINNED_UPSTREAM} is required, found {found}")


def make_image(path, width, height):
    image = Image.new("RGB", (width, height), (90, 120, 150))
    extension = os.path.splitext(path)[1].lower()
    fmt = {".jpg": "JPEG", ".jpeg": "JPEG", ".png": "PNG", ".bmp": "BMP", ".webp": "WEBP",
           ".tif": "TIFF", ".tiff": "TIFF"}.get(extension, "PNG")
    image.save(path, fmt)


def upstream_written_samples(directory):
    """Files saved by X-AnyLabeling 4.2.0 itself, covering its shape types and fields."""
    def shape(**fields):
        base = {"label": "scratch", "points": [], "group_id": None, "description": "",
                "difficult": False, "shape_type": "polygon", "flags": {}, "attributes": {},
                "kie_linking": []}
        base.update(fields)
        return Shape().load_from_dict(base).to_dict()

    samples = []
    specs = {
        "x42_all_types.png": dict(
            shapes=[
                shape(shape_type="rectangle", points=[[10, 12], [60, 12], [60, 40], [10, 40]],
                      group_id=3, description="kept", difficult=True, score=0.87,
                      flags={"occluded": True}, attributes={"severity": "high"}),
                shape(shape_type="polygon", label="dent", points=[[70, 10], [95, 15], [90, 45], [72, 40]],
                      kie_linking=[[0, 1]]),
                shape(shape_type="rotation", points=[[20, 50], [50, 50], [50, 70], [20, 70]], direction=0.25),
                shape(shape_type="point", points=[[5.5, 6.25]]),
                shape(shape_type="line", points=[[1, 1], [30, 30]]),
                shape(shape_type="linestrip", points=[[2, 80], [20, 85], [40, 82]]),
                shape(shape_type="circle", points=[[100, 60], [110, 60]], locked=True),
            ],
            flags={"good": False, "ng": True}, other_data={"description": "image note", "checked": True},
            size=(160, 100)),
        "x42_empty.jpg": dict(shapes=[], flags={}, other_data={}, size=(64, 48)),
        "中文_图片.bmp": dict(
            shapes=[shape(shape_type="rectangle", label="划痕", points=[[1, 2], [30, 2], [30, 20], [1, 20]])],
            flags={}, other_data={"checked": False}, size=(48, 32)),
    }
    for image_name, spec in specs.items():
        stem = os.path.splitext(image_name)[0]
        image_path = os.path.join(directory, image_name)
        width, height = spec["size"]
        make_image(image_path, width, height)
        json_path = os.path.join(directory, stem + ".json")
        LabelFile().save(filename=json_path, shapes=copy.deepcopy(spec["shapes"]), image_path=image_name,
                         image_height=height, image_width=width, image_data=None,
                         other_data=dict(spec["other_data"]), flags=dict(spec["flags"]))
        samples.append(("upstream 4.2.0 save: " + image_name, json_path, image_path))
    return samples


def place(directory, name, data_bytes, image_source=None):
    """Writes a sample JSON and an image of its size next to it."""
    os.makedirs(directory, exist_ok=True)
    json_path = os.path.join(directory, name)
    with open(json_path, "wb") as f:
        f.write(data_bytes)
    data = json.loads(data_bytes.decode("utf-8-sig"))
    image_name = os.path.basename(data["imagePath"])
    image_path = os.path.join(directory, image_name)
    if image_source:
        shutil.copyfile(image_source, image_path)
    else:
        make_image(image_path, int(data["imageWidth"]), int(data["imageHeight"]))
    return json_path, image_path


def downloaded_samples(directory):
    samples = []
    for path, digest in EXAMPLES.items():
        url = f"https://raw.githubusercontent.com/CVHub520/X-AnyLabeling/{EXAMPLES_TAG}/{path}"
        data = urllib.request.urlopen(url, timeout=60).read()
        if hashlib.sha256(data).hexdigest() != digest:
            sys.exit(f"{url} changed: digest does not match")
        name = os.path.basename(path)
        sub = os.path.join(directory, os.path.splitext(name)[0])
        samples.append(("upstream example (2.4.0): " + name, *place(sub, name, data)))
    return samples


def fixture_samples(directory, fixtures_dir):
    samples = []
    for name in FIXTURES:
        with open(os.path.join(fixtures_dir, name), "rb") as f:
            data = f.read()
        sub = os.path.join(directory, os.path.splitext(name)[0])
        samples.append(("fixture: " + name, *place(sub, name, data)))
    return samples


def field_samples(directory, field_dir):
    samples = []
    for name in sorted(os.listdir(field_dir)):
        if not name.lower().endswith(".json"):
            continue
        with open(os.path.join(field_dir, name), "rb") as f:
            data = f.read()
        image_name = os.path.basename(json.loads(data.decode("utf-8-sig"))["imagePath"])
        sub = os.path.join(directory, os.path.splitext(name)[0])
        samples.append(("field: " + name, *place(sub, name, data, os.path.join(field_dir, image_name))))
    return samples


def same(a, b):
    """Equal, with numbers compared as numbers (1 == 1.0) but true and false never equal to 1 and 0."""
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, bool) or isinstance(b, bool):
        return type(a) is type(b) and a == b
    return a == b


def check_same():
    """The comparison itself: a field turning from true into 1 must not pass."""
    for a, b, expected in [(1, 1.0, True), (0, 0.0, True), (True, True, True), (False, False, True),
                           (True, 1, False), (1, True, False), (False, 0, False), (0.0, False, False),
                           ({"good": False, "ng": True}, {"good": 0, "ng": 1}, False),
                           ([True, 2], [1, 2.0], False), ([True, 2], [True, 2.0], True),
                           (None, False, False), ("1", 1, False)]:
        if same(a, b) != expected:
            sys.exit(f"same({a!r}, {b!r}) is {not expected}, expected {expected}")


def booleans_to_numbers(value, skip_last_shape=False):
    """For --tamper: every true/false becomes 1/0, as a writer bug would do it. Returns how many."""
    count = 0

    def walk(v):
        nonlocal count
        if isinstance(v, dict):
            for k, x in v.items():
                if isinstance(x, bool):
                    v[k] = int(x)
                    count += 1
                else:
                    walk(x)
        elif isinstance(v, list):
            for i, x in enumerate(v):
                if isinstance(x, bool):
                    v[i] = int(x)
                    count += 1
                else:
                    walk(x)

    for key, v in value.items():
        if key == "checked":  # written by this program, checked on its own
            continue
        if key == "flags":
            for name, flag in v.items():
                if name != ADDED_FLAG and isinstance(flag, bool):
                    v[name] = int(flag)
                    count += 1
        elif key == "shapes":
            walk(v[:-1] if skip_last_shape else v)  # the last shape is the one this program added
        elif isinstance(v, bool):
            value[key] = int(v)
            count += 1
        else:
            walk(v)
    return count


def corners(points):
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return min(xs), min(ys), max(xs), max(ys)


def run_tool(tool, image_path, json_path, out_path):
    result = subprocess.run([tool, image_path, json_path, out_path], capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.decode("utf-8", "replace"))
    return json.loads(result.stdout)


def check_sample(tool, label, json_path, image_path, tamper=False):
    """Returns how many original values --tamper changed (0 without it)."""
    failures_before = len(failures)
    with open(json_path, "rb") as f:
        original_bytes = f.read()
    raw_in = json.loads(original_bytes.decode("utf-8-sig"))
    try:
        before = LabelFile(json_path)
    except Exception as e:  # the sample itself must be valid upstream
        fail(label, f"X-AnyLabeling cannot read the sample: {e}")
        return 0

    edited_dir = os.path.join(os.path.dirname(json_path), "edited")
    os.makedirs(edited_dir, exist_ok=True)
    shutil.copyfile(image_path, os.path.join(edited_dir, os.path.basename(image_path)))
    out_path = os.path.join(edited_dir, os.path.basename(json_path))
    try:
        report = run_tool(tool, image_path, json_path, out_path)
    except RuntimeError as e:
        fail(label, f"liteanno_compat_roundtrip failed: {e}")
        return 0
    with open(json_path, "rb") as f:
        if f.read() != original_bytes:
            fail(label, "the sample was changed by reading it")
    if report["readOnly"] != "None":
        fail(label, f"read-only for this program: {report['readOnly']}")
        return 0
    if report["uneditedNeedsWrite"]:
        fail(label, "unedited, it would be written")
    if report["shapes"] != len(before.shapes):
        fail(label, f"{report['shapes']} shapes read, X-AnyLabeling reads {len(before.shapes)}")

    tampered = 0
    if tamper:
        with open(out_path, encoding="utf-8") as f:
            broken = json.load(f)
        tampered = booleans_to_numbers(broken, skip_last_shape=True)
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(broken, f, ensure_ascii=False, indent=2)

    # 3. X-AnyLabeling reads the edited file.
    try:
        after = LabelFile(out_path)
    except Exception as e:
        fail(label, f"X-AnyLabeling cannot read the edited file: {e}")
        return tampered
    if len(after.shapes) != len(before.shapes) + 1:
        fail(label, f"{len(after.shapes)} shapes after editing, expected {len(before.shapes) + 1}")
        return tampered
    relabeled = report["relabeled"]
    for i, (old, new) in enumerate(zip(before.shapes, after.shapes)):
        expected = old.to_dict()
        actual = new.to_dict()
        if i == relabeled:
            expected["label"] = expected["label"] + "_relabeled"
        # A rectangle is written from its top-left corner, as X-AnyLabeling's own save does;
        # an old two-point rectangle drawn from another corner starts elsewhere when read.
        if expected["shape_type"] == "rectangle":
            expected["points"] = corners(expected["points"])
            actual["points"] = corners(actual["points"])
        if not same(expected, actual):
            fail(label, f"shape {i} changed:\n  before {expected}\n  after  {actual}")
    added = after.shapes[-1].to_dict()
    if added["label"] != ADDED_LABEL or added["shape_type"] != "rectangle" or len(added["points"]) != 4:
        fail(label, f"added rectangle reads as {added}")
    expected_flags = dict(before.flags)
    expected_flags[ADDED_FLAG] = True
    if not same(after.flags, expected_flags):
        fail(label, f"flags {after.flags}, expected {expected_flags}")
    if after.other_data.get("checked") is not True:
        fail(label, "the confirmation does not read as checked=true")
    for key, value in before.other_data.items():
        if key != "checked" and not same(after.other_data.get(key), value):
            fail(label, f"field {key!r} changed: {value!r} -> {after.other_data.get(key)!r}")

    # Raw JSON: every field this program does not write keeps its value, and the objects of
    # untouched shapes are the same apart from four-corner rectangles (format spec 4.5).
    with open(out_path, "rb") as f:
        raw_out = json.loads(f.read().decode("utf-8"))
    for key, value in raw_in.items():
        if key not in raw_out:
            fail(label, f"top-level field {key!r} was dropped")
        elif key not in WRITTEN_KEYS and not same(raw_out[key], value):
            fail(label, f"top-level field {key!r} changed")
    if raw_in.get("imageData") is not None and raw_out.get("imageData") != raw_in["imageData"]:
        fail(label, "imageData changed")
    for i, shape_in in enumerate(raw_in.get("shapes", [])):
        shape_out = raw_out["shapes"][i]
        a, b = dict(shape_in), dict(shape_out)
        if i == relabeled:
            a["label"] = a["label"] + "_relabeled"
        if a.get("shape_type") == "rectangle":
            if corners(a.pop("points")) != corners(b.pop("points")):
                fail(label, f"rectangle {i} moved")
        a.setdefault("shape_type", b.get("shape_type"))
        if not same(a, b):
            fail(label, f"shape object {i} changed:\n  before {shape_in}\n  after  {shape_out}")

    # 4. X-AnyLabeling saves the edited file again, as its window does; this program reads it.
    resaved = os.path.join(edited_dir, "resaved_" + os.path.basename(json_path))
    LabelFile().save(filename=resaved, shapes=[s.to_dict() for s in after.shapes], image_path=after.image_path,
                     image_height=raw_out["imageHeight"], image_width=raw_out["imageWidth"], image_data=None,
                     other_data=dict(after.other_data), flags=dict(after.flags))
    with open(resaved, encoding="utf-8") as f:
        if "liteanno" in raw_out and json.load(f).get("liteanno") != raw_out["liteanno"]:
            fail(label, "X-AnyLabeling dropped the liteanno section on its save")
    again = run_tool(tool, image_path, resaved, os.path.join(edited_dir, "again.json"))
    if again["readOnly"] != "None" or again["uneditedNeedsWrite"] or again["shapes"] != len(after.shapes):
        fail(label, f"reading X-AnyLabeling's save of the edited file: {again}")

    if len(failures) == failures_before:
        print(f"ok   {label} ({len(before.shapes)} shapes)", flush=True)
    return tampered


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("tool", help="liteanno_compat_roundtrip")
    parser.add_argument("fixtures", help="tests/data/annotations")
    parser.add_argument("work", help="an empty folder for the samples")
    parser.add_argument("--field-dir", help="a folder of real annotation files with their images")
    parser.add_argument("--tamper", action="store_true",
                        help="checks the checks: turns every original true/false of the edited files into 1/0 "
                             "and passes only when each sample with such a value fails")
    args = parser.parse_args()

    check_same()
    check_upstream_version()
    if os.path.exists(args.work):
        shutil.rmtree(args.work)
    os.makedirs(args.work)

    samples = []
    upstream_dir = os.path.join(args.work, "upstream42")
    os.makedirs(upstream_dir)
    for label, json_path, image_path in upstream_written_samples(upstream_dir):
        # One folder per sample, so edited/ holds one image.
        sub = os.path.join(args.work, "u42_" + str(len(samples)))
        os.makedirs(sub)
        new_json = os.path.join(sub, os.path.basename(json_path))
        new_image = os.path.join(sub, os.path.basename(image_path))
        shutil.copyfile(json_path, new_json)
        shutil.copyfile(image_path, new_image)
        samples.append((label, new_json, new_image))
    samples += downloaded_samples(os.path.join(args.work, "examples"))
    samples += fixture_samples(os.path.join(args.work, "fixtures"), args.fixtures)
    if args.field_dir:
        samples += field_samples(os.path.join(args.work, "field"), args.field_dir)

    if args.tamper:
        tampered_samples = 0
        missed = []
        for label, json_path, image_path in samples:
            before = len(failures)
            if check_sample(args.tool, label, json_path, image_path, tamper=True):
                tampered_samples += 1
                if len(failures) == before:
                    missed.append(label)
        print(f"{len(samples)} samples, {tampered_samples} with true/false values turned into 1/0, "
              f"{len(missed)} of them not caught")
        for label in missed:
            print(f"MISSED {label}")
        return 1 if missed or tampered_samples == 0 else 0

    for label, json_path, image_path in samples:
        check_sample(args.tool, label, json_path, image_path)

    print(f"{len(samples)} samples, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
