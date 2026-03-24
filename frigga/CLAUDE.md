# Frigga

Zero-dependency Node.js (>=24) HTTP proxy for the Anthropic Messages API. Forwards `/v1/messages` and `/v1/messages/count_tokens` to `https://api.anthropic.com` with credential injection, streaming passthrough, and NDJSON logging.

## Commands

```bash
npm start                    # Run server (node src/index.js)
npm test                     # Run all tests (node --test tests/**/*.test.js)
npm run check                # Prettier + ESLint check (no writes)
npm run fix                  # Prettier + ESLint auto-fix
node src/index.js --port=3000 --host=127.0.0.1 --log-body
```

## Structure

```
src/index.js       — Entry point, CLI parsing, server startup
src/server.js      — HTTP server, request handler, upstream forwarding, logging
src/cli.js         — CLI argument parsing and validation
src/constants.js   — Shared constants (ports, timeouts, upstream URL)
tests/handler.test.js — Unit tests (pure handler logic, no HTTP)
tests/server.test.js  — Integration tests (real HTTP server)
docs/rfc_*.md      — 6 RFCs documenting design decisions
```

## Conventions

- **ES Modules** (`import`/`export`), `const`/`let` only, arrow functions preferred
- **Zero runtime dependencies** — only Node.js built-ins (`http`, `https`, `crypto`, `stream`, `url`)
- **Testing** uses Node.js built-in `node:test` — no external test framework
- **Linting**: ESLint strict mode (`===`, no `var`, no shadow, no unused vars except `_`-prefixed) + Prettier defaults
- **Logging**: All structured output as NDJSON — INFO to stdout, WARN/ERROR to stderr

## Key Patterns

- **Pure request handler**: `handleRequest()` is a pure function returning `{ statusCode, headers, body }` or `null` (forward upstream) — enables unit testing without HTTP
- **Header whitelists**: Explicit allow-lists for both request and response header forwarding (prefix matching for `anthropic-ratelimit-*`, `anthropic-priority-*`)
- **Streaming pipeline**: `stream.pipeline()` for transparent response relay — no buffering, SSE passthrough, client disconnect cancels upstream
- **Exactly-once logging**: `createRequestLogEmitter()` uses a closure-scoped guard to prevent duplicate log entries across error paths
- **Graceful shutdown**: `server.close()` → drain active connections (30s timeout) → `server.closeAllConnections()`
