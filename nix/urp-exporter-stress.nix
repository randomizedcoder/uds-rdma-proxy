# Stress/soak the urp-exporter against a synthetic fleet -- NO kernel module,
# NO hardware, fully CI-able (design 39 PR3).
#
#   nix run .#urp-exporter-stress                 # 20s @ 100Hz, 16 endpoints
#   STRESS_SECONDS=120 STRESS_MOCK=64,8,4 nix run .#urp-exporter-stress
#
# A real generic-netlink mock is impossible in userspace (the `urp` genl family
# needs the kernel module), so the exporter is built with the `mock` feature and
# driven via URP_EXPORTER_MOCK=<endpoints>,<qps>,<streams> -- it serves that
# synthetic fleet through the *real* render + HTTP path. The harness then hammers
# /metrics at a fixed rate and asserts the exporter stays cheap and leak-free:
#
#   - 5xx / non-200:  must be ZERO (the parser/render never errors a scrape)
#   - p99 scrape:     must stay under STRESS_P99_MS (default 100ms)
#   - RSS growth:     must stay under STRESS_RSS_GROWTH_KIB (default 4096 KiB)
#   - fd count:       must not grow (fd leak = RED)
#
# The 0-alloc render + ns/render-linearity claims (§39.8a) are asserted
# separately by the `urp-exporter` unit tests (render_is_zero_alloc_when_warm,
# render_scales_with_fleet) which run in the `urp-exporter-tests` CI check.
#
# Env: STRESS_SECONDS (20), STRESS_HZ (100), STRESS_MOCK ("16,8,4"),
#      STRESS_PORT (19975), STRESS_P99_MS (100), STRESS_RSS_GROWTH_KIB (4096).
{ pkgs, urpExporterMock }:

pkgs.writeShellApplication {
  name = "urp-exporter-stress";
  runtimeInputs = [ pkgs.coreutils pkgs.curl pkgs.gawk pkgs.gnugrep pkgs.procps ];
  text = ''
    SECONDS_TOTAL="''${STRESS_SECONDS:-20}"
    HZ="''${STRESS_HZ:-100}"
    MOCK="''${STRESS_MOCK:-16,8,4}"
    PORT="''${STRESS_PORT:-19975}"
    P99_MS="''${STRESS_P99_MS:-100}"
    RSS_GROWTH_KIB="''${STRESS_RSS_GROWTH_KIB:-4096}"

    tmp="$(mktemp -d)"
    cleanup() {
      [ -n "''${pid:-}" ] && kill "$pid" 2>/dev/null || true
      rm -rf "$tmp"
    }
    trap cleanup EXIT

    echo "== urp-exporter stress: fleet=$MOCK, ''${HZ}Hz for ''${SECONDS_TOTAL}s on :$PORT =="

    URP_EXPORTER_MOCK="$MOCK" \
      ${urpExporterMock}/bin/urp-exporter --listen "127.0.0.1:$PORT" --cache-ttl-ms 0 &
    pid=$!

    # Wait for readiness (bounded).
    ready=0
    for _ in $(seq 1 50); do
      if curl -fsS -o /dev/null "http://127.0.0.1:$PORT/metrics" 2>/dev/null; then ready=1; break; fi
      sleep 0.1
    done
    if [ "$ready" -ne 1 ]; then echo "FAIL: exporter never became ready" >&2; exit 1; fi
    if ! kill -0 "$pid" 2>/dev/null; then echo "FAIL: exporter exited early" >&2; exit 1; fi

    rss0="$(grep VmRSS "/proc/$pid/status" | awk '{print $2}')"
    fd0="$(find "/proc/$pid/fd" -mindepth 1 | wc -l)"

    # Drive /metrics at ~HZ, logging "<http_code> <time_total_s> <size_bytes>".
    interval="$(awk -v h="$HZ" 'BEGIN{printf "%.4f", 1.0/h}')"
    deadline=$(( $(date +%s) + SECONDS_TOTAL ))
    reqs=0
    : > "$tmp/log"
    while [ "$(date +%s)" -lt "$deadline" ]; do
      curl -s -o /dev/null -w '%{http_code} %{time_total} %{size_download}\n' \
        "http://127.0.0.1:$PORT/metrics" >> "$tmp/log" || echo "000 0 0" >> "$tmp/log"
      reqs=$((reqs + 1))
      sleep "$interval"
    done

    rss1="$(grep VmRSS "/proc/$pid/status" | awk '{print $2}')"
    fd1="$(find "/proc/$pid/fd" -mindepth 1 | wc -l)"

    # Analyse.
    non200="$(awk '$1 != 200 {n++} END{print n+0}' "$tmp/log")"
    size_med="$(awk '{print $3}' "$tmp/log" | sort -n | awk '{a[NR]=$1} END{print (NR? a[int(NR/2)+0] : 0)}')"
    p50_ms="$(awk '{print $2*1000}' "$tmp/log" | sort -n | awk '{a[NR]=$1} END{print (NR? a[int(NR*0.50)+0] : 0)}')"
    p99_ms="$(awk '{print $2*1000}' "$tmp/log" | sort -n | awk '{a[NR]=$1} END{print (NR? a[int(NR*0.99)+0] : 0)}')"
    rss_growth=$(( rss1 - rss0 ))
    fd_growth=$(( fd1 - fd0 ))

    echo "-- results --"
    printf 'requests      : %s\n' "$reqs"
    printf 'non-200       : %s\n' "$non200"
    printf 'payload bytes : %s (median)\n' "$size_med"
    printf 'scrape p50    : %.2f ms\n' "$p50_ms"
    printf 'scrape p99    : %.2f ms\n' "$p99_ms"
    printf 'RSS           : %s -> %s KiB (Δ %s)\n' "$rss0" "$rss1" "$rss_growth"
    printf 'fd count      : %s -> %s (Δ %s)\n' "$fd0" "$fd1" "$fd_growth"

    # Verdict.
    fail=0
    if [ "$non200" -ne 0 ]; then echo "RED: $non200 non-200 responses" >&2; fail=1; fi
    if awk -v p="$p99_ms" -v b="$P99_MS" 'BEGIN{exit !(p > b)}'; then
      echo "RED: p99 ''${p99_ms}ms over budget ''${P99_MS}ms" >&2; fail=1; fi
    if [ "$rss_growth" -gt "$RSS_GROWTH_KIB" ]; then
      echo "RED: RSS grew ''${rss_growth} KiB (> ''${RSS_GROWTH_KIB})" >&2; fail=1; fi
    if [ "$fd_growth" -gt 0 ]; then echo "RED: fd leak (Δ ''${fd_growth})" >&2; fail=1; fi

    if [ "$fail" -eq 0 ]; then echo "GREEN: exporter stayed cheap + leak-free"; else exit 1; fi
  '';
}
