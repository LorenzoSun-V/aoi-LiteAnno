# OpenCV (ported code)

`src/formats/PolygonRaster.cpp` is a port of the polygon filling in OpenCV 5.0.0
(`modules/imgproc/src/drawing.cpp`: `clipLine`, `LineIterator`, `Line`,
`CollectPolyEdges`, `FillEdgeCollection`). COCO segmentation areas are pixel counts of
`cv2.fillPoly`, as X-AnyLabeling computes them, so the same algorithm is needed to write
the same numbers.

`src/formats/MaskContours.cpp` is a port of the external contour search
(`modules/imgproc/src/contours_new.cpp`, `contours_common.cpp`), `arcLength`
(`modules/geometry/src/geometry.cpp`) and `approxPolyDP` (`modules/geometry/src/approx.cpp`)
of OpenCV 5.0.0. Mask import turns masks into polygons the way X-AnyLabeling does
(`findContours` with `RETR_EXTERNAL` and `CHAIN_APPROX_SIMPLE`, then `approxPolyDP`), so the
same algorithm is needed to give the same points. No OpenCV library is built or linked.

OpenCV is licensed under the Apache License 2.0 (`LICENSE` in this folder).
