# cargo-fuzz runner as a flake target (design 30 §30.12): wraps the
# Rust fuzz targets (frame_decode … bench_frame_decode,
# bench_differential) so they run via `nix run` like the C fuzzers —
# impure by nature (cargo needs the working tree + network for the
# first dependency fetch), so this is an app, not a check.
#
#   nix run .#fuzz-rust                      # bench_differential, 60 s
#   nix run .#fuzz-rust -- bench_frame_decode 300
{ pkgs, rustToolchain }:

pkgs.writeShellApplication {
  name = "fuzz-rust";
  runtimeInputs = [ rustToolchain pkgs.cargo-fuzz pkgs.stdenv.cc ];
  text = ''
    target=''${1:-bench_differential}
    duration=''${2:-60}
    if [ ! -d fuzz/fuzz_targets ]; then
      echo "error: run from the repository root (fuzz/fuzz_targets not found)" >&2
      exit 2
    fi
    echo "cargo fuzz run $target for ''${duration}s (regressions replay first)"
    for f in "fuzz/regressions/bench-frame"/*; do
      [ -e "$f" ] || continue
      (cd fuzz && cargo fuzz run "$target" "../$f")
    done
    (cd fuzz && cargo fuzz run "$target" -- -max_total_time="$duration")
  '';
}
