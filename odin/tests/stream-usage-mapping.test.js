/**
 * Tests for RFC-013: Usage Token Mapping Correction
 *
 * Verifies:
 * - promptTokenCount → input_tokens direct mapping (no subtraction)
 * - candidatesTokenCount → output_tokens mapping
 * - cache_read_input_tokens and cache_creation_input_tokens hardcoded to 0
 * - Unified four-field usage shape across message_start and message_delta
 * - All 5 scenarios from RFC §6
 */

import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { streamSSEResponse } from '../src/converter.js';

// ─── Helpers ─────────────────────────────────────────────────────────────────

function makeSSELine(obj) {
    return `data: ${JSON.stringify(obj)}\n\n`;
}

function makeGoogleEvent({
    parts = [{ text: 'hello' }],
    usage = null,
    finishReason = null,
    responseId = null,
}) {
    const event = {};
    if (responseId) event.responseId = responseId;
    event.candidates = [{ content: { parts } }];
    if (finishReason) event.candidates[0].finishReason = finishReason;
    if (usage) event.usageMetadata = usage;

    return event;
}

function createStream(sseLines) {
    const text = sseLines.join('');
    const encoder = new TextEncoder();

    return new ReadableStream({
        start(controller) {
            controller.enqueue(encoder.encode(text));
            controller.close();
        },
    });
}

async function collectEvents(stream, model = 'test-model') {
    const results = [];
    for await (const sse of streamSSEResponse(stream, model)) {
        const lines = sse.trim().split('\n');
        const eventLine = lines.find((l) => l.startsWith('event: '));
        const dataLine = lines.find((l) => l.startsWith('data: '));
        if (eventLine && dataLine) {
            results.push({
                event: eventLine.slice(7),
                data: JSON.parse(dataLine.slice(6)),
            });
        }
    }

    return results;
}

function findEvent(events, type) {
    return events.find((e) => e.event === type);
}

const EXPECTED_CACHE_FIELDS = {
    cache_read_input_tokens: 0,
    cache_creation_input_tokens: 0,
};

// ─── RFC-013 §6 Test Scenarios ───────────────────────────────────────────────

describe('streamSSEResponse usage token mapping (RFC-013)', () => {
    it('Scenario 1: standard response — single event', async () => {
        const stream = createStream([
            makeSSELine(
                makeGoogleEvent({
                    responseId: 'resp-1',
                    usage: {
                        promptTokenCount: 100,
                        candidatesTokenCount: 50,
                        totalTokenCount: 150,
                    },
                    finishReason: 'STOP',
                }),
            ),
        ]);

        const events = await collectEvents(stream);
        const start = findEvent(events, 'message_start');
        const delta = findEvent(events, 'message_delta');

        assert.deepStrictEqual(start.data.message.usage, {
            input_tokens: 100,
            output_tokens: 50,
            ...EXPECTED_CACHE_FIELDS,
        });
        assert.deepStrictEqual(delta.data.usage, {
            input_tokens: 100,
            output_tokens: 50,
            ...EXPECTED_CACHE_FIELDS,
        });
    });

    it('Scenario 2: multi-event stream — tokens accumulate', async () => {
        const stream = createStream([
            makeSSELine(
                makeGoogleEvent({
                    responseId: 'resp-2',
                    usage: { promptTokenCount: 100, candidatesTokenCount: 5, totalTokenCount: 105 },
                }),
            ),
            makeSSELine(
                makeGoogleEvent({
                    usage: {
                        promptTokenCount: 100,
                        candidatesTokenCount: 80,
                        totalTokenCount: 180,
                    },
                    finishReason: 'STOP',
                }),
            ),
        ]);

        const events = await collectEvents(stream);
        const start = findEvent(events, 'message_start');
        const delta = findEvent(events, 'message_delta');

        assert.deepStrictEqual(start.data.message.usage, {
            input_tokens: 100,
            output_tokens: 5,
            ...EXPECTED_CACHE_FIELDS,
        });
        assert.deepStrictEqual(delta.data.usage, {
            input_tokens: 100,
            output_tokens: 80,
            ...EXPECTED_CACHE_FIELDS,
        });
    });

    it('Scenario 3: zero output tokens', async () => {
        const stream = createStream([
            makeSSELine(
                makeGoogleEvent({
                    responseId: 'resp-3',
                    usage: { promptTokenCount: 200, candidatesTokenCount: 0, totalTokenCount: 200 },
                    finishReason: 'STOP',
                }),
            ),
        ]);

        const events = await collectEvents(stream);
        const start = findEvent(events, 'message_start');
        const delta = findEvent(events, 'message_delta');

        assert.deepStrictEqual(start.data.message.usage, {
            input_tokens: 200,
            output_tokens: 0,
            ...EXPECTED_CACHE_FIELDS,
        });
        assert.deepStrictEqual(delta.data.usage, {
            input_tokens: 200,
            output_tokens: 0,
            ...EXPECTED_CACHE_FIELDS,
        });
    });

    it('Scenario 4: unknown extra field (cachedContentTokenCount) is ignored', async () => {
        const stream = createStream([
            makeSSELine(
                makeGoogleEvent({
                    responseId: 'resp-4',
                    usage: {
                        promptTokenCount: 100,
                        candidatesTokenCount: 50,
                        totalTokenCount: 150,
                        cachedContentTokenCount: 30,
                    },
                    finishReason: 'STOP',
                }),
            ),
        ]);

        const events = await collectEvents(stream);
        const start = findEvent(events, 'message_start');
        const delta = findEvent(events, 'message_delta');

        assert.deepStrictEqual(start.data.message.usage, {
            input_tokens: 100,
            output_tokens: 50,
            ...EXPECTED_CACHE_FIELDS,
        });
        assert.deepStrictEqual(delta.data.usage, {
            input_tokens: 100,
            output_tokens: 50,
            ...EXPECTED_CACHE_FIELDS,
        });
    });

    it('Scenario 5: usage missing from event — accumulators retain previous values', async () => {
        const stream = createStream([
            makeSSELine(
                makeGoogleEvent({
                    responseId: 'resp-5',
                    usage: {
                        promptTokenCount: 100,
                        candidatesTokenCount: 10,
                        totalTokenCount: 110,
                    },
                }),
            ),
            makeSSELine(makeGoogleEvent({ finishReason: 'STOP' })),
        ]);

        const events = await collectEvents(stream);
        const delta = findEvent(events, 'message_delta');

        assert.deepStrictEqual(delta.data.usage, {
            input_tokens: 100,
            output_tokens: 10,
            ...EXPECTED_CACHE_FIELDS,
        });
    });

    it('message_start and message_delta emit identical four-field usage shape', async () => {
        const stream = createStream([
            makeSSELine(
                makeGoogleEvent({
                    responseId: 'resp-6',
                    usage: { promptTokenCount: 42, candidatesTokenCount: 7, totalTokenCount: 49 },
                    finishReason: 'STOP',
                }),
            ),
        ]);

        const events = await collectEvents(stream);
        const startUsage = findEvent(events, 'message_start').data.message.usage;
        const deltaUsage = findEvent(events, 'message_delta').data.usage;

        const expectedKeys = [
            'input_tokens',
            'output_tokens',
            'cache_read_input_tokens',
            'cache_creation_input_tokens',
        ];
        assert.deepStrictEqual(Object.keys(startUsage).sort(), expectedKeys.sort());
        assert.deepStrictEqual(Object.keys(deltaUsage).sort(), expectedKeys.sort());
    });
});
