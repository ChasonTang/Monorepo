/**
 * Tests for RFC-017: Fix stop_reason Precedence for Tool Use SSE Responses
 *
 * Covers all seven scenarios from the precedence table in §4.2.2.
 * Tests exercise the full streamSSEResponse pipeline by feeding simulated
 * Antigravity SSE payloads through a ReadableStream.
 */

import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { streamSSEResponse } from '../src/converter.js';

// ─── Helpers ─────────────────────────────────────────────────────────────────

function makeSSELine(parts, finishReason, responseId = 'req_test_001') {
    const candidate = {
        content: { role: 'model', parts },
    };
    if (finishReason) {
        candidate.finishReason = finishReason;
    }

    return `data: ${JSON.stringify({
        response: {
            candidates: [candidate],
            usageMetadata: { promptTokenCount: 10, candidatesTokenCount: 20 },
            modelVersion: 'test-model',
            responseId,
        },
    })}`;
}

function createSSEStream(lines) {
    const text = `${lines.join('\n')}\n`;

    return new ReadableStream({
        start(controller) {
            controller.enqueue(new TextEncoder().encode(text));
            controller.close();
        },
    });
}

async function getStopReason(stream) {
    let stopReason = null;
    for await (const chunk of streamSSEResponse(stream, 'test-model')) {
        const dataLine = chunk.split('\n')[1];
        if (!dataLine) continue;
        const data = JSON.parse(dataLine.replace('data: ', ''));
        if (data.type === 'message_delta') {
            stopReason = data.delta.stop_reason;
        }
    }

    return stopReason;
}

// ─── RFC-017 §4.2.2 Precedence Table ────────────────────────────────────────

describe('stop_reason precedence (RFC-017)', () => {
    it('§1 — tool call with STOP finish → tool_use', async () => {
        const stream = createSSEStream([
            makeSSELine(
                [{ functionCall: { name: 'Read', args: { path: '/tmp/x' }, id: 'toolu_001' } }],
                null,
            ),
            makeSSELine([{ text: '' }], 'STOP'),
        ]);
        assert.equal(await getStopReason(stream), 'tool_use');
    });

    it('§2 — tool call with OTHER finish → tool_use', async () => {
        const stream = createSSEStream([
            makeSSELine(
                [{ functionCall: { name: 'Read', args: { path: '/tmp/x' }, id: 'toolu_002' } }],
                null,
            ),
            makeSSELine([{ text: '' }], 'OTHER'),
        ]);
        assert.equal(await getStopReason(stream), 'tool_use');
    });

    it('§3 — tool call truncated by MAX_TOKENS → max_tokens', async () => {
        const stream = createSSEStream([
            makeSSELine(
                [{ functionCall: { name: 'Read', args: { path: '/tmp/x' }, id: 'toolu_003' } }],
                null,
            ),
            makeSSELine([{ text: '' }], 'MAX_TOKENS'),
        ]);
        assert.equal(await getStopReason(stream), 'max_tokens');
    });

    it('§4 — text-only with STOP finish → end_turn', async () => {
        const stream = createSSEStream([
            makeSSELine([{ text: 'Hello world' }], null),
            makeSSELine([{ text: '' }], 'STOP'),
        ]);
        assert.equal(await getStopReason(stream), 'end_turn');
    });

    it('§5 — text-only truncated by MAX_TOKENS → max_tokens', async () => {
        const stream = createSSEStream([
            makeSSELine([{ text: 'Hello world' }], null),
            makeSSELine([{ text: '' }], 'MAX_TOKENS'),
        ]);
        assert.equal(await getStopReason(stream), 'max_tokens');
    });

    it('§6 — text-only with no finishReason → end_turn (output default)', async () => {
        const stream = createSSEStream([makeSSELine([{ text: 'Hello world' }], null)]);
        assert.equal(await getStopReason(stream), 'end_turn');
    });

    it('§7 — tool call with no finishReason → tool_use', async () => {
        const stream = createSSEStream([
            makeSSELine(
                [{ functionCall: { name: 'Read', args: { path: '/tmp/x' }, id: 'toolu_007' } }],
                null,
            ),
        ]);
        assert.equal(await getStopReason(stream), 'tool_use');
    });
});
