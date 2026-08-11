# Fuzz crash reproducers (regression corpus)

When a fuzz target (design 27) finds a crash, commit the minimized reproducer
here under a per-target subdirectory so it is replayed on every CI run and the
bug stays fixed:

```
fuzz/regressions/<target>/<reproducer-file>
```

`<target>` is the fuzz name without the `fuzz-` prefix, matching the CI replay
step — e.g. `fuzz/regressions/classify/`, `.../rx-seq/`, `.../reorder/`.

Replay locally:

```
nix run .#fuzz-<target> -- fuzz/regressions/<target>/*
```

- Per-push (`ci.yml`, `fuzz-smoke`): replays `classify` + `rx-seq` reproducers,
  then runs each hermetic harness for 45 s.
- Nightly (`nightly.yml`, `fuzz-long`): replays all reproducers, then runs each
  harness (incl. `reorder`) for 10 min; crash artifacts are uploaded.

The live-module F2 fuzzers (netlink blind/coverage-guided, racer, hostile wire)
run nightly inside the `microvm-sanitizers` pair test under KASAN/KMEMLEAK.
