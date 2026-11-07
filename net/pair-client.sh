#!/bin/sh
# The joining machine's side of the two-machine network test (docs/multiplayer.md "Stage 1b"), headless: joins the
# host at HOST twice, playing harness/net/ml2_01_two_players.txt as player 1 (Plymouth): a game to TICKS that saves at
# tick SAVE_AT, then the same session resumed from this machine's own save to TICKS. Run from the repository root
# after building op2-engine (debug), e.g.:
#   sh harness/net/pair-client.sh HOST_ADDRESS tools/target/net-test 4000 1500
# while the host runs `op2-engine --host --mission ml2_01.dll --script harness/net/ml2_01_two_players.txt
# --until-tick TICKS --net-save-at SAVE_AT`, then `--host --load <its save> ...`. Each join waits 20 s first, for the
# host to be up. Prints the network log lines and the final state hashes.
set -u
HOST=$1 OUT=$2 TICKS=$3 SAVE_AT=$4
ENGINE=tools/target/debug/op2-engine
run() {
    dir=$1
    shift
    rm -rf "$dir"
    mkdir -p "$dir/cfg" "$dir/saves" "$dir/data"
    sleep 20
    timeout 1200 xvfb-run -a "$ENGINE" --join "$HOST" --name SteamMachine --colony plymouth --script harness/net/ml2_01_two_players.txt \
        --until-tick "$TICKS" --config-dir "$dir/cfg" --save-dir "$dir/saves" --data-dir "$dir/data" --net-log "$dir/session.o2rp" "$@" \
        > "$dir/client.log" 2>&1
    echo "== $dir: exit $?"
    grep -E "lobby|net:|state hash|rror|script:" "$dir/client.log" | grep -v XSETTINGS | sed 's/^.*Z[^a-z]*//' | tail -30
}
run "$OUT/first" --net-save-at "$SAVE_AT"
cp "$OUT/first/saves/quicksave.op2rs" "$OUT/save.op2rs" || exit 1
run "$OUT/resumed" --load "$OUT/save.op2rs"
