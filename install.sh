#!/usr/bin/env bash
# Installs Ableton Live 12 as a Linux desktop app: the Linux build from the
# Push 3 recovery image, the content the image lacks from the Windows Suite
# installer (Core Library, lessons, high-quality stem models, cursors, icon,
# edition), and the shim and launcher next to this script. Live's own files
# stay as Ableton made them. Live then runs like any Linux program: settings
# in ~/.config/Ableton and ~/.local/share/Ableton, libraries in the desktop's
# Music folder (<Music>/Ableton). Writes uninstall.sh into the install folder.
# A run that stops (an error, a full disk, the window closed) can be started
# again: it continues after the last finished part. What the tools report goes
# to install.log in the install folder.
#
# usage: install.sh [--prefix DIR]
#   Put the Push 3 recovery image (Push3-RecoveryImage-*.zip) and the Windows
#   Live Suite installer (ableton_live_suite_*_64.zip) in this script's folder.
#   --prefix DIR  install folder for the program (default ~/.local/share/ableton-live-12)
set -euo pipefail

# Started from a file manager there is no terminal to show the progress in:
# run again inside one, and keep its window open at the end.
if [ ! -t 0 ] && [ ! -t 1 ] && [ -z "${LIVE_INSTALLER_TERMINAL:-}" ] && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
    self=$(readlink -f "$0")
    run=(env LIVE_INSTALLER_TERMINAL=1 bash -c 'bash "$0" "$@"; status=$?; echo; read -r -p "Press Enter to close this window. " _ </dev/tty; exit $status' "$self" "$@")
    case "${XDG_CURRENT_DESKTOP:-}" in
        *KDE*) order=(konsole) ;;
        *GNOME* | *Unity* | *Pantheon*) order=(kgx ptyxis gnome-terminal) ;;
        *Cinnamon* | *X-Cinnamon*) order=(gnome-terminal) ;;
        *XFCE*) order=(xfce4-terminal) ;;
        *MATE*) order=(mate-terminal) ;;
        *LXQt*) order=(qterminal) ;;
        *) order=() ;;
    esac
    order+=(x-terminal-emulator konsole kgx ptyxis gnome-terminal xfce4-terminal mate-terminal qterminal
            tilix alacritty kitty foot wezterm xterm)
    for term in "${order[@]}"; do
        command -v "$term" >/dev/null || continue
        case $term in
            kgx | ptyxis | gnome-terminal) exec "$term" -- "${run[@]}" ;;
            xfce4-terminal | mate-terminal) exec "$term" -x "${run[@]}" ;;
            kitty | foot) exec "$term" "${run[@]}" ;;
            wezterm) exec "$term" start -- "${run[@]}" ;;
            *) exec "$term" -e "${run[@]}" ;;
        esac
    done
    message="No terminal program found. Open a terminal in $(dirname "$self") and run ./install.sh"
    command -v kdialog >/dev/null && exec kdialog --error "$message"
    command -v zenity >/dev/null && exec zenity --error --text="$message"
    command -v notify-send >/dev/null && exec notify-send "Ableton Live installer" "$message"
    exit 1
fi

LIVE_SHA256=8f9052719b35d6add174c2fe9a61897c7115717bc4d3d6623697881976251dc0 # Live 12.4.7 (Push 3 2.4.7)
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
PREFIX="$DATA_HOME/ableton-live-12"
NEEDED_GB=25
# Sizes of 2.4.7 / 12.4.7 for the progress bars where the tools report none
# (another version only makes the bar less exact).
PUSH3_BYTES=3655802632  # /opt/push3
SUITE_BYTES=3375249763  # SUITE_PARTS

# Libraries from the image's /usr/lib. compat_lib: what Live links and desktop
# systems lack (FFmpeg 5, ICU 70, Theora). Fallback: Qt's xcb helpers and
# libnsl, used only when the system has none (copy_lib).
COMPAT=(libavcodec.so.59.18.100 libavformat.so.59.16.100 libavutil.so.57.17.100 libswresample.so.4.3.100
        libswscale.so.6.4.100 libicudata.so.70.1 libicui18n.so.70.1 libicuuc.so.70.1
        libtheoradec.so.1.1.4 libtheoraenc.so.1.1.2)
FALLBACK=(libnsl.so.3.0.0 libxcb-icccm.so.4.0.0 libxcb-image.so.0.0.0 libxcb-util.so.1.0.0
          libxcb-keysyms.so.1.0.0 libxcb-render-util.so.0.0.0 libxcb-xinerama.so.0.0.0
          libxcb-xinput.so.0.1.0 libxcb-xkb.so.1.0.0)
# Qt's xcb helpers, next to Live's Qt (its RUNPATH $ORIGIN wins over Live's RPATH).
XCB_LIBS=(libxcb-icccm.so.4 libxcb-image.so.0 libxcb-util.so.1 libxcb-keysyms.so.1 libxcb-render-util.so.0
          libxcb-xinerama.so.0 libxcb-xinput.so.0 libxcb-xkb.so.1)
# The Suite installer's files that are needed (innoextract --include).
SUITE_PARTS=("/app/Resources/Core Library" "/app/Resources/Builtin Lessons" "/app/Resources/Builtin/Models"
             "/app/Resources/Cursors" "/app/Resources/Icons" "/app/Program/Installation.cfg")
export DEBUGFS_PAGER=__none__

# --- terminal output ------------------------------------------------------------------
# A numbered line per step, a progress bar under it while it runs, then the
# time it took. Without a terminal (a log file) only the lines are written.
STEPS=13
STEP=0
STEP_START=$SECONDS
if [ -t 1 ]; then
    TTY=1 BOLD=$'\e[1m' DIM=$'\e[2m' GREEN=$'\e[32m' RED=$'\e[31m' YELLOW=$'\e[33m' RESET=$'\e[0m'
