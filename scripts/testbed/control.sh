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
# The sshd this channel arrives over, and the one no test may attack -- see
# ADR-0054. services.sh puts it there; a machine provisioned before it existed
# has only port 22, which is what the fallback below is for.
CONTROL_PORT="${MOLE_TESTBED_CONTROL_PORT:-2022}"

[ -n "$ADDRESS" ] || { echo "Set MOLE_TESTBED_ADDRESS." >&2; exit 2; }
[ $# -ge 1 ] || { echo "Usage: control.sh install | <command> [argument…]" >&2; exit 2; }

# The control port when it answers, port 22 when it does not. Falling back
# matters exactly once -- installing on a machine that has no control sshd yet --
# and is announced rather than silent, because a channel that quietly moved onto
# the port the tier blackholes is the fault this whole file is about.
PORT="$CONTROL_PORT"
if ! ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 \
        -p "$CONTROL_PORT" "$ACCOUNT@$ADDRESS" true 2>/dev/null; then
    PORT=22
    echo "control.sh: nothing answers on port $CONTROL_PORT; using port 22." >&2
    echo "control.sh: run services.sh to put the control sshd there -- until then" >&2
    echo "control.sh: an outage on port 22 cuts this channel with the transfer." >&2
fi

on_server() {
    ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -p "$PORT" "$ACCOUNT@$ADDRESS" "$@"
}

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

# The port this channel arrives over, read from the server that serves it rather
# than typed in twice. Everything below refuses to damage it: an instrument that
# can cut off the machine it has to put back is not one anybody can leave
# running. See ADR-0054.
CONTROL_PORT="$(awk '/^Port /{print $2}' /etc/ssh/sshd_config.control 2>/dev/null | head -1)"
CONTROL_UNIT=sshd-control

say() { printf 'mole-control: %s\n' "$*"; }

# Says no, and says why. Used where a command would take away the path back.
refuse() { printf 'mole-control: refusing: %s\n' "$*" >&2; exit 3; }

# Schedules the undo, with systemd rather than a detached `sleep`.
#
# It used to write a small script and start it with `setsid ... &`, and that
# stopped surviving the ssh session that started it: a blackhole asked to clear
# itself after thirty seconds was measured still standing after ninety, with no
# clearer process left on the machine. An outage then lasted until somebody
# noticed, and every test written around "the outage is N seconds" was measuring
# something else.
#
# AccuracySec is not a detail either. A transient timer defaults to a minute of
# slack, so a thirty-second outage could last ninety on its own.
schedule_undo() {
    local seconds="$1"
    shift
    systemctl stop mole-netem-clear.timer 2>/dev/null || true
    systemctl stop mole-netem-clear.service 2>/dev/null || true
    systemctl reset-failed mole-netem-clear.service 2>/dev/null || true
    if ! systemd-run --collect --unit=mole-netem-clear --on-active="${seconds}s" \
            --timer-property=AccuracySec=1s /bin/bash -c "$*" >/dev/null 2>&1; then
        # Said out loud rather than swallowed. An undo that was never scheduled
        # is a machine left damaged, which is worth more than a tidy line.
        say "WARNING: could not schedule the undo -- put it back by hand"
        return 1
    fi
}

usage() {
    cat <<'USAGE'
mole-control <command>

  service stop|start|restart <unit>   a server goes away and comes back
  kill-connections <port>             established connections die; the server lives
  fill <percent>                      the small disk fills to about this much
  empty                               and empties again
  blackhole <port> [seconds]          every packet leaving that port is dropped,
                                      and nothing else is touched -- so a
                                      transfer stalls dead while this channel,
                                      on its own port, still answers. Naming
                                      that port is refused.
  netem delay <ms>|loss <pc>|rate <bits> [seconds]   clears itself after
                                      `seconds` (30 by default), because this
                                      channel travels over the link it damages
  netem clear                         now
  hostkey rotate|restore              the second sshd gets a new identity, and
                                      gets its old one back. Never the first one,
                                      which every transfer in the tier uses, and
                                      never this channel's own
  many-files <count>                  a directory with that many empty entries
                                      under the SFTP root, and its path printed.
                                      Made here because a hundred thousand
                                      creations over SFTP is a hundred thousand
                                      round trips. `no-files` removes it.
  no-files                            and takes it away again
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
    # Stopping the server this command arrived over would end the command and
    # leave nothing able to start it again.
    if [ "$unit" = "$CONTROL_UNIT" ]; then
        refuse "$CONTROL_UNIT is the server this channel arrives over"
    fi
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

blackhole)
    # A total outage for one port, rather than for the machine.
    #
    # `netem loss 100%` on the root qdisc was the obvious way to cut a transfer
    # off, and it cuts ARP with it: the machine stops answering anything at all,
    # including the command that would put it back, and the only way in is the
    # hypervisor's guest agent. Ask me how I know -- twice now.
    #
    # This drops only packets leaving the port named, which is the transfer's,
    # while this channel arrives on a different one and keeps working. The
    # stall is identical from the transfer's point of view: no bytes, no error,
    # nothing.
    port="${2:-22}"
    seconds="${3:-30}"
    # The one port this may not have. Dropping what leaves it would cut this
    # channel along with the transfer, which is exactly the fault the per-port
    # instrument exists to avoid -- and it would do it silently, because the
    # blackhole is applied before anything notices it is unreachable.
    if [ -n "$CONTROL_PORT" ] && [ "$port" = "$CONTROL_PORT" ]; then
        refuse "port $port is the control channel's own"
    fi
    # Four bands with a priomap that sends *everything* to the first one, so the
    # only way into the band that drops packets is the filter below. A default
    # priomap routes by TOS, and ssh sets TOS -- which put this channel in the
    # dropping band along with the transfer, and cut the machine off exactly as
    # the whole-interface version did.
    # A rate limit already in force is put back underneath, not thrown away.
    #
    # This used to replace the root qdisc outright, which deleted the
    # `netem rate` both outage tests apply seconds beforehand -- and its clearer
    # then left the interface with no qdisc at all. From the moment the outage
    # started the link was at full speed and stayed there, so those tests were
    # written on arithmetic ("the payload outlasts the outage") that had stopped
    # being true. An instrument that quietly undoes the other half of the setup
    # measures something nobody asked about.
    rate="$(cat /run/mole-rate 2>/dev/null || true)"

    tc qdisc del dev "$IFACE" root 2>/dev/null
    tc qdisc add dev "$IFACE" root handle 1: prio bands 4 \
        priomap 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
    # `if`, not `[ ... ] &&`. This script runs under `set -e`, where a test that
    # comes out false is a command that failed -- so an absent rate limit killed
    # the script here, silently, before it could schedule the undo. That is what
    # "the blackhole never cleared" actually was.
    if [ -n "$rate" ]; then
        tc qdisc add dev "$IFACE" parent 1:1 handle 10: tbf rate "$rate" burst 32kbit latency 400ms
    fi
    tc qdisc add dev "$IFACE" parent 1:4 handle 40: netem loss 100%
    tc filter add dev "$IFACE" protocol ip parent 1:0 prio 4 u32 \
        match ip sport "$port" 0xffff flowid 1:4
    say "everything leaving port $port is dropped on $IFACE"
    if [ -n "$rate" ]; then
        say "the $rate limit stays in force underneath it"
    fi

    # Back to what was in force before the outage rather than to no qdisc at all.
    schedule_undo "$seconds" "tc qdisc del dev $IFACE root 2>/dev/null; \
        rate=\$(cat /run/mole-rate 2>/dev/null || true); \
        if [ -n \"\$rate\" ]; then tc qdisc add dev $IFACE root tbf rate \"\$rate\" burst 32kbit latency 400ms; fi"
    say "it clears itself in ${seconds}s, scheduled with systemd"
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
           # Recorded so a blackhole can put it back underneath itself rather
           # than replacing it -- see the note there.
           printf '%s' "${3:-1mbit}" > /run/mole-rate
           say "rate limited to ${3:-1mbit} on $IFACE" ;;
    clear) rm -f /run/mole-rate; say "netem cleared on $IFACE"; exit 0 ;;
    *)     usage; exit 2 ;;
    esac
    # Scheduled with systemd, and a new one replaces the old by name.
    #
    # It used to be an anonymous `sleep N; tc qdisc del`, which deletes whatever
    # qdisc it finds when it wakes up rather than the one it was scheduled for --
    # so two tests in a row interfered with each other. Then it was a named
    # script started with setsid, which stopped surviving the ssh session at all.
    # A transient timer is owned by init and cancels by name.
    schedule_undo "$seconds" "tc qdisc del dev $IFACE root 2>/dev/null; rm -f /run/mole-rate; true"
    say "it clears itself in ${seconds}s, because this channel travels over the link it just damaged"
    ;;

