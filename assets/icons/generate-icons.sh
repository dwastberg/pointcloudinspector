#!/bin/sh

set -eu

icon_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir="$icon_dir/source"
macos_dir="$icon_dir/platform/macos"
linux_dir="$icon_dir/platform/linux/hicolor"
windows_dir="$icon_dir/platform/windows"

for tool in rsvg-convert magick perl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing required icon tool: $tool" >&2
        exit 1
    fi
done

render_svg() {
    source_size=$1
    output_size=$2
    output_path=$3
    rsvg-convert \
        -w "$output_size" \
        -h "$output_size" \
        "$source_dir/survey-lens-$source_size.svg" \
        -o "$output_path"
}

render_master() {
    output_size=$1
    output_path=$2
    magick "$source_dir/pcinspector-master.png" \
        -filter Lanczos \
        -resize "${output_size}x${output_size}" \
        "PNG32:$output_path"
}

render_optical() {
    output_size=$1
    output_path=$2
    case "$output_size" in
        16) source_size=16 ;;
        20|24) source_size=24 ;;
        30|32) source_size=32 ;;
        36|40|48) source_size=48 ;;
        60|64|72|80|96) source_size=96 ;;
        *)
            echo "No optical master assigned for ${output_size}px" >&2
            exit 1
            ;;
    esac
    render_svg "$source_size" "$output_size" "$output_path"
}

# Embedded Qt window icon.
render_master 256 "$icon_dir/pcinspector-256.png"

# Linux hicolor theme.
for size in 16 24 32 48 64 96; do
    render_optical "$size" "$linux_dir/${size}x${size}/apps/pcinspector.png"
done
for size in 128 256 512; do
    render_master "$size" "$linux_dir/${size}x${size}/apps/pcinspector.png"
done
cp "$source_dir/survey-lens-96.svg" \
    "$linux_dir/scalable/apps/pcinspector.svg"

# Windows exact-size source images and multi-representation ICO.
for size in 16 20 24 30 32 36 40 48 60 64 72 80 96; do
    render_optical "$size" "$windows_dir/png/pcinspector-$size.png"
done
for size in 128 256; do
    render_master "$size" "$windows_dir/png/pcinspector-$size.png"
done
magick \
    "$windows_dir/png/pcinspector-16.png" \
    "$windows_dir/png/pcinspector-20.png" \
    "$windows_dir/png/pcinspector-24.png" \
    "$windows_dir/png/pcinspector-30.png" \
    "$windows_dir/png/pcinspector-32.png" \
    "$windows_dir/png/pcinspector-36.png" \
    "$windows_dir/png/pcinspector-40.png" \
    "$windows_dir/png/pcinspector-48.png" \
    "$windows_dir/png/pcinspector-60.png" \
    "$windows_dir/png/pcinspector-64.png" \
    "$windows_dir/png/pcinspector-72.png" \
    "$windows_dir/png/pcinspector-80.png" \
    "$windows_dir/png/pcinspector-96.png" \
    "$windows_dir/png/pcinspector-128.png" \
    "$windows_dir/png/pcinspector-256.png" \
    "$windows_dir/PointCloudInspector.ico"

# macOS standard and Retina iconset representations.
render_optical 16 "$macos_dir/PointCloudInspector.iconset/icon_16x16.png"
render_optical 32 "$macos_dir/PointCloudInspector.iconset/icon_16x16@2x.png"
render_optical 32 "$macos_dir/PointCloudInspector.iconset/icon_32x32.png"
render_optical 64 "$macos_dir/PointCloudInspector.iconset/icon_32x32@2x.png"
render_master 128 "$macos_dir/PointCloudInspector.iconset/icon_128x128.png"
render_master 256 "$macos_dir/PointCloudInspector.iconset/icon_128x128@2x.png"
render_master 256 "$macos_dir/PointCloudInspector.iconset/icon_256x256.png"
render_master 512 "$macos_dir/PointCloudInspector.iconset/icon_256x256@2x.png"
render_master 512 "$macos_dir/PointCloudInspector.iconset/icon_512x512.png"
render_master 1024 "$macos_dir/PointCloudInspector.iconset/icon_512x512@2x.png"
perl "$icon_dir/build-icns.pl" \
    "$macos_dir/PointCloudInspector.iconset" \
    "$macos_dir/PointCloudInspector.icns"