else
    TTY=0 BOLD="" DIM="" GREEN="" RED="" YELLOW="" RESET=""
fi

clear_line() { [ $TTY = 0 ] || printf '\r\e[K'; }
step() {
    STEP=$((STEP + 1))
    STEP_START=$SECONDS
    printf '%s[%2d/%d]%s %s\n' "$BOLD" "$STEP" "$STEPS" "$RESET" "$1"
    [ -z "${LOG:-}" ] || printf '\n== %s [%d/%d] %s\n' "$(date '+%F %T')" "$STEP" "$STEPS" "$1" >> "$LOG"
}
ok() {
    local t=$((SECONDS - STEP_START))
    clear_line
    printf '        %s✓%s %s%s%dm%02ds%s\n' "$GREEN" "$RESET" "${1:+$1  }" "$DIM" $((t / 60)) $((t % 60)) "$RESET"
}
note() { clear_line; printf '        %s%s%s\n' "$YELLOW" "$*" "$RESET"; }
# Once the install folder is in use, die also shows what the tools said in
# this step (install.log), whether the disk is full, and that a new run
# continues from here.
die() {
    local said free
    clear_line
    printf '\n%serror:%s %s\n' "$RED$BOLD" "$RESET" "$*" >&2
    [ -n "${LOG:-}" ] || exit 1
    said=$(awk '/^== / { said = ""; next } { said = said $0 "\n" } END { printf "%s", said }' "$LOG" 2>/dev/null | tail -n 6)
    [ -z "$said" ] || printf '       The tools said (all in %s):\n%s\n' "$LOG" "$(sed 's/^/         /' <<< "$said")" >&2
    printf 'error: %s\n' "$*" >> "$LOG" 2>/dev/null || true
    free=$(df -Pk "$PREFIX" 2>/dev/null | awk 'NR==2 {print $4 * 1024}')
    [ -z "$free" ] || [ "$free" -ge 1000000000 ] ||
        printf '       %s is full (%s free): make space there first.\n' "$PREFIX" "$(human "$free")" >&2
    [ "$STEP" -le 2 ] || printf '       Run install.sh again to continue; what is finished stays.\n' >&2
    exit 1
}
human() { awk -v b="$1" 'BEGIN { if (b >= 1e9) printf "%.1f GB", b / 1e9; else printf "%.0f MB", b / 1e6 }'; }
# bar DONE TOTAL: one line, redrawn in place.
bar() {
    [ $TTY = 1 ] || return 0
    local done=$1 total=$2 cols width fill pct
    [ "$total" -gt 0 ] || total=1
    [ "$done" -le "$total" ] || done=$total
    cols=$(tput cols 2>/dev/null || echo 80)
    width=$((cols - 38))
    [ $width -ge 10 ] || width=10
    [ $width -le 50 ] || width=50
    pct=$((done * 100 / total))
    fill=$((done * width / total))
    printf '\r\e[K        [%s%s] %3d%%  %s of %s' "$(printf '%*s' $fill '' | tr ' ' '#')" \
        "$(printf '%*s' $((width - fill)) '' | tr ' ' '.')" $pct "$(human "$done")" "$(human "$total")"
}
# watch PID PATH TOTAL: progress of a background command by the size of what it writes.
watch_size() {
    local pid=$1 path=$2 total=$3
    while kill -0 "$pid" 2>/dev/null; do
        bar "$(du -sb "$path" 2>/dev/null | cut -f1 || echo 0)" "$total"
        sleep 0.5
    done
    wait "$pid"
}
# ask "question" default(y|n): reads the terminal, takes the default without one.
ask() {
    local answer=""
    if (exec </dev/tty) 2>/dev/null; then read -r -p "        $1 " answer </dev/tty || echo; fi
    answer=${answer:-$2}
    [[ $answer == [yY]* ]]
}

deps_hint() {
    local id="" like=""
    if [ -r /etc/os-release ]; then
        id=$(. /etc/os-release && echo "${ID:-}")
        like=$(. /etc/os-release && echo "${ID_LIKE:-}")
    fi
    case " $id $like " in
        *" arch "*) echo "sudo pacman -S --needed unzip e2fsprogs pipewire-alsa" ;;
        *" debian "* | *" ubuntu "*) echo "sudo apt install unzip e2fsprogs pipewire-alsa" ;;
        *" fedora "*) echo "sudo dnf install unzip e2fsprogs pipewire-alsa" ;;
        *" suse "* | *" opensuse "*) echo "sudo zypper install unzip e2fsprogs pipewire-alsa" ;;
        *) echo "install: unzip, e2fsprogs (debugfs), pipewire-alsa" ;;
    esac
}

# zip_member ZIP REGEX: the first member whose name matches (case-insensitive).
# The listing is read whole: grep -q or -m1 in a pipe stops reading early, and
# with pipefail the writer's SIGPIPE then counts as a failure.
zip_member() {
    local list
    list=$(unzip -Z1 "$1" 2>/dev/null) || return 1
    re="$2" awk 'tolower($0) ~ tolower(ENVIRON["re"]) { print; found = 1; exit } END { exit !found }' <<< "$list"
}

