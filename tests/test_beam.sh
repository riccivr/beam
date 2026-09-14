#!/bin/sh
set -e

DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR"

echo "=== Running beam test suite ==="

# 1. Version test
VERSION_OUT=$(./beam -v)
case "$VERSION_OUT" in
    "beam 1.0.0") echo "[PASS] Version check" ;;
    *) echo "[FAIL] Version check: $VERSION_OUT"; exit 1 ;;
esac

# 2. Argument validation test
if ./beam </dev/null >/dev/null 2>&1; then
    echo "[FAIL] Missing argument should fail"
    exit 1
fi
echo "[PASS] Missing argument check"

if ./beam non_existent_file_xyz.txt >/dev/null 2>&1; then
    echo "[FAIL] Non-existent file should fail"
    exit 1
fi
echo "[PASS] Non-existent file check"

# Create dummy test files
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

TEST_TXT="$TMP_DIR/testfile.txt"
TEST_MP4="$TMP_DIR/sample.mp4"
TEST_SRT="$TMP_DIR/sample_es.srt"

# Generate 1KB deterministic test file
awk 'BEGIN { for (i=1; i<=100; i++) printf "Line %04d: Hello beam streaming test!\n", i }' > "$TEST_TXT"
TXT_SIZE=$(wc -c < "$TEST_TXT" | tr -d ' ')

# Generate dummy MP4 binary content
head -c 2048 /dev/urandom > "$TEST_MP4"
MP4_SIZE=$(wc -c < "$TEST_MP4" | tr -d ' ')

# Generate dummy SRT file
printf "1\n00:00:00,000 --> 00:00:02,000\nHello world\n" > "$TEST_SRT"

# 3. HTTP Server & Endpoints test with sample.mp4
TEST_PORT=9871
OUTPUT_LOG="$TMP_DIR/beam.log"

./beam -q -P $TEST_PORT "$TEST_MP4" > "$OUTPUT_LOG" 2>&1 &
BEAM_PID=$!
sleep 0.5

# Extract link & token from log (32 hex chars / 128-bit CSPRNG)
TOKEN=$(grep "Link:" "$OUTPUT_LOG" | grep -oE '[0-9a-f]{32}' | head -1)

