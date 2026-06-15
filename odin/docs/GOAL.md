# Goal

A single CLI binary that dispatches between server and client roles based on `argv[0]`.

## Client

Implements an `HTTPS_PROXY`. For each `CONNECT host:port` request:

1. Encode a request control frame and send it to the server.
2. Wait for the server's response control frame.
3. Transparently relay bytes between the local TCP socket and the server.

## Server

For each incoming session:

1. Decode the request control frame.
2. Open a TCP connection to the target HTTPS server.
3. Reply with a response control frame.
4. Transparently relay bytes between the client and the target.
