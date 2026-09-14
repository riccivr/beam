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
if ./beam >/dev/null 2>&1; then
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

# Generate 1KB deterministic test file
awk 'BEGIN { for (i=1; i<=100; i++) printf "Line %04d: Hello beam streaming test!\n", i }' > "$TEST_TXT"
TXT_SIZE=$(wc -c < "$TEST_TXT" | tr -d ' ')

# Generate dummy MP4 binary content
head -c 2048 /dev/urandom > "$TEST_MP4"
MP4_SIZE=$(wc -c < "$TEST_MP4" | tr -d ' ')

# 3. HTTP Server & Endpoints test with sample.mp4
TEST_PORT=9871
OUTPUT_LOG="$TMP_DIR/beam.log"

./beam -q -P $TEST_PORT "$TEST_MP4" > "$OUTPUT_LOG" 2>&1 &
BEAM_PID=$!
sleep 0.5

# Extract link & token from log
LINK_LINE=$(grep "Link:" "$OUTPUT_LOG" || true)
TOKEN=$(echo "$LINK_LINE" | awk -F'/s/' '{print $2}' | awk -F'/' '{print $1}')

if [ -z "$TOKEN" ]; then
    echo "[FAIL] Could not extract token from output:"
    cat "$OUTPUT_LOG"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi
echo "[PASS] Started beam server on port $TEST_PORT, token=$TOKEN"

case "$LINK_LINE" in
    *"sample.mp4"*) echo "[PASS] Generated share URL includes filename for native browser playback" ;;
    *) echo "[FAIL] Share URL does not include filename: $LINK_LINE"; kill $BEAM_PID 2>/dev/null || true; exit 1 ;;
esac

# 4. Invalid path and invalid token check
STATUS_404=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$TEST_PORT/s/badtoken")
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

# 5. Direct native browser stream check (Content-Type, Content-Disposition inline, Accept-Ranges)
STREAM_RESP=$(curl -s -i "http://127.0.0.1:$TEST_PORT/s/$TOKEN/sample.mp4")
if echo "$STREAM_RESP" | grep -q "Content-Type: video/mp4" && \
   echo "$STREAM_RESP" | grep -q "Content-Disposition: inline; filename=\"sample.mp4\"" && \
   echo "$STREAM_RESP" | grep -q "Accept-Ranges: bytes"; then
    echo "[PASS] Native browser stream headers correct (inline disposition, video/mp4, Accept-Ranges)"
else
    echo "[FAIL] Native stream headers unexpected:\n$STREAM_RESP"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# 6. Full file download check
curl -s "http://127.0.0.1:$TEST_PORT/s/$TOKEN/sample.mp4" > "$TMP_DIR/downloaded.mp4"
if cmp -s "$TEST_MP4" "$TMP_DIR/downloaded.mp4"; then
    echo "[PASS] Full file download matches byte-for-byte"
else
    echo "[FAIL] Downloaded file differs from source"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# 7. Range requests (HTTP 206 Partial Content)
# Request first 10 bytes: bytes=0-9
curl -s -H "Range: bytes=0-9" -i "http://127.0.0.1:$TEST_PORT/s/$TOKEN/sample.mp4" > "$TMP_DIR/range1.out"
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
curl -s -H "Range: bytes=50-99" -i "http://127.0.0.1:$TEST_PORT/s/$TOKEN/sample.mp4" > "$TMP_DIR/range2.out"
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
curl -s -H "Range: bytes=-100" -i "http://127.0.0.1:$TEST_PORT/s/$TOKEN/sample.mp4" > "$TMP_DIR/range3.out"
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
STATUS_416=$(curl -s -o /dev/null -w "%{http_code}" -H "Range: bytes=999999-9999999" "http://127.0.0.1:$TEST_PORT/s/$TOKEN/sample.mp4")
if [ "$STATUS_416" = "416" ]; then
    echo "[PASS] Invalid range returns 416 Range Not Satisfiable"
else
    echo "[FAIL] Invalid range returned $STATUS_416 instead of 416"
    kill $BEAM_PID 2>/dev/null || true
    exit 1
fi

# 8. HEAD request check
HEAD_RESP=$(curl -s -I "http://127.0.0.1:$TEST_PORT/s/$TOKEN/sample.mp4")
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
ONESHOT_TOKEN=$(grep "Link:" "$ONESHOT_LOG" | awk -F'/s/' '{print $2}' | awk -F'/' '{print $1}')

curl -s "http://127.0.0.1:9873/s/$ONESHOT_TOKEN/testfile.txt" > /dev/null
sleep 0.5

if kill -0 $BEAM_1_PID 2>/dev/null; then
    echo "[FAIL] One-shot server still running after full download"
    kill -9 $BEAM_1_PID 2>/dev/null || true
    exit 1
else
    echo "[PASS] One-shot server auto-terminated after download completed"
fi

echo "=== All tests passed successfully! ==="
