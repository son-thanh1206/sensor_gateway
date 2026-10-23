#!/bin/bash
# soak.sh - integration test of assignment Step 7: run the gateway under
# valgrind with many sensor nodes for a while, stop it with SIGINT and check
# that it shut down cleanly.
#
#   test/soak.sh [seconds]          (default 180, run from the project root)
#   HELGRIND=1 test/soak.sh 30      also run a shorter round under helgrind
#
# Environment: SOAK_PORT (default 24000) is the TCP base port P.

set -u

DURATION=${1:-180}
PORT=${SOAK_PORT:-24000}
ROOT=$(pwd)
OUT=$ROOT/test/out/soak
N_EXTRA=100                     # extra temperature nodes, IDs 100.. (not in the map)

[ -f Makefile ] && [ -f room_sensor.map ] || { echo "run from the project root"; exit 2; }

failures=0
check() {                       # check <description> <command...>
    local desc=$1; shift
    if "$@"; then echo "  PASS  $desc"; else echo "  FAIL  $desc"; failures=$((failures + 1)); fi
}

wait_for_log() {                # wait_for_log <dir> <pattern> <seconds>
    for _ in $(seq $(( $3 * 10 ))); do
        grep -q "$2" "$1/gateway.log" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

# start_server <dir> <port> <wrapper...>: runs bin/server in <dir>, sets SRV.
start_server() {
    local dir=$1 port=$2; shift 2
    rm -rf "$dir"; mkdir -p "$dir"
    cp "$ROOT/room_sensor.map" "$dir/"
    ( cd "$dir" && exec "$@" "$ROOT/bin/server" "$port" $((port + 100)) 2> server.err ) &
    SRV=$!
    if ! wait_for_log "$dir" "Connection manager started" 60; then
        echo "server did not start, see $dir/server.err"; kill "$SRV" 2>/dev/null; exit 1
    fi
}

start_extra_nodes() {           # start_extra_nodes <port> <count> <first id>
    EXTRA_PIDS=()
    for ((i = 0; i < $2; ++i)); do
        "$ROOT/bin/sensor_node" temperature $(($3 + i)) 127.0.0.1 "$1" 1000 >/dev/null 2>&1 &
        EXTRA_PIDS+=($!)
    done
}

stop_extra_nodes() {
    kill "${EXTRA_PIDS[@]}" 2>/dev/null
    wait "${EXTRA_PIDS[@]}" 2>/dev/null
}

stop_server() {                 # sets SRV_RC
    kill -INT "$SRV"
    wait "$SRV"; SRV_RC=$?
}

log_seq_continuous()  { awk '$1 != NR { exit 1 }' "$1/gateway.log"; }
log_format_ok() {
    ! grep -qvE '^[0-9]+ [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} (MAIN|CONN|DATA|STOR|WEB) .+$' \
        "$1/gateway.log"
}
not() { ! "$@"; }
count() { grep -c "$1" "$2" 2>/dev/null; }

# Every valgrind log (main + log process) must show no leak and no error.
valgrind_clean() {
    local f ok=0
    for f in "$1"/valgrind.*.txt; do
        grep -q "in use at exit: 0 bytes in 0 blocks" "$f" || { echo "    leak in $f"; ok=1; }
        grep -q "ERROR SUMMARY: 0 errors" "$f" || { echo "    errors in $f"; ok=1; }
    done
    return $ok
}

# --track-fds lists each descriptor still open at exit; the ones inherited
# from the shell are marked "<inherited from parent>" and are not ours.
no_fd_leak() {
    local f ok=0
    for f in "$1"/valgrind.*.txt; do
        if ! awk '/Open (file descriptor|AF_|pipe)/ { pending = $0; next }
                  pending != "" { if ($0 !~ /inherited from parent/) { print "    " pending; bad = 1 }
                                  pending = "" }
                  END { exit bad }' "$f"; then
            ok=1
        fi
    done
    return $ok
}

no_child_left() {
    local f pid
    for f in "$1"/valgrind.*.txt; do
        pid=${f##*valgrind.}; pid=${pid%.txt}
        kill -0 "$pid" 2>/dev/null && return 1
    done
    return 0
}

echo "== build (debug)"
make debug >/dev/null && make bin/test_sensors >/dev/null || { echo "build failed"; exit 1; }

# ---------------------------------------------------------------------------
echo "== soak: $((17 + N_EXTRA)) sensor nodes for ${DURATION}s under valgrind"
start_server "$OUT/memcheck" "$PORT" valgrind --leak-check=full --show-leak-kinds=all \
    --errors-for-leak-kinds=all --track-fds=yes --log-file=valgrind.%p.txt
PORT=$PORT "$ROOT/bin/test_sensors" test/sensors.csv "$DURATION" > "$OUT/test_sensors.out" 2>&1 &
SENSORS=$!
start_extra_nodes "$PORT" "$N_EXTRA" 100
wait "$SENSORS"
stop_extra_nodes
sleep 1
stop_server

D=$OUT/memcheck
opened=$(count "has opened a new connection" "$D/gateway.log")
closed=$(count "has closed the connection" "$D/gateway.log")
rows=$(sqlite3 "$D/gateway.db" "SELECT COUNT(*) FROM SensorData" 2>/dev/null || echo -1)
read_data=$(count "datamgr: id=" "$D/server.err")
read_stor=$(count "storagemgr: id=" "$D/server.err")
echo "  connections opened=$opened closed=$closed; frames datamgr=$read_data storagemgr=$read_stor rows=$rows"

check "server exit code 0 (got $SRV_RC)"              test "$SRV_RC" -eq 0
check "valgrind: no leak, no error"                   valgrind_clean "$D"
check "no descriptor left open at exit"               no_fd_leak "$D"
check "log process reaped"                            no_child_left "$D"
check "gateway.log seq continuous"                    log_seq_continuous "$D"
check "gateway.log lines match section 3.8 format"    log_format_ok "$D"
check "shutdown logged"                               grep -q "MAIN Gateway shutting down (received SIGINT)" "$D/gateway.log"
check "more than 64 connections ($opened)"            test "$opened" -ge $((17 + N_EXTRA))
check "every opened connection closed"                test "$opened" -eq "$closed"
check "no SQL connection lost"                        not grep -q "SQL server lost" "$D/gateway.log"
check "all frames stored before exit"                 test "$read_data" -gt 0 -a "$read_data" -eq "$read_stor" -a "$read_stor" -eq "$rows"

# ---------------------------------------------------------------------------
echo "== descriptor limit: 40 nodes with ulimit -n 32"
start_server "$OUT/fdlimit" $((PORT + 10)) bash -c 'ulimit -n 32 && exec "$0" "$@"'
start_extra_nodes $((PORT + 10)) 40 100
sleep 8
cpu=$(ps -o %cpu= -p "$SRV" | tr -d ' ' | cut -d. -f1)
stop_extra_nodes
sleep 1
stop_server

D=$OUT/fdlimit
opened=$(count "has opened a new connection" "$D/gateway.log")
refused=$(count "(too many connections)" "$D/gateway.log")
echo "  opened=$opened refused=$refused cpu=${cpu}%"
check "server exit code 0 (got $SRV_RC)"              test "$SRV_RC" -eq 0
check "some nodes served"                             test "$opened" -gt 0
check "extra nodes refused, each logged once"         test "$refused" -gt 0 -a $((opened + refused)) -le 40
check "server not busy-looping"                       test "${cpu:-100}" -lt 50
check "storage unaffected"                            not grep -q "SQL server lost" "$D/gateway.log"

# ---------------------------------------------------------------------------
if [ "${HELGRIND:-0}" = 1 ]; then
    echo "== helgrind: 30s"
    start_server "$OUT/helgrind" $((PORT + 20)) valgrind --tool=helgrind --log-file=valgrind.%p.txt
    PORT=$((PORT + 20)) "$ROOT/bin/test_sensors" test/sensors.csv 30 > /dev/null 2>&1
    sleep 1
    stop_server
    check "helgrind: no error" bash -c "grep -h 'ERROR SUMMARY' '$OUT'/helgrind/valgrind.*.txt | grep -qv ' 0 errors' && exit 1 || exit 0"
fi

echo
if [ "$failures" -eq 0 ]; then echo "soak: all checks passed"; else echo "soak: $failures check(s) failed"; fi
[ "$failures" -eq 0 ]
