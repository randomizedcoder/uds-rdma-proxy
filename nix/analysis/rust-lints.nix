# nix/analysis/rust-lints.nix
#
# clippy + rustfmt --check over the Cargo workspace (uds-rdma-protocol,
# uds-rdma-protocol-ffi, urp-cli). The pinned nightly toolchain's
# default profile already ships both tools. clippy needs the vendored
# dependency graph, hence buildRustPackage + cargoLock (same pattern as
# nix/urp-cli.nix); rustfmt --check only parses source, so a bare
# runCommand suffices. Same report.txt + count.txt contract as the
# kernel-side targets; findings never fail the build.
{ pkgs, src, rustToolchain }:

{
  clippy = pkgs.rustPlatform.buildRustPackage {
    pname = "urp-analysis-clippy";
    version = "0.1.0";
    inherit src;

    cargoLock.lockFile = ../../Cargo.lock;
    nativeBuildInputs = [ rustToolchain ];
    doCheck = false;

    buildPhase = ''
      runHook preBuild
      # Per-crate: the ffi crate is a no_std staticlib with its own panic
      # handler, so --all-targets (which builds its test harness against
      # std) would hit E0152 "duplicate lang item panic_impl". Lint it as
      # --lib only; the other crates get the full treatment.
      {
        cargo clippy -p uds-rdma-protocol --all-targets --offline 2>&1 || true
        cargo clippy -p urp-cli --all-targets --offline 2>&1 || true
        cargo clippy -p urp-bench --all-targets --offline 2>&1 || true
        # --release: panic=abort is pinned in the release profile, and the
        # no_std panic handler can't build under the unwinding dev profile.
        cargo clippy -p uds-rdma-protocol-ffi --lib --release --offline 2>&1 || true
      } | tee clippy.log
      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      mkdir -p $out
      cp clippy.log $out/build.log
      grep -E '^(warning|error)(\[[^]]+\])?:' clippy.log \
        | grep -v '^warning: .* generated' \
        | sort -u > $out/report.txt || true
      wc -l < $out/report.txt | tr -d ' ' > $out/count.txt
      echo "urp-analysis-clippy: $(cat $out/count.txt) findings"
      runHook postInstall
    '';
  };

  rustfmt-check = pkgs.runCommand "urp-analysis-rustfmt"
    { nativeBuildInputs = [ rustToolchain ]; }
    ''
      mkdir -p $out
      export HOME=$TMPDIR
      cd ${src}
      cargo fmt --all --check > $out/build.log 2>&1 || true
      cp $out/build.log $out/report.txt
      grep -c '^Diff in' $out/build.log > $out/count.txt || true
      echo "urp-analysis-rustfmt: $(cat $out/count.txt) files with diffs"
    '';
}
