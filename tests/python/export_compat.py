"""Architecture 11.1: every export compared with X-AnyLabeling 4.2.0's own converter.

Runs with a Python that has the pinned X-AnyLabeling release installed. Builds two small
projects (rectangles, polygons, rotated boxes) with the cases that decide the output: coordinates outside
the image and exactly on 0, two-point rectangles, rounding ties, difficult objects, polygons
joined by group_id, negative samples, skipped and unconfirmed images. Exports them with
liteanno-cli and converts the same files with upstream's LabelConverter
(custom_to_yolo, custom_to_voc, custom_to_coco, custom_to_dota, custom_to_mask), then compares:

- YOLO (also OBB), DOTA and VOC files byte for byte (VOC apart from <folder>, which names each output folder);
- COCO files after setting "info" and "licenses" equal, through json.dumps so that 1 and
  1.0 differ, and our file must be exactly json.dumps(indent=4) of itself;
- masks pixel for pixel (PNG compression may differ).

LabelMe has no upstream converter: its files are checked against the source shapes.

Only confirmed images are exported; the manifest lists the others.
"""

import json
import os
import shutil
import subprocess
import sys

import cv2
import numpy as np
from anylabeling.views.labeling.label_converter import LabelConverter

CLASSES = ["scratch", "dent", "stain"]


def shape(label, shape_type, points, group_id=None, difficult=False):
    return {
        "label": label,
        "points": points,
        "group_id": group_id,
        "description": "",
        "difficult": difficult,
        "shape_type": shape_type,
        "flags": {},
        "attributes": {},
        "kie_linking": [],
    }


def rect(label, x1, y1, x2, y2, **kw):
    return shape(label, "rectangle", [[x1, y1], [x2, y1], [x2, y2], [x1, y2]], **kw)


RECTANGLES = {
    # name: (width, height, shapes, checked, skip reason)
    "a01.png": (200, 100, [rect("scratch", 10.5, 20.25, 80.0, 60.75), rect("dent", 0.0, 0.0, 199.0, 99.0)], True, None),
    "a02.png": (640, 480, [rect("stain", -3.2, 5.0, 650.0, 481.5), rect("scratch", 100.0, 100.0, 120.5, 140.25,
                                                                         difficult=True)], True, None),
    # Two-point rectangle, drawn from the bottom right.
    "a03.png": (300, 200, [shape("dent", "rectangle", [[250.0, 150.0], [30.0, 40.0]])], True, None),
    "a04.png": (300, 200, [], True, None),  # negative sample
    "a05.png": (300, 200, [rect("dent", 1.0, 1.0, 50.0, 50.0)], False, None),  # not confirmed
    "a06.png": (300, 200, [], False, "blurred"),  # skipped
    "a07.png": (300, 200, [rect("scratch", 2.5, 3.5, 4.5, 5.5), rect("scratch", 0.123456789, 1e-05, 299.999, 199.0)],
                True, None),
}

POLYGONS = {
    "p01.png": (200, 100, [shape("scratch", "polygon", [[10.5, 10.5], [50.25, 12.0], [40.0, 60.75], [12.5, 55.5]]),
                           shape("dent", "polygon", [[0.0, 0.0], [199.0, 0.0], [100.0, 99.0]], difficult=True)],
                True, None),
    # Two polygons of one instance, and one outside the image.
    "p02.png": (320, 240, [shape("stain", "polygon", [[10.0, 10.0], [60.0, 10.0], [35.0, 50.0]], group_id=1),
                           shape("stain", "polygon", [[100.0, 100.0], [150.0, 100.0], [125.0, 150.5]], group_id=1),
                           shape("scratch", "polygon", [[-5.0, 200.0], [330.0, 210.0], [300.0, 260.0], [2.5, 230.5]])],
                True, None),
    "p03.png": (320, 240, [], True, None),
    "p04.png": (320, 240, [shape("dent", "polygon", [[1.5, 2.5], [3.5, 2.5], [2.5, 4.5]])], True, None),
}


ROTATIONS = {
    "r01.png": (200, 100, [dict(shape("scratch", "rotation", [[20.5, 10.0], [80.0, 12.25], [78.0, 40.0], [18.5, 37.75]]),
                                direction=0.0349),
                           dict(shape("dent", "rotation", [[100, 20], [150, 20], [150, 60], [100, 60]], difficult=True),
                                direction=0)],
                True, None),
    "r02.png": (320, 240, [], True, None),
    "r03.png": (320, 240, [dict(shape("stain", "rotation", [[0.0, 0.0], [320.0, 0.0], [320.0, 240.0], [0.0, 240.0]]),
                                direction=0.0)], True, None),
}


