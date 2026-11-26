#!/bin/bash
# The hub's image tested whole (#195), with docker, as CI does it:
#
#   packaging/container/test-hub.sh [--start-only] <image>
#
# A hub starts from <image> with the person's public key given. A computer, the
# same image's omabook in another container, links to it over ssh
# (`omabook hub add`) and syncs a library with it; the hub takes a moment
# snapshot before the sync that changes a book, and that book's details are
# restored from it on the hub and synced back. Along the way: the person's key
# and the computer's key run nothing but the hub's commands, the healthcheck's
# `omabook hub status` answers, and the hub runs as omabook, not root.
# --start-only stops after the hub answers (for an image of another
# architecture than the runner's tests in full).
#
# Everything it makes is named omabook-hub-test-<pid> and removed at the end.
set -euo pipefail

start_only=0
if [[ ${1:-} == --start-only ]]; then
    start_only=1
    shift
fi
image=${1:?usage: test-hub.sh [--start-only] <image>}
name=omabook-hub-test-$$
hub=$name-hub
failures=0

say() { echo "== $*"; }
fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}
cleanup() {
    if ((failures > 0)) || [[ ${keep_logs:-0} == 1 ]]; then
        echo "--- the hub's log" >&2
        docker logs "$hub" >&2 || true
    fi
    [[ ${keep_hub:-0} == 1 ]] && return
    docker rm -f "$hub" >/dev/null 2>&1 || true
    docker volume rm -f "$name-state" "$name-libraries" "$name-destinations" "$name-computer" >/dev/null 2>&1 || true
    docker network rm "$name" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker network create "$name" >/dev/null

# The computer: a shell in the image as root, its home a volume of its own, so
# it is no hub (the hub's state isn't its own) and keeps what it made between
# the steps.
computer() {
    docker run --rm -i --network "$name" --hostname ci-computer --user root --entrypoint bash -v "$name-computer:/root" \
        -e HOME=/root -e QT_QPA_PLATFORM=offscreen -e LANG=C.UTF-8 "$image" -c "$1"
}
# The hub's own user, in the hub's container.
on_hub() { docker exec -i "$hub" bash -c "$1"; }
# Until its daemon answers, as the healthcheck asks it.
wait_for_hub() {
    local _
    for _ in $(seq 60); do
        if on_hub 'omabook hub status' >/dev/null 2>&1; then
            return 0
        fi
        if [[ $(docker inspect -f '{{.State.Running}}' "$hub") != true ]]; then
            fail "the hub stopped"
            exit 1
        fi
        sleep 1
    done
    fail "the hub's daemon doesn't answer"
    exit 1
}

say "the person's ssh key, on the computer"
computer 'set -e
mkdir -p ~/.ssh && chmod 700 ~/.ssh
ssh-keygen -q -t ed25519 -N "" -C person -f ~/.ssh/id_ed25519
cat > ~/.ssh/config <<EOF
Host hub
    HostName hub
    Port 2222
    User omabook
    StrictHostKeyChecking accept-new
    BatchMode yes
EOF'
person_key=$(computer 'cat ~/.ssh/id_ed25519.pub')

say "the hub starts from $image"
docker run -d --name "$hub" --hostname omabook-hub --network "$name" --network-alias hub \
    -e OMABOOK_HUB_KEYS="$person_key" \
    -v "$name-state:/var/lib/omabook-hub" -v "$name-libraries:/libraries" -v "$name-destinations:/destinations" \
    "$image" >/dev/null
wait_for_hub
on_hub 'omabook hub status' || fail "the hub's status doesn't answer"
[[ $(on_hub 'id -un') == omabook ]] || fail "the hub doesn't run as omabook"
on_hub 'pgrep -u omabook -x sshd >/dev/null' || fail "sshd isn't running as omabook"
computer 'ssh-keyscan -p 2222 hub 2>/dev/null | grep -q ssh-ed25519' || fail "sshd doesn't answer on 2222"
if ((start_only)); then
    ((failures == 0)) && say "the hub started and answers"
    exit "$((failures > 0))"
fi

say "the person's key opens no shell"
if computer 'ssh hub id' 2>/dev/null; then fail "the person's key ran a command"; fi
if computer 'ssh -tt hub' </dev/null >/dev/null 2>&1; then fail "the person's key opened a terminal"; fi

say "nor does a key in authorized_keys without a forced command: the account's shell is the gate"
on_hub "mkdir -p ~/.ssh && printf '%s\n' '$person_key' > ~/.ssh/authorized_keys"
if computer 'ssh -o IdentitiesOnly=yes -i ~/.ssh/id_ed25519 hub id' 2>/dev/null | grep -q uid=; then
    fail "a bare key in authorized_keys opened a shell"
fi
on_hub 'rm ~/.ssh/authorized_keys'

say "snapshots set up on the hub, and a second disk for them; the hub started again takes them"
on_hub 'omabook snapshot setup --hub' | grep -q "The recovery key" || fail "snapshot setup --hub showed no recovery key"
on_hub 'omabook snapshot destination add disk /destinations/omabook --hub' >/dev/null || fail "no destination on /destinations"
host_key=$(on_hub 'cat ~/ssh/ssh_host_ed25519_key.pub')
docker restart "$hub" >/dev/null
wait_for_hub
[[ $(on_hub 'cat ~/ssh/ssh_host_ed25519_key.pub') == "$host_key" ]] || fail "the ssh host key changed as the hub started again"
on_hub 'omabook hub status' | grep -q "Snapshots aren't set up" && fail "the hub doesn't take the snapshots set up"

say "a library on the computer, its person, and the link to the hub"
computer 'set -e
mkdir -p /tmp/work
printf "Call me Ishmael.\n" > "/tmp/work/Sea Story - Ann Sailor.txt"
omabook add "/tmp/work/Sea Story - Ann Sailor.txt" >/dev/null
omabook identity >/dev/null
omabook hub add hub --name ci-computer'
on_hub 'omabook hub computers' | grep -q ci-computer || fail "the hub doesn't list the computer"
on_hub "grep -c 'restrict,command=' ~/.ssh/authorized_keys" | grep -qx 1 || fail "the computer's key isn't a forced command's"

say "the computer's key runs the session and nothing else"
if computer 'ssh -i ~/.local/share/omabook/hubs/key -o IdentitiesOnly=yes hub id' | grep -q uid=; then
    fail "the computer's key ran id"
fi

say "the first sync"
computer 'omabook hub sync' || fail "the first sync failed"
computer 'omabook hub status hub' | grep -q "1 books" || fail "the hub doesn't keep the book"

say "the book renamed, and synced: the hub takes a moment snapshot first"
computer 'omabook set 1 --title "Renamed Story" >/dev/null && omabook hub sync' || fail "the second sync failed"
computer 'omabook hub status hub' | grep -q "1 books" || fail "the hub lost the book"

say "the book's details restored on the hub from the snapshot before that sync"
# The newest snapshot, restic lists them oldest first.
snapshot=$(on_hub 'RESTIC_PASSWORD_FILE=~/secrets/snapshots restic -r ~/.local/share/omabook/snapshots snapshots --json' |
    grep -o '"short_id":"[0-9a-f]*"' | tail -1 | cut -d'"' -f4)
[[ -n $snapshot ]] || fail "the hub took no snapshot"
# Asked of the daemon by the hub's own session, as the hub's user.
diff=$(on_hub "omabook hub session" <<<"{\"request\":\"snapshot-diff\",\"library\":\"Books\",\"from\":\"$snapshot\"}")
grep -q '"before":"Sea Story"' <<<"$diff" || fail "the snapshot's diff doesn't show the rename: $diff"
book=$(grep -o '"id":"b:[0-9a-f]*"' <<<"$diff" | head -1 | cut -d'"' -f4)
restored=$(on_hub "omabook hub session" <<<"{\"request\":\"snapshot-restore\",\"library\":\"Books\",\"snapshot\":\"$snapshot\",\"book\":\"$book\",\"parts\":[\"details\"]}")
grep -q '"ok":true' <<<"$restored" || fail "the restore was refused: $restored"

say "synced back: the computer has the title again"
computer 'omabook hub sync >/dev/null && omabook list' | grep -q "Sea Story" || fail "the restored title didn't reach the computer"

say "a computer forgotten on the hub is no longer let in"
on_hub 'omabook hub forget ci-computer' >/dev/null || fail "the hub couldn't forget the computer"
if computer 'omabook hub status hub' >/dev/null 2>&1; then fail "a forgotten computer still reached the hub"; fi

if ((failures > 0)); then
    exit 1
fi
say "all is well"
