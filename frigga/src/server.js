import http from "node:http";
import https from "node:https";
import { pipeline } from "node:stream";
import { SHUTDOWN_TIMEOUT_MS, UPSTREAM_BASE_URL } from "./constants.js";

const VALID_ROUTES = new Set(["/v1/messages", "/v1/messages/count_tokens"]);

const REQUEST_HEADER_FORWARD = new Set([
  "content-type",
  "accept",
  "accept-encoding",
  "user-agent",
  "anthropic-beta",
  "anthropic-version",
  "authorization",
]);

const RESPONSE_HEADER_FORWARD = new Set([
  "content-type",
  "content-length",
  "content-encoding",
  "request-id",
  "retry-after",
  "x-should-retry",
]);

const RESPONSE_HEADER_PREFIXES = ["anthropic-ratelimit-"];

/**
 * Build upstream request headers from the client request headers.
 * @param {Record<string, string>} incomingHeaders - req.headers (lower-cased by Node.js)
 * @returns {Record<string, string>}
 */
export function buildUpstreamHeaders(incomingHeaders) {
  const headers = {};
  for (const name of REQUEST_HEADER_FORWARD) {
    if (incomingHeaders[name] !== undefined) {
      headers[name] = incomingHeaders[name];
    }
  }
  return headers;
}

/**
 * Filter upstream response headers for the client.
 * @param {Record<string, string>} headers - upstreamRes.headers (lower-cased by Node.js)
 * @returns {Record<string, string>}
 */
export function filterResponseHeaders(headers) {
  const filtered = {};
  for (const [name, value] of Object.entries(headers)) {
    if (
      RESPONSE_HEADER_FORWARD.has(name) ||
      RESPONSE_HEADER_PREFIXES.some((prefix) => name.startsWith(prefix))
    ) {
      filtered[name] = value;
    }
  }
  return filtered;
}

/**
 * Build the full upstream URL by joining the base URL with the request path.
 * Handles base URLs with path prefixes (e.g., /api/gateway/anthropic).
 * @param {string} baseUrl - The upstream base URL (may include a path prefix)
 * @param {string} reqUrl - The incoming request URL (e.g., /v1/messages?stream=true)
 * @returns {URL}
 */
export function buildUpstreamUrl(baseUrl, reqUrl) {
  const base = new URL(baseUrl);
  const incoming = new URL(reqUrl, "http://localhost");
  const basePath = base.pathname.endsWith("/")
    ? base.pathname
    : `${base.pathname}/`;
  const reqPath = incoming.pathname.replace(/^\//, "");
  const result = new URL(basePath + reqPath, base.origin);
  result.search = incoming.search;

  // Defense-in-depth: verify the resolved path stays under basePath.
  // The double new URL() normalization already prevents traversal, but
  // this guard catches regressions if the logic above is ever modified.
  if (!result.pathname.startsWith(basePath)) {
    throw new Error(
      `upstream path traversal blocked: ${result.pathname} escapes ${basePath}`,
    );
  }

  return result;
}

/**
 * Abort the upstream request.
 * @param {import("node:http").ClientRequest} upstreamReq
 */
export function abortUpstream(upstreamReq) {
  if (upstreamReq && !upstreamReq.destroyed) {
    upstreamReq.destroy();
  }
}

/**
 * Collect the entire request body into a single Buffer.
 * Resolves with the concatenated body; rejects on stream error.
 * @param {import("node:http").IncomingMessage} req
 * @returns {Promise<Buffer>}
 */
export function bufferRequestBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => resolve(Buffer.concat(chunks)));
    req.on("error", (err) => {
      chunks.length = 0;
      reject(err);
    });
  });
}

/**
 * Create a request log emitter with exactly-once guard.
 * @param {object} ctx
 * @param {import("node:http").IncomingMessage} ctx.req
 * @param {Buffer} [ctx.requestBody] - buffered request body (omit to skip request_body)
 * @param {number} ctx.startTime - Date.now() at request start
 * @returns {(status: number, responseHeaders?: Record<string, string>) => void}
 */