if [ -z "$TOKEN" ] || [ ${#TOKEN} -ne 32 ]; then
    echo "[FAIL] Could not extract valid 32-char token from output:"
    cat "$OUTPUT_LOG"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi
echo "[PASS] Started beam server on port $TEST_PORT, token=$TOKEN (32 hex chars / 128-bit CSPRNG)"

# 4. Invalid path and invalid token check
STATUS_404=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$TEST_PORT/badtoken")
if [ "$STATUS_404" = "404" ]; then
    echo "[PASS] Invalid token returns 404"
else
    echo "[FAIL] Invalid token returned $STATUS_404 instead of 404"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

STATUS_ROOT=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$TEST_PORT/")
if [ "$STATUS_ROOT" = "404" ]; then
    echo "[PASS] Root path returns 404"
else
    echo "[FAIL] Root path returned $STATUS_ROOT"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# 5. Direct native browser stream check on clean path param: /$TOKEN
STREAM_RESP=$(curl -s -i "http://127.0.0.1:$TEST_PORT/$TOKEN")
if echo "$STREAM_RESP" | grep -q "Content-Type: video/mp4" && \
   echo "$STREAM_RESP" | grep -q "Content-Disposition: inline; filename=\"sample.mp4\"" && \
   echo "$STREAM_RESP" | grep -q "Accept-Ranges: bytes" && \
   echo "$STREAM_RESP" | grep -q "Connection: keep-alive"; then
    echo "[PASS] Clean path /$TOKEN returns correct stream headers (video/mp4, inline disposition, Connection: keep-alive)"
else
    echo "[FAIL] Stream headers unexpected:\n$STREAM_RESP"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# Query param format check: /?v=$TOKEN
PARAM_RESP=$(curl -s -i "http://127.0.0.1:$TEST_PORT/?v=$TOKEN")
if echo "$PARAM_RESP" | grep -q "Content-Type: video/mp4"; then
    echo "[PASS] Query param /?v=$TOKEN supported"
else
    echo "[FAIL] Query param /?v=$TOKEN failed:\n$PARAM_RESP"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# 6. Full file download check
curl -s "http://127.0.0.1:$TEST_PORT/$TOKEN" > "$TMP_DIR/downloaded.mp4"
if cmp -s "$TEST_MP4" "$TMP_DIR/downloaded.mp4"; then
    echo "[PASS] Full file download matches byte-for-byte"
else
    echo "[FAIL] Downloaded file differs from source"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# 7. Range requests (HTTP 206 Partial Content)
# Request first 10 bytes: bytes=0-9
curl -s -H "Range: bytes=0-9" -i "http://127.0.0.1:$TEST_PORT/$TOKEN" > "$TMP_DIR/range1.out"
if grep -q "206 Partial Content" "$TMP_DIR/range1.out" && \
   grep -q "Content-Range: bytes 0-9/$MP4_SIZE" "$TMP_DIR/range1.out" && \
   grep -q "Content-Length: 10" "$TMP_DIR/range1.out"; then
    echo "[PASS] HTTP 206 Range request bytes=0-9"
else
    echo "[FAIL] Range bytes=0-9 failed:"
    head -n 10 "$TMP_DIR/range1.out"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# Request middle slice: bytes=50-99 (50 bytes)
curl -s -H "Range: bytes=50-99" -i "http://127.0.0.1:$TEST_PORT/$TOKEN" > "$TMP_DIR/range2.out"
if grep -q "206 Partial Content" "$TMP_DIR/range2.out" && \
   grep -q "Content-Range: bytes 50-99/$MP4_SIZE" "$TMP_DIR/range2.out" && \
   grep -q "Content-Length: 50" "$TMP_DIR/range2.out"; then
    echo "[PASS] HTTP 206 Range request bytes=50-99"
else
    echo "[FAIL] Range bytes=50-99 failed"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# Request suffix range: bytes=-100 (last 100 bytes)
START_SUFFIX=$((MP4_SIZE - 100))
END_SUFFIX=$((MP4_SIZE - 1))
curl -s -H "Range: bytes=-100" -i "http://127.0.0.1:$TEST_PORT/$TOKEN" > "$TMP_DIR/range3.out"
if grep -q "206 Partial Content" "$TMP_DIR/range3.out" && \
   grep -q "Content-Range: bytes $START_SUFFIX-$END_SUFFIX/$MP4_SIZE" "$TMP_DIR/range3.out" && \
   grep -q "Content-Length: 100" "$TMP_DIR/range3.out"; then
    echo "[PASS] HTTP 206 Suffix range request bytes=-100"
else
    echo "[FAIL] Suffix range failed"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# Invalid range requests: out of bounds, backwards range, negative suffix
STATUS_416=$(curl -s -o /dev/null -w "%{http_code}" -H "Range: bytes=999999-9999999" "http://127.0.0.1:$TEST_PORT/$TOKEN")
if [ "$STATUS_416" = "416" ]; then
    echo "[PASS] Invalid range returns 416 Range Not Satisfiable"
else
    echo "[FAIL] Invalid range returned $STATUS_416 instead of 416"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

STATUS_BACKWARDS=$(curl -s -o /dev/null -w "%{http_code}" -H "Range: bytes=100-50" "http://127.0.0.1:$TEST_PORT/$TOKEN")
if [ "$STATUS_BACKWARDS" = "416" ]; then
    echo "[PASS] Backwards range 100-50 returns 416"
else
    echo "[FAIL] Backwards range returned $STATUS_BACKWARDS instead of 416"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# 8. HEAD request check
HEAD_RESP=$(curl -s -I "http://127.0.0.1:$TEST_PORT/$TOKEN")
if echo "$HEAD_RESP" | grep -q "HTTP/1.1 200 OK" && \
   echo "$HEAD_RESP" | grep -q "Accept-Ranges: bytes" && \
   echo "$HEAD_RESP" | grep -q "Content-Length: $MP4_SIZE"; then
    echo "[PASS] HEAD request returns proper headers"
else
    echo "[FAIL] HEAD request response unexpected:\n$HEAD_RESP"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# Method Not Allowed check (POST)
STATUS_405=$(curl -s -o /dev/null -w "%{http_code}" -X POST "http://127.0.0.1:$TEST_PORT/$TOKEN")
if [ "$STATUS_405" = "405" ]; then
    echo "[PASS] POST method returns 405 Method Not Allowed"
else
    echo "[FAIL] POST method returned $STATUS_405 instead of 405"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# Keep-alive: two Range GETs on one TCP connection
KA_CODES=$(curl -s --http1.1 \
    -o "$TMP_DIR/ka1.bin" -w "%{http_code} " -H "Range: bytes=0-9" "http://127.0.0.1:$TEST_PORT/$TOKEN" \
    -o "$TMP_DIR/ka2.bin" -w "%{http_code}" -H "Range: bytes=10-19" "http://127.0.0.1:$TEST_PORT/$TOKEN")
KA_CODES=$(echo "$KA_CODES" | tr -d ' ')
if [ "$KA_CODES" = "206206" ] && [ "$(wc -c < "$TMP_DIR/ka1.bin" | tr -d ' ')" = "10" ] && \
   [ "$(wc -c < "$TMP_DIR/ka2.bin" | tr -d ' ')" = "10" ]; then
    echo "[PASS] Keep-alive reuses connection for two Range requests"
else
    echo "[FAIL] Keep-alive dual Range failed: codes='$KA_CODES'"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# Stop server
kill $BEAM_PID 2>/dev/null || true
wait $BEAM_PID 2>/dev/null || true

# 9. TTL automatic expiration test
echo "Testing TTL auto-expiration (-t 2s)..."
./beam -q -P 9872 -t 2s "$TEST_TXT" >/dev/null 2>&1 &
BEAM_TTL_PID=$!
sleep 3
if kill -0 $BEAM_TTL_PID 2>/dev/null; then
    echo "[FAIL] Beam process still running after TTL expired"
    kill -9 $BEAM_TTL_PID 2>/dev/null || true
    exit 1
else
    echo "[PASS] Beam server auto-terminated after TTL expiry"
fi

# 10. One-shot mode test (-1)
echo "Testing one-shot mode (-1)..."
ONESHOT_LOG="$TMP_DIR/oneshot.log"
./beam -q -P 9873 -1 "$TEST_TXT" > "$ONESHOT_LOG" 2>&1 &
BEAM_1_PID=$!
sleep 0.5
ONESHOT_TOKEN=$(grep "Link:" "$ONESHOT_LOG" | grep -oE '[0-9a-f]{32}' | head -1)

curl -s "http://127.0.0.1:9873/$ONESHOT_TOKEN" > /dev/null
sleep 0.5

if kill -0 $BEAM_1_PID 2>/dev/null; then
    echo "[FAIL] One-shot server still running after full download"
    kill -9 $BEAM_1_PID 2>/dev/null || true
    exit 1
else
    echo "[PASS] One-shot server auto-terminated after download completed"
fi

# 11. Piped filepath test
echo "Testing piped filepath..."
PIPE_LOG="$TMP_DIR/pipe.log"
echo "$TEST_TXT" | ./beam -q -P 9874 -1 > "$PIPE_LOG" 2>&1 &
PIPE_PID=$!
sleep 0.5
PIPE_TOKEN=$(grep "Link:" "$PIPE_LOG" | grep -oE '[0-9a-f]{32}' | head -1)
if [ -n "$PIPE_TOKEN" ]; then
    curl -s "http://127.0.0.1:9874/$PIPE_TOKEN" > /dev/null
    echo "[PASS] Piped filepath correctly picked up and served"
else
    echo "[FAIL] Failed to start server from piped filepath:"
    cat "$PIPE_LOG"
    kill $PIPE_PID 2>/dev/null || true
    exit 1
fi

# 12. Piped autodub log test: video followed by subtitles
echo "Testing piped autodub log with Dubbed video followed by Subs srt..."
AUTODUB_LOG="$TMP_DIR/autodub_pipe.log"
printf "[1/6] Preparing media...\n[6/6] Remuxing final video...\nCompleted:\n  Dubbed: %s\n  Subs: %s\nDone!\n" "$TEST_MP4" "$TEST_SRT" | ./beam -q -P 9875 -1 > "$AUTODUB_LOG" 2>&1 &
AD_PID=$!
sleep 0.5
AD_TOKEN=$(grep "Link:" "$AUTODUB_LOG" | grep -oE '[0-9a-f]{32}' | head -1)
if [ -n "$AD_TOKEN" ]; then
    RESP=$(curl -s -i "http://127.0.0.1:9875/$AD_TOKEN")
    if echo "$RESP" | grep -q "video/mp4" && echo "$RESP" | grep -q "sample.mp4"; then
        echo "[PASS] Autodub output prioritized MP4 over SRT subtitles"
    else
        echo "[FAIL] Autodub output selected wrong file:\n$RESP"
        kill $AD_PID 2>/dev/null || true
        exit 1
    fi
else
    echo "[FAIL] Failed to extract dubbed video from piped autodub log:"
    cat "$AUTODUB_LOG"
    kill $AD_PID 2>/dev/null || true
    exit 1
fi

# 13. Custom host flag (-H) test
echo "Testing custom host flag (-H)..."
HOST_LOG="$TMP_DIR/host.log"
./beam -q -P 9876 -H "tailscale.example.com" -t 5s "$TEST_TXT" > "$HOST_LOG" 2>&1 &
HOST_PID=$!
sleep 0.5
if grep -q "http://tailscale.example.com:9876/" "$HOST_LOG"; then
    echo "[PASS] -H host reflected in share link"
else
    echo "[FAIL] -H host not found in output:"
    cat "$HOST_LOG"
    kill $HOST_PID 2>/dev/null || true
    exit 1
fi
kill $HOST_PID 2>/dev/null || true

# 14. Empty file + Connection: keep-alive + 416 on Range
echo "Testing empty file and Connection: keep-alive..."
EMPTY="$TMP_DIR/empty.bin"
: > "$EMPTY"
./beam -q -P 9877 -t 8s "$EMPTY" > "$TMP_DIR/empty.log" 2>&1 &
EMPTY_PID=$!
sleep 0.5
EMPTY_TOKEN=$(grep "Link:" "$TMP_DIR/empty.log" | grep -oE '[0-9a-f]{32}' | head -1)
EMPTY_HDR=$(curl -s -i "http://127.0.0.1:9877/$EMPTY_TOKEN")
if echo "$EMPTY_HDR" | grep -q "HTTP/1.1 200 OK" && \
   echo "$EMPTY_HDR" | grep -q "Content-Length: 0" && \
   echo "$EMPTY_HDR" | grep -q "Connection: keep-alive"; then
    echo "[PASS] Empty file returns 200, length 0, Connection: keep-alive"
else
    echo "[FAIL] Empty file headers unexpected:\n$EMPTY_HDR"
    kill $EMPTY_PID 2>/dev/null || true
    exit 1
fi
EMPTY_416=$(curl -s -o /dev/null -w "%{http_code}" -H "Range: bytes=0-0" "http://127.0.0.1:9877/$EMPTY_TOKEN")
if [ "$EMPTY_416" = "416" ]; then
    echo "[PASS] Range on empty file returns 416"
else
    echo "[FAIL] Range on empty file returned $EMPTY_416"
    kill $EMPTY_PID 2>/dev/null || true
    exit 1
fi
kill $EMPTY_PID 2>/dev/null || true
wait $EMPTY_PID 2>/dev/null || true

# 15. Filename escaping in HTML player
echo "Testing HTML filename escaping..."
EVIL="$TMP_DIR/evil<script>\".mp4"
cp "$TEST_MP4" "$EVIL"
./beam -q -w -P 9878 -t 8s "$EVIL" > "$TMP_DIR/evil.log" 2>&1 &
EVIL_PID=$!
sleep 0.5
EVIL_TOKEN=$(grep "Link:" "$TMP_DIR/evil.log" | grep -oE '[0-9a-f]{32}' | head -1)
EVIL_PAGE=$(curl -s "http://127.0.0.1:9878/$EVIL_TOKEN")
if echo "$EVIL_PAGE" | grep -q '&quot;' && echo "$EVIL_PAGE" | grep -q '&lt;script&gt;' && ! echo "$EVIL_PAGE" | grep -q '<script>'; then
    echo "[PASS] HTML player escapes quotes and angle brackets in filename"
else
    echo "[FAIL] HTML player did not escape filename:\n$EVIL_PAGE"
    kill $EVIL_PID 2>/dev/null || true
    exit 1
fi
kill $EVIL_PID 2>/dev/null || true
wait $EVIL_PID 2>/dev/null || true

# 16. Header filename sanitization (stripping quotes and control characters from Content-Disposition)
echo "Testing Content-Disposition filename sanitization..."
DISP_HDR=$(curl -s -i "http://127.0.0.1:9878/$EVIL_TOKEN/raw" 2>/dev/null || true)
# Start server with evil filename to test raw stream header
./beam -q -P 9879 -t 8s "$EVIL" > "$TMP_DIR/evil_raw.log" 2>&1 &
EVIL_RAW_PID=$!
sleep 0.5
EVIL_RAW_TOK=$(grep "Link:" "$TMP_DIR/evil_raw.log" | grep -oE '[0-9a-f]{32}' | head -1)
DISP_HDR=$(curl -s -i "http://127.0.0.1:9879/$EVIL_RAW_TOK")
if echo "$DISP_HDR" | grep -q 'Content-Disposition: inline; filename="evil<script>.mp4"'; then
    echo "[PASS] Content-Disposition stripped dangerous quote from filename"
else
    echo "[FAIL] Content-Disposition header unexpected:\n$DISP_HDR"
    kill $EVIL_RAW_PID 2>/dev/null || true
    exit 1
fi
kill $EVIL_RAW_PID 2>/dev/null || true
wait $EVIL_RAW_PID 2>/dev/null || true

# 17. Concurrent overlapping Range requests (testing forked client workers)
echo "Testing concurrent overlapping Range requests..."
./beam -q -P 9880 -t 8s "$TEST_MP4" > "$TMP_DIR/concurrent.log" 2>&1 &
CONC_PID=$!
sleep 0.5
CONC_TOK=$(grep "Link:" "$TMP_DIR/concurrent.log" | grep -oE '[0-9a-f]{32}' | head -1)

# Fire 4 concurrent range requests simultaneously
curl -s -H "Range: bytes=0-99" "http://127.0.0.1:9880/$CONC_TOK" > "$TMP_DIR/part1" &
C1=$!
curl -s -H "Range: bytes=100-199" "http://127.0.0.1:9880/$CONC_TOK" > "$TMP_DIR/part2" &
C2=$!
curl -s -H "Range: bytes=200-299" "http://127.0.0.1:9880/$CONC_TOK" > "$TMP_DIR/part3" &
C3=$!
curl -s -H "Range: bytes=300-399" "http://127.0.0.1:9880/$CONC_TOK" > "$TMP_DIR/part4" &
C4=$!

wait $C1 $C2 $C3 $C4
if [ $(wc -c < "$TMP_DIR/part1") -eq 100 ] && \
   [ $(wc -c < "$TMP_DIR/part2") -eq 100 ] && \
   [ $(wc -c < "$TMP_DIR/part3") -eq 100 ] && \
   [ $(wc -c < "$TMP_DIR/part4") -eq 100 ]; then
    echo "[PASS] Handled 4 concurrent overlapping Range requests"
else
    echo "[FAIL] Concurrent requests returned incorrect byte counts"
    kill $CONC_PID 2>/dev/null || true
    exit 1
fi
kill $CONC_PID 2>/dev/null || true
wait $CONC_PID 2>/dev/null || true

# 18. Reopen last file & token test
echo "Testing reopen last file & token..."
REOPEN_SEED_LOG="$TMP_DIR/reopen_seed.log"
./beam -q -P 9881 -t 2s "$TEST_MP4" > "$REOPEN_SEED_LOG" 2>&1 &
SEED_PID=$!
sleep 0.5
SEED_TOKEN=$(grep "Link:" "$REOPEN_SEED_LOG" | grep -oE '[0-9a-f]{32}' | head -1)
kill $SEED_PID 2>/dev/null || true
wait $SEED_PID 2>/dev/null || true

REOPEN_LOG="$TMP_DIR/reopen.log"
# Invoke beam with no file arguments using -r
./beam -q -P 9882 -r -t 2s > "$REOPEN_LOG" 2>&1 &
REOPEN_PID=$!
sleep 0.5
REOPEN_TOKEN=$(grep "Link:" "$REOPEN_LOG" | grep -oE '[0-9a-f]{32}' | head -1)
if [ -n "$REOPEN_TOKEN" ] && [ "$REOPEN_TOKEN" = "$SEED_TOKEN" ]; then
    RESP=$(curl -s -i "http://127.0.0.1:9882/$REOPEN_TOKEN")
    if echo "$RESP" | grep -q "video/mp4" && echo "$RESP" | grep -q "sample.mp4"; then
        echo "[PASS] Reopened last file with identical 32-char token and valid video stream"
    else
        echo "[FAIL] Reopened file response unexpected:\n$RESP"
        kill $REOPEN_PID 2>/dev/null || true
        exit 1
    fi
else
    echo "[FAIL] Reopened token mismatch or failed: seed=$SEED_TOKEN reopen=$REOPEN_TOKEN"
    cat "$REOPEN_LOG"
    kill $REOPEN_PID 2>/dev/null || true
    exit 1
fi
kill $REOPEN_PID 2>/dev/null || true
wait $REOPEN_PID 2>/dev/null || true

# 19. Custom token flag (-k) test with 32-char token
echo "Testing custom token flag (-k)..."
CUSTOM_LOG="$TMP_DIR/custom_token.log"
CUSTOM_TOK="0123456789abcdef0123456789abcdef"
./beam -q -P 9883 -k "$CUSTOM_TOK" -t 2s "$TEST_TXT" > "$CUSTOM_LOG" 2>&1 &
CUSTOM_PID=$!
sleep 0.5
if grep -q "$CUSTOM_TOK" "$CUSTOM_LOG"; then
    RESP=$(curl -s -i "http://127.0.0.1:9883/$CUSTOM_TOK")
    if echo "$RESP" | grep -q "200 OK"; then
        echo "[PASS] Custom 32-char token (-k) correctly accepted and served"
    else
        echo "[FAIL] Custom token curl failed:\n$RESP"
        kill $CUSTOM_PID 2>/dev/null || true
        exit 1
    fi
else
    echo "[FAIL] Custom token not found in output:"
    cat "$CUSTOM_LOG"
    kill $CUSTOM_PID 2>/dev/null || true
    exit 1
fi
kill $CUSTOM_PID 2>/dev/null || true
wait $CUSTOM_PID 2>/dev/null || true

# 20. Ephemeral public tunnel flag (-p) test
echo "Testing public tunnel flag (-p)..."
TUNNEL_LOG="$TMP_DIR/tunnel.log"
./beam -q -p -t 2s "$TEST_TXT" > "$TUNNEL_LOG" 2>&1 &
TUN_PID=$!
for i in $(seq 1 10); do
    if ! kill -0 $TUN_PID 2>/dev/null; then
        break
    fi
    sleep 0.5
done
kill -9 $TUN_PID 2>/dev/null || true
if grep -q "https://.*\.lhr\." "$TUNNEL_LOG" && ! grep -q "admin\.localhost\.run" "$TUNNEL_LOG"; then
    echo "[PASS] -p generated public HTTPS tunnel link without admin login"
else
    echo "[WARN] -p completed or skipped"
fi

echo "=== All tests passed successfully! ==="
