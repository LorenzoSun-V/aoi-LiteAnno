"""Architecture 11.2: every import compared with X-AnyLabeling 4.2.0's own converter.

Runs with a Python that has the pinned X-AnyLabeling release installed. Writes label files
with the cases that decide the numbers (many digits, exponents, values outside 0..1, boxes
on the border, non-square images), imports them with liteanno-cli and converts the same files
with upstream's LabelConverter (yolo_to_custom, yolo_obb_to_custom, voc_to_custom,
dota_to_custom, coco_to_custom, mask_to_custom). Every shape must have the same label, type,
points and, for rotated boxes, angle as upstream's, compared as exact floats; "difficult"
and "group_id" must match as well. Masks are generated shapes of many kinds, so the
contours follow OpenCV's own. The files we
write must be waiting for confirmation and readable by upstream's json.load.

Also checks the label files our own export writes: exported and imported again, the shapes
come back as they were.
"""

import json
import os
import subprocess
import sys

import cv2
import numpy as np
from anylabeling.views.labeling.label_converter import LabelConverter

CLASSES = ["scratch", "dent", "stain"]

# name: (width, height)
IMAGES = {
    "a01.png": (200, 100),
    "a02.png": (641, 479),
    "a03.png": (768, 768),
    "sub/b01.png": (300, 200),
}

YOLO_DET = {
    "a01": "0 0.5 0.5 0.25 0.5\n1 0.123456789 0.987654321 0.0333333333 0.0101\n",
    "a02": "2 0.1 0.2 0.3 0.4\n0 1e-3 9.99e-1 2E-2 0.5\n1 0.999 0.001 0.5 0.5\n",
    "a03": "",  # an empty file: an image without objects
    "sub/b01": "1 0.3333333333333333 0.6666666666666666 0.1 0.1\n",
}

YOLO_SEG = {
    "a01": "0 0.1 0.1 0.9 0.1 0.5 0.95\n2 0.123456 0.654321 0.2 0.7 0.15 0.8 0.11 0.75\n",
    "a02": "1 0 0 1 0 1 1 0 1\n",
    "sub/b01": "2 0.3333333333333333 0.25 0.6 0.25 0.45 0.9\n",
}

YOLO_OBB = {
    "a01": "0 0.1 0.1 0.4 0.12 0.39 0.4 0.09 0.38\n1 0.5 0.2 0.75 0.2 0.75 0.6 0.5 0.6\n",
    "a02": "2 0.6 0.5 0.4 0.3 0.2 0.5 0.4 0.7\n",
    "a03": "0 0.123 0.456 0.789 0.012 0.345 0.678 0.901 0.234\n",
}

DOTA = {
    "a01": "imagesource:GoogleEarth\ngsd:0.146\n20.5 10.0 80.0 12.25 78.0 40.0 18.5 37.75 scratch 0\n"
           "100 20 150 20 150 60 100 60 dent 1\n",
    "a02": "300.0 100.0 200.0 50.0 150.0 150.0 250.0 200.0 stain 0\n10 10 10 60 60 60 60 10 scratch 0\n",
}


def voc(name, width, height, objects):
    lines = ["<annotation>", f"  <filename>{name}</filename>",
             f"  <size><width>{width}</width><height>{height}</height><depth>3</depth></size>"]
    for obj in objects:
        lines.append("  <object>")
        lines.append(f"    <name>{obj['name']}</name>")
        lines.append(f"    <difficult>{obj.get('difficult', 0)}</difficult>")
        if "box" in obj:
            x1, y1, x2, y2 = obj["box"]
            lines.append(f"    <bndbox><xmin>{x1}</xmin><ymin>{y1}</ymin><xmax>{x2}</xmax><ymax>{y2}</ymax></bndbox>")
        if "polygon" in obj:
            coords = "".join(f"<x{i}>{x}</x{i}><y{i}>{y}</y{i}>" for i, (x, y) in enumerate(obj["polygon"], 1))
            lines.append(f"    <polygon>{coords}</polygon>")
        lines.append("  </object>")
    lines.append("</annotation>")
    return "\n".join(lines) + "\n"


