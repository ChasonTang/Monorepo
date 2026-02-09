import http from 'node:http';

import { anthropicToGoogle, streamSSEResponse } from './converter.js';
import { sendRequest } from './cloudcode.js';

// ─── Request Logging ────────────────────────────────────────────────────────

/**
 * Log an incoming request summary to stderr. In debug mode, also log full
 * headers and body.
 *
 * @param {http.IncomingMessage} req
 * @param {Object|null} body - Parsed request body (if available)
 * @param {boolean} debug
 * @returns {number} Start timestamp for duration calculation
 */
function logRequest(req, body, debug) {
    const start = Date.now();
    // Summary line is deferred until response is sent (see logResponse)
    if (debug) {
        console.error(`[Odin:debug] Headers:`, JSON.stringify(req.headers, null, 2));
        if (body) {
            console.error(`[Odin:debug] Body:`, JSON.stringify(body, null, 2));
        }
    }
    return start;
}

/**
 * Log the response summary line to stderr.
 *
 * @param {http.IncomingMessage} req
 * @param {number} statusCode
 * @param {number} startTime
 * @param {string} [suffix] - Optional suffix (e.g., "← UNKNOWN ENDPOINT")
 */
function logResponse(req, statusCode, startTime, suffix) {
    const duration = Date.now() - startTime;
    const line = `[Odin] ${new Date().toISOString()} ${req.method} ${req.url} ${statusCode} ${duration}ms`;
    console.error(suffix ? `${line}  ${suffix}` : line);
}

// ─── Body Parsing ───────────────────────────────────────────────────────────

/**
 * Read and parse JSON request body.
 *
 * @param {http.IncomingMessage} req
 * @returns {Promise<Object|null>} Parsed JSON or null if empty/invalid
 */
function readBody(req) {
    return new Promise((resolve) => {
        const chunks = [];
        req.on('data', (chunk) => chunks.push(chunk));
        req.on('end', () => {
            const raw = Buffer.concat(chunks).toString();
            if (!raw) {
                resolve(null);
                return;
            }
            try {
                resolve(JSON.parse(raw));
            } catch {
                resolve(null);
            }
        });
        req.on('error', () => resolve(null));
    });
}

// ─── Response Helpers ───────────────────────────────────────────────────────

/**
 * Send a JSON response.
 *
 * @param {http.ServerResponse} res
 * @param {number} statusCode
 * @param {Object} body
 */
function sendJSON(res, statusCode, body) {
    const payload = JSON.stringify(body);
    res.writeHead(statusCode, {
        'Content-Type': 'application/json',
        'Content-Length': Buffer.byteLength(payload)
    });
    res.end(payload);
}

/**
 * Send an Anthropic-format error response.
 *
 * @param {http.ServerResponse} res
 * @param {number} statusCode
 * @param {string} errorType
 * @param {string} message
 */
function sendError(res, statusCode, errorType, message) {
    sendJSON(res, statusCode, {
        type: 'error',
        error: {
            type: errorType,
            message
        }
    });
}

// ─── Server Factory ─────────────────────────────────────────────────────────

/**
 * Create and return the Odin HTTP server.
 *
 * @param {Object} options
 * @param {string} options.apiKey - Cloud Code API key
 * @param {boolean} options.debug - Enable debug logging
 * @param {import('./logger.js').RequestLogger|null} options.logger - Request log writer (optional)
 * @returns {http.Server}
 */
