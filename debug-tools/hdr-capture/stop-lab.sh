#!/usr/bin/env bash
# Stop only recorded process groups belonging to this private runtime directory.
set -euo pipefail
lab=${1:?Usage: stop-lab.sh /tmp/hh.XXXXXX}
[[ $lab =~ ^/tmp/hh\.[[:alnum:]]{6}$ && -d $lab && ! -L $lab ]] || exit 1
[[ $(stat -c '%u' -- "$lab") == "$(id -u)" ]] || exit 1
for name in probe lock source child parent; do
    [[ -f $lab/$name.pid ]] || continue
    read -r pid < "$lab/$name.pid"
    [[ $pid =~ ^[1-9][0-9]*$ ]] || exit 1
    [[ -r /proc/$pid/environ ]] || continue
    grep -zFxq "XDG_RUNTIME_DIR=$lab" "/proc/$pid/environ" || { echo "Refusing unrelated PID $pid" >&2; exit 1; }
    [[ $(ps -p "$pid" -o pgid= | tr -d ' ') == "$pid" ]] || { echo "Refusing non-private process group $pid" >&2; exit 1; }
    kill -TERM -- "-$pid" 2>/dev/null || true
    for ((i=0; i<50; i++)); do
        kill -0 -- "-$pid" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 -- "-$pid" 2>/dev/null; then
        kill -KILL -- "-$pid" 2>/dev/null || true
    fi
done
