//! Scrape-fan-out microbenchmark: blocking serial GETs vs the io_uring batch
//! (design 39 §39.4 PR4 decision procedure). Only compiled under the `io-uring`
//! feature. Needs a loaded urp module with at least one endpoint, so it runs on
//! hardware (an hp node), not in CI.
//!
//! Triggered by `URP_EXPORTER_BENCH[=<endpoint-name>]` (empty = first endpoint
//! from the dump). Both paths issue `N` verbose GETs for the same endpoint --
//! the per-GET send/recv cost is identical regardless of distinct names, so
//! hammering one endpoint faithfully measures the fan-out mechanics at each `N`
//! without needing `N` real endpoints. Two separate sockets keep the blocking
//! path's ACK backlog from contaminating the io_uring drain.

use std::time::Instant;

use urp_netlink::{fetch_endpoints, get_endpoint, get_endpoints_batch_uring, UrpSocket};

/// Run the N-sweep and exit the process. Diverges (never returns to `main`).
pub fn run_and_exit(target: Option<&str>) -> ! {
    let mut sock_blk = match UrpSocket::connect() {
        Ok(s) => s,
        Err(e) => {
            eprintln!("bench: netlink connect failed ({e}); is the urp module loaded?");
            std::process::exit(1);
        }
    };
    let mut sock_urg = UrpSocket::connect().expect("bench: second socket");

    let eps = fetch_endpoints(&mut sock_blk, None).expect("bench: dump");
    if eps.is_empty() {
        eprintln!("bench: no endpoints present; add one first");
        std::process::exit(1);
    }
    let name = target
        .map(String::from)
        .unwrap_or_else(|| eps[0].name.clone());
    eprintln!(
        "bench: hammering endpoint {name:?} ({} endpoint(s) present), 200 iters/N",
        eps.len()
    );
    println!("# design 39 §39.4 -- blocking serial GETs vs io_uring batch");
    println!("#   N   blocking_us   uring_us   speedup");

    let iters = 200usize;
    for &nn in &[1usize, 4, 16, 64, 256] {
        let names: Vec<&str> = std::iter::repeat(name.as_str()).take(nn).collect();

        // Blocking: N serial doit GETs.
        let t = Instant::now();
        for _ in 0..iters {
            for _ in 0..nn {
                let _ = get_endpoint(&mut sock_blk, &name);
            }
        }
        let blk_us = t.elapsed().as_secs_f64() * 1e6 / iters as f64;

        // io_uring: one submit/reap for all N.
        let t = Instant::now();
        for _ in 0..iters {
            let _ = get_endpoints_batch_uring(&mut sock_urg, &names).expect("bench: uring batch");
        }
        let urg_us = t.elapsed().as_secs_f64() * 1e6 / iters as f64;

        println!(
            "{nn:>5}   {blk_us:>10.1}   {urg_us:>8.1}   {:>5.2}x",
            blk_us / urg_us
        );
    }
    std::process::exit(0);
}
