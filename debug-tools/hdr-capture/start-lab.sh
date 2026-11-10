#!/usr/bin/env bash
# Starts only a private, headless capture lab. Never manages system services.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
width=${1:-1024}
height=${2:-512}
refresh=${3:-60}
permission=${4:-off}
case "$permission" in
    off) enforce_permissions=false ;;
    allow|deny|ask) enforce_permissions=true ;;
    *) echo "Permission mode must be off, allow, deny or ask" >&2; exit 1 ;;
esac
for value in "$width" "$height" "$refresh"; do
    [[ $value =~ ^[1-9][0-9]*$ ]] || { echo "Invalid lab geometry/rate" >&2; exit 1; }
done
(( width <= 8192 && height <= 8192 && refresh <= 240 )) || exit 1
hypr=${HYPRLAND_BINARY:-$repo/build-hdr/Hyprland}
[[ -x $hypr ]] || { echo "Build Hyprland first: $hypr" >&2; exit 1; }
sway=""
while IFS= read -r path; do
    [[ ! -x $path/bin/sway ]] || sway=$path/bin/sway
done < <(nix build --inputs-from "$repo" --no-link --print-out-paths nixpkgs#sway-unwrapped)
aq=""
while IFS= read -r path; do
    [[ ! -f $path/lib/libaquamarine.so ]] || aq=$path/lib
done < <(nix build --impure --no-link --print-out-paths --file "$repo/debug-tools/hdr-capture/aquamarine-nested.nix")
[[ -n $sway && -n $aq ]] || { echo "Lab dependency build failed" >&2; exit 1; }
child_path=$PATH
if [[ -n ${HDR_LAB_DIALOG_BIN:-} ]]; then
    [[ -x $HDR_LAB_DIALOG_BIN/hyprland-dialog ]] || { echo "Invalid HDR_LAB_DIALOG_BIN" >&2; exit 1; }
    child_path=$HDR_LAB_DIALOG_BIN:$PATH
fi
lab=$(mktemp -d /tmp/hh.XXXXXX)
chmod 700 "$lab"
failed=1
cleanup() {
    if (( failed )); then
        "$repo/debug-tools/hdr-capture/stop-lab.sh" "$lab" || true
        echo "Lab startup failed; logs retained in $lab" >&2
    fi
}
trap cleanup EXIT
printf 'output * mode %sx%s\ndefault_border none\n' "$width" "$height" > "$lab/sway.conf"
cat > "$lab/hyprland.lua" <<EOF
hl.monitor({ output = "", mode = "${width}x${height}@${refresh}", position = "0x0", scale = 1,
    bitdepth = 10, cm = "hdr", supports_wide_color = 1, supports_hdr = 1,
    sdr_min_luminance = 0, sdr_max_luminance = 160 })
hl.config({
    experimental = { xx_image_capture_color_management = true },
    xwayland = { enabled = false }, animations = { enabled = false },
    general = { gaps_in = 0, gaps_out = 0, border_size = 0 },
    decoration = { rounding = 0, blur = { enabled = false }, shadow = { enabled = false } },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true,
        disable_watchdog_warning = true, background_color = "rgb(000000)" },
    ecosystem = { enforce_permissions = $enforce_permissions },
    render = { direct_scanout = 0, cm_auto_hdr = 0 },
})
EOF
if [[ $permission != off ]]; then
    printf 'hl.permission(".*color-managed-capture", "screencopy", "%s")\n' "$permission" >> "$lab/hyprland.lua"
fi
nohup setsid env -u LD_LIBRARY_PATH -u LD_PRELOAD -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$lab" XDG_CONFIG_HOME="$lab/config" XDG_DATA_HOME="$lab/data" XDG_CACHE_HOME="$lab/cache" \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=gles2 WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 \
    LIBSEAT_BACKEND=disabled dbus-run-session -- "$sway" --unsupported-gpu -c "$lab/sway.conf" \
    > "$lab/parent.log" 2>&1 < /dev/null &
parent=$!
printf '%s\n' "$parent" > "$lab/parent.pid"
parent_socket=""
for ((i=0; i<200; i++)); do
    kill -0 "$parent" || exit 1
    for socket in "$lab"/wayland-*; do
        [[ ! -S $socket ]] || parent_socket=${socket##*/}
    done
    [[ -z $parent_socket ]] || break
    sleep 0.1
done
[[ -n $parent_socket ]] || exit 1
nohup setsid env -u LD_PRELOAD -u DISPLAY -u HYPRLAND_INSTANCE_SIGNATURE -u DBUS_SESSION_BUS_ADDRESS \
    LD_LIBRARY_PATH="$aq" PATH="$child_path" XDG_RUNTIME_DIR="$lab" XDG_CONFIG_HOME="$lab/config" XDG_DATA_HOME="$lab/data" XDG_CACHE_HOME="$lab/cache" \
    WAYLAND_DISPLAY="$parent_socket" AQ_DRM_DEVICES=/dev/null LIBSEAT_BACKEND=disabled \
    HYPRLAND_NO_SD_VARS=1 HYPRLAND_NO_SD_TARGET=1 dbus-run-session -- "$hypr" --config "$lab/hyprland.lua" \
    > "$lab/child.log" 2>&1 < /dev/null &
child=$!
printf '%s\n' "$child" > "$lab/child.pid"
lock=""
for ((i=0; i<200; i++)); do
    kill -0 "$child" || exit 1
    for candidate in "$lab"/hypr/*/hyprland.lock; do
        [[ ! -f $candidate ]] || lock=$candidate
    done
    [[ -z $lock ]] || break
    sleep 0.1
done
[[ -n $lock ]] || exit 1
mapfile -t lock_lines < "$lock"
signature=$(basename -- "$(dirname -- "$lock")")
child_socket=${lock_lines[1]}
ctl() { env -u LD_LIBRARY_PATH -u LD_PRELOAD XDG_RUNTIME_DIR="$lab" HYPRLAND_INSTANCE_SIGNATURE="$signature" hyprctl "$@"; }
ctl output create headless Main > "$lab/setup.log"
ctl eval 'hl.monitor({output="WAYLAND-1",disabled=true})' >> "$lab/setup.log"
ctl -j monitors > "$lab/monitors.json"
printf 'export XDG_RUNTIME_DIR=%q\nexport WAYLAND_DISPLAY=%q\nexport HYPRLAND_INSTANCE_SIGNATURE=%q\nexport HDR_CAPTURE_LAB=1\n' \
    "$lab" "$child_socket" "$signature" > "$lab/lab.env"
failed=0
printf '%s\n' "$lab"
