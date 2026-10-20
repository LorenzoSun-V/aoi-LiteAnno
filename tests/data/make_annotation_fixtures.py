#!/usr/bin/env python3
"""Writes the annotation fixtures in tests/data/annotations.

The files are committed; this script documents how they were made. Usage:
    python3 make_annotation_fixtures.py tests/data/annotations
"""
import json, pathlib, sys

out = pathlib.Path(sys.argv[1])
broken = out / "broken"
broken.mkdir(parents=True, exist_ok=True)

def canonical(value, ensure_ascii=False):
    return json.dumps(value, ensure_ascii=ensure_ascii, indent=2).encode("utf-8")

def write(name, data):
    (out / name).write_bytes(data)

def shape(label, points, shape_type, **extra):
    s = {"label": label, "score": None, "points": points, "group_id": None, "description": "",
         "difficult": False, "shape_type": shape_type, "flags": {}, "attributes": {}, "kie_linking": []}
    s.update(extra)
    return s

# X-AnyLabeling 3.x / 4.x as written by its LabelFile.save (json.dump, indent=2).
upstream_v4 = {
    "version": "3.2.1",
    "flags": {},
    "checked": False,
    "shapes": [
        shape("scratch", [[120.5, 80.0], [260.0, 80.0], [260.0, 150.25], [120.5, 150.25]], "rectangle"),
        dict(shape("dent", [[10.0, 10.0], [50.0, 12.5], [40.0, 60.0]], "polygon"),
             score=0.87, group_id=1, description="边缘", difficult=True, attributes={"severity": "重"}),
        shape("crack", [[300.0, 300.0], [400.0, 320.0], [390.0, 370.0], [290.0, 350.0]], "rotation",
              direction=0.19739555984988078),
        shape("pin", [[5.0, 6.0]], "point"),
        shape("edge", [[0.0, 0.0], [100.0, 100.0]], "line"),
        shape("wire", [[1.0, 1.0], [2.0, 3.0], [4.0, 1.0]], "linestrip"),
        shape("hole", [[200.0, 200.0], [210.0, 200.0]], "circle", locked=True),
    ],
    "imagePath": "0001.jpg",
    "imageData": None,
    "imageHeight": 768,
    "imageWidth": 768,
    "description": "",
}
write("upstream_v4.json", canonical(upstream_v4))

# X-AnyLabeling 2.x: no "checked", diagonal two-point rectangle, fewer shape fields.
upstream_v2 = {
    "version": "2.3.6",
    "flags": {},
    "shapes": [
        {"label": "scratch", "points": [[260.0, 150.25], [120.5, 80.0]], "group_id": None,
         "description": "", "shape_type": "rectangle", "flags": {}},
        {"label": "dent", "text": "", "points": [[10.0, 10.0], [50.0, 12.5], [40.0, 60.0]],
         "group_id": None, "shape_type": "polygon", "flags": {}},
    ],
    "imagePath": "0002.jpg",
    "imageData": None,
    "imageHeight": 480,
    "imageWidth": 640,
}
write("upstream_v2_legacy.json", canonical(upstream_v2))

# LabelMe style with the image embedded as base64 (a 1x1 PNG).
embedded = {
    "version": "5.0.1",
    "flags": {},
    "shapes": [
        {"label": "scratch", "points": [[0.0, 0.0], [1.0, 1.0]], "group_id": None,
         "shape_type": "rectangle", "flags": {}},
    ],
    "imagePath": "0003.png",
    "imageData": "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8/x8AAwMCAO+ip1sAAAAASUVORK5CYII=",
    "imageHeight": 1,
    "imageWidth": 1,
    "lineColor": [0, 255, 0, 128],
    "fillColor": [255, 0, 0, 128],
}
write("embedded_image_data.json", canonical(embedded))

# Only the three keys upstream cannot open a file without; integer coordinates.
missing_fields = {
    "shapes": [
        {"label": "scratch", "points": [[1, 2], [3, 2], [3, 4], [1, 4]], "shape_type": "rectangle"},
    ],
    "imagePath": "0004.png",
    "imageData": None,
}
write("missing_fields.json", canonical(missing_fields))
write("empty_object.json", b"{}")

unknown_fields = {
    "version": "0.1.0",
    "flags": {"OK": False, "NG": True, "legacy_tag": True},
    "checked": False,
    "shapes": [
        dict(shape("scratch", [[10.0, 20.0], [30.0, 20.0], [30.0, 40.0], [10.0, 40.0]], "rectangle"),
             flags={"occluded": True}, attributes={"severity": "轻", "length_mm": 12.5},
             track_id=17, model_meta={"name": "det-v3", "thresholds": [0.25, 0.5]}),
    ],
    "imagePath": "0005.bmp",
    "imageData": None,
    "imageHeight": 768,
    "imageWidth": 768,
    "liteanno": {"formatVersion": "1.0", "reviewer_note": "keep me"},
    "camera": {"id": "line1-cam2", "exposure_us": 1200, "gain": 1.5e-07,
               "serial": 18446744073709551615, "offset": -9223372036854775808},
    "tags": ["day", "ng"],
    "description": "备注\t含制表符",
}
write("unknown_fields.json", canonical(unknown_fields))