soname() { local rest=${1#*.so.}; echo "${1%%.so.*}.so.${rest%%.*}"; }

# Start and size in bytes of GPT partition N of the disk image whose first MiB is in FILE.
gpt_partition() {
    local file=$1 n=$2 table esize entry first last
    [ "$(dd if="$file" bs=1 skip=512 count=8 status=none)" = "EFI PART" ] || return 1
    table=$(od -An -t u8 -j 584 -N 8 "$file")
    esize=$(od -An -t u4 -j 596 -N 4 "$file")
    entry=$((table * 512 + (n - 1) * esize))
    first=$(od -An -t u8 -j $((entry + 32)) -N 8 "$file")
    last=$(od -An -t u8 -j $((entry + 40)) -N 8 "$file")
    echo $((first * 512)) $(((last - first + 1) * 512))
}

# extract_partition ZIP MEMBER N OUT: partition N of the disk image MEMBER in
# ZIP to the file OUT (zero blocks left as holes), streamed: the whole image
# is never on disk. The first dd only counts what passes, for the bar.
extract_partition() {
    local zip=$1 member=$2 n=$3 out=$4 range start size pid read
    (unzip -p "$zip" "$member" 2>/dev/null || true) | head -c 1048576 > gpt.bin
    range=$(gpt_partition gpt.bin "$n") || die "could not read the partition table of $member"
    read -r start size <<< "$range"
    rm -f gpt.bin
    : > dd.log
    ( (unzip -p "$zip" "$member" 2>>"$LOG" || true) | { dd bs=4M status=progress 2> dd.log || true; } |
        dd of="$out" bs=4M iflag=fullblock,skip_bytes,count_bytes skip="$start" count="$size" conv=sparse status=none ) &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        [ -e dd.log ] || die "the work folder $WORK was removed while installing"
        read=$(tr '\r' '\n' < dd.log | grep -o '^[0-9]*' | tail -1 || true)
        bar "${read:-0}" $((start + size))
        sleep 0.5
    done
    wait $pid || die "could not read $member"
    rm -f dd.log
    [ "$(stat -c %s "$out")" = "$size" ] || die "$member ends early: is $zip complete?"
}

# copy_lib NAME DIR: a library GTK desktops often lack, from this system, or
# the image's copy when the system has none.
copy_lib() {
    local name=$1 dir=$2 d
    [ ! -e "$dir/$name" ] || return 0
    for d in /usr/lib64 /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib "$WORK/fallback"; do
        if [ -e "$d/$name" ]; then
            cp -L "$d/$name" "$dir/$name"
            return 0
        fi
    done
    die "$name is missing; install it or copy it to $dir"
}

# --- arguments and checks ---------------------------------------------------
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=${2:?--prefix needs a folder}; shift 2 ;;
        -h | --help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument: $1 (usage: install.sh [--prefix DIR])" ;;
    esac
done
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]:-.}")" && pwd)
# find_zip WHAT PATTERN: the one zip in this script's folder that matches.
find_zip() {
    local found=()
    shopt -s nullglob nocaseglob
    found=("$SCRIPT_DIR"/$2)
    shopt -u nullglob nocaseglob
    [ ${#found[@]} -gt 0 ] || die "no $1 in $SCRIPT_DIR (expected $2)"
    [ ${#found[@]} -eq 1 ] || die "several files could be the $1 in $SCRIPT_DIR; keep one:
$(printf '         %s\n' "${found[@]##*/}")"
    echo "${found[0]}"
}
PUSH_ZIP=$(find_zip "Push 3 recovery image" "Push3-RecoveryImage-*.zip")
SUITE_ZIP=$(find_zip "Live Suite installer" "ableton_live_suite_*.zip")
[ -f "$SCRIPT_DIR/shim/libmenufix.so" ] && [ -f "$SCRIPT_DIR/tools/AbletonLive12-launcher" ] ||
    die "shim/ and tools/ must be next to install.sh (keep the folder as it came)"
MODS=$SCRIPT_DIR
PREFIX=$(realpath -m -- "$PREFIX")
# Live registers itself at every start through a shell command that does not
# quote its own folder: a space there gives an error dialog on each start.
case $PREFIX in *[[:space:]\'\"\\\$\`]*) die "the install folder must not contain spaces or quotes: $PREFIX" ;; esac

printf '\n%sAbleton Live 12 for Linux: installer%s\n' "$BOLD" "$RESET"
printf '  Push 3 image  %s\n  Suite         %s\n  Install to    %s\n\n' "$PUSH_ZIP" "$SUITE_ZIP" "$PREFIX"

step "Checking what the installation needs"
[ "$(id -u)" != 0 ] || die "run this as your normal user; it asks for sudo only for the loader link"
[ "$(uname -m)" = x86_64 ] || die "Live's Linux build is for x86-64 only"
missing=()
for tool in unzip head realpath du od dd tr awk stat find; do
    command -v "$tool" >/dev/null || missing+=("$tool")
done
DEBUGFS=$(command -v debugfs || ls /usr/sbin/debugfs /sbin/debugfs 2>/dev/null | head -1 || true)
[ -n "$DEBUGFS" ] || missing+=(debugfs)
[ ${#missing[@]} -eq 0 ] || die "missing: ${missing[*]}
       install them with:
         $(deps_hint)"
mkdir -p "$PREFIX" || die "could not create $PREFIX"
[ -w "$PREFIX" ] || die "$PREFIX is not writable"
# One installation into a folder at a time: a second one would take the first
# one's work files away.
exec 9>"$PREFIX/.install.lock"
if command -v flock >/dev/null && ! flock -n 9; then
    die "another installation into $PREFIX is still running"
fi
# live_running: whether a Live from this install folder runs.
live_running() {
    local p exe
    for p in /proc/[0-9]*; do
        exe=$(readlink "$p/exe" 2>/dev/null) || continue
        case $exe in "$PREFIX"/push3_opt/*) return 0 ;; esac
    done
    return 1
}
# What an earlier run left. uninstall.sh is written last, so without it the
# program is from a run that stopped: the parts it finished have a marker
# (.install-program, .install-content) and are kept, the rest is done again.
if [ -e "$PREFIX/push3_opt" ] && [ -e "$PREFIX/uninstall.sh" ]; then
    ! live_running || die "Live is installed in $PREFIX and running; quit it first"
    note "Live is already installed in $PREFIX."
    [ $TTY = 0 ] || printf '\e[?25h'
    ask "Install it again? Your settings, licence, libraries and projects stay. [y/N]" n ||
        die "Live is already installed in $PREFIX (to remove it: $PREFIX/uninstall.sh)"
    [ $TTY = 0 ] || printf '\e[?25l'
    rm -rf "${PREFIX:?}"/{push3_opt,compat_lib,shim,tools,Options-desktop.txt,uninstall.sh,.install-program,.install-content} ||
        die "could not remove the installed program from $PREFIX"
elif [ -e "$PREFIX/.install-program" ]; then
    note "Continuing the installation an earlier run did not finish."
elif [ -e "$PREFIX/push3_opt" ] || [ -e "$PREFIX/compat_lib" ]; then
    rm -rf "${PREFIX:?}"/{push3_opt,compat_lib,.install-content} || die "could not remove the unfinished program in $PREFIX"
    note "Removed the unfinished copy of Live an earlier run left; starting it again."
fi
LOG="$PREFIX/install.log"
printf '\n== %s install.sh from %s into %s\n' "$(date '+%F %T')" "$SCRIPT_DIR" "$PREFIX" >> "$LOG" ||
    die "could not write to $PREFIX"
# A live session (a USB system) keeps the home folder in RAM.
case $(findmnt -no FSTYPE --target "$PREFIX" 2>/dev/null || true) in
    tmpfs | overlay)
        note "$PREFIX is in RAM or on an overlay (a live session?): the installation uses that memory and may be gone after a restart." ;;
esac
# Less space when parts are already done (the program is about 4.5 GB of it).
[ ! -e "$PREFIX/.install-program" ] || NEEDED_GB=13
[ ! -e "$PREFIX/.install-content" ] || NEEDED_GB=1
free=$(( $(df -Pk "$PREFIX" | awk 'NR==2 {print $4}') * 1024 ))
[ "$free" -ge $((NEEDED_GB * 1000000000)) ] ||
    die "$PREFIX has $(human "$free") free; the installation needs about $NEEDED_GB GB while it runs (about 7 GB after)"
rm -rf "$PREFIX"/.install-work* # left by an interrupted run
WORK=$(mktemp -d "$PREFIX/.install-work.XXXXXX")
cleanup() {
    local jobs
    jobs=$(jobs -p)
    [ -z "$jobs" ] || kill $jobs 2>/dev/null || true
    rm -rf "$WORK"
    [ $TTY = 0 ] || printf '\e[?25h'
}
trap cleanup EXIT
stopped() {
    clear_line
    printf '\n%sStopped.%s Run install.sh again to continue; what is finished stays.\n' "$YELLOW$BOLD" "$RESET" >&2
    exit "$1"
}
trap 'stopped 130' INT TERM
trap 'exit 129' HUP # the terminal window closed
[ $TTY = 0 ] || printf '\e[?25l' # no cursor blinking over the bars
ok "$(human "$free") free"

# --- input files --------------------------------------------------------------------
step "Checking the Push 3 image and the Suite installer"
for zip in "$PUSH_ZIP" "$SUITE_ZIP"; do
    [ -r "$zip" ] || die "cannot read $zip"
    unzip -Z1 "$zip" >/dev/null 2>>"$LOG" ||
        die "$(basename "$zip") is not a complete zip: still being copied or downloaded, or damaged (copy it again)"
done
zip_member "$PUSH_ZIP" '\.wic$' >/dev/null || die "$(basename "$PUSH_ZIP") holds no .wic image (is it the Push 3 recovery image?)"
zip_member "$SUITE_ZIP" 'installer.*\.exe$' >/dev/null ||
    die "$(basename "$SUITE_ZIP") holds no Live installer .exe (is it the Windows Live Suite installer?)"
ok "$(basename "$PUSH_ZIP"), $(basename "$SUITE_ZIP")"

# --- Push 3 image: /opt/push3 and libraries ---------------------------------------
cd "$WORK" # debugfs takes output paths without quoting: keep them relative
APP="$PREFIX/push3_opt/push3"
# done_before MARKER: the steps up to MARKER were finished by an earlier run.
done_before() {
    [ -e "$PREFIX/$1" ] || return 1
    ok "done by an earlier run"
}
step "Reading the recovery partition of the Push 3 image"
if ! done_before .install-program; then
    wic=$(zip_member "$PUSH_ZIP" '\.wic$') || die "$PUSH_ZIP holds no .wic image"
    extract_partition "$PUSH_ZIP" "$wic" 2 recovery.img
    ok
fi

step "Unpacking the device image from it"
if ! done_before .install-program; then
    size=$("$DEBUGFS" -R "stat /opt/recovery/CombinedImage.zip" recovery.img 2>>"$LOG" |
        awk '{ for (i = 1; i < NF; i++) if ($i == "Size:") { print $(i + 1); exit } }' || true)
    [ -n "$size" ] || die "no CombinedImage.zip in the recovery partition"
    "$DEBUGFS" -R "dump /opt/recovery/CombinedImage.zip combined.zip" recovery.img >/dev/null 2>>"$LOG" &
    watch_size $! combined.zip "$size" || die "could not copy CombinedImage.zip"
    [ "$(stat -c %s combined.zip)" = "$size" ] || die "could not copy all of CombinedImage.zip"
    rm -f recovery.img
    ok
fi

step "Reading Live's system partition"
if ! done_before .install-program; then
    wic=$(zip_member combined.zip '\.wic$') || die "CombinedImage.zip holds no .wic image"
    extract_partition combined.zip "$wic" 3 rootfs.img
    rm -f combined.zip
    ok
fi

step "Copying Live out of the image"
if ! done_before .install-program; then
    mkdir -p opt lib fallback
    "$DEBUGFS" -R "rdump /opt/push3 opt" rootfs.img >/dev/null 2>>"$LOG" &
    watch_size $! opt "$PUSH3_BYTES" || die "could not copy /opt/push3"
    for f in "${COMPAT[@]}" "${FALLBACK[@]}"; do echo "dump /usr/lib/$f lib/$f"; done > libs.cmd
    "$DEBUGFS" -f libs.cmd rootfs.img >/dev/null 2>>"$LOG"
    rm -f rootfs.img
    [ -x opt/push3/Live ] || die "the image has no /opt/push3/Live"
    rm -rf "${PREFIX:?}"/{push3_opt,compat_lib,.install-content} # the content goes into push3_opt too
    mkdir -p "$PREFIX/push3_opt" "$PREFIX/compat_lib"
    mv opt/push3 "$APP"
    for f in "${COMPAT[@]}"; do
        [ -f "lib/$f" ] || die "the image has no /usr/lib/$f"
        mv "lib/$f" "$PREFIX/compat_lib/$f"
        ln -sf "$f" "$PREFIX/compat_lib/$(soname "$f")"
    done
    for f in "${FALLBACK[@]}"; do
        if [ -f "lib/$f" ]; then mv "lib/$f" "fallback/$(soname "$f")"; fi
    done
    for name in "${XCB_LIBS[@]}"; do copy_lib "$name" "$APP/lib"; done
    copy_lib libnsl.so.3 "$PREFIX/compat_lib"
    touch "$PREFIX/.install-program"
    if [ "$(sha256sum "$APP/Live" | cut -d' ' -f1)" = "$LIVE_SHA256" ]; then
        ok "Live 12.4.7"
    else
        ok
        note "This is not Live 12.4.7 (Push 3 2.4.7), which the shim was made for; fixes that do not match stay off."
    fi
fi

step "Installing the shim and the launcher"
mkdir -p "$PREFIX/shim" "$PREFIX/tools"
cp "$MODS/shim/libmenufix.so" "$PREFIX/shim/"
cp "$MODS/Options-desktop.txt" "$PREFIX/"
cp -r "$MODS/tools/." "$PREFIX/tools/"
# Executable here even when the folder came on a USB stick or a Windows file system.
chmod +x "$PREFIX/tools/"*.sh "$PREFIX/tools/AbletonLive12-launcher" "$PREFIX/tools/make-assets" \
    "$PREFIX/tools/bundled/innoextract"
cp "$PREFIX/tools/AbletonLive12-launcher" "$APP/AbletonLive12"
mkdir -p "$APP/lib/xkbcommon"
cp "$PREFIX/tools/bundled/"lib*.so.* "$APP/lib/xkbcommon/"
ok

# --- Windows Suite: content the image lacks -----------------------------------------
step "Unpacking the Suite installer"
if ! done_before .install-content; then
    mkdir -p suite
    total=$(unzip -Zt "$SUITE_ZIP" 2>>"$LOG" | grep -o '[0-9]* bytes uncompressed' | cut -d' ' -f1 || true)
    unzip -q "$SUITE_ZIP" -d suite 2>>"$LOG" &
    watch_size $! suite "${total:-6500000000}" || die "could not unpack $SUITE_ZIP"
    exe=$(find suite -maxdepth 2 -name '*Installer.exe' -print -quit)
    [ -n "$exe" ] || die "$SUITE_ZIP holds no Ableton Live installer .exe"
    ok
fi

# bundled_innoextract: the innoextract in tools/bundled (tools/bundled/README),
# when it runs here. It reads Live 12.4's installer (Inno Setup 6.4), which the
# innoextract 1.9 most distributions ship cannot. It links bzip2's library as
# libbz2.so.1.0, which Fedora and openSUSE name libbz2.so.1: a link in the work
# folder stands in for a library missing under its name.
bundled_innoextract() {
    local ie=$PREFIX/tools/bundled/innoextract lib d found
    mkdir -p "$WORK/lib"
    for lib in $(ldd "$ie" 2>/dev/null | awk '/not found/ { print $1 }'); do
        for d in /usr/lib64 /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /lib64 /usr/lib; do
            found=$(find "$d" -maxdepth 1 -name "${lib%%.so*}.so.*" -print -quit 2>/dev/null || true)
            if [ -n "$found" ]; then
                ln -sf "$found" "$WORK/lib/$lib"
                break
            fi
        done
    done
    LD_LIBRARY_PATH="$WORK/lib" "$ie" --version >/dev/null 2>>"$LOG" || return 1
    echo "$ie"
}

step "Extracting the Core Library, lessons and stem models"
if ! done_before .install-content; then
    include=()
    for part in "${SUITE_PARTS[@]}"; do include+=(-I "$part"); done
    # The bundled one first, the system's if that fails (it may be a newer one).
    extractors=()
    if ie=$(bundled_innoextract); then extractors+=("$ie"); fi
    if command -v innoextract >/dev/null; then extractors+=("$(command -v innoextract)"); fi
    [ ${#extractors[@]} -gt 0 ] ||
        die "the bundled innoextract does not run on this system and none is installed; install innoextract 1.10 or newer"
    status=0
    for ie in "${extractors[@]}"; do
        rm -rf suite/x
        printf 'innoextract: %s\n' "$ie" >> "$LOG"
        LD_LIBRARY_PATH="$WORK/lib" "$ie" -s -d suite/x "${include[@]}" "$exe" >/dev/null 2>>"$LOG" &
        if watch_size $! suite/x "$SUITE_BYTES"; then status=0; break; else status=$?; fi
        # Killed (by the kernel when memory ran out): another one would be too.
        [ $status -lt 128 ] || break
    done
    if [ $status -ge 128 ]; then
        die "innoextract was stopped (signal $((status - 128))): did the memory run out? Close other programs and run install.sh again"
    fi
    [ $status = 0 ] || die "innoextract could not read $(basename "$exe")"
    find suite -maxdepth 2 -name '*.bin' -delete
    rm -f "$exe"
    [ -d "suite/x/app/Resources/Core Library" ] || die "the Suite installer has no Core Library (is it the Suite edition?)"
    ok
fi

step "Adding them to Live, with cursors and icon"
if done_before .install-content; then
    CORE=$(cat "$PREFIX/.install-content")
else
    # The Core Library goes where Live looks for it: <Music>/Ableton in your home
    # (tools/import-windows-content.sh), or the isolated live-home/ an earlier
    # installation left in the install folder, which Live then keeps using.
    [ ! -d "$PREFIX/live-home" ] ||
        note "Using the Live data an earlier installation left in $PREFIX/live-home (delete it to use your home)."
    imported=$("$PREFIX/tools/import-windows-content.sh" "$WORK/suite/x/app" 2>>"$LOG") || die "could not copy the Suite content"
    CORE=$(printf '%s\n' "$imported" | sed -n 's/^done: Core Library in \(.*\); Live scans.*/\1/p')
    [ -n "$CORE" ] || die "could not copy the Core Library"
    LD_LIBRARY_PATH="$APP/lib:$PREFIX/compat_lib" QT_PLUGIN_PATH="$APP/plugins" \
        "$PREFIX/tools/make-assets" "$WORK/suite/x/app/Resources" "$APP" >/dev/null 2>>"$LOG" ||
        die "could not make the cursors and icon"
    printf '%s\n' "$CORE" > "$PREFIX/.install-content"
    cd "$PREFIX"
    rm -rf "$WORK"
    ok "$(human "$(du -sb "$PREFIX" "$CORE" | awk '{n += $1} END {print n}')") installed; Core Library in ${CORE/#"$HOME"/\~}"
fi
cd "$PREFIX"
rm -rf "$WORK"

# --- desktop integration -----------------------------------------------------------
step "Adding Ableton Live 12 to the application menu"
APPS="$DATA_HOME/applications"
DESKTOP="$APPS/ableton-live-12.desktop"
MIME_XML="$DATA_HOME/mime/packages/ableton-live-12.xml"
mkdir -p "$APPS" "$(dirname "$MIME_XML")"
cat > "$MIME_XML" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="application/x-ableton-live-set">
    <comment>Ableton Live Set</comment>
    <glob pattern="*.als"/>
  </mime-type>
  <mime-type type="application/x-ableton-live-pack">
    <comment>Ableton Live Pack</comment>
    <glob pattern="*.alp"/>
  </mime-type>
</mime-info>
EOF
cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=Ableton Live 12
Comment=Music production software
Exec="$APP/AbletonLive12" %U
Path=$APP
Icon=$APP/products/live/Live/AppLive/Linux/Icons/live_256.png
Terminal=false
Categories=Audio;AudioVideo;
StartupWMClass=ableton-live
MimeType=application/x-ableton-live-set;application/x-ableton-live-pack;x-scheme-handler/ableton;
EOF
command -v update-mime-database >/dev/null && update-mime-database "$DATA_HOME/mime" >/dev/null 2>&1 || true
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" >/dev/null 2>&1 || true
# ableton:// links (authorization in the browser) go to this Live. Live's own
# registration points at the unmodified Live binary, which cannot start alone.
command -v xdg-mime >/dev/null && xdg-mime default ableton-live-12.desktop x-scheme-handler/ableton >/dev/null 2>&1 || true
ok ".als and .alp files, ableton:// links"

# --- program loader -----------------------------------------------------------------
# Live asks for the loader at /lib/ld-linux-x86-64.so.2, the Push's path. Arch
# has it; most distributions have it only at /lib64.
step "Checking the program loader"
LOADER_LINK=0
if [ -e /lib/ld-linux-x86-64.so.2 ]; then
    ok "/lib/ld-linux-x86-64.so.2 is there"
else
    note "Live asks for /lib/ld-linux-x86-64.so.2, which this system does not have."
    note "The fix is a link to /lib64/ld-linux-x86-64.so.2 (the same loader); nothing else changes."
    [ $TTY = 0 ] || printf '\e[?25h'
    if [ -e /lib64/ld-linux-x86-64.so.2 ] && ask "Create it now with sudo? [Y/n]" y &&
        sudo ln -s /lib64/ld-linux-x86-64.so.2 /lib/ld-linux-x86-64.so.2; then
        LOADER_LINK=1
        ok "link created"
    else
        note "Live will not start without it. Create it later with:"
        note "  sudo ln -s /lib64/ld-linux-x86-64.so.2 /lib/ld-linux-x86-64.so.2"
    fi
fi

# --- uninstaller ----------------------------------------------------------------------
step "Writing the uninstaller"
{
    echo '#!/usr/bin/env bash'
    echo "# Removes Ableton Live 12, installed by install.sh on $(date +%F): the program,"
    echo '# the Core Library the installer copied, the menu entry and its file types.'
    echo '# Your User Library, recordings and projects in <Music>/Ableton always stay.'
    echo "# Asks whether to delete Live's settings and licence too, and whether to"
    echo '# remove the loader link if the installer made it.'
    echo '#'
    echo '# usage: uninstall.sh [--purge]   (--purge: delete the settings and licence without asking)'
    echo 'set -euo pipefail'
    echo '# What install.sh installed outside the install folder.'
    printf 'PREFIX=%q\nDESKTOP=%q\nMIME_XML=%q\nDATA_HOME=%q\nLOADER_LINK=%q\nCORE=%q\n' \
        "$PREFIX" "$DESKTOP" "$MIME_XML" "$DATA_HOME" "$LOADER_LINK" "$CORE"
    cat <<'EOF'

# Started from a file manager there is no terminal to show the progress in:
# run again inside one, and keep its window open at the end.
if [ ! -t 0 ] && [ ! -t 1 ] && [ -z "${LIVE_INSTALLER_TERMINAL:-}" ] && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
    self=$(readlink -f "$0")
    run=(env LIVE_INSTALLER_TERMINAL=1 bash -c 'bash "$0" "$@"; status=$?; echo; read -r -p "Press Enter to close this window. " _ </dev/tty; exit $status' "$self" "$@")
    case "${XDG_CURRENT_DESKTOP:-}" in
        *KDE*) order=(konsole) ;;
        *GNOME* | *Unity* | *Pantheon*) order=(kgx ptyxis gnome-terminal) ;;
        *Cinnamon* | *X-Cinnamon*) order=(gnome-terminal) ;;
        *XFCE*) order=(xfce4-terminal) ;;
        *MATE*) order=(mate-terminal) ;;
        *LXQt*) order=(qterminal) ;;
        *) order=() ;;
    esac
    order+=(x-terminal-emulator konsole kgx ptyxis gnome-terminal xfce4-terminal mate-terminal qterminal
            tilix alacritty kitty foot wezterm xterm)
    for term in "${order[@]}"; do
        command -v "$term" >/dev/null || continue
        case $term in
            kgx | ptyxis | gnome-terminal) exec "$term" -- "${run[@]}" ;;
            xfce4-terminal | mate-terminal) exec "$term" -x "${run[@]}" ;;
            kitty | foot) exec "$term" "${run[@]}" ;;
            wezterm) exec "$term" start -- "${run[@]}" ;;
            *) exec "$term" -e "${run[@]}" ;;
        esac
    done
    message="No terminal program found. Open a terminal in $(dirname "$self") and run ./uninstall.sh"
    command -v kdialog >/dev/null && exec kdialog --error "$message"
    command -v zenity >/dev/null && exec zenity --error --text="$message"
    command -v notify-send >/dev/null && exec notify-send "Ableton Live uninstaller" "$message"
    exit 1
