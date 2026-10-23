#!/bin/sh
# Renders the icon sources in svg/ and logo.svg to the PNG files the program embeds (the static Qt
# build has no SVG module). Needs rsvg-convert (librsvg). Run from this folder after
# changing an icon, and commit the PNG files.
set -e
render() {  # name colour suffix
    for scale in 1 2; do
        out="png/$1$3.png"
        if [ $scale = 2 ]; then out="png/$1$3@2x.png"; fi
        sed "s/currentColor/$2/" "svg/$1.svg" | rsvg-convert -w $((20 * scale)) -h $((20 * scale)) -o "$out"
    done
}
mkdir -p png
for file in svg/*.svg; do
    name=$(basename "$file" .svg)
    render "$name" "#1F2933" ""
    render "$name" "#1F6FEB" "-on"
done
render check "#FFFFFF" "-white"
# The program's logo, in its own colours, at the sizes window title bars and task bars use.
for size in 16 24 32 48 64 128 256; do
    rsvg-convert -w $size -h $size -o "png/logo-$size.png" logo.svg
done
render delete "#B42318" "-red"
