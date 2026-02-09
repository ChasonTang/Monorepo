import crypto from 'node:crypto';

import { STREAMING_URL, PROJECT_ID, buildHeaders } from './constants.js';

// ─── Logging ─────────────────────────────────────────────────────────────────

/**
 * Log the upstream request details in debug mode.
 *
 * @param {string} url - Target URL
 * @param {Object} headers - Request headers
 * @param {Object} payload - Request payload
 * @param {boolean} debug
 */
function logUpstream(url, headers, payload, debug) {
    if (!debug) return;
    console.error(`[Odin:debug] → Cloud Code: ${url}`);
    console.error(`[Odin:debug] → Headers:`, JSON.stringify(headers, null, 2));
    console.error(`[Odin:debug] → Payload:`, JSON.stringify(payload, null, 2));
}

// ─── Cloud Code API Client ──────────────────────────────────────────────────

/**
 * Send a request to the Cloud Code API and return the response.
 *
 * Builds the Cloud Code request wrapper around the Google-format request,
 * sets all required headers, and returns the raw Response object so the
 * caller can consume the SSE stream.
 *
 * @param {Object} googleRequest - Converted Google Generative AI format request
 * @param {string} model - Model name (e.g. "claude-sonnet-4-5-thinking")
 * @param {string} apiKey - Bearer token for Cloud Code authentication
 * @param {boolean} debug - Enable debug logging
 * @returns {Promise<Response>} Raw fetch Response (body is a ReadableStream)
 * @throws {Error} If the fetch itself fails (network error)
 */
export async function sendRequest(googleRequest, model, apiKey, debug) {
    const headers = buildHeaders(apiKey, model);

    // Build Cloud Code request wrapper (§2.5.4)
    const payload = {
        project: PROJECT_ID,
        model,
        request: googleRequest,
        userAgent: 'antigravity',
        requestType: 'agent',
        requestId: `agent-${crypto.randomUUID()}`
    };

    logUpstream(STREAMING_URL, headers, payload, debug);

    const response = await fetch(STREAMING_URL, {
        method: 'POST',
        headers,
        body: JSON.stringify(payload)
    });

    return response;
}