def make_project(cli, root, images, shape_types):
    os.makedirs(root)
    run([cli, "init", root, "--classes", ",".join(CLASSES), "--shape-types", shape_types])
    for name, (width, height, shapes, checked, skip) in images.items():
        image = np.zeros((height, width, 3), dtype=np.uint8)
        image[:, :] = (30, 90, 160)
        assert cv2.imwrite(os.path.join(root, name), image)
        data = {
            "version": "4.2.0",
            "flags": {},
            "shapes": shapes,
            "imagePath": name,
            "imageData": None,
            "imageHeight": height,
            "imageWidth": width,
            "checked": checked,
        }
        if skip:
            data["liteanno"] = {"formatVersion": "1.0", "status": "skipped", "skipReason": skip}
        with open(os.path.join(root, os.path.splitext(name)[0] + ".json"), "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8")
    if result.returncode != 0:
        sys.exit(f"{' '.join(command)} failed:\n{result.stdout}\n{result.stderr}")
    return result.stdout


def read(path):
    with open(path, "rb") as f:
        return f.read()


def exported(manifest_path):
    with open(manifest_path, encoding="utf-8") as f:
        manifest = json.load(f)
    return [item["image"] for item in manifest["items"] if item["result"] in ("exported", "negative")], manifest


def check_manifest(manifest, images):
    results = {item["image"]: item["result"] for item in manifest["items"]}
    for name, (_, _, shapes, checked, skip) in images.items():
        if skip:
            expected = "skipped"
        elif not checked:
            expected = "not-done"
        else:
            expected = "exported" if shapes else "negative"
        if results.get(name) != expected:
            sys.exit(f"manifest: {name} is {results.get(name)}, expected {expected}")


def compare_bytes(what, ours, theirs):
    if ours != theirs:
        sys.exit(f"{what} differs\n--- ours ---\n{ours.decode('utf-8', 'replace')}\n"
                 f"--- upstream ---\n{theirs.decode('utf-8', 'replace')}")


def check_yolo(cli, root, work, images, fmt, mode):
    out = os.path.join(work, fmt)
    run([cli, "export", root, "--format", fmt, "--out", out])
    names, manifest = exported(os.path.join(out, "liteanno-export.json"))
    check_manifest(manifest, images)
    classes_file = os.path.join(out, "classes.txt")
    compare_bytes("classes.txt", read(classes_file), ("\n".join(CLASSES) + "\n").encode())
    converter = LabelConverter(classes_file=classes_file)
    upstream = os.path.join(work, fmt + "-upstream")
    os.makedirs(upstream)
    for name in names:
        stem = os.path.splitext(name)[0]
        converter.custom_to_yolo(os.path.join(root, stem + ".json"), os.path.join(upstream, stem + ".txt"), mode)
        compare_bytes(f"{fmt} {stem}.txt", read(os.path.join(out, stem + ".txt")), read(os.path.join(upstream, stem + ".txt")))
    print(f"{fmt}: {len(names)} files identical")


def without_folder(xml):
    lines = xml.decode("utf-8").split("\n")
    return "\n".join(line for line in lines if not line.strip().startswith("<folder>")).encode("utf-8"), \
        [line for line in lines if line.strip().startswith("<folder>")]


def check_voc(cli, root, work, images, fmt, mode):
    out = os.path.join(work, fmt)
    run([cli, "export", root, "--format", fmt, "--out", out])
    names, manifest = exported(os.path.join(out, "liteanno-export.json"))
    check_manifest(manifest, images)
    converter = LabelConverter()
    upstream = os.path.join(work, fmt + "-upstream")
    os.makedirs(upstream)
    for name in names:
        stem = os.path.splitext(name)[0]
        converter.custom_to_voc(os.path.join(root, name), os.path.join(root, stem + ".json"),
                                os.path.join(upstream, stem + ".xml"), mode)
        ours, our_folder = without_folder(read(os.path.join(out, stem + ".xml")))
        theirs, _ = without_folder(read(os.path.join(upstream, stem + ".xml")))
        compare_bytes(f"{fmt} {stem}.xml", ours, theirs)
        if our_folder != [f"  <folder>{os.path.realpath(out)}</folder>"]:
            sys.exit(f"{fmt} {stem}.xml: folder is {our_folder}")
    print(f"{fmt}: {len(names)} files identical")


