// Compile the control-plane proto into Rust with tonic (server + client).
//
// Needs `protoc` on PATH (or $PROTOC). The nix derivations put
// `pkgs.protobuf` in nativeBuildInputs and set PROTOC=…/bin/protoc so the
// codegen runs offline in the sandbox.
fn main() -> Result<(), Box<dyn std::error::Error>> {
    tonic_build::configure()
        .build_server(true)
        .build_client(true)
        .compile_protos(
            &["../../proto/urp_control/v1/control.proto"],
            &["../../proto"],
        )?;
    Ok(())
}