unrecognized = {
    "version": "4.2.0",
    "flags": {},
    "checked": False,
    "shapes": [
        shape("ok", [[0.0, 0.0], [10.0, 0.0], [10.0, 10.0]], "polygon"),
        {"label": "x", "points": [[1, 2], [3, 4]], "shape_type": "ellipse"},
        {"points": [[1, 2]], "shape_type": "point"},
        {"label": "y", "points": [[1, "2"]], "shape_type": "point"},
        {"label": "z", "points": [[0, 0], [1, 1]], "shape_type": "line", "kie_linking": [[1, 2.0]]},
        {"label": "r", "points": [[0, 0], [1, 0], [1, 1]], "shape_type": "rectangle"},
        "not an object",
        {"label": "q", "points": [[0, 0], [10, 0], [10, 10], [0, 10]], "shape_type": "quadrilateral"},
        {"label": "c", "points": [[0, 0], [4, 0], [4, 4], [0, 4], [1, 1], [5, 1], [5, 5], [1, 5]],
         "shape_type": "cuboid", "cuboid3d": {"depth": [1.0, 1.0]}},
        {"label": 5, "points": [[1, 1]], "shape_type": "point"},
        {"label": "w", "points": [], "shape_type": "polygon"},
        {"label": "t", "points": [[1, 1], [2, 2]], "shape_type": "polygon"},
        {"label": "u", "points": [[1, 1], [2, 2], [3, 3]], "shape_type": None},
    ],
    "imagePath": "0006.jpg",
    "imageData": None,
    "imageHeight": 100,
    "imageWidth": 100,
}
write("unrecognized_objects.json", canonical(unrecognized))

def ours(name, **liteanno):
    return {
        "version": "0.1.0", "flags": {}, "checked": False,
        "shapes": [shape("scratch", [[1.0, 1.0], [5.0, 1.0], [5.0, 5.0], [1.0, 5.0]], "rectangle")],
        "imagePath": name, "imageData": None, "imageHeight": 100, "imageWidth": 100,
        "liteanno": liteanno,
    }

write("future_version.json", canonical(dict(ours("0007.png", formatVersion="2.0"), newThing={"a": 1})))
write("minor_version.json", canonical(ours("0008.png", formatVersion="1.5", reviewRound=2)))
write("bad_format_version.json", canonical(ours("0009.png", formatVersion="one")))

checked_invalid = ours("0010.png", formatVersion="1.0")
checked_invalid["checked"] = 1
write("checked_invalid.json", canonical(checked_invalid))

write("skipped.json", canonical(ours("0011.png", formatVersion="1.0", status="skipped", skipReason="图片模糊")))
conflict = ours("0012.png", formatVersion="1.0", status="skipped", skipReason="重复")
conflict["checked"] = True
write("state_conflict.json", canonical(conflict))
write("unknown_status.json", canonical(ours("0013.png", formatVersion="1.0", status="review_later")))

# Same content as upstream_v4.json with a BOM, CRLF line endings and 4-space indentation.
write("bom_crlf.json", b"\xef\xbb\xbf" + json.dumps(upstream_v4, ensure_ascii=False, indent=4).replace("\n", "\r\n").encode("utf-8"))
write("compact.json", json.dumps(upstream_v4, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))

chinese = {
    "version": "3.0.0",
    "flags": {"合格": True},
    "checked": True,
    "shapes": [dict(shape("划伤", [[1.5, 2.5], [3.5, 2.5], [3.5, 4.5], [1.5, 4.5]], "rectangle"),
                    description="第一行\n第二行\u0001控制字符 \"引号\" \\反斜杠 😀")],
    "imagePath": "样本 01.png",
    "imageData": None,
    "imageHeight": 64,
    "imageWidth": 64,
}
write("chinese_text.json", canonical(chinese, ensure_ascii=True))

write("top_level_array.json", canonical([{"label": "a"}]))
write("shapes_not_array.json", canonical({"version": "4.0.0", "flags": {}, "shapes": {"0": {}},
                                          "imagePath": "0014.png", "imageData": None}))

# Files the strict parser rejects (format spec 13.5); Python's json module accepts some of them.
(broken / "nan_coordinate.json").write_bytes(b'{"shapes": [{"label": "a", "points": [[NaN, 1.0]], "shape_type": "point"}], "imagePath": "a.jpg", "imageData": null}')
(broken / "infinity.json").write_bytes(b'{"shapes": [], "imagePath": "a.jpg", "imageData": null, "x": Infinity}')
(broken / "overflow_float.json").write_bytes(b'{"shapes": [], "imagePath": "a.jpg", "imageData": null, "x": 1e400}')
(broken / "huge_integer.json").write_bytes(b'{"shapes": [], "imagePath": "a.jpg", "imageData": null, "x": 123456789012345678901234567890}')
(broken / "invalid_utf8.json").write_bytes(b'{"shapes": [], "imagePath": "a\xff.jpg", "imageData": null}')
(broken / "lone_surrogate.json").write_bytes(b'{"shapes": [], "imagePath": "\\ud800.jpg", "imageData": null}')
(broken / "trailing_comma.json").write_bytes(b'{"shapes": [], "imagePath": "a.jpg", "imageData": null,}')
(broken / "truncated.json").write_bytes(b'{"shapes": [{"label": "a", "points": [[1.0, ')
(broken / "empty.json").write_bytes(b"")


# The exact bytes tst_annotationwrite expects for a new file: one rectangle, image labels
# OK/NG with NG selected.
golden = {
    "version": "0.1.0",
    "flags": {"OK": False, "NG": True},
    "checked": False,
    "shapes": [shape("scratch", [[120.5, 80.0], [260.0, 80.0], [260.0, 150.25], [120.5, 150.25]], "rectangle")],
    "imagePath": "0001.bmp",
    "imageData": None,
    "imageHeight": 768,
    "imageWidth": 768,
    "liteanno": {"formatVersion": "1.0"},
}
write("golden_new_file.json", canonical(golden))
