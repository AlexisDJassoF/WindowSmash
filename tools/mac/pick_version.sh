# Picks the game version to start, for the launchers (tools/mac/launch.sh sources this; the macOS
# tools/pick_version.bat): sets SSBU_TREE to its folder - the port's own (.) or a version's own
# (v13.0.5, ...: tools/install_version.py, docs/VERSIONS.md). With one version built it is that one;
# with several it lists them (each folder's version.txt) and asks - in Terminal, or in a list with
# --gui - offering the one picked last time (last_version.txt). SSBU_TREE set beforehand to a built
# folder skips the question. Run from the port's folder; returns 1, after saying why, when nothing is
# built (or the list was cancelled). Uses launch.sh's gui, message and choose_one.
# (Written for macOS's own bash, 3.2.)

# A game version is built in a folder when its game is: build-mac/WindowSmash.app
built_tree() {
    [ -x "$1/build-mac/WindowSmash.app/Contents/MacOS/ssbu" ]
}

# The versions built here, one per line: <folder>, a tab, <label> (the port's own first, then the
# version folders, which also need their game data set up: dump/modules/romfs.cfg)
list_trees() {
    local d ver
    for d in . */; do
        d=${d%/}
        if [ "$d" != . ]; then
            [ -d "$d" ] && [ -f "$d/dump/modules/romfs.cfg" ] || continue
        fi
        built_tree "$d" || continue
        ver=
        [ -f "$d/version.txt" ] && IFS= read -r ver < "$d/version.txt"
        ver=${ver%$'\r'}
        if [ "$d" = . ]; then
            printf '%s\t%s\n' "$d" "${ver:-this folder}  - build-mac/, save/"
        else
            printf '%s\t%s\n' "$d" "${ver:-$d}  - $d/"
        fi
    done
}

pick_version() {
    local n=0 i pick last answer t l
    local trees labels
    if [ -n "$SSBU_TREE" ] && built_tree "$SSBU_TREE"; then
        return 0
    fi
    trees=()
    labels=()
    while IFS=$'\t' read -r t l; do
        trees[n]=$t
        labels[n]=$l
        n=$((n + 1))
    done < <(list_trees)
    if [ "$n" = 0 ]; then
        message "No game version is built here yet: double-click Install WindowSmash.command (in $PWD) to build the game. (More versions: docs/VERSIONS.md.)"
        return 1
    fi
    pick=$((n - 1))
    last=
    [ -f last_version.txt ] && IFS= read -r last < last_version.txt
    last=$(printf '%s' "${last%$'\r'}" | tr '[:upper:]' '[:lower:]')
    for ((i = 0; i < n; i++)); do
        [ "$(printf '%s' "${trees[i]}" | tr '[:upper:]' '[:lower:]')" = "$last" ] && pick=$i
    done
    if [ "$n" -gt 1 ]; then
        if [ "$gui" = 1 ]; then
            answer=$(choose_one "Which game version?" "${labels[pick]}" "${labels[@]}") || return 1
            for ((i = 0; i < n; i++)); do
                [ "${labels[i]}" = "$answer" ] && pick=$i
            done
        else
            echo "Game versions built here:"
            for ((i = 0; i < n; i++)); do
                echo "  $((i + 1)). ${labels[i]}"
            done
            while :; do
                answer=
                read -r -p "Which one [$((pick + 1))]: " answer || echo
                [ -z "$answer" ] && break
                case "$answer" in
                    *[!0-9]*) ;;
                    *)
                        if [ "$answer" -ge 1 ] && [ "$answer" -le "$n" ]; then
                            pick=$((answer - 1))
                            break
                        fi
                        ;;
                esac
                echo "Type one of the numbers above."
            done
        fi
    fi
    printf '%s\n' "${trees[pick]}" > last_version.txt
    [ "$gui" = 1 ] || echo "Game version ${labels[pick]}"
    SSBU_TREE=${trees[pick]}
    export SSBU_TREE
    return 0
}