hostkey)
    # A changed host key is the one SSH warning nobody may wave through, so a
    # test has to be able to cause it. Only ever on the second server, and the
    # reason changed with ADR-0054: this channel no longer arrives over the
    # first, but every other case in the interference tier transfers over it, and
    # a server whose identity moved under them would have them all failing for
    # something that is not the product. The second server is the one nothing
    # else is using.
    dir=/etc/ssh/rekey
    case "${2:-}" in
    rotate)
        [ -f "$dir/ssh_host_ed25519_key.original" ] \
            || cp "$dir/ssh_host_ed25519_key" "$dir/ssh_host_ed25519_key.original"
        [ -f "$dir/ssh_host_ed25519_key.original.pub" ] \
            || cp "$dir/ssh_host_ed25519_key.pub" "$dir/ssh_host_ed25519_key.original.pub"
        rm -f "$dir/ssh_host_ed25519_key" "$dir/ssh_host_ed25519_key.pub"
        ssh-keygen -q -t ed25519 -N '' -f "$dir/ssh_host_ed25519_key"
        systemctl restart sshd-rekey
        say "the second sshd has a new host key"
        ;;
    restore)
        if [ -f "$dir/ssh_host_ed25519_key.original" ]; then
            mv "$dir/ssh_host_ed25519_key.original" "$dir/ssh_host_ed25519_key"
            mv "$dir/ssh_host_ed25519_key.original.pub" "$dir/ssh_host_ed25519_key.pub"
            systemctl restart sshd-rekey
        fi
        say "the second sshd has its own host key back"
        ;;
    *) usage; exit 2 ;;
    esac
    ;;

