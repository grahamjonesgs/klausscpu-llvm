#!/usr/bin/env bash
# linux-std/library-tests.sh [WORKDIR]: Rust's own library test suites for
# std on Linux, built for klausscpu-unknown-linux-musl and run on the board:
#   coretests     core (2770 tests)
#   alloctests    alloc (unit tests 333, integration tests 1331, and
#                 vec_deque_alloc_error)
#   std/tests     std's integration tests (sync, thread, thread_local, env,
#                 process_spawning, pipe_subprocess, time, panic, error, ...;
#                 the Windows ones left out)
# at opt-level 2 with debug assertions and overflow checks, std included.
# They are copied from the patched rust checkout (KLAUSSCPU_RUST_SRC, default
# the one KLAUSSCPU_RUSTC was built in) into WORKDIR (default
# ~/.cache/klausscpu-rust-library-tests), built with klauss-linux-cargo and run
# one binary at a time with klauss-linux-run (which takes the board locks).
# Prints a line per binary and a summary; exits 1 if anything failed.
# About 50 minutes on the board, most of it alloctests' integration tests.
#   LT_ONLY   a space-separated subset of: coretests alloctests std
#   LT_TIMEOUT  seconds per test binary (default 2700; exit 124 when it hits)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RUSTDIR=$(cd "$HERE/.." && pwd)
[ -f "$RUSTDIR/.klaussbuild.env" ] && . "$RUSTDIR/.klaussbuild.env"
: "${KLAUSSCPU_RUSTC:?set KLAUSSCPU_RUSTC (README.md part 2)}"
SRC=${KLAUSSCPU_RUST_SRC:-$(cd "$(dirname "$KLAUSSCPU_RUSTC")/../../../.." && pwd)}
LIB=$SRC/library
[ -d "$LIB/coretests" ] || { echo "no rust library at $LIB (set KLAUSSCPU_RUST_SRC)" >&2; exit 2; }
W=${1:-$HOME/.cache/klausscpu-rust-library-tests}
ONLY=${LT_ONLY:-coretests alloctests std}
mkdir -p "$W"
cd "$W"
# A fresh copy every time; the target directory is kept, but a std built
# before a change to the libc fork can be stale (cargo -Zbuild-std does not
# always notice): delete $W/target after changing it.
rm -rf coretests alloctests stdtests alloc
cp -r "$LIB/coretests" "$LIB/alloctests" .
ln -s "$LIB/alloc" alloc             # alloctests' unit tests use ../alloc/src
mkdir -p stdtests
cp -r "$LIB/std/tests" stdtests/tests
# Inside the rust tree these build as part of std's package, which enables
# sync_poison_mod for them.
sed -i '1i #![feature(sync_poison_mod)]' stdtests/tests/sync/lib.rs
python3 - <<'EOF'
import os
skip = {'win_delete_self.rs', 'windows.rs', 'windows_unix_socket.rs'}
out = ['[package]', 'name = "stdtests"', 'version = "0.0.0"', 'edition = "2024"',
       'autotests = false', '', '[lib]', 'path = "lib.rs"', '',
       '[dev-dependencies]',
       'rand = { version = "0.9.0", default-features = false, features = ["alloc"] }',
       'rand_xorshift = "0.4.0"', '']
for f in sorted(os.listdir('stdtests/tests')):
    if f.endswith('.rs') and f not in skip:
        n = f[:-3]
        out += ['[[test]]', f'name = "{n}"', f'path = "tests/{f}"']
        out += ['harness = false'] if n == 'pipe_subprocess' else []
        out += ['']
for d in ('sync', 'thread_local'):
    out += ['[[test]]', f'name = "{d}"', f'path = "tests/{d}/lib.rs"', '']
open('stdtests/Cargo.toml', 'w').write('\n'.join(out))
open('stdtests/lib.rs', 'w').write('')
EOF
cat > Cargo.toml <<'EOF'
[workspace]
members = ["coretests", "alloctests", "stdtests"]
resolver = "2"

[profile.release]
opt-level = 2
debug-assertions = true
overflow-checks = true
EOF
pkgs=()
for s in $ONLY; do
	case $s in
	coretests | alloctests) pkgs+=(-p "$s") ;;
	std) pkgs+=(-p stdtests) ;;
	*) echo "unknown suite $s" >&2; exit 2 ;;
	esac
done
echo "building (${pkgs[*]})"
"$RUSTDIR/klauss-linux-cargo" test --release "${pkgs[@]}" --no-run --message-format=json \
	2>build.err >build.json || { tail -30 build.err; exit 1; }
FAILS=0 RUNS=0
mkdir -p logs
while read -r name exe; do
	RUNS=$((RUNS + 1))
	args=(-q)
	[ "$name" = pipe_subprocess ] && args=()
	# </dev/null: ssh would otherwise read the rest of this list.
	timeout "${LT_TIMEOUT:-2700}" "$RUSTDIR/klauss-linux-run" "$exe" "${args[@]}" </dev/null >"logs/$name.log" 2>&1
	rc=$?
	res=$(grep -E '^test result' "logs/$name.log" | sed 's/; finished in/, in/' | tail -1)
	if [ $rc = 0 ]; then
		echo "PASS $name: ${res:-exit 0}"
	else
		FAILS=$((FAILS + 1))
		echo "FAIL $name: exit $rc ${res} (logs/$name.log)"
	fi
done < <(python3 - <<'EOF'
import json
for l in open('build.json'):
    m = json.loads(l)
    if m.get('reason') == 'compiler-artifact' and m.get('executable') and m['profile'].get('test'):
        kind = m['target']['kind']
        if 'bench' in kind:
            continue
        name = m['target']['name'] + ('-lib' if 'lib' in kind else '')
        print(name, m['executable'])
EOF
)
echo "library-tests: $((RUNS - FAILS)) of $RUNS binaries passed (logs in $W/logs)"
[ $FAILS = 0 ]