export function createRequestLogEmitter({ req, requestBody, startTime }) {
  let emitted = false;

  return function emitRequestLog(status, responseHeaders) {
    if (emitted) return;
    emitted = true;

    const entry = {
      timestamp: new Date().toISOString(),
      level: "INFO",
      event: "request",
      method: req.method,
      url: req.url,
      status,
      duration_ms: Date.now() - startTime,
      request_headers: req.headers,
    };
    if (requestBody !== undefined) {
      entry.request_body = requestBody.toString("utf-8");
    }
    if (responseHeaders) entry.response_headers = responseHeaders;
    process.stdout.write(`${JSON.stringify(entry)}\n`);

    // Drain the request stream if still active (local error path only;
    // on the forwarding path the stream is already consumed by bufferRequestBody).
    if (!req.readableEnded && !req.destroyed) {
      req.on("error", () => {});
      req.resume();
    }
  };
}

/**
 * Pure request handler function.
 * @param {{ method: string, url: string }} req
 * @param {boolean} isShuttingDown
 * @returns {{ statusCode: number, headers: Record<string, string>, body: string } | null}
 */
export function handleRequest({ method, url }, isShuttingDown) {
  const pathname = new URL(url, "http://localhost").pathname;

  const responseHeaders = { "Content-Type": "application/json" };
  if (isShuttingDown) {
    responseHeaders["connection"] = "close";
  }

  // Route matching
  if (!VALID_ROUTES.has(pathname)) {
    return {
      statusCode: 404,
      headers: responseHeaders,
      body: JSON.stringify({
        type: "error",
        error: {
          type: "not_found_error",
          message: "The requested URL was not found",
        },
      }),
    };
  }

  // Method validation
  if (method !== "POST") {
    responseHeaders["Allow"] = "POST";
    return {
      statusCode: 405,
      headers: responseHeaders,
      body: JSON.stringify({
        type: "error",
        error: { type: "invalid_request_error", message: "Method not allowed" },
      }),
    };
  }

  // Validation passed — forward to upstream
  return null;
}

/**
 * Start the HTTP proxy server.
 * @param {{ port: number, host: string, logBody: boolean }} config
 * @returns {Promise<http.Server>}
 */
