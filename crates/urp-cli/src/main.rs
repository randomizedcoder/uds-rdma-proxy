//! `urp` -- CLI for the urp generic-netlink family.

mod attr;
mod commands;
mod error;
mod format;
mod netlink;
mod uapi;

use clap::{Parser, Subcommand};

use crate::error::UrpError;

#[derive(Parser, Debug)]
#[command(name = "urp", version, about = "UDS-RDMA Proxy control CLI")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// Create a new endpoint.
    Add(commands::add::AddArgs),
    /// Remove an endpoint by name.
    Remove(commands::remove::RemoveArgs),
    /// Mutate live endpoint config.
    Set(commands::set::SetArgs),
    /// Show endpoint(s).
    Show(commands::show::ShowArgs),
    /// Print stats for endpoint(s).
    Stats(commands::stats::StatsArgs),
    /// Drain an endpoint (no new streams, finish existing).
    Drain(commands::drain::DrainArgs),
    /// Subscribe to state-change events.
    Monitor(commands::monitor::MonitorArgs),
}

fn main() {
    let cli = Cli::parse();
    let res = dispatch(cli.cmd);
    if let Err(e) = res {
        eprintln!("error: {e}");
        std::process::exit(exit_code(&e));
    }
}

fn dispatch(cmd: Cmd) -> Result<(), UrpError> {
    match cmd {
        Cmd::Add(a) => commands::add::run(a),
        Cmd::Remove(a) => commands::remove::run(a),
        Cmd::Set(a) => commands::set::run(a),
        Cmd::Show(a) => commands::show::run(a),
        Cmd::Stats(a) => commands::stats::run(a),
        Cmd::Drain(a) => commands::drain::run(a),
        Cmd::Monitor(a) => commands::monitor::run(a),
    }
}

fn exit_code(e: &UrpError) -> i32 {
    match e {
        UrpError::KernelModuleNotLoaded => 2,
        UrpError::EndpointNotFound => 3,
        UrpError::EndpointExists => 4,
        UrpError::InvalidArgument(_) => 5,
        UrpError::PermissionDenied => 6,
        _ => 1,
    }
}

#[cfg(test)]
mod cli_tests {
    use super::*;
    use clap::CommandFactory;

    #[test]
    fn clap_validation_missing_name() {
        let res = Cli::try_parse_from(["urp", "add"]);
        assert!(res.is_err(), "urp add without --name should fail");
    }

    #[test]
    fn clap_validation_num_qps_range() {
        let res = Cli::try_parse_from([
            "urp",
            "add",
            "--name",
            "e0",
            "--listen-path",
            "/run/u",
            "--num-qps",
            "99",
        ]);
        assert!(res.is_err(), "out-of-range --num-qps should fail");
    }

    #[test]
    fn clap_renders() {
        let _ = Cli::command().debug_assert();
    }
}