def check_coco(cli, root, work, images, fmt, mode, file_name):
    out = os.path.join(work, fmt)
    run([cli, "export", root, "--format", fmt, "--out", out])
    names, manifest = exported(os.path.join(out, "liteanno-export.json"))
    check_manifest(manifest, images)
    classes_file = os.path.join(work, fmt + "-classes.txt")
    with open(classes_file, "w", encoding="utf-8") as f:
        f.write("\n".join(CLASSES))
    upstream = os.path.join(work, fmt + "-upstream")
    os.makedirs(upstream)
    LabelConverter(classes_file=classes_file).custom_to_coco([os.path.join(root, n) for n in names], root, upstream, mode)
    ours_bytes = read(os.path.join(out, file_name))
    ours = json.loads(ours_bytes)
    theirs = json.loads(read(os.path.join(upstream, file_name)))
    if ours_bytes != json.dumps(ours, indent=4, ensure_ascii=False).encode("utf-8"):
        sys.exit(f"{fmt}: not written as json.dump(indent=4)")
    for key in ("info", "licenses"):
        theirs[key] = ours[key]
    # The one deliberate difference: category ids are the project's classes[].id (0, 1, 2 here),
    # where upstream numbers from 1 and adds "_background_" as 0 for segmentation.
    theirs["categories"] = [dict(c, id=c["id"] - 1) for c in theirs["categories"] if c["name"] != "_background_"]
    for annotation in theirs["annotations"]:
        annotation["category_id"] -= 1
    if [c["id"] for c in ours["categories"]] != list(range(len(CLASSES))):
        sys.exit(f"{fmt}: category ids {[c['id'] for c in ours['categories']]}")
    a = json.dumps(ours, indent=4, ensure_ascii=False)
    b = json.dumps(theirs, indent=4, ensure_ascii=False)
    if a != b:
        sys.exit(f"{fmt} differs\n--- ours ---\n{a}\n--- upstream ---\n{b}")
    print(f"{fmt}: {len(ours['images'])} images, {len(ours['annotations'])} annotations identical")


def check_obb_and_dota(cli, root, work, images):
    out = os.path.join(work, "yolo-obb")
    run([cli, "export", root, "--format", "yolo-obb", "--out", out])
    names, manifest = exported(os.path.join(out, "liteanno-export.json"))
    check_manifest(manifest, images)
    converter = LabelConverter(classes_file=os.path.join(out, "classes.txt"))
    upstream = os.path.join(work, "yolo-obb-upstream")
    os.makedirs(upstream)
    for name in names:
        stem = os.path.splitext(name)[0]
        converter.custom_to_yolo(os.path.join(root, stem + ".json"), os.path.join(upstream, stem + ".txt"), "obb")
        compare_bytes(f"yolo-obb {stem}.txt", read(os.path.join(out, stem + ".txt")),
                      read(os.path.join(upstream, stem + ".txt")))
    print(f"yolo-obb: {len(names)} files identical")

    out = os.path.join(work, "dota")
    run([cli, "export", root, "--format", "dota", "--out", out])
    names, manifest = exported(os.path.join(out, "liteanno-export.json"))
    check_manifest(manifest, images)
    upstream = os.path.join(work, "dota-upstream")
    os.makedirs(upstream)
    for name in names:
        stem = os.path.splitext(name)[0]
        LabelConverter().custom_to_dota(os.path.join(root, stem + ".json"), os.path.join(upstream, stem + ".txt"))
        compare_bytes(f"dota {stem}.txt", read(os.path.join(out, stem + ".txt")), read(os.path.join(upstream, stem + ".txt")))
    print(f"dota: {len(names)} files identical")


