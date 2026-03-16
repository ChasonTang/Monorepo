import http from "node:http";
import crypto from "node:crypto";
import { SHUTDOWN_TIMEOUT_MS } from "./constants.js";

const VALID_ROUTES = new Set(["/v1/messages", "/v1/messages/count_tokens"]);

/**
 * Pure request handler function.
 * @param {{ method: string, url: string, headers: Record<string, string> }} req
 * @param {Buffer} apiKeyHash - Pre-computed SHA-256 digest of the configured API key
 * @param {boolean} isShuttingDown
 * @returns {{ statusCode: number, headers: Record<string, string>, body: string }}
 */
export function handleRequest(
  { method, url, headers },
  apiKeyHash,
  isShuttingDown,
) {
  const pathname = new URL(url, "http://localhost").pathname;

  const responseHeaders = { "Content-Type": "application/json" };
  if (isShuttingDown) {
    responseHeaders["Connection"] = "close";
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

  // Authorization validation
  const authHeader = headers["authorization"];
  if (!authHeader) {
    return {
      statusCode: 401,
      headers: responseHeaders,
      body: JSON.stringify({
        type: "error",
        error: {
          type: "authentication_error",
          message: "Missing Authorization header",
        },
      }),
    };
  }

  if (!authHeader.startsWith("Bearer ")) {
    return {
      statusCode: 401,
      headers: responseHeaders,
      body: JSON.stringify({
        type: "error",
        error: {
          type: "authentication_error",
          message: "Malformed Authorization header",
        },
      }),
    };
  }

  const token = authHeader.slice(7);
  if (token.length === 0) {
    return {
      statusCode: 401,
      headers: responseHeaders,
      body: JSON.stringify({
        type: "error",
        error: { type: "authentication_error", message: "Empty Bearer token" },
      }),
    };
  }

  const tokenHash = crypto.createHash("sha256").update(token).digest();
  if (!crypto.timingSafeEqual(tokenHash, apiKeyHash)) {
    return {
      statusCode: 401,
      headers: responseHeaders,
      body: JSON.stringify({
        type: "error",
        error: { type: "authentication_error", message: "Invalid API key" },
      }),
    };
  }

  // Current phase: 501 Not Implemented
  return {
    statusCode: 501,
    headers: responseHeaders,
    body: JSON.stringify({
      type: "error",
      error: {
        type: "not_implemented",
        message: "Upstream forwarding is not yet implemented",
      },
    }),
  };
}

/**
 * Start the HTTP proxy server.
 * @param {{ port: number, host: string, apiKey: string }} config
 * @returns {Promise<http.Server>}
 */
export function startServer({ port, host, apiKey }) {
  const apiKeyHash = crypto.createHash("sha256").update(apiKey).digest();
  let isShuttingDown = false;
  let forceTimer;

  const server = http.createServer((req, res) => {
    const startTime = Date.now();
    const chunks = [];
    let errored = false;

    req.on("data", (chunk) => {
      chunks.push(chunk);
    });

    req.on("error", (err) => {
      errored = true;
      res.destroy();
      process.stderr.write(
        `${JSON.stringify({
          timestamp: new Date().toISOString(),
          level: "ERROR",
          event: "request_error",
          method: req.method,
          url: req.url,
          request_headers: req.headers,
          message: err.message,
        })}\n`,
      );
    });

    req.on("end", () => {
      if (errored) return;
      const requestBody = Buffer.concat(chunks).toString("utf-8");

      const result = handleRequest(
        { method: req.method, url: req.url, headers: req.headers },
        apiKeyHash,
        isShuttingDown,
      );

      for (const [name, value] of Object.entries(result.headers)) {
        res.setHeader(name, value);
      }
      res.writeHead(result.statusCode);
      res.end(result.body, () => {
        process.stdout.write(
          `${JSON.stringify({
            timestamp: new Date().toISOString(),
            level: "INFO",
            event: "request",
            method: req.method,
            url: req.url,
            status: result.statusCode,
            duration_ms: Date.now() - startTime,
            request_headers: req.headers,
            request_body: requestBody,
            response_headers: res.getHeaders(),
            response_body: result.body,
          })}\n`,
        );
      });
    });
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
