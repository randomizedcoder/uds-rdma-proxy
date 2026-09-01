//! Hand-rolled HTTP/1.1 responder (design 39 §39.3 rule 1 -- no `hyper`, no
//! `tiny_http`, no `tokio`). Prometheus scrapes a target serially and
//! infrequently, so a blocking one-request-at-a-time handler is sufficient. The
//! header read is bounded (max bytes + read timeout) so a slow or oversized
//! client cannot wedge or balloon the exporter.
//!
//! The routing decision is factored into pure functions (`route`, `status_for`)
//! so the whole request-classification truth table is testable without a socket
//! (design 39 §39.7 `http_request_route`).

use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Duration;

/// Largest request head we will buffer before giving up with 400. A metrics
/// scrape's request is tiny; anything large is junk or an attack.
pub const MAX_HEAD: usize = 8 * 1024;

/// What the client asked for, once the request line has been understood.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Route {
    /// `GET /metrics` -- serve the exposition.
    Metrics,
    /// `GET /` -- serve a tiny landing page.
    Root,
    /// A well-formed `GET` for a path we do not serve.
    NotFound,
    /// A well-formed request whose method is not `GET`.
    MethodNotAllowed,
    /// The request line could not be parsed.
    BadRequest,
}

impl Route {
    fn status(self) -> u16 {
        match self {
            Route::Metrics | Route::Root => 200,
            Route::NotFound => 404,
            Route::MethodNotAllowed => 405,
            Route::BadRequest => 400,
        }
    }
}

/// Parse the request head (everything up to the first CRLFCRLF). Only the
/// request line matters for routing; headers are ignored. Requires canonical
/// `METHOD SP TARGET SP HTTP/x` with CRLF line endings.
pub fn route(head: &[u8]) -> Route {
    // First line ends at the first CRLF. LF-only or missing terminator is malformed.
    let line_end = match find(head, b"\r\n") {
        Some(i) => i,
        None => return Route::BadRequest,
    };
    let line = &head[..line_end];
    let mut parts = line.split(|&b| b == b' ');
    let (method, target, version) = match (parts.next(), parts.next(), parts.next()) {
        (Some(m), Some(t), Some(v)) => (m, t, v),
        _ => return Route::BadRequest,
    };
    if parts.next().is_some() || !version.starts_with(b"HTTP/") {
        return Route::BadRequest;
    }
    if method != b"GET" {
        return Route::MethodNotAllowed;
    }
    // Strip any ?query before comparing the path.
    let path = match find(target, b"?") {
        Some(i) => &target[..i],
        None => target,
    };
    match path {
        b"/metrics" => Route::Metrics,
        b"/" => Route::Root,
        _ => Route::NotFound,
    }
}

/// The HTTP status a received request should get, folding in the two read-layer
/// conditions the pure `route` cannot see: an oversized head (400) and a read
/// timeout / truncated head (408). Pure -- drives the §39.7 test.
pub fn status_for(head: &[u8], timed_out: bool, oversized: bool) -> u16 {
    if oversized {
        return 400;
    }
    if find(head, b"\r\n\r\n").is_none() {
        // No complete head. A timeout is 408; otherwise it is a malformed 400.
        return if timed_out { 408 } else { 400 };
    }
    route(head).status()
}

/// Naive substring search -- the needles here are 1-4 bytes and the haystack is
/// <= MAX_HEAD, so this is cheaper than pulling in a dependency.
fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || needle.len() > hay.len() {
        return None;
    }
    hay.windows(needle.len()).position(|w| w == needle)
}