def check_mask(cli, root, work, images):
    out = os.path.join(work, "mask")
    run([cli, "export", root, "--format", "mask", "--out", out])
    names, manifest = exported(os.path.join(out, "liteanno-export.json"))
    check_manifest(manifest, images)
    with open(os.path.join(out, "mask_colors.json"), encoding="utf-8") as f:
        mapping = json.load(f)
    if mapping != {"type": "grayscale", "colors": {name: i + 1 for i, name in enumerate(CLASSES)}}:
        sys.exit(f"mask_colors.json is {mapping}")
    upstream = os.path.join(work, "mask-upstream")
    os.makedirs(upstream)
    converter = LabelConverter()
    for name in names:
        stem = os.path.splitext(name)[0]
        theirs_path = os.path.join(upstream, stem + ".png")
        width, height = images[name][0], images[name][1]
        if images[name][2]:
            converter.custom_to_mask(os.path.join(root, stem + ".json"), theirs_path, mapping)
        else:
            converter.write_empty_mask(theirs_path, mapping, width, height)
        ours = cv2.imread(os.path.join(out, stem + ".png"), cv2.IMREAD_UNCHANGED)
        theirs = cv2.imread(theirs_path, cv2.IMREAD_UNCHANGED)
        if ours is None or ours.dtype != np.uint8 or ours.shape != theirs.shape or not np.array_equal(ours, theirs):
            sys.exit(f"mask {stem}.png differs: {None if ours is None else (ours.shape, ours.dtype)} "
                     f"vs {theirs.shape}, {0 if ours is None or ours.shape != theirs.shape else int((ours != theirs).sum())} pixels")
    print(f"mask: {len(names)} masks pixel for pixel identical")


def check_labelme(cli, root, work, images, name):
    out = os.path.join(work, name)
    run([cli, "export", root, "--format", "labelme", "--out", out])
    names, manifest = exported(os.path.join(out, "liteanno-export.json"))
    for name in names:
        stem = os.path.splitext(name)[0]
        with open(os.path.join(out, stem + ".json"), encoding="utf-8") as f:
            data = json.load(f)
        if data["imagePath"] != name or not os.path.exists(os.path.join(out, name)):
            sys.exit(f"labelme {stem}.json: image {data['imagePath']} not next to it")
        if (data["imageWidth"], data["imageHeight"]) != images[name][:2]:
            sys.exit(f"labelme {stem}.json: size")
        expected_keys = ["label", "points", "group_id", "description", "shape_type", "flags", "mask"]
        for ours, source in zip(data["shapes"], images[name][2]):
            if list(ours) != expected_keys:
                sys.exit(f"labelme {stem}.json: keys {list(ours)}")
            if source["shape_type"] == "rectangle":
                xs = [p[0] for p in source["points"]]
                ys = [p[1] for p in source["points"]]
                if ours["shape_type"] != "rectangle" or ours["points"] != [[min(xs), min(ys)], [max(xs), max(ys)]]:
                    sys.exit(f"labelme {stem}.json: rectangle {ours}")
            elif source["shape_type"] == "rotation":
                if ours["shape_type"] != "polygon" or ours["points"] != source["points"]:
                    sys.exit(f"labelme {stem}.json: rotated box {ours}")
            elif ours["points"] != source["points"] or ours["shape_type"] != source["shape_type"]:
                sys.exit(f"labelme {stem}.json: {ours}")
    print(f"{name}: {len(names)} files as expected")


def main():
    cli, work = sys.argv[1], sys.argv[2]
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    rectangles = os.path.join(work, "rectangles")
    polygons = os.path.join(work, "polygons")
    make_project(cli, rectangles, RECTANGLES, "rectangle,polygon")
    make_project(cli, polygons, POLYGONS, "rectangle,polygon")

    check_yolo(cli, rectangles, work, RECTANGLES, "yolo-det", "hbb")
    check_yolo(cli, polygons, work, POLYGONS, "yolo-seg", "seg")
    check_voc(cli, rectangles, work, RECTANGLES, "voc", "rectangle")
    check_voc(cli, polygons, work, POLYGONS, "voc-polygon", "polygon")
    check_coco(cli, rectangles, work, RECTANGLES, "coco-det", "rectangle", "coco_detection.json")
    check_coco(cli, polygons, work, POLYGONS, "coco-seg", "polygon", "coco_instance_segmentation.json")

    rotations = os.path.join(work, "rotations")
    make_project(cli, rotations, ROTATIONS, "rotation")
    check_obb_and_dota(cli, rotations, work, ROTATIONS)
    check_mask(cli, polygons, work, POLYGONS)
    check_labelme(cli, rectangles, work, RECTANGLES, "labelme-rectangles")
    check_labelme(cli, rotations, work, ROTATIONS, "labelme-rotations")

    # Polygons cannot go into a detection export: nothing is written unless asked to skip.
    out = os.path.join(work, "refused")
    result = subprocess.run([cli, "export", polygons, "--format", "yolo-det", "--out", out],
                            capture_output=True, text=True, encoding="utf-8")
    if result.returncode == 0 or os.path.exists(os.path.join(out, "liteanno-export.json")):
        sys.exit("an export with unconvertible objects was written")
    print("unconvertible objects: refused, nothing written")


if __name__ == "__main__":
    main()
