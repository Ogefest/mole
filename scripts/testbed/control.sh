#!/usr/bin/env bash
#
# The control channel: how a test interferes with the server while it is
# working.
#
# The difference between a server and a piece of test equipment is whether a
# test can do something to it mid-transfer. Reading a file from a machine that
# is behaving is a small part of what a file manager has to survive; the rest is
# what happens when the server restarts, the connection dies, the disk fills or
# the network goes slow, and none of that can be asserted without being able to
# cause it.
#
# Two halves:
#
#   install  puts `mole-control` on the machine
#   <command> runs one, over ssh, and prints what it did
#
# Absent by default. Nothing reaches for this unless MOLE_TEST_CONTROL names it,
# so a suite running on a developer's machine cannot start stopping services on
# anything.
#
# Usage:
#   MOLE_TESTBED_ADDRESS=… scripts/testbed/control.sh install
#   MOLE_TESTBED_ADDRESS=… scripts/testbed/control.sh fill 95
#   MOLE_TESTBED_ADDRESS=… scripts/testbed/control.sh netem loss 30%
#   MOLE_TESTBED_ADDRESS=… scripts/testbed/control.sh restore
#
set -uo pipefail

ADDRESS="${MOLE_TESTBED_ADDRESS:-}"
ACCOUNT="${MOLE_TESTBED_ACCOUNT:-moletest}"

[ -n "$ADDRESS" ] || { echo "Set MOLE_TESTBED_ADDRESS." >&2; exit 2; }
[ $# -ge 1 ] || { echo "Usage: control.sh install | <command> [argument…]" >&2; exit 2; }

on_server() { ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new "$ACCOUNT@$ADDRESS" "$@"; }

if [ "$1" = "install" ]; then
    on_server "sudo tee /usr/local/bin/mole-control >/dev/null" <<'CONTROL'
#!/usr/bin/env bash
#
# Does something to this machine on purpose, and says what it did.
#
# Every command prints one line describing what happened, because a test that
# fails after interfering has to be able to say what it interfered with. A
# failure whose cause is "something was done to the server" and nothing more is
# a failure nobody can act on.
#
set -uo pipefail

DATA=/srv/moledata
BALLAST="$DATA/.mole-ballast"
IFACE="$(ip -o -4 route show default | awk '{print $5}' | head -1)"

say() { printf 'mole-control: %s\n' "$*"; }

usage() {
    cat <<'USAGE'
mole-control <command>

  service stop|start|restart <unit>   a server goes away and comes back
  kill-connections <port>             established connections die; the server lives
  fill <percent>                      the small disk fills to about this much
  empty                               and empties again
  netem delay <ms>|loss <pc>|rate <bits> [seconds]   clears itself after
                                      `seconds` (30 by default), because this
                                      channel travels over the link it damages
  netem clear                         now
  room <sftp|s3|webdav|ftp>           bytes free where that service keeps its
                                      files, so a test can decline to fill a
                                      disk it would take the machine down with
  status                              what is currently being done to this machine
  restore                             undo everything: no ballast, no netem, all up
USAGE
}

case "${1:-}" in
service)
    action="${2:-}"; unit="${3:-}"
    [ -n "$action" ] && [ -n "$unit" ] || { usage; exit 2; }
    systemctl "$action" "$unit"
    # vsftpd does not set SO_REUSEADDR, so a start treading on the previous
    # instance's port exits 2 in silence. Waiting is cheaper than debugging
    # that a second time.
    if [ "$action" != "stop" ]; then
        for _ in $(seq 1 20); do
            systemctl is-active --quiet "$unit" && break
            sleep 1
        done
    fi
    say "$action $unit -> $(systemctl is-active "$unit")"
    ;;

kill-connections)
    port="${2:-}"
    [ -n "$port" ] || { usage; exit 2; }
    # The connections, not the server. This is the case a transfer has to
    # survive or report honestly: the far end is still there and the socket is
    # not.
    before=$(ss -tn state established "( sport = :$port )" | tail -n +2 | wc -l)
    ss -K state established "( sport = :$port )" >/dev/null 2>&1
    say "killed $before established connection(s) on port $port; $(systemctl is-active sshd 2>/dev/null || echo server) still up"
    ;;