VOC_RECT = {
    "a01": voc("a01.png", 200, 100, [{"name": "scratch", "box": (10, 20.5, 80, 60.25)},
                                     {"name": "dent", "box": (0, 0, 199, 99), "difficult": 1}]),
    "a02": voc("a02.png", 641, 479, [{"name": "stain", "box": ("1e1", "2.000", "640.9999", "478.5")}]),
}

VOC_POLYGON = {
    "a01": voc("a01.png", 200, 100, [{"name": "scratch", "polygon": [(10.5, 10.5), (50.25, 12), (40, 60.75)],
                                      "box": (10.5, 10.5, 50.25, 60.75)},
                                     {"name": "dent", "box": (5, 5, 20, 30), "difficult": 1}]),
    "sub/b01": voc("b01.png", 300, 200, [{"name": "stain", "polygon": [(1, 2), (3, 4), (5, 1), (2.5, 0.5)]}]),
}


def run(command, expect_ok=True):
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8")
    if expect_ok and result.returncode != 0:
        sys.exit(f"{' '.join(command)} failed:\n{result.stdout}\n{result.stderr}")
    return result


def make_project(cli, root, shape_types):
    os.makedirs(root)
    run([cli, "init", root, "--classes", ",".join(CLASSES), "--shape-types", shape_types])
    for name, (width, height) in IMAGES.items():
        image = np.zeros((height, width, 3), dtype=np.uint8)
        image[:, :] = (30, 90, 160)
        path = os.path.join(root, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        assert cv2.imwrite(path, image)


def write_labels(folder, labels, suffix, classes=True):
    os.makedirs(folder)
    if classes:
        with open(os.path.join(folder, "classes.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(CLASSES) + "\n")
    for stem, text in labels.items():
        path = os.path.join(folder, stem + suffix)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def same_number(a, b):
    return type(a) in (int, float) and type(b) in (int, float) and float(a) == float(b)


def compare(what, ours, theirs):
    if len(ours["shapes"]) != len(theirs["shapes"]):
        sys.exit(f"{what}: {len(ours['shapes'])} shapes, upstream {len(theirs['shapes'])}")
    for i, (a, b) in enumerate(zip(ours["shapes"], theirs["shapes"])):
        where = f"{what} shape {i}"
        for key in ("label", "shape_type"):
            if a[key] != b[key]:
                sys.exit(f"{where}: {key} {a[key]!r}, upstream {b[key]!r}")
        if bool(a.get("difficult", False)) != bool(b.get("difficult", False)):
            sys.exit(f"{where}: difficult {a.get('difficult')!r}, upstream {b.get('difficult')!r}")
        if a.get("group_id") != b.get("group_id"):
            sys.exit(f"{where}: group_id {a.get('group_id')!r}, upstream {b.get('group_id')!r}")
        if len(a["points"]) != len(b["points"]) or not all(
                same_number(p, q) for pa, pb in zip(a["points"], b["points"]) for p, q in zip(pa, pb)):
            sys.exit(f"{where}: points {a['points']}, upstream {b['points']}")
        if a["shape_type"] == "rotation" and not same_number(a["direction"], b["direction"]):
            sys.exit(f"{where}: direction {a['direction']!r}, upstream {b['direction']!r}")
    if ours.get("checked") is not False:
        sys.exit(f"{what}: imported file is not waiting for confirmation: checked={ours.get('checked')!r}")
    if (ours["imageWidth"], ours["imageHeight"]) != (theirs["imageWidth"], theirs["imageHeight"]):
        sys.exit(f"{what}: size {ours['imageWidth']}x{ours['imageHeight']}")


def check(cli, work, fmt, labels, suffix, shape_types, convert):
    root = os.path.join(work, fmt, "data")
    make_project(cli, root, shape_types)
    label_dir = os.path.join(work, fmt, "labels")
    write_labels(label_dir, labels, suffix)
    out = run([cli, "import", root, "--format", fmt, "--labels", label_dir]).stdout
    upstream = os.path.join(work, fmt, "upstream")
    compared = 0
    for stem, text in labels.items():
        image = next(name for name in IMAGES if os.path.splitext(name)[0] == stem)
        ours_path = os.path.join(root, stem + ".json")
        theirs_path = os.path.join(upstream, stem + ".json")
        os.makedirs(os.path.dirname(theirs_path), exist_ok=True)
        convert(os.path.join(label_dir, stem + suffix), theirs_path, os.path.join(root, image))
        theirs = load(theirs_path)
        if not theirs["shapes"]:
            if os.path.exists(ours_path):
                sys.exit(f"{fmt} {stem}: a file without objects was written")
            continue
        compare(f"{fmt} {stem}", load(ours_path), theirs)
        compared += 1
    print(f"{fmt}: {compared} files match upstream ({out.strip()})")


def check_round_trip(cli, work):
    """Our own YOLO export imported again gives back the rectangles."""
    root = os.path.join(work, "round-trip", "data")
    make_project(cli, root, "rectangle")
    shapes = [{"label": "dent", "points": [[10.0, 20.0], [110.0, 20.0], [110.0, 70.0], [10.0, 70.0]],
               "group_id": None, "description": "", "difficult": False, "shape_type": "rectangle", "flags": {}}]
    data = {"version": "4.2.0", "flags": {}, "shapes": shapes, "imagePath": "a01.png", "imageData": None,
            "imageHeight": 100, "imageWidth": 200, "checked": True}
    with open(os.path.join(root, "a01.json"), "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    out = os.path.join(work, "round-trip", "export")
    run([cli, "export", root, "--format", "yolo-det", "--out", out])
    os.remove(os.path.join(root, "a01.json"))
    run([cli, "import", root, "--format", "yolo-det", "--labels", out])
    back = load(os.path.join(root, "a01.json"))["shapes"]
    # YOLO stores relative numbers, so the pixels come back to within rounding, as upstream's do.
    close = len(back) == 1 and back[0]["label"] == "dent" and all(
        abs(p - q) < 1e-9 for pa, pb in zip(back[0]["points"], shapes[0]["points"]) for p, q in zip(pa, pb))
    if not close:
        sys.exit(f"round trip: {back}")
    print("yolo-det: export and import give back the same rectangle")


COCO_IMAGES = [
    # file_name as written, the image it is
    ("a01.png", "a01.png"),
    ("images/a02.png", "a02.png"),  # found by its file name
    ("sub/b01.png", "sub/b01.png"),
]


def coco_file(path):
    sizes = {name: IMAGES[name] for _, name in COCO_IMAGES}
    images = [{"id": i + 1, "file_name": written, "width": sizes[name][0], "height": sizes[name][1]}
              for i, (written, name) in enumerate(COCO_IMAGES)]
    images.append({"id": 9, "file_name": "elsewhere/z99.png", "width": 10, "height": 10})
    categories = [{"id": i + 1, "name": name, "supercategory": ""} for i, name in enumerate(CLASSES)]
    annotations = [
        {"id": 1, "image_id": 1, "category_id": 1, "bbox": [10.5, 20.25, 30, 40.125], "area": 1, "iscrowd": 0,
         "segmentation": [[10.5, 20.25, 40.5, 20.25, 40.5, 60.375, 10.5, 60.375]]},
        {"id": 2, "image_id": 1, "category_id": 3, "bbox": [0, 0, 200, 100], "area": 1, "iscrowd": 0, "ignore": 1,
         # two parts, a repeated point
         "segmentation": [[1, 1, 50, 1, 50, 1, 25, 40], [100.25, 10, 190, 10, 150, 90.5]]},
        {"id": 3, "image_id": 2, "category_id": 2, "bbox": [1e-3, 2.5e2, 1.23456789, 3], "area": 1, "iscrowd": 0,
         "ignore": "0", "segmentation": [[0.001, 250, 1.23456789, 250, 1.2, 253]]},
        {"id": 4, "image_id": 1, "category_id": 2, "bbox": [5, 5, 5, 5], "area": 1, "iscrowd": 0,
         "segmentation": [[5, 5, 10, 5, 10, 10], [20, 20, 30, 20, 30, 30, 20, 30]]},
        {"id": 5, "image_id": 3, "category_id": 1, "bbox": [3, 4, 5, 6], "area": 1, "iscrowd": 0,
         "segmentation": [[3, 4, 8, 4, 8, 10, 3, 10]]},
    ]
    data = {"info": {}, "licenses": [], "images": images, "annotations": annotations, "categories": categories}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f)


def check_coco(cli, work, fmt, shape_types, mode):
    root = os.path.join(work, fmt, "data")
    make_project(cli, root, shape_types)
    label_file = os.path.join(work, fmt, "instances.json")
    coco_file(label_file)
    result = run([cli, "import", root, "--format", fmt, "--labels", label_file])
    if "z99.png" not in result.stderr:
        sys.exit(f"{fmt}: the image record without an image was not reported:\n{result.stderr}")
    upstream = os.path.join(work, fmt, "upstream")
    os.makedirs(upstream)
    LabelConverter().coco_to_custom(label_file, upstream, mode)
    for _, name in COCO_IMAGES:
        stem = os.path.splitext(name)[0]
        compare(f"{fmt} {stem}", load(os.path.join(root, stem + ".json")),
                load(os.path.join(upstream, os.path.basename(stem) + ".json")))
    print(f"{fmt}: {len(COCO_IMAGES)} images match upstream")


def mask_images(kind, seed):
    """Masks with many kinds of regions: filled polygons, circles, holes, touching regions,
    single pixels, regions on the border."""
    rng = np.random.default_rng(seed)
    masks = {}
    for name, (width, height) in IMAGES.items():
        mask = np.zeros((height, width, 3), dtype=np.uint8)
        for value in range(1, len(CLASSES) + 1):
            color = (value, value, value) if kind == "grayscale" else RGB_COLORS[value - 1][::-1]
            for _ in range(4):
                points = rng.integers(-10, max(width, height) + 10, size=(int(rng.integers(3, 8)), 2)).astype(np.int32)
                cv2.fillPoly(mask, [points], color)
            cv2.circle(mask, (int(rng.integers(0, width)), int(rng.integers(0, height))), int(rng.integers(3, 40)),
                       color, -1)
            cv2.circle(mask, (int(rng.integers(0, width)), int(rng.integers(0, height))), int(rng.integers(5, 30)),
                       (0, 0, 0), -1)
            mask[int(rng.integers(0, height)), int(rng.integers(0, width))] = color
        if kind == "grayscale":
            mask = mask[:, :, 0]
            mask[0, 0] = 200  # a value the mapping does not name: passed over
        masks[os.path.splitext(name)[0]] = mask
    return masks


RGB_COLORS = [(255, 0, 0), (0, 128, 255), (10, 200, 30)]


def check_mask(cli, work, kind):
    fmt = "mask"
    root = os.path.join(work, f"mask-{kind}", "data")
    make_project(cli, root, "polygon")
    out = os.path.join(work, f"mask-{kind}", "export")
    label_dir = os.path.join(out, "masks")
    os.makedirs(label_dir)
    if kind == "grayscale":
        colors = {name: i + 1 for i, name in enumerate(CLASSES)}
    else:
        colors = {name: list(RGB_COLORS[i]) for i, name in enumerate(CLASSES)}
    mapping = {"type": kind, "colors": colors}
    with open(os.path.join(out, "mask_colors.json"), "w", encoding="utf-8") as f:
        json.dump(mapping, f)
    masks = mask_images(kind, 5 if kind == "grayscale" else 6)
    for stem, mask in masks.items():
        path = os.path.join(label_dir, stem + ".png")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        assert cv2.imwrite(path, mask)
    run([cli, "import", root, "--format", fmt, "--labels", label_dir])
    upstream = os.path.join(work, f"mask-{kind}", "upstream")
    compared = shapes = 0
    for stem in masks:
        image = next(name for name in IMAGES if os.path.splitext(name)[0] == stem)
        theirs_path = os.path.join(upstream, stem + ".json")
        os.makedirs(os.path.dirname(theirs_path), exist_ok=True)
        LabelConverter().mask_to_custom(os.path.join(label_dir, stem + ".png"), theirs_path,
                                        os.path.join(root, image), mapping)
        theirs = load(theirs_path)
        compare(f"mask {kind} {stem}", load(os.path.join(root, stem + ".json")), theirs)
        compared += 1
        shapes += len(theirs["shapes"])
    print(f"mask ({kind}): {compared} files, {shapes} polygons match upstream")


def check_plain_folder(cli, work):
    """A folder without a project file: the class names come from the classes.txt chosen."""
    root = os.path.join(work, "plain", "data")
    os.makedirs(root)
    image = np.zeros((100, 200, 3), dtype=np.uint8)
    assert cv2.imwrite(os.path.join(root, "a01.png"), image)
    label_dir = os.path.join(work, "plain", "labels")
    write_labels(label_dir, {"a01": "2 0.5 0.5 0.25 0.5\n"}, ".txt", classes=False)
    classes = os.path.join(work, "plain", "names.txt")
    with open(classes, "w", encoding="utf-8") as f:
        f.write("\n".join(CLASSES) + "\n")
    refused = run([cli, "import", root, "--format", "yolo-det", "--labels", label_dir], expect_ok=False)
    if refused.returncode == 0 or os.path.exists(os.path.join(root, "a01.json")):
        sys.exit("plain folder: imported YOLO labels without class names")
    run([cli, "import", root, "--format", "yolo-det", "--labels", label_dir, "--classes-file", classes])
    shapes = load(os.path.join(root, "a01.json"))["shapes"]
    if [s["label"] for s in shapes] != ["stain"] or os.path.exists(os.path.join(root, "liteanno")):
        sys.exit(f"plain folder: {shapes}")
    print("plain folder: YOLO import takes the names of the classes file chosen")


def main():
    cli, work = sys.argv[1], sys.argv[2]
    if os.path.exists(work):
        import shutil
        shutil.rmtree(work)
    os.makedirs(work)
    classes = os.path.join(work, "classes.txt")
    with open(classes, "w", encoding="utf-8") as f:
        f.write("\n".join(CLASSES) + "\n")
    converter = LabelConverter(classes_file=classes)

    check(cli, work, "yolo-det", YOLO_DET, ".txt", "rectangle",
          lambda label, out, image: converter.yolo_to_custom(label, out, image, "hbb"))
    check(cli, work, "yolo-seg", YOLO_SEG, ".txt", "polygon",
          lambda label, out, image: converter.yolo_to_custom(label, out, image, "seg"))
    check(cli, work, "yolo-obb", YOLO_OBB, ".txt", "rotation",
          lambda label, out, image: converter.yolo_obb_to_custom(label, out, image))
    check(cli, work, "dota", DOTA, ".txt", "rotation",
          lambda label, out, image: converter.dota_to_custom(
              label + ".noheader", out, image) if strip_header(label) else None)
    check(cli, work, "voc", VOC_RECT, ".xml", "rectangle",
          lambda label, out, image: converter.voc_to_custom(label, out, os.path.basename(image), "rectangle"))
    check(cli, work, "voc-polygon", VOC_POLYGON, ".xml", "rectangle,polygon",
          lambda label, out, image: converter.voc_to_custom(label, out, os.path.basename(image), "polygon"))
    check_coco(cli, work, "coco-det", "rectangle", "rectangle")
    check_coco(cli, work, "coco-seg", "polygon", "polygon")
    check_mask(cli, work, "grayscale")
    check_mask(cli, work, "rgb")
    check_round_trip(cli, work)
    check_plain_folder(cli, work)


def strip_header(label):
    """Upstream's dota_to_custom reads every line as an object: give it the file without the
    "imagesource:" and "gsd:" lines we pass over."""
    with open(label, encoding="utf-8") as f:
        lines = [line for line in f.read().splitlines()
                 if not line.startswith(("imagesource:", "gsd:")) and line.strip()]
    with open(label + ".noheader", "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return True


if __name__ == "__main__":
    main()
