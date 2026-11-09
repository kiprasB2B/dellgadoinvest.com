#!/usr/bin/env bash
# Integration tests for the Dellgado.invest static server.
# Usage: tests/test.sh path/to/server-binary
set -u

BIN=${1:-./build/server}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${TEST_PORT:-$(( 20000 + RANDOM % 20000 ))}
BASE="http://127.0.0.1:$PORT"
WORK=$(mktemp -d)
LOG="$WORK/server.log"
PASS=0
FAIL=0

# Sandbox layout:  $WORK/site/public  (docroot, a copy of public/)
#                  $WORK/site/secret.txt  (must never be served)
mkdir -p "$WORK/site"
cp -R "$ROOT/public" "$WORK/site/public"
echo "TOPSECRET-OUTSIDE-DOCROOT" > "$WORK/site/secret.txt"
echo "TOPSECRET-DOTFILE" > "$WORK/site/public/.env"
ln -s ../secret.txt "$WORK/site/public/link-to-secret.txt"
mkdir -p "$WORK/site/public/sub"
echo "sub index" > "$WORK/site/public/sub/index.html"

cleanup() {
  if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -KILL "$SERVER_PID" 2>/dev/null
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

ok()   { PASS=$((PASS + 1)); printf '  ok    %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL  %s\n' "$1"; [ -n "${2:-}" ] && printf '        %s\n' "$2"; }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected '$3', got '$2'"; fi; }

# status for a path (curl does not normalise with --path-as-is)
status() { curl -s --path-as-is -o /dev/null -w '%{http_code}' "$@"; }

# raw request over a TCP socket; prints the full response
raw() {
  exec 3<>"/dev/tcp/127.0.0.1/$PORT" || return 1
  printf '%b' "$1" >&3
  timeout 5 cat <&3
  exec 3<&- 3>&-
}

echo "== starting $BIN on port $PORT"
PORT=$PORT DOCROOT="$WORK/site/public" REQUEST_TIMEOUT_SEC=2 "$BIN" >"$LOG" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
  curl -s -o /dev/null "$BASE/healthz" && break
  sleep 0.1
done

echo "== basic"
check "GET / -> 200" "$(status "$BASE/")" 200
check "GET / content-type" "$(curl -s -o /dev/null -w '%{content_type}' "$BASE/")" "text/html; charset=utf-8"
curl -s "$BASE/" -o "$WORK/got.html"
if cmp -s "$WORK/got.html" "$ROOT/public/index.html"; then ok "GET / body == public/index.html"; else bad "GET / body == public/index.html"; fi
check "GET /index.html -> 200" "$(status "$BASE/index.html")" 200
size=$(wc -c < "$ROOT/public/index.html" | tr -d ' ')
check "Content-Length matches file" "$(curl -s -o /dev/null -w '%{size_download}' "$BASE/")" "$size"
check "GET /sub/ -> 200" "$(status "$BASE/sub/")" 200
check "GET /sub -> 200 (dir index)" "$(status "$BASE/sub")" 200
check "query string ignored" "$(status "$BASE/?utm=1")" 200

echo "== HEAD"
resp=$(raw 'HEAD / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n')
check "HEAD / -> 200" "$(printf '%s' "$resp" | head -1 | tr -d '\r')" "HTTP/1.1 200 OK"
check "HEAD Content-Length" "$(printf '%s' "$resp" | tr -d '\r' | awk -F': ' 'tolower($1)=="content-length"{print $2}')" "$size"
body=$(printf '%s' "$resp" | tr -d '\r' | awk 'f{print} /^$/{f=1}')
check "HEAD has no body" "${#body}" 0

echo "== headers"
hdrs=$(curl -s -D - -o /dev/null "$BASE/" | tr -d '\r')
for h in "X-Content-Type-Options: nosniff" \
         "Referrer-Policy: strict-origin-when-cross-origin" \
         "Strict-Transport-Security: max-age=31536000" \
         "Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src https://fonts.gstatic.com; img-src 'self' data:"; do
  if printf '%s\n' "$hdrs" | grep -qxF "$h"; then ok "header: ${h%%:*}"; else bad "header: $h"; fi
done

echo "== healthz / 404 / 405"
check "/healthz -> 200" "$(status "$BASE/healthz")" 200
check "/healthz body" "$(curl -s "$BASE/healthz")" ok
check "/nope -> 404" "$(status "$BASE/nope")" 404
check "404 is html" "$(curl -s -o /dev/null -w '%{content_type}' "$BASE/nope")" "text/html; charset=utf-8"
if curl -s "$BASE/nope" | grep -q "404"; then ok "404 page body"; else bad "404 page body"; fi
for m in POST PUT DELETE PATCH OPTIONS; do
  check "$m / -> 405" "$(status -X "$m" "$BASE/")" 405
done
if curl -s -D - -o /dev/null -X POST "$BASE/" | tr -d '\r' | grep -qx "Allow: GET, HEAD"; then ok "405 has Allow"; else bad "405 has Allow"; fi

echo "== path traversal (must never leak files outside public/)"
leak=0
trav_paths=(
  "/../secret.txt" "/../../etc/passwd" "/%2e%2e/secret.txt" "/%2E%2E/%2E%2E/etc/passwd"
  "/%2e%2e%2fsecret.txt" "/..%2fsecret.txt" "/..%2F..%2F..%2Fetc%2Fpasswd"
  "/sub/../../secret.txt" "/sub/%2e%2e/%2e%2e/secret.txt" "//etc/passwd" "/./../secret.txt"
  "/%252e%252e/secret.txt" "/..%5csecret.txt" "/index.html%00.txt" "/%00"
  "/link-to-secret.txt" "/.env" "/%2eenv" "/etc/passwd"
)
for p in "${trav_paths[@]}"; do
  code=$(curl -s --path-as-is -o "$WORK/out" -w '%{http_code}' "$BASE$p")
  if grep -q -e TOPSECRET -e 'root:' "$WORK/out" || [ "$code" = 200 ]; then
    bad "traversal $p" "status $code"; leak=1
  else
    ok "traversal $p -> $code"
  fi
done
raw_reqs=(
  'GET ../secret.txt HTTP/1.1\r\nHost: t\r\n\r\n'
  'GET http://t/../secret.txt HTTP/1.1\r\nHost: t\r\n\r\n'
  'GET http://t/%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\nHost: t\r\n\r\n'
  'GET /etc/passwd HTTP/1.1\r\nHost: t\r\n\r\n'
  'GET /..\\secret.txt HTTP/1.1\r\nHost: t\r\n\r\n'
  'GET /index.html\x00/../../secret.txt HTTP/1.1\r\nHost: t\r\n\r\n'
)
for r in "${raw_reqs[@]}"; do
  resp=$(raw "$r" | tr -d '\0')
  first=$(printf '%s' "$resp" | head -1 | tr -d '\r')
  if printf '%s' "$resp" | grep -q -e TOPSECRET -e 'root:' || [[ "$first" == *" 200 "* ]]; then
    bad "raw traversal: $r" "$first"; leak=1
  else
    ok "raw traversal -> ${first#HTTP/1.1 }"
  fi
done

echo "== limits"
big=$(head -c 9000 /dev/zero | tr '\0' 'a')
first=$(raw "GET / HTTP/1.1\r\nHost: t\r\nX-Big: $big\r\n\r\n" | head -1 | tr -d '\r')
check "header > 8 KB -> 431" "$first" "HTTP/1.1 431 Request Header Fields Too Large"
first=$(raw 'GARBAGE\r\n\r\n' | head -1 | tr -d '\r')
check "malformed request -> 400" "$first" "HTTP/1.1 400 Bad Request"
first=$(raw 'GET / HTTP/2.0\r\n\r\n' | head -1 | tr -d '\r')
check "HTTP/2.0 request line -> 505" "$first" "HTTP/1.1 505 HTTP Version Not Supported"
t0=$(date +%s)
first=$(raw 'GET / HTTP/1.1\r\nHost: t\r\n' | head -1 | tr -d '\r')
t1=$(date +%s)
check "slow/incomplete headers -> 408" "$first" "HTTP/1.1 408 Request Timeout"
if [ $((t1 - t0)) -le 4 ]; then ok "timeout fired in $((t1 - t0))s"; else bad "timeout took $((t1 - t0))s"; fi

echo "== concurrency"
# 3 idle connections must not block other clients
idle=()
for _ in 1 2 3; do (exec 3<>"/dev/tcp/127.0.0.1/$PORT"; sleep 1.5) & idle+=($!); done
pids=()
for i in $(seq 1 40); do
  curl -s -o /dev/null -w '%{http_code}\n' "$BASE/" > "$WORK/c$i" & pids+=($!)
done
wait "${pids[@]}"
okc=$(cat "$WORK"/c* | grep -c '^200$')
check "40 parallel GETs -> 200" "$okc" 40
wait "${idle[@]}"

echo "== logging"
lines=$(grep -c '"GET /healthz"' "$LOG")
if [ "$lines" -ge 1 ]; then ok "access log lines on stdout"; else bad "access log lines on stdout"; fi

echo "== graceful shutdown (SIGTERM)"
kill -TERM "$SERVER_PID"
wait "$SERVER_PID"; rc=$?
SERVER_PID=
check "exit code after SIGTERM" "$rc" 0
if grep -q "shutdown requested" "$LOG"; then ok "shutdown logged"; else bad "shutdown logged"; fi
if grep -qE "ERROR: (Address|Leak)Sanitizer|runtime error:" "$LOG"; then
  bad "sanitizer reports"; grep -E "Sanitizer|runtime error" "$LOG" | head
else
  ok "no sanitizer reports"
fi

echo
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ] && [ "$leak" -eq 0 ]
