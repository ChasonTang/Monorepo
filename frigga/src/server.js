import http from 'node:http';
import crypto from 'node:crypto';
import { SHUTDOWN_TIMEOUT_MS } from './constants.js';

const VALID_ROUTES = new Set(['/v1/messages', '/v1/messages/count_tokens']);

/**
 * Pure request handler function.
 * @param {{ method: string, url: string, headers: Record<string, string> }} req
 * @param {Buffer} apiKeyHash - Pre-computed SHA-256 digest of the configured API key
 * @param {boolean} isShuttingDown
 * @returns {{ statusCode: number, headers: Record<string, string>, body: string }}
 */
export function handleRequest({ method, url, headers }, apiKeyHash, isShuttingDown) {
  const pathname = new URL(url, 'http://localhost').pathname;

  const responseHeaders = { 'Content-Type': 'application/json' };
  if (isShuttingDown) {
    responseHeaders['Connection'] = 'close';
  }

  // Route matching
  if (!VALID_ROUTES.has(pathname)) {
    return {
      statusCode: 404,
      headers: responseHeaders,
      body: JSON.stringify({
        type: 'error',
        error: { type: 'not_found_error', message: 'The requested URL was not found' }
      }),
    };
  }

  // Method validation
  if (method !== 'POST') {
    responseHeaders['Allow'] = 'POST';
    return {
      statusCode: 405,
      headers: responseHeaders,
      body: JSON.stringify({
        type: 'error',
        error: { type: 'invalid_request_error', message: 'Method not allowed' }
      }),
    };
  }

  // Authorization validation
  const authHeader = headers['authorization'];
  if (!authHeader) {
    return {
      statusCode: 401,
      headers: responseHeaders,
      body: JSON.stringify({
        type: 'error',
        error: { type: 'authentication_error', message: 'Missing Authorization header' }
      }),
    };
  }

  if (!authHeader.startsWith('Bearer ')) {
    return {
      statusCode: 401,
      headers: responseHeaders,
      body: JSON.stringify({
        type: 'error',
        error: { type: 'authentication_error', message: 'Malformed Authorization header' }
      }),
    };
  }

  const token = authHeader.slice(7);
  if (token.length === 0) {
    return {
      statusCode: 401,
      headers: responseHeaders,
      body: JSON.stringify({
        type: 'error',
        error: { type: 'authentication_error', message: 'Empty Bearer token' }
      }),
    };
  }

  const tokenHash = crypto.createHash('sha256').update(token).digest();
  if (!crypto.timingSafeEqual(tokenHash, apiKeyHash)) {
    return {
      statusCode: 401,
      headers: responseHeaders,
      body: JSON.stringify({
        type: 'error',
        error: { type: 'authentication_error', message: 'Invalid API key' }
      }),
    };
  }

  // Current phase: 501 Not Implemented
  return {
    statusCode: 501,
    headers: responseHeaders,
    body: JSON.stringify({
      type: 'error',
      error: { type: 'not_implemented', message: 'Upstream forwarding is not yet implemented' }
    }),
  };
}

/**
 * Start the HTTP proxy server.
 * @param {{ port: number, host: string, apiKey: string }} config
 * @returns {Promise<http.Server>}
 */
export function startServer({ port, host, apiKey }) {
  const apiKeyHash = crypto.createHash('sha256').update(apiKey).digest();
  let isShuttingDown = false;
  let forceTimer;

  const server = http.createServer((req, res) => {
    const startTime = Date.now();

    const result = handleRequest(
      { method: req.method, url: req.url, headers: req.headers },
      apiKeyHash,
      isShuttingDown
    );

    res.writeHead(result.statusCode, result.headers);
    res.end(result.body, () => {
      const duration = Date.now() - startTime;
      const timestamp = new Date().toISOString();
      process.stdout.write(
        `[${timestamp}] [INFO] ${req.method} ${req.url} ${result.statusCode} ${duration}ms\n`
      );
    });
  });

  function shutdown() {
    if (isShuttingDown) return;
    isShuttingDown = true;

    const timestamp = new Date().toISOString();
    process.stdout.write(
      `[${timestamp}] [INFO] Shutting down: stop accepting new connections\n`
    );

    server.close();
    server.closeIdleConnections();

    forceTimer = setTimeout(() => {
      const ts = new Date().toISOString();
      process.stderr.write(`[${ts}] [WARN] Shutdown timed out, forcing close\n`);
      server.closeAllConnections();
      process.exit(1);
    }, SHUTDOWN_TIMEOUT_MS);
    forceTimer.unref();
  }

  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);

  server.on('close', () => {
    clearTimeout(forceTimer);
    process.removeListener('SIGINT', shutdown);
    process.removeListener('SIGTERM', shutdown);
    if (isShuttingDown) {
      const ts = new Date().toISOString();
      process.stdout.write(`[${ts}] [INFO] Shutdown complete\n`);
    }
  });

  return new Promise((resolve, reject) => {
    function onStartupError(err) {
      process.removeListener('SIGINT', shutdown);
      process.removeListener('SIGTERM', shutdown);
      reject(err);
    }

    server.once('error', onStartupError);

    server.listen(port, host, () => {
      server.removeListener('error', onStartupError);

      server.on('error', (err) => {
        const timestamp = new Date().toISOString();
        process.stderr.write(
          `[${timestamp}] [ERROR] Server error: ${err.message}\n`
        );
      });

      const timestamp = new Date().toISOString();
      const addr = server.address();
      process.stdout.write(
        `[${timestamp}] [INFO] Frigga listening on ${addr.address}:${addr.port}\n`
      );
      resolve(server);
    });
  });
}
