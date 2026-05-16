#!/bin/bash
# Start server in background
./animate_server &
SERVER_PID=$!
sleep 2
# Run client with a command
echo "Login alice" | ./animate_client
# Kill server
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null