export function startServer({ port, host, logBody }) {
  let isShuttingDown = false;
  let forceTimer;

  const server = http.createServer(async (req, res) => {
    const startTime = Date.now();

    const result = handleRequest(
      { method: req.method, url: req.url },
      isShuttingDown,
    );

    // ── Local error path ──────────────────────────────────
    if (result !== null) {
      const { statusCode, headers, body } = result;
      res.writeHead(statusCode, headers);
      res.end(body);

      const emitRequestLog = createRequestLogEmitter({ req, startTime });
      emitRequestLog(statusCode);
      return;
    }

    // ── Forwarding path ───────────────────────────────────

    // Phase 1: Buffer the request body
    let requestBody;
    try {
      requestBody = await bufferRequestBody(req);
    } catch (err) {
      // Client disconnected or stream error during buffering.
      // req and res share the same TCP socket (HTTP/1.1), so the socket
      // is almost certainly dead. Destroy res explicitly for cleanup.
      if (!res.destroyed) {
        res.destroy();
      }
      process.stderr.write(
        `${JSON.stringify({
          timestamp: new Date().toISOString(),
          level: "ERROR",
          event: "client_error",
          method: req.method,
          url: req.url,
          message: err.code || err.message,
          duration_ms: Date.now() - startTime,
        })}\n`,
      );
      const emitRequestLog = createRequestLogEmitter({
        req,
        requestBody: logBody ? Buffer.alloc(0) : undefined,
        startTime,
      });
      emitRequestLog(499);
      return;
    }

    // Phase 2: Forward to upstream
    // eslint-disable-next-line prefer-const -- assigned after event handlers to eliminate race conditions (RFC-004 §4.2.7)
    let upstreamReq;
    let upstreamResponseReceived = false;

    const emitRequestLog = createRequestLogEmitter({
      req,
      requestBody: logBody ? requestBody : undefined,
      startTime,
    });

    // Client disconnect detection (post-buffering, pre/during upstream response)
    res.on("close", () => {
      if (!res.writableFinished) {
        abortUpstream(upstreamReq);
        if (!upstreamResponseReceived) {
          process.stderr.write(
            `${JSON.stringify({
              timestamp: new Date().toISOString(),
              level: "WARN",
              event: "client_disconnect",
              method: req.method,
              url: req.url,
              duration_ms: Date.now() - startTime,
            })}\n`,
          );
          emitRequestLog(499);
        }
      }
    });

    // Build upstream request
    const upstreamUrl = buildUpstreamUrl(UPSTREAM_BASE_URL, req.url);
    const upstreamHeaders = buildUpstreamHeaders(req.headers);

    upstreamReq = https.request(
      upstreamUrl,
      { method: "POST", headers: upstreamHeaders },
      (upstreamRes) => {
        upstreamResponseReceived = true;

        const responseHeaders = filterResponseHeaders(upstreamRes.headers);
        responseHeaders["x-accel-buffering"] = "no";
        if (isShuttingDown) {
          responseHeaders["connection"] = "close";
        }

        res.writeHead(upstreamRes.statusCode, responseHeaders);

        pipeline(upstreamRes, res, (err) => {
          if (err) {
            process.stderr.write(
              `${JSON.stringify({
                timestamp: new Date().toISOString(),
                level: "ERROR",
                event: "upstream_error",
                method: req.method,
                url: req.url,
                message: err.code || err.message,
                duration_ms: Date.now() - startTime,
              })}\n`,
            );
          }
          emitRequestLog(upstreamRes.statusCode, upstreamRes.headers);
        });
      },
    );

    // Upstream connection error
    upstreamReq.on("error", (err) => {
      process.stderr.write(
        `${JSON.stringify({
          timestamp: new Date().toISOString(),
          level: "ERROR",
          event: "upstream_error",
          method: req.method,
          url: req.url,
          message: err.code || err.message,
          duration_ms: Date.now() - startTime,
        })}\n`,
      );

      if (!res.headersSent && !res.destroyed) {
        const errorHeaders = { "content-type": "application/json" };
        if (isShuttingDown) {
          errorHeaders["connection"] = "close";
        }
        res.writeHead(502, errorHeaders);
        res.end(
          JSON.stringify({
            type: "error",
            error: {
              type: "api_error",
              message: `upstream connection failed: ${err.code || err.message}`,
            },
          }),
        );
      }

      emitRequestLog(502);
    });

    upstreamReq.end(requestBody);
  });

  function shutdown() {
    if (isShuttingDown) return;
    isShuttingDown = true;

    process.stdout.write(
      `${JSON.stringify({
        timestamp: new Date().toISOString(),
        level: "INFO",
        event: "shutdown",
        message: "stop accepting new connections",
      })}\n`,
    );

    server.close();
    server.closeIdleConnections();

    forceTimer = setTimeout(() => {
      process.stderr.write(
        `${JSON.stringify({
          timestamp: new Date().toISOString(),
          level: "WARN",
          event: "shutdown",
          message: "timed out, forcing close",
        })}\n`,
      );
      server.closeAllConnections();
      process.exit(1);
    }, SHUTDOWN_TIMEOUT_MS);
    forceTimer.unref();
  }

  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);

  server.on("close", () => {
    clearTimeout(forceTimer);
    process.removeListener("SIGINT", shutdown);
    process.removeListener("SIGTERM", shutdown);
    if (isShuttingDown) {
      process.stdout.write(
        `${JSON.stringify({
          timestamp: new Date().toISOString(),
          level: "INFO",
          event: "shutdown",
          message: "complete",
        })}\n`,
      );
    }
  });

  return new Promise((resolve, reject) => {
    function onStartupError(err) {
      process.removeListener("SIGINT", shutdown);
      process.removeListener("SIGTERM", shutdown);
      reject(err);
    }

    server.once("error", onStartupError);

    server.listen(port, host, () => {
      server.removeListener("error", onStartupError);

      server.on("error", (err) => {
        process.stderr.write(
          `${JSON.stringify({
            timestamp: new Date().toISOString(),
            level: "ERROR",
            event: "server_error",
            message: err.message,
          })}\n`,
        );
      });

      const addr = server.address();
      process.stdout.write(
        `${JSON.stringify({
          timestamp: new Date().toISOString(),
          level: "INFO",
          event: "startup",
          host: addr.address,
          port: addr.port,
        })}\n`,
      );
      resolve(server);
    });
  });
}