/// Read a bounded request head from `stream`, honouring `timeout`, and write the
/// appropriate response. `body` is the already-rendered `/metrics` text; it is
/// only sent for a 200 `Route::Metrics`. Returns the status served (for logging
/// / self-metrics). Never panics on malformed input.
pub fn serve(stream: &mut TcpStream, timeout: Duration, body: &str) -> std::io::Result<u16> {
    stream.set_read_timeout(Some(timeout)).ok();
    let mut head = Vec::with_capacity(512);
    let mut chunk = [0u8; 1024];
    let mut oversized = false;
    let mut timed_out = false;
    loop {
        match stream.read(&mut chunk) {
            Ok(0) => break, // client closed
            Ok(n) => {
                head.extend_from_slice(&chunk[..n]);
                if find(&head, b"\r\n\r\n").is_some() {
                    break;
                }
                if head.len() > MAX_HEAD {
                    oversized = true;
                    break;
                }
            }
            Err(e)
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut =>
            {
                timed_out = true;
                break;
            }
            Err(e) => return Err(e),
        }
    }

    // Decide status, and for a 200 decide whether it is the metrics body or the
    // tiny landing page (both route to 200 but serve different content).
    const LANDING: &str = "urp-exporter\nGET /metrics\n";
    let status = status_for(&head, timed_out, oversized);
    let metrics_200 = status == 200 && route(&head) == Route::Metrics;
    let body_for_200 = if metrics_200 { body } else { LANDING };
    write_response(stream, status, body_for_200, metrics_200)?;
    Ok(status)
}

fn write_response(
    stream: &mut TcpStream,
    status: u16,
    body: &str,
    metrics_200: bool,
) -> std::io::Result<()> {
    let (reason, payload): (&str, &[u8]) = match status {
        200 => ("OK", body.as_bytes()),
        400 => ("Bad Request", b"bad request\n"),
        404 => ("Not Found", b"not found\n"),
        405 => ("Method Not Allowed", b"method not allowed\n"),
        408 => ("Request Timeout", b"request timeout\n"),
        _ => ("Internal Server Error", b"error\n"),
    };
    let ctype = if metrics_200 {
        "text/plain; version=0.0.4; charset=utf-8"
    } else {
        "text/plain; charset=utf-8"
    };
    let header = format!(
        "HTTP/1.1 {status} {reason}\r\n\
         Content-Type: {ctype}\r\n\
         Content-Length: {}\r\n\
         Connection: close\r\n\r\n",
        payload.len()
    );
    stream.write_all(header.as_bytes())?;
    stream.write_all(payload)?;
    stream.flush()
}

#[cfg(test)]
mod tests {
    use super::*;

    // design 39 §39.7: raw request bytes -> HTTP status.
    #[test]
    fn http_request_route_truth_table() {
        // (head bytes, timed_out, oversized, expected status)
        let cases: [(&[u8], bool, bool, u16); 11] = [
            // positive: the metrics scrape
            (
                b"GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n",
                false,
                false,
                200,
            ),
            // positive: metrics with a query string
            (b"GET /metrics?foo=1 HTTP/1.1\r\n\r\n", false, false, 200),
            // positive: landing page
            (b"GET / HTTP/1.1\r\n\r\n", false, false, 200),
            // negative: unknown path
            (b"GET /nope HTTP/1.1\r\n\r\n", false, false, 404),
            // negative: non-GET method
            (b"POST /metrics HTTP/1.1\r\n\r\n", false, false, 405),
            // corner: HEAD is not GET -> 405
            (b"HEAD /metrics HTTP/1.1\r\n\r\n", false, false, 405),
            // negative: malformed request line (no spaces)
            (b"GETmetrics\r\n\r\n", false, false, 400),
            // corner: LF-only line endings are not valid HTTP -> malformed
            (b"GET /metrics HTTP/1.1\n\n", false, false, 400),
            // corner: pipelined bytes -- first request line wins
            (
                b"GET /metrics HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n",
                false,
                false,
                200,
            ),
            // boundary: truncated head + timeout -> 408
            (b"GET /metr", true, false, 408),
            // boundary: oversized head -> 400
            (b"GET /metrics HTTP/1.1\r\n", false, true, 400),
        ];
        for (i, (head, timed_out, oversized, want)) in cases.into_iter().enumerate() {
            let got = status_for(head, timed_out, oversized);
            assert_eq!(
                got,
                want,
                "case {i}: head={:?}",
                String::from_utf8_lossy(head)
            );
        }
    }
}