many-files)
    # A directory big enough to be a question rather than a formality.
    #
    # Whether a listing paginates, what it costs in memory and what the progress
    # reading does with a hundred thousand entries are all properties of the
    # backend -- but making the directory through the backend would be a hundred
    # thousand round trips, which is a test nobody would wait for. The machine
    # makes it in one command instead, and the test does the part that is
    # actually under examination.
    count="${2:-100000}"
    case "$count" in
    ''|*[!0-9]*) refuse "many-files takes a count" ;;
    esac
    home=$(getent passwd "$(stat -c %U "$DATA")" | cut -d: -f6)
    dir="$home/sftp/mole-many-files"
    rm -rf "$dir"
    mkdir -p "$dir"
    # In batches, and with the shell doing the naming: one touch per file would
    # be a hundred thousand processes.
    seq 1 "$count" | sed "s|^|$dir/entry-|; s|\$|.txt|" | xargs -r -n 2000 touch
    chown -R "$(stat -c %U "$DATA")" "$dir"
    made=$(find "$dir" -maxdepth 1 -type f | wc -l)
    [ "$made" = "$count" ] || refuse "asked for $count entries and made $made"
    say "$dir has $made entries"
    printf '%s\n' "$dir"
    ;;

no-files)
    home=$(getent passwd "$(stat -c %U "$DATA")" | cut -d: -f6)
    rm -rf "$home/sftp/mole-many-files"
    say "the many-files directory is gone"
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
    for unit in ssh sshd-rekey sshd-control vsftpd apache2 minio; do
        printf 'mole-control:   %-12s %s\n' "$unit" "$(systemctl is-active "$unit" 2>/dev/null || echo unknown)"
    done
    if [ -n "$CONTROL_PORT" ]; then
        say "control channel on port $CONTROL_PORT, which nothing here may damage"
    else
        # Said plainly rather than softened. On a machine with no control sshd
        # this command arrives over port 22, which `blackhole 22` takes away.
        say "WARNING: no control sshd; this channel is on port 22, which blackhole can cut"
    fi
    ;;

restore)
    pkill -f /run/mole-netem-clear 2>/dev/null
    # The recorded rate goes too, or the next blackhole would put back a limit
    # nobody asked for.
    rm -f /run/mole-rate
    rm -f "$BALLAST"
    home=$(getent passwd "$(stat -c %U "$DATA")" | cut -d: -f6)
    rm -rf "$home/sftp/mole-many-files"
    if [ -f /etc/ssh/rekey/ssh_host_ed25519_key.original ]; then
        mv /etc/ssh/rekey/ssh_host_ed25519_key.original /etc/ssh/rekey/ssh_host_ed25519_key
        mv /etc/ssh/rekey/ssh_host_ed25519_key.original.pub /etc/ssh/rekey/ssh_host_ed25519_key.pub
        systemctl restart sshd-rekey 2>/dev/null
    fi
    tc qdisc del dev "$IFACE" root 2>/dev/null
    for unit in ssh sshd-rekey sshd-control vsftpd apache2 minio; do
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
    printf "  export MOLE_TEST_CONTROL='ssh -o BatchMode=yes -p %s %s@%s sudo mole-control'\n\n" \
        "$PORT" "$ACCOUNT" "$ADDRESS"
    exit 0
fi

on_server "sudo /usr/local/bin/mole-control $(printf '%q ' "$@")"
