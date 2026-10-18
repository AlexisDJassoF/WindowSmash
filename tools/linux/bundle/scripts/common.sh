# Shared by the launchers (sourced from the folder the game is in).
#
# The game's data - its RomFS, about 17 GB - stays in the files it came in, read in place. For
# the original game that is one NCA. For an update (tools/update.py) it is pieced together from
# several files by the layer map dump/modules/romfs.layers: the base game's NCA and the update's
# own data, romfs_patch.bin. dump/modules/romfs.cfg says where those files are. On the first run
# (or when one has moved) the launchers look for each by name and size, and write that down:
# they search this folder, ./game, VMware's shared folders (/mnt/hgfs), /media, /mnt and your
# home folder. SSBU_NCA=<path> (the NCA) and SSBU_ROMFS_PATCH=<path> (romfs_patch.bin) name a
# file outright.

# Filled in by tools/linux/package_linux.py: "plain" (the RomFS in place in the NCA, from byte
# ROMFS_OFFSET on) or "layered", and the files, in romfs.cfg's order, one per line:
#   <variable naming it outright>|<name>|<size in bytes>|<its folder on Windows>
ROMFS_KIND="@ROMFS_KIND@"
ROMFS_OFFSET="@ROMFS_OFFSET@"
ROMFS_FILES='@ROMFS_FILES@'
ROMFS_MAP=dump/modules/romfs.layers

# romfs.cfg (runtime/hle_fs.c) for the files at the given paths
romfs_cfg_text() {
    if [ "$ROMFS_KIND" = layered ]; then
        printf 'layered\n%s\n' "$PWD/$ROMFS_MAP"
        printf '%s\n' "$@"
    else
        printf '%s\n%s\n' "$1" "$ROMFS_OFFSET"
    fi
}

file_size() {
    stat -L -c %s "$1" 2>/dev/null
}

# The files' paths romfs.cfg names now, one per line, if it is of this package's kind
romfs_cfg_paths() {
    local cfg=dump/modules/romfs.cfg
    [ -f "$cfg" ] || return 0
    if [ "$ROMFS_KIND" = layered ]; then
        [ "$(head -n 1 "$cfg")" = layered ] && tail -n +3 "$cfg"
    else
        [ "$(head -n 1 "$cfg")" != layered ] && head -n 1 "$cfg"
    fi
}

find_game_data() {
    local cfg=dump/modules/romfs.cfg old=() paths=() i=0 var name size windir path d text
    if [ "$ROMFS_KIND" = layered ] && [ ! -f "$ROMFS_MAP" ]; then
        echo "$ROMFS_MAP is missing: copy the whole ssbu-linux folder again."
        return 1
    fi
    mapfile -t old < <(romfs_cfg_paths)
    while IFS='|' read -r var name size windir; do
        path="${!var}"
        # (where romfs.cfg already has it, if it is still there)
        if [ -z "$path" ] && [ -f "${old[i]}" ] && [ "$(file_size "${old[i]}")" = "$size" ]; then
            path="${old[i]}"
        fi
        if [ -z "$path" ]; then
            echo "Looking for the game data ($name)..."
            for d in "$PWD" "$PWD/game" /mnt/hgfs /media /mnt "$HOME"; do
                [ -d "$d" ] || continue
                path=$(find -L "$d" -maxdepth 9 -name "$name" -type f -size "${size}c" -print -quit 2>/dev/null)
                [ -n "$path" ] && break
            done
        fi
        if [ -z "$path" ] || [ ! -f "$path" ]; then
            echo
            echo "Cannot find the game data: $name ($(awk "BEGIN { printf \"%.1f\", $size / 1e9 }") GB)."
            echo "On Windows it is in  $windir\\"
            echo "Copy it into $PWD/game/, or share that folder with this VM (VMware: VM > Settings >"
            echo "Options > Shared Folders), or run with $var=/path/to/$name bash run.sh"
            return 1
        fi
        if [ "$(file_size "$path")" != "$size" ]; then
            echo "$path is not the game data this folder needs: $name of $size bytes."
            return 1
        fi
        paths+=("$(readlink -f "$path")")
        i=$((i + 1))
    done <<< "$ROMFS_FILES"
    text=$(romfs_cfg_text "${paths[@]}")
    if [ "$text" != "$(cat "$cfg" 2>/dev/null)" ]; then
        printf '%s\n' "$text" > "$cfg"
        printf 'Game data: %s\n' "${paths[@]}"
    fi
    return 0
}

check_libraries() {
    chmod +x ./ssbu 2>/dev/null
    local missing
    missing=$(LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd ./ssbu 2>/dev/null | grep "not found")
    if [ -n "$missing" ]; then
        echo "Some libraries the game needs are not installed:"
        echo "$missing"
        echo "Install them with:  bash install-deps.sh"
        return 1
    fi
    if ! vulkan_installed; then
        echo "No Vulkan driver is installed (the renderer needs one). Install them with:  bash install-deps.sh"
        return 1
    fi
    return 0
}

# The Vulkan loader, and a driver for it: a manifest in any of the loader's folders (most systems
# have only some of them), or one named outright
vulkan_installed() {
    local f
    { ldconfig -p 2>/dev/null || /sbin/ldconfig -p 2>/dev/null; } | grep -q "libvulkan.so.1" ||
        [ -e /usr/lib/x86_64-linux-gnu/libvulkan.so.1 ] || [ -e /usr/lib64/libvulkan.so.1 ] || return 1
    [ -n "$VK_ICD_FILENAMES$VK_DRIVER_FILES" ] && return 0
    for f in /usr/share/vulkan/icd.d/*.json /usr/local/share/vulkan/icd.d/*.json /etc/vulkan/icd.d/*.json; do
        [ -f "$f" ] && return 0
    done
    return 1
}