fi

CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"

# --- terminal output ------------------------------------------------------------------
# The same as install.sh: a numbered line per step, a progress bar under it
# while it runs, then the time it took.
STEPS=5
STEP=0
STEP_START=$SECONDS
if [ -t 1 ]; then
    TTY=1 BOLD=$'\e[1m' DIM=$'\e[2m' GREEN=$'\e[32m' RED=$'\e[31m' YELLOW=$'\e[33m' RESET=$'\e[0m'
else
    TTY=0 BOLD="" DIM="" GREEN="" RED="" YELLOW="" RESET=""
fi

clear_line() { [ $TTY = 0 ] || printf '\r\e[K'; }
step() {
    STEP=$((STEP + 1))
    STEP_START=$SECONDS
    printf '%s[%2d/%d]%s %s\n' "$BOLD" "$STEP" "$STEPS" "$RESET" "$1"
}
ok() {
    local t=$((SECONDS - STEP_START))
    clear_line
    printf '        %s✓%s %s%s%dm%02ds%s\n' "$GREEN" "$RESET" "${1:+$1  }" "$DIM" $((t / 60)) $((t % 60)) "$RESET"
}
note() { clear_line; printf '        %s%s%s\n' "$YELLOW" "$*" "$RESET"; }
die() {
    clear_line
    printf '\n%serror:%s %s\n' "$RED$BOLD" "$RESET" "$*" >&2
    exit 1
}
human() { awk -v b="$1" 'BEGIN { if (b >= 1e9) printf "%.1f GB", b / 1e9; else printf "%.0f MB", b / 1e6 }'; }
# bar DONE TOTAL: one line, redrawn in place.
bar() {
    [ $TTY = 1 ] || return 0
    local done=$1 total=$2 cols width fill pct
    [ "$total" -gt 0 ] || total=1
    [ "$done" -le "$total" ] || done=$total
    [ "$done" -ge 0 ] || done=0
    cols=$(tput cols 2>/dev/null || echo 80)
    width=$((cols - 38))
    [ $width -ge 10 ] || width=10
    [ $width -le 50 ] || width=50
    pct=$((done * 100 / total))
    fill=$((done * width / total))
    printf '\r\e[K        [%s%s] %3d%%  %s of %s' "$(printf '%*s' $fill '' | tr ' ' '#')" \
        "$(printf '%*s' $((width - fill)) '' | tr ' ' '.')" $pct "$(human "$done")" "$(human "$total")"
}
# ask "question" default(y|n): reads the terminal, takes the default without one.
ask() {
    local answer=""
    if (exec </dev/tty) 2>/dev/null; then read -r -p "        $1 " answer </dev/tty || echo; fi
    answer=${answer:-$2}
    [[ $answer == [yY]* ]]
}
size_of() { { du -sbc -- "$@" 2>/dev/null || true; } | tail -1 | cut -f1; }
# remove PATH...: deletes in the background, with a bar of how much is gone.
remove() {
    local total pid
    [ $# -gt 0 ] || return 0
    total=$(size_of "$@")
    rm -rf -- "$@" &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        bar $((total - $(size_of "$@"))) "$total"
        sleep 0.5
    done
    wait $pid
}

# --- arguments and checks ---------------------------------------------------
PURGE=0
while [ $# -gt 0 ]; do
    case $1 in
        --purge) PURGE=1; shift ;;
        -h | --help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument: $1 (usage: uninstall.sh [--purge])" ;;
    esac
done

printf '\n%sAbleton Live 12 for Linux: uninstaller%s\n' "$BOLD" "$RESET"
printf '  Remove from   %s\n\n' "$PREFIX"

step "Checking the installation"
[ "$(id -u)" != 0 ] || die "run this as your normal user; it asks for sudo only for the loader link"
[ -d "$PREFIX/push3_opt" ] || die "no Ableton Live 12 installation in $PREFIX"
for p in /proc/[0-9]*; do
    exe=$(readlink "$p/exe" 2>/dev/null) || continue
    case $exe in "$PREFIX"/push3_opt/*) die "Live is still running; quit it first" ;; esac
done
CONFIG="$CONFIG_HOME/Ableton"
DATA="$DATA_HOME/Ableton"
MAX=("$CONFIG_HOME/Cycling '74" "$DATA_HOME/Max 9") # Max for Live's
if [ -d "$PREFIX/live-home" ]; then
    SETTINGS="$PREFIX/live-home (settings, licence, User Library)"
    CONFIG="" DATA="" MAX=() # all inside live-home
else
    SETTINGS="${CONFIG/#"$HOME"/\~} and ${DATA/#"$HOME"/\~} (settings, licence, file database, Max for Live's settings)"
fi
ok "$(human "$(size_of "$PREFIX")") in ${PREFIX/#"$HOME"/\~}"

# Both questions first, so the rest runs without stopping.
[ $TTY = 0 ] || printf '\e[?25h'
if [ $PURGE = 0 ]; then
    note "Live's settings and licence: $SETTINGS"
    if ask "Delete them too? [y/N]" n; then PURGE=1; fi
fi
UNLINK=0
if [ "$LOADER_LINK" = 1 ] && [ "$(readlink /lib/ld-linux-x86-64.so.2 2>/dev/null)" = /lib64/ld-linux-x86-64.so.2 ]; then
    note "The installer created the link /lib/ld-linux-x86-64.so.2 (other programs may use it now)."
    if ask "Remove it with sudo? [Y/n]" y; then UNLINK=1; fi
fi
cleanup() { [ $TTY = 0 ] || printf '\e[?25h'; }
trap cleanup EXIT
trap 'exit 130' INT TERM
trap 'exit 129' HUP # the terminal window closed
[ $TTY = 0 ] || printf '\e[?25l' # no cursor blinking over the bars

# --- removing ---------------------------------------------------------------------
step "Removing the Core Library"
case $CORE in
    "$PREFIX"/*) ok "inside the install folder" ;;
    *)
        if [ -d "$CORE" ]; then
            freed=$(size_of "$CORE")
            remove "$CORE" || die "could not delete $CORE"
            ok "$(human "$freed") from ${CORE/#"$HOME"/\~}"
        else
            ok "already gone"
        fi
        ;;
esac

step "Removing the menu entry and file types"
# Before the program: the KWin rule script is in its tools/.
[ ! -x "$PREFIX/tools/kwin-plugin-rule.sh" ] || "$PREFIX/tools/kwin-plugin-rule.sh" --remove >/dev/null 2>&1 || true
rm -f "$DESKTOP" "$MIME_XML"
command -v update-mime-database >/dev/null && update-mime-database "$DATA_HOME/mime" >/dev/null 2>&1 || true
command -v update-desktop-database >/dev/null && update-desktop-database "$(dirname "$DESKTOP")" >/dev/null 2>&1 || true
sed -i '/^x-scheme-handler\/ableton=ableton-live-12\.desktop;\{0,1\}$/d' "$CONFIG_HOME/mimeapps.list" 2>/dev/null || true
ok ".als and .alp files, ableton:// links"

step "Removing the program"
freed=$(size_of "$PREFIX")
if [ $PURGE = 1 ]; then
    remove "$PREFIX" || die "could not delete $PREFIX"
else
    # An isolated home from an older installation (live-home/) is Live data: kept.
    mapfile -d '' parts < <(find "$PREFIX" -mindepth 1 -maxdepth 1 ! -name live-home -print0)
    remove "${parts[@]}" || die "could not delete $PREFIX"
    rmdir "$PREFIX" 2>/dev/null || true
fi
ok "$(human "$((freed - $(size_of "$PREFIX")))")"

step "Live's settings, licence and the program loader link"
done_what=()
if [ $PURGE = 1 ]; then
    if [ -n "$CONFIG" ]; then
        freed=$(size_of "$CONFIG" "$DATA" "${MAX[@]}")
        remove "$CONFIG" "$DATA" "${MAX[@]}" || die "could not delete Live's settings"
        done_what+=("settings and licence deleted ($(human "$freed"))")
    else
        done_what+=("settings and licence deleted")
    fi
else
    done_what+=("settings and licence kept")
fi
if [ $UNLINK = 1 ]; then
    [ $TTY = 0 ] || printf '\e[?25h'
    if sudo rm /lib/ld-linux-x86-64.so.2; then
        done_what+=("loader link removed")
    else
        note "The link stays; remove it later with: sudo rm /lib/ld-linux-x86-64.so.2"
    fi
fi
ok "$(IFS=,; echo "${done_what[*]}" | sed 's/,/, /g')"

printf '\n%s%sDone.%s Ableton Live 12 is uninstalled.\n' "$GREEN" "$BOLD" "$RESET"
[ ! -d "$PREFIX/live-home" ] || printf 'Your Live data stays in %s\n' "$PREFIX/live-home"
LIBRARY=$(dirname "$CORE")
case $LIBRARY in
    "$PREFIX"/*) ;;
    *) [ ! -d "$LIBRARY" ] || printf 'Your User Library, recordings and projects stay in %s\n' "$LIBRARY" ;;
esac
EOF
} > "$PREFIX/uninstall.sh.part"
chmod +x "$PREFIX/uninstall.sh.part"
# The installation counts as complete from here (see the checks at the start).
mv "$PREFIX/uninstall.sh.part" "$PREFIX/uninstall.sh"
rm -f "$PREFIX/.install-program" "$PREFIX/.install-content"
ok "$PREFIX/uninstall.sh"

if [ ! -e /usr/share/alsa/alsa.conf.d/50-pipewire.conf ] && [ ! -e /etc/alsa/conf.d/50-pipewire.conf ]; then
    printf '\n'
    note "pipewire-alsa seems missing; without it Live takes the sound card from the desktop. Install it with:"
    note "  $(deps_hint)"
fi
printf '\n%s%sDone.%s Start Ableton Live 12 from the application menu, or run:\n' "$GREEN" "$BOLD" "$RESET"
printf '  %s\n' "$APP/AbletonLive12"
printf 'Uninstall: %s\n' "$PREFIX/uninstall.sh"
