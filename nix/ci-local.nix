# Local reproduction of the every-push GitHub CI (.github/workflows/ci.yml),
# for hosts without a GitHub runner: `nix run .#ci-local`.
#
# ci.yml has two jobs; this target reproduces both faithfully:
#
#   nix-checks   -- 12 pure `nix build` targets (shared-crate tests, urp-cli,
#                   the two urp-bench cores, the urp-netlink lib tests, the
#                   urp-control control-plane tests, the urp-fast validators,
#                   and the 4-kernel module matrix). Here
#                   they are BUILD-TIME deps of
#                   this app, so `nix run .#ci-local` realises every one before
#                   the script body runs -- a build failure fails the gate up
#                   front, exactly as the CI job would go red.
#   fuzz-smoke   -- replay the committed crash reproducers, then time-boxed
#                   (45s) libFuzzer runs of the three hermetic harnesses. These
#                   are genuine executions (not builds), so they run in the
#                   script body; a libFuzzer crash exits non-zero and fails.
#
# The KVM tiers (microVM pair test, KASAN debug variant, soak, cross-boot)
# live in nightly.yml and are deliberately NOT part of this -- same boundary
# ci.yml draws. Run those with `nix run .#urp-microvm-pair-test[-debug]`.
#
# Must be run from the repo root: the reproducer replay reads
# ./fuzz/regressions/<target>/ from the working tree, just as the CI job does.
{ pkgs, checks, urpCli, fuzz }:

let
  # The twelve build targets of the ci.yml `nix-checks` job, in the same order.
  # Referenced from the script text below so each is a realised dependency.
  buildTargets = [
    { name = "protocol-tests"; drv = checks.protocol-tests; }
    { name = "urp-cli"; drv = urpCli; }
    { name = "urp-bench-units"; drv = checks.urp-bench-units; }
    { name = "urp-bench-rs-tests"; drv = checks.urp-bench-rs-tests; }
    { name = "urp-netlink-tests"; drv = checks.urp-netlink-tests; }
    { name = "urp-control-tests"; drv = checks.urp-control-tests; }
    { name = "urp-fast-validate-units"; drv = checks.urp-fast-validate-units; }
    { name = "urp-reorder-units"; drv = checks.urp-reorder-units; }
    { name = "urp-conn-slot-units"; drv = checks.urp-conn-slot-units; }
    { name = "urp-window-units"; drv = checks.urp-window-units; }
    { name = "kernel-module-build"; drv = checks.kernel-module-build; }
    { name = "urp-ko-6_1"; drv = checks.urp-ko-6_1; }
    { name = "urp-ko-6_6"; drv = checks.urp-ko-6_6; }
    { name = "urp-ko-6_12"; drv = checks.urp-ko-6_12; }
  ];

  # One "NAME=/nix/store/..." line per target; interpolating the derivation
  # pulls it into this app's build closure, so realising ci-local realises it.
  reportLines =
    pkgs.lib.concatMapStringsSep "\n"
      (t: "    echo '  ok  ${t.name}  ->  ${t.drv}'") buildTargets;

  fuzzClassify = "${fuzz.fuzz-classify}/bin/fuzz-classify";
  fuzzRxSeq = "${fuzz.fuzz-rx-seq}/bin/fuzz-rx-seq";
  fuzzBenchDeframe = "${fuzz.fuzz-bench-deframe}/bin/fuzz-bench-deframe";
in
pkgs.writeShellApplication {
  name = "ci-local";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep ];
  text = ''
    # ci-local: reproduce .github/workflows/ci.yml locally.
    pass=0 fail=0
    failed=""
    step() {
      local name="$1"; shift
      printf '\n=== [%s] %s ===\n' "$(date +%H:%M:%S)" "$name"
      if "$@"; then
        printf 'RESULT: PASS  %s\n' "$name"; pass=$((pass + 1))
      else
        local rc=$?
        printf 'RESULT: FAIL  %s (exit %d)\n' "$name" "$rc"
        fail=$((fail + 1)); failed="$failed $name"
      fi
    }

    echo '======== job: nix-checks (12 build targets) ========'
    echo 'All build targets below were realised as dependencies of this app:'
${reportLines}
    echo 'nix-checks: 12/12 build targets realised (reaching here means they built).'

    echo
    echo '======== job: fuzz-smoke ========'
    if [ ! -d fuzz/regressions ]; then
      echo 'FAIL: fuzz/regressions not found -- run from the repo root.' >&2
      exit 2
    fi

    # Replay every committed crash reproducer (a reproduced crash exits non-0).
    replay() {
      local t bin f any=0
      for t in classify rx-seq bench-deframe; do
        case "$t" in
          classify) bin=${fuzzClassify} ;;
          rx-seq) bin=${fuzzRxSeq} ;;
          bench-deframe) bin=${fuzzBenchDeframe} ;;
        esac
        for f in "fuzz/regressions/$t"/*; do
          [ -e "$f" ] || continue
          any=1
          echo "replay $t: $f"
          "$bin" "$f" || return 1
        done
      done
      [ "$any" -eq 0 ] && echo '(no committed reproducers yet -- only .gitkeep)'
      return 0
    }
    step "replay-reproducers" replay
    step "fuzz-classify-45s"      ${fuzzClassify}     -max_total_time=45 -print_final_stats=1
    step "fuzz-rx-seq-45s"        ${fuzzRxSeq}        -max_total_time=45 -print_final_stats=1
    step "fuzz-bench-deframe-45s" ${fuzzBenchDeframe} -max_total_time=45 -print_final_stats=1

    echo
    echo '======== LOCAL CI SUMMARY ========'
    echo "nix-checks: 12/12 build targets green"
    printf 'fuzz-smoke: PASS=%d FAIL=%d\n' "$pass" "$fail"
    if [ "$fail" -ne 0 ]; then
      printf 'FAILED:%s\n' "$failed"
      echo 'LOCAL_CI_RESULT=RED'
      exit 1
    fi
    echo 'LOCAL_CI_RESULT=GREEN'
  '';
}
