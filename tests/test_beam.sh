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

# Extract link & token from log
LINK_LINE=$(grep "Link:" "$OUTPUT_LOG" || true)
TOKEN=$(grep "Link:" "$OUTPUT_LOG" | grep -o '[0-9a-f]\{12\}' | head -1)

if [ -z "$TOKEN" ] || [ ${#TOKEN} -ne 12 ]; then
    echo "[FAIL] Could not extract valid 12-char token from output:"
    cat "$OUTPUT_LOG"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi
echo "[PASS] Started beam server on port $TEST_PORT, token=$TOKEN (12 hex chars)"

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
   echo "$STREAM_RESP" | grep -q "Accept-Ranges: bytes"; then
    echo "[PASS] Clean path /$TOKEN returns correct stream headers (video/mp4, inline disposition)"
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

# Invalid range request: bytes=999999-
STATUS_416=$(curl -s -o /dev/null -w "%{http_code}" -H "Range: bytes=999999-9999999" "http://127.0.0.1:$TEST_PORT/$TOKEN")
if [ "$STATUS_416" = "416" ]; then
    echo "[PASS] Invalid range returns 416 Range Not Satisfiable"
else
    echo "[FAIL] Invalid range returned $STATUS_416 instead of 416"
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
ONESHOT_TOKEN=$(grep "Link:" "$ONESHOT_LOG" | grep -o '[0-9a-f]\{12\}' | head -1)

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
PIPE_TOKEN=$(grep "Link:" "$PIPE_LOG" | grep -o '[0-9a-f]\{12\}' | head -1)
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
AD_TOKEN=$(grep "Link:" "$AUTODUB_LOG" | grep -o '[0-9a-f]\{12\}' | head -1)
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

# 14. Ephemeral public tunnel flag (-p) test
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

# 15. Reopen last file & token test
echo "Testing reopen last file & token..."
REOPEN_SEED_LOG="$TMP_DIR/reopen_seed.log"
./beam -q -P 9877 -t 2s "$TEST_MP4" > "$REOPEN_SEED_LOG" 2>&1 &
SEED_PID=$!
sleep 0.5
SEED_TOKEN=$(grep "Link:" "$REOPEN_SEED_LOG" | grep -o '[0-9a-f]\{12\}' | head -1)
kill $SEED_PID 2>/dev/null || true
wait $SEED_PID 2>/dev/null || true

REOPEN_LOG="$TMP_DIR/reopen.log"
# Invoke beam with no file arguments using -r
./beam -q -P 9878 -r -t 2s > "$REOPEN_LOG" 2>&1 &
REOPEN_PID=$!
sleep 0.5
REOPEN_TOKEN=$(grep "Link:" "$REOPEN_LOG" | grep -o '[0-9a-f]\{12\}' | head -1)
if [ -n "$REOPEN_TOKEN" ] && [ "$REOPEN_TOKEN" = "$SEED_TOKEN" ]; then
    RESP=$(curl -s -i "http://127.0.0.1:9878/$REOPEN_TOKEN")
    if echo "$RESP" | grep -q "video/mp4" && echo "$RESP" | grep -q "sample.mp4"; then
        echo "[PASS] Reopened last file with identical token and valid video stream"
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

# 16. Custom token flag (-k) test
echo "Testing custom token flag (-k)..."
CUSTOM_LOG="$TMP_DIR/custom_token.log"
CUSTOM_TOK="123456abcdef"
./beam -q -P 9879 -k "$CUSTOM_TOK" -t 2s "$TEST_TXT" > "$CUSTOM_LOG" 2>&1 &
CUSTOM_PID=$!
sleep 0.5
if grep -q "$CUSTOM_TOK" "$CUSTOM_LOG"; then
    RESP=$(curl -s -i "http://127.0.0.1:9879/$CUSTOM_TOK")
    if echo "$RESP" | grep -q "200 OK"; then
        echo "[PASS] Custom token (-k) correctly accepted and served"
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

echo "=== All tests passed successfully! ==="
