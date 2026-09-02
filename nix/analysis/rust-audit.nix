# nix/analysis/rust-audit.nix
#
# cargo-audit over the workspace Cargo.lock against the RustSec advisory DB
# (design 39 §39.6). The exporter is the tree's first network-facing listener
# (and adds the optional io-uring dep), i.e. new supply-chain + attack surface,
# so this introduces dependency-vulnerability scanning to the analysis tier.
#
# Hermetic + offline: the sandbox has no network, so the advisory DB is pinned
# as the `advisory-db` flake input and cargo-audit runs `--no-fetch --db <path>`.
# Same report.txt + count.txt contract as the other analysis targets; findings
# NEVER fail the build (advisory, report-only, manual-run).
{ pkgs, src, advisory-db }:

pkgs.runCommand "urp-analysis-rust-audit"
{
  nativeBuildInputs = [ pkgs.cargo-audit ];
}
  ''
    mkdir -p $out
    export HOME=$TMPDIR
    # cargo-audit reads Cargo.lock; scan the whole workspace lock (a superset of
    # the exporter's closure -- auditing everything is strictly better). Invoke
    # the binary directly with the `audit` subcommand so no `cargo` is needed on
    # PATH in the sandbox.
    cargo-audit audit \
      --no-fetch \
      --db ${advisory-db} \
      --file ${src}/Cargo.lock \
      > $out/build.log 2>&1 || true
    cp $out/build.log $out/report.txt
    # cargo-audit prints one "Crate: <name>" block per advisory; count them.
    grep -c '^Crate:' $out/report.txt > $out/count.txt || echo 0 > $out/count.txt
    # Fall back to 0 if grep found nothing (grep -c prints 0 but exits 1).
    [ -s $out/count.txt ] || echo 0 > $out/count.txt
    echo "urp-analysis-rust-audit: $(cat $out/count.txt) advisories"
  ''