export function createServer({ apiKey, debug, logger }) {
    const server = http.createServer(async (req, res) => {
        const body = await readBody(req);
        const startTime = logRequest(req, body, debug);

        const { method, url } = req;
        // Strip query string for route matching
        const path = url.split('?')[0];

        // ── Strict Routing ──────────────────────────────────────────────

        if (method === 'GET' && path === '/health') {
            // Health check
            sendJSON(res, 200, { status: 'ok' });
            logResponse(req, 200, startTime);
            logger?.log({ req, body, statusCode: 200, startTime });
            return;
        }

        if (method === 'POST' && path === '/v1/messages') {
            // ── Main Anthropic Messages API endpoint ─────────────────────

            // Validate request body
            if (!body) {
                sendError(res, 400, 'invalid_request_error', 'Request body is required');
                logResponse(req, 400, startTime);
                logger?.log({ req, body, statusCode: 400, startTime });
                return;
            }

            // Only streaming mode is supported (§3.4)
            if (!body.stream) {
                sendError(res, 400, 'invalid_request_error', 'Only streaming mode is supported. Set "stream": true in your request.');
                logResponse(req, 400, startTime);
                logger?.log({ req, body, statusCode: 400, startTime });
                return;
            }

            try {
                // 1. Convert Anthropic request to Google format
                const model = body.model || 'claude-sonnet-4-5-thinking';
                const googleRequest = anthropicToGoogle(body);

                if (debug) {
                    console.error(`[Odin:debug] Converted Google request:`, JSON.stringify(googleRequest, null, 2));
                }

                // 2. Send to Cloud Code API
                const cloudResponse = await sendRequest(googleRequest, model, apiKey, debug);

                // 3. Handle upstream error responses
                if (!cloudResponse.ok) {
                    const errorBody = await cloudResponse.text();
                    let errorMessage = `Cloud Code API error: ${cloudResponse.status}`;
                    let errorType = 'api_error';
                    let statusCode = 500;

                    try {
                        const errorJson = JSON.parse(errorBody);
                        errorMessage = errorJson.error?.message || errorJson.message || errorBody;
                    } catch {
                        errorMessage = errorBody || errorMessage;
                    }

                    // Map upstream HTTP status to Anthropic error types
                    if (cloudResponse.status === 401 || cloudResponse.status === 403) {
                        errorType = 'authentication_error';
                        statusCode = 401;
                    } else if (cloudResponse.status === 429) {
                        errorType = 'rate_limit_error';
                        statusCode = 429;
                    } else if (cloudResponse.status === 400) {
                        errorType = 'invalid_request_error';
                        statusCode = 400;
                    }

                    if (debug) {
                        console.error(`[Odin:debug] Cloud Code error (${cloudResponse.status}):`, errorBody);
                    }

                    sendError(res, statusCode, errorType, errorMessage);
                    logResponse(req, statusCode, startTime);
                    logger?.log({ req, body, statusCode, startTime });
                    return;
                }

                // 4. Stream SSE response back to client
                res.writeHead(200, {
                    'Content-Type': 'text/event-stream',
                    'Cache-Control': 'no-cache',
                    'Connection': 'keep-alive'
                });

                for await (const sseEvent of streamSSEResponse(cloudResponse.body, model, debug)) {
                    res.write(sseEvent);
                }

                res.end();
                logResponse(req, 200, startTime);
                logger?.log({ req, body, statusCode: 200, startTime });

            } catch (err) {
                // Network errors or unexpected failures
                console.error(`[Odin] Error processing /v1/messages:`, err.message);
                if (debug) {
                    console.error(`[Odin:debug] Full error:`, err);
                }

                // If headers haven't been sent yet, send a proper error response
                if (!res.headersSent) {
                    sendError(res, 500, 'api_error', `Internal proxy error: ${err.message}`);
                    logResponse(req, 500, startTime);
                    logger?.log({ req, body, statusCode: 500, startTime });
                } else {
                    // Headers already sent (streaming in progress), just end the response
                    res.end();
                    logResponse(req, 200, startTime);
                    logger?.log({ req, body, statusCode: 200, startTime });
                }
            }
            return;
        }

        if (method === 'POST' && path === '/') {
            // Silent handler for Claude Code heartbeat
            sendJSON(res, 200, {});
            logResponse(req, 200, startTime);
            logger?.log({ req, body, statusCode: 200, startTime });
            return;
        }

        if (method === 'POST' && path === '/api/event_logging/batch') {
            // Silent handler for Claude Code telemetry
            sendJSON(res, 200, {});
            logResponse(req, 200, startTime);
            logger?.log({ req, body, statusCode: 200, startTime });
            return;
        }

        // ── Unknown Endpoint (404) ──────────────────────────────────────

        // Log full request for debugging unknown endpoints
        if (debug) {
            console.error(`[Odin:debug] Unknown endpoint hit: ${method} ${path}`);
            console.error(`[Odin:debug] Headers:`, JSON.stringify(req.headers, null, 2));
            if (body) {
                console.error(`[Odin:debug] Body:`, JSON.stringify(body, null, 2));
            }
        }

        sendError(res, 404, 'not_found_error', `Unknown endpoint: ${method} ${path}`);
        logResponse(req, 404, startTime, '← UNKNOWN ENDPOINT');
        logger?.log({ req, body, statusCode: 404, startTime });
    });

    return server;
}
