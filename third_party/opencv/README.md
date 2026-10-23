# OpenCV (ported code)

`src/formats/PolygonRaster.cpp` is a port of the polygon filling in OpenCV 5.0.0
(`modules/imgproc/src/drawing.cpp`: `clipLine`, `LineIterator`, `Line`,
`CollectPolyEdges`, `FillEdgeCollection`). COCO segmentation areas are pixel counts of
`cv2.fillPoly`, as X-AnyLabeling computes them, so the same algorithm is needed to write
the same numbers. No OpenCV library is built or linked.

OpenCV is licensed under the Apache License 2.0 (`LICENSE` in this folder).