fill)
    percent="${2:-95}"
    total=$(df --output=size -k "$DATA" | tail -1)
    used=$(df --output=used -k "$DATA" | tail -1)
    want=$(( total * percent / 100 ))
    need=$(( want - used ))
    if [ "$need" -le 0 ]; then
        say "already at or above $percent% of $DATA"
        exit 0
    fi
    # fallocate, so filling four gigabytes is instant rather than a minute of
    # writing zeroes -- a test that has to wait for the disk to fill is a test
    # nobody runs.
    fallocate -l "${need}K" "$BALLAST" 2>/dev/null || dd if=/dev/zero of="$BALLAST" bs=1K count="$need" status=none
    say "filled $DATA to $(df --output=pcent "$DATA" | tail -1 | tr -d ' %')%"
    ;;

empty)
    rm -f "$BALLAST"
    say "emptied $DATA, now $(df --output=pcent "$DATA" | tail -1 | tr -d ' %')% used"
    ;;

netem)
    what="${2:-}"
    # Every netem is temporary, and that is not tidiness.
    #
    # This channel reaches the machine over the network it is being asked to
    # damage. `netem loss 100%` therefore cuts off the command that would undo
    # it: the machine is unreachable, including by whoever is trying to put it
    # back, and only a reboot from the hypervisor recovers it. Ask me how I
    # know. So an unattended timer removes it, and a test that dies mid-run
    # leaves a machine that heals itself.
    seconds="${4:-30}"
    tc qdisc del dev "$IFACE" root 2>/dev/null
    case "$what" in
    delay) tc qdisc add dev "$IFACE" root netem delay "${3:-200ms}"; say "netem delay ${3:-200ms} on $IFACE" ;;
    loss)  tc qdisc add dev "$IFACE" root netem loss "${3:-10%}";    say "netem loss ${3:-10%} on $IFACE" ;;
    rate)  tc qdisc add dev "$IFACE" root tbf rate "${3:-1mbit}" burst 32kbit latency 400ms
           say "rate limited to ${3:-1mbit} on $IFACE" ;;
    clear) say "netem cleared on $IFACE"; exit 0 ;;
    *)     usage; exit 2 ;;
    esac
    setsid bash -c "sleep $seconds; tc qdisc del dev $IFACE root 2>/dev/null" >/dev/null 2>&1 &
    say "it clears itself in ${seconds}s, because this channel travels over the link it just damaged"
    ;;

room)
    # The machine is the only thing that knows its own layout, and a suite that
    # asked for ten gigabytes on the four-gigabyte disk would take every other
    # suite down with it. So the question is asked here rather than answered by
    # a number typed into a script somewhere else.
    case "${2:-}" in
    sftp)   path=$(getent passwd "$(stat -c %U "$DATA")" | cut -d: -f6)/sftp ;;
    s3)     path=/var/lib/minio ;;
    webdav) path="$DATA/webdav" ;;
    ftp)    path="$DATA/ftp" ;;
    *)      usage; exit 2 ;;
    esac
    [ -d "$path" ] || path="$(dirname "$path")"
    df --output=avail -B1 "$path" | tail -1 | tr -d ' '
    ;;

status)
    say "disk $(df --output=pcent "$DATA" | tail -1 | tr -d ' ') used, ballast $([ -f "$BALLAST" ] && echo present || echo absent)"
    say "netem $(tc qdisc show dev "$IFACE" | head -1)"
    for unit in ssh sshd-rekey vsftpd apache2 minio; do
        printf 'mole-control:   %-12s %s\n' "$unit" "$(systemctl is-active "$unit" 2>/dev/null || echo unknown)"
    done
    ;;

restore)
    rm -f "$BALLAST"
    tc qdisc del dev "$IFACE" root 2>/dev/null
    for unit in ssh sshd-rekey vsftpd apache2 minio; do
        systemctl is-active --quiet "$unit" || systemctl start "$unit" 2>/dev/null
    done
    say "restored: no ballast, no netem, every server up"
    ;;

*)
    usage
    exit 2
    ;;
esac
CONTROL

    on_server "sudo chmod +x /usr/local/bin/mole-control"
    # Passwordless for this one command only. The account already has full sudo
    # on a disposable machine, so this changes nothing about what is possible --
    # it makes the channel usable without a terminal to type into.
    on_server "sudo tee /etc/sudoers.d/mole-control >/dev/null" <<SUDOERS
$ACCOUNT ALL=(root) NOPASSWD: /usr/local/bin/mole-control
SUDOERS
    on_server "sudo chmod 0440 /etc/sudoers.d/mole-control"
    on_server "sudo /usr/local/bin/mole-control status"
    printf '\nInstalled. Point the suite at it with:\n\n'
    printf "  export MOLE_TEST_CONTROL='ssh -o BatchMode=yes %s@%s sudo mole-control'\n\n" "$ACCOUNT" "$ADDRESS"
    exit 0
fi

on_server "sudo /usr/local/bin/mole-control $(printf '%q ' "$@")"
