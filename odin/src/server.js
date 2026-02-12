import http from 'node:http';

import { anthropicToGoogle, streamSSEResponse } from './converter.js';
import { sendRequest } from './cloudcode.js';

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
        'Content-Length': Buffer.byteLength(payload),
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
            message,
        },
    });
}

// ─── Server Factory ─────────────────────────────────────────────────────────

/**
 * Create and return the Odin HTTP server.
 *
 * @param {Object} options
 * @param {string} options.apiKey - Cloud Code API key
 * @param {boolean} options.debug - Enable debug logging
 * @param {import('./logger.js').RequestLogger} options.logger - Request log writer (required)
 * @returns {http.Server}
 */
export function createServer({ apiKey, debug, logger }) {
    const server = http.createServer(async (req, res) => {
        const body = await readBody(req);
        const startTime = Date.now();

        const { method, url } = req;
        // Strip query string for route matching
        const path = url.split('?')[0];

        // ── Strict Routing ──────────────────────────────────────────────

        if (method === 'GET' && path === '/health') {
            // Health check
            sendJSON(res, 200, { status: 'ok' });
            logger.log({ req, body, statusCode: 200, startTime });

            return;
        }

        if (method === 'POST' && path === '/v1/messages/count_tokens') {
            // Token counting endpoint — recognized but not implemented
            sendError(
                res,
                501,
                'not_implemented_error',
                'The /v1/messages/count_tokens endpoint is not implemented.',
            );
            logger.log({ req, body, statusCode: 501, startTime });

            return;
        }

        if (method === 'POST' && path === '/v1/messages') {
            // ── Main Anthropic Messages API endpoint ─────────────────────

            // Validate request body
            if (!body) {
                sendError(res, 400, 'invalid_request_error', 'Request body is required');
                logger.log({ req, body, statusCode: 400, startTime });

                return;
            }

            // Only streaming mode is supported (§3.4)
            if (!body.stream) {
                sendError(
                    res,
                    400,
                    'invalid_request_error',
                    'Only streaming mode is supported. Set "stream": true in your request.',
                );
                logger.log({ req, body, statusCode: 400, startTime });

                return;
            }

            // Model is required per Anthropic API spec
            if (!body.model) {
                sendError(res, 400, 'invalid_request_error', '"model" is required.');
                logger.log({ req, body, statusCode: 400, startTime });

                return;
            }

            try {
                // 1. Convert Anthropic request to Google format
                const model = body.model;
                const googleRequest = anthropicToGoogle(body);

                // Capture debug info for log file (stderr debug output is unchanged)
                const debugInfo = debug ? { googleRequest } : undefined;

                if (debug) {
                    console.error(
                        `[Odin:debug] Converted Google request:`,
                        JSON.stringify(googleRequest, null, 2),
                    );
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
                        console.error(
                            `[Odin:debug] Cloud Code error (${cloudResponse.status}):`,
                            errorBody,
                        );
                    }

                    sendError(res, statusCode, errorType, errorMessage);

                    // Log with Cloud Code error details
                    logger.log({
                        req,
                        body,
                        statusCode,
                        startTime,
                        error: {
                            source: 'cloud_code',
                            upstreamStatus: cloudResponse.status,
                            message: errorMessage,
                            rawBody: errorBody,
                        },
                        debug: debugInfo,
                    });

                    return;
                }

                // 4. Stream SSE response back to client
                res.writeHead(200, {
                    'Content-Type': 'text/event-stream',
                    'Cache-Control': 'no-cache',
                    Connection: 'keep-alive',
                });

                const events = debug ? [] : undefined;
                for await (const sseEvent of streamSSEResponse(cloudResponse.body, model, debug)) {
                    res.write(sseEvent);
                    if (events) {
                        // Parse "event: xxx\ndata: {...}\n\n" into structured form
                        const lines = sseEvent.trim().split('\n');
                        const eventName = lines[0]?.slice(7); // strip "event: "
                        const eventData = lines[1]
                            ? JSON.parse(lines[1].slice(6)) // strip "data: "
                            : null;
                        events.push({ event: eventName, data: eventData });
                    }
                }

                res.end();
                logger.log({
                    req,
                    body,
                    statusCode: 200,
                    startTime,
                    response: events ? { events } : undefined,
                    debug: debugInfo,
                });
            } catch (err) {
                // Network errors or unexpected failures
                console.error(`[Odin] Error processing /v1/messages:`, err.message);
                if (debug) {
                    console.error(`[Odin:debug] Full error:`, err);
                }

                // If headers haven't been sent yet, send a proper error response
                if (!res.headersSent) {
                    sendError(res, 500, 'api_error', `Internal proxy error: ${err.message}`);
                    logger.log({
                        req,
                        body,
                        statusCode: 500,
                        startTime,
                        error: {
                            source: 'internal',
                            message: err.message,
                            stack: err.stack,
                        },
                    });
                } else {
                    // Headers already sent (streaming in progress), just end the response
                    res.end();
                    logger.log({
                        req,
                        body,
                        statusCode: 200,
                        startTime,
                        error: {
                            source: 'internal',
                            message: err.message,
                            stack: err.stack,
                        },
                    });
                }
            }

            return;
        }

        if (method === 'POST' && path === '/') {
            // Silent handler for Claude Code heartbeat
            sendJSON(res, 200, {});
            logger.log({ req, body, statusCode: 200, startTime });

            return;
        }

        if (method === 'POST' && path === '/api/event_logging/batch') {
            // Silent handler for Claude Code telemetry
            sendJSON(res, 200, {});
            logger.log({ req, body, statusCode: 200, startTime });

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
        logger.log({ req, body, statusCode: 404, startTime });
    });

    return server;
}
