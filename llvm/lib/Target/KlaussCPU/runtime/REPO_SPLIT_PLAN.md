# Repo split plan — compiler vs. software

Today everything lives in one tree: the KlaussCPU LLVM backend (`llvm/lib/Target/
KlaussCPU/*.{cpp,h,td}` + clang/lld/Triple changes) **and** all the software
written with it (`llvm/lib/Target/KlaussCPU/runtime/`). This plan splits them.

## Why

- The tracked software is only **~7.7 MB** of source, but working on it drags the
  full **3.6 GB** LLVM fork history.
- The backend is feature-complete; active work is in the runtime/OS layer. The
  split matches where the work happens.
- Independent versioning, CI, and clone size for the software.

## End state — two repos

| Repo | Contents | Role |
|---|---|---|
| `klausscpu-llvm` (existing fork) | `llvm/lib/Target/KlaussCPU/*.{cpp,h,td}` + the clang/lld/`Triple`/intrinsic/builtin changes scattered in the tree. **Minus** `runtime/`. | Produces the toolchain (`clang`, `ld.lld`, `llc`). |
| `klausscpu-runtime` (new) | The contents of `runtime/` at the repo root: FreeRTOS port, Zephyr port (`zephyr-ws/klausscpu-zephyr`), lwIP glue, picolibc glue, test programs, build scripts. | The software; depends on a **built** `klausscpu-llvm`. |

The link between them is **one env var**, not a submodule:
`KLAUSSCPU_LLVM_BIN=<klausscpu-llvm>/build/bin` (already used by the west build;
the Makefile's `BUILD_DIR` should default from it).

> Do **not** make `klausscpu-llvm` a git submodule of the software repo — embedding
> a 3.6 GB fork defeats the purpose. Documentation + env var is the contract.

## Phases

### Phase 0 — Boundaries & toolchain contract
- [ ] Confirm the split line is exactly `runtime/` (everything under it moves;
      backend `.cpp/.td/.h` stay).
- [ ] Standardize the toolchain path on `KLAUSSCPU_LLVM_BIN`
      (→ `<fork>/build/bin`). Audit every hardcoded path (see Phase 3).

### Phase 1 — Extract the software repo *with history*
```sh
# Fresh mirror so the original is untouched
git clone --no-local <klausscpu-llvm-fork> klausscpu-runtime
cd klausscpu-runtime
pip install git-filter-repo           # if not installed
git filter-repo \
  --path llvm/lib/Target/KlaussCPU/runtime/ \
  --path-rename llvm/lib/Target/KlaussCPU/runtime/:
# → repo now contains just the software at its root, with the slice of history
#   that touched runtime/. Inspect: git log --stat | head; ls
git remote add origin <new-remote-url>
git push -u origin main
```
Alternative (no history): fresh `git init`, copy current `runtime/` contents, one
initial commit. Simpler, but loses the "Steps" development story.

### Phase 2 — Remove the software from the fork
```sh
# In klausscpu-llvm — keep history, just stop carrying the tree
git rm -r llvm/lib/Target/KlaussCPU/runtime
git commit -m "Move runtime/ to the klausscpu-runtime repo"
```
Do **not** rewrite the fork's history to purge `runtime/` — `.git` stays
LLVM-sized regardless, and rewriting breaks clean rebases onto upstream LLVM.

### Phase 3 — Make the software repo stand alone
- [ ] De-hardcode toolchain paths → `${KLAUSSCPU_LLVM_BIN}`:
  - [ ] `runtime/Makefile` (`BUILD_DIR`, `OBJCOPY`, etc.)
  - [ ] `runtime/freertos/wolfssl/build-wolfssl.sh` / `build-wolfssh.sh` toolchain file
  - [ ] `zephyr-ws/.../wolfssl-toolchain.cmake`
  - [ ] the `west build` invocations (`-DKLAUSSCPU_LLVM_BIN=...`)
- [ ] Update `zephyr-ws/klausscpu-zephyr/README.md`: new repo root, the
      "fresh-clone" section, build commands.
- [ ] Docs split: backend `CLAUDE.md` stays in the fork; create a runtime-focused
      `CLAUDE.md`/README in the software repo (move the "Systems built" runtime
      section out of the backend `CLAUDE.md`).

### Phase 4 — Wire the relationship (docs)
- [ ] Software README: "Requires the `klausscpu-llvm` fork built; set
      `KLAUSSCPU_LLVM_BIN`." Pin the expected commit/branch.
- [ ] Fork README: link to `klausscpu-runtime`.

### Phase 5 — Verify & cut over
- [ ] Build the fork; build the software repo against it.
- [ ] Confirm `zephyr.elf` (ssh_shell) **and** a FreeRTOS image still build.
- [ ] Make `klausscpu-runtime` the source of truth; stop editing `runtime/` in
      the fork.

### Phase 6 — Update notes
- [ ] Update memory/index paths to the new layout.

## The one real downside
A change that spans **both** repos (e.g. a new backend intrinsic + its use in
`uart_stubs.c`) becomes two commits in two repos. Today this is rare (backend is
stable), so it's an acceptable trade — but coordinate such changes deliberately.

## Effort / risk
Low–medium. The extraction is mechanical (`git filter-repo`); the fiddly part is
de-hardcoding toolchain paths (Phase 3) and re-verifying the build (Phase 5). The
original repo is untouched until Phase 2, so a dry-run extraction is safe to
inspect first.
