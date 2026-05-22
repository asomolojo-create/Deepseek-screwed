#!/usr/bin/env bash
set -euo pipefail

# run_test.sh - build, start server, run client script, stop server, show logs
cd "$(dirname "$0")"

echo "== build =="
make clean && make -j2

echo "== start server =="
./animate_server 4 > server.log 2>&1 &
SERVER_PID=$!
echo "Server PID: $SERVER_PID"
# ensure server ready
sleep 1

echo "== run client script =="
printf 'Login alice\ncreate canvas 10 10 ff0000\ncreate circle 3 00ff00 1\nplace sprite 1 2 2 2\nplacement bottom 3\ngenerate 1 testout 0 0 1\ndisconnect\n' | ./animate_client $SERVER_PID > client.log 2>&1 || true

echo "== client output =="
cat client.log || true

echo "== server tail =="
tail -n 200 server.log || true

echo "== generated files =="
ls -l testout.dat testout.mp4 testout.log || true

echo "== stop server =="
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo "== done =="
