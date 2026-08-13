// Compile the REAL C bench core for the bench_differential target
// (design 30 §30.12, F0 differential pattern): the Rust decoder is
// checked byte-for-byte against tools/urp-bench-core.c, so wire-format
// drift between the twins is a fuzz crash, not a code-review hope.
fn main() {
    cc::Build::new()
        .file("../tools/urp-bench-core.c")
        .include("../tools")
        .warnings(false)
        .compile("urp_bench_core");
    println!("cargo:rerun-if-changed=../tools/urp-bench-core.c");
    println!("cargo:rerun-if-changed=../tools/urp-bench-core.h");
}
