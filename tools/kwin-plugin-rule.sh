#!/bin/sh
# Adds a KWin window rule for Live's plug-in editor windows (WM_CLASS instance
# "ableton-live-plugin", set by shim/window.cpp): "Minimized: Force No", which
# makes KWin treat them as not minimizable, so they get no minimize button.
# Other window managers (Mutter, Xfwm, Openbox, ...) follow the Motif
# "no minimize" hint that shim/window.cpp sets; KWin ignores it, so on KDE a
# rule is the only way.
# Runs only in a KDE session (Plasma 5 or 6); does nothing when the rule
# already exists.
# Remove with: tools/kwin-plugin-rule.sh --remove
set -eu
case ":${XDG_CURRENT_DESKTOP:-}:" in *:KDE:*) ;; *) exit 0 ;; esac
if command -v kwriteconfig6 >/dev/null 2>&1 && command -v kreadconfig6 >/dev/null 2>&1; then
    v=6
elif command -v kwriteconfig5 >/dev/null 2>&1 && command -v kreadconfig5 >/dev/null 2>&1; then
    v=5
else
    exit 0
fi
RC=kwinrulesrc
ID=ableton-live-plugin-editors
read_general() { "kreadconfig$v" --file "$RC" --group General --key "$1"; }
write() { "kwriteconfig$v" --file "$RC" "$@"; }

# The rule list: [General] Order on KWin 6; [General] rules plus count on
# KWin 5 (KWin 6 still reads those). Keep whichever the file already uses.
if [ -n "$(read_general Order)" ]; then
    key=Order
elif [ -n "$(read_general rules)" ] || [ "$v" = 5 ]; then
    key=rules
else
    key=Order
fi
list=$(read_general "$key")

set_list() {
    write --group General --key "$key" "$1"
    if [ "$key" = rules ]; then
        write --group General --key count "$(printf '%s' "$1" | tr ',' '\n' | grep -c .)" || true
    fi
}

if [ "${1:-}" = --remove ]; then
    set_list "$(printf '%s' "$list" | tr ',' '\n' | grep -vx "$ID" | paste -sd, -)"
    for k in Description wmclass wmclasscomplete wmclassmatch minimize minimizerule; do
        write --group "$ID" --key "$k" --delete
    done
else
    case ",$list," in *",$ID,"*) exit 0 ;; esac
    write --group "$ID" --key Description "Ableton Live plug-in editors: no minimize"
    write --group "$ID" --key wmclass '^ableton-live-plugin\s'
    write --group "$ID" --key wmclasscomplete --type bool true
    write --group "$ID" --key wmclassmatch 3 # regular expression
    write --group "$ID" --key minimize --type bool false
    write --group "$ID" --key minimizerule 2 # force
    set_list "${list:+$list,}$ID"
fi
dbus-send --session --type=method_call --dest=org.kde.KWin /KWin org.kde.KWin.reconfigure >/dev/null 2>&1 || true
