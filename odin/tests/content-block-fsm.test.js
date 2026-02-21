/**
 * Tests for RFC-007: SSE Stream Converter — Finite State Machine Refactor
 *
 * Covers:
 * - classifyPart() input classification
 * - ContentBlockFSM process() state transitions
 * - ContentBlockFSM flush() terminal operation
 * - All 15 test scenarios from RFC §6
 */

import { describe, it, beforeEach } from 'node:test';
import assert from 'node:assert/strict';
import { ContentBlockFSM, classifyPart } from '../src/converter.js';

// ─── Helpers ─────────────────────────────────────────────────────────────────

function eventTypes(events) {
    return events.map((e) => e.event);
}

function deltaTypes(events) {
    return events.filter((e) => e.event === 'content_block_delta').map((e) => e.data.delta.type);
}

// ─── classifyPart ────────────────────────────────────────────────────────────

describe('classifyPart', () => {
    it('classifies thought part as thinking', () => {
        assert.equal(classifyPart({ thought: true, text: 'hmm' }), 'thinking');
    });

    it('classifies text part as text', () => {
        assert.equal(classifyPart({ text: 'hello' }), 'text');
    });

    it('classifies functionCall part as tool_use', () => {
        assert.equal(classifyPart({ functionCall: { name: 'f', args: {} } }), 'tool_use');
    });

    it('returns null for unknown part shape', () => {
        assert.equal(classifyPart({ unknownField: 123 }), null);
    });

    it('prioritizes thought over plain text', () => {
        assert.equal(classifyPart({ thought: true, text: 'x' }), 'thinking');
    });

    it('classifies empty text as text', () => {
        assert.equal(classifyPart({ text: '' }), 'text');
    });
});

// ─── RFC §6 Test Scenarios ───────────────────────────────────────────────────

describe('ContentBlockFSM', () => {
    let fsm;

    beforeEach(() => {
        fsm = new ContentBlockFSM();
    });

    // ── Scenario 1: Single thinking block ────────────────────────────────

    it('§6.1 — single thinking block emits start + delta', () => {
        const events = fsm.process({ thought: true, text: 'hmm' });

        assert.equal(events.length, 2);
        assert.equal(events[0].event, 'content_block_start');
        assert.equal(events[0].data.index, 0);
        assert.deepStrictEqual(events[0].data.content_block, {
            type: 'thinking',
            thinking: '',
        });
        assert.equal(events[1].event, 'content_block_delta');
        assert.equal(events[1].data.index, 0);
        assert.deepStrictEqual(events[1].data.delta, {
            type: 'thinking_delta',
            thinking: 'hmm',
        });
    });

    // ── Scenario 2: Single text block ────────────────────────────────────

    it('§6.2 — single text block emits start + delta', () => {
        const events = fsm.process({ text: 'hello' });

        assert.equal(events.length, 2);
        assert.equal(events[0].event, 'content_block_start');
        assert.deepStrictEqual(events[0].data.content_block, {
            type: 'text',
            text: '',
        });
        assert.equal(events[1].event, 'content_block_delta');
        assert.deepStrictEqual(events[1].data.delta, {
            type: 'text_delta',
            text: 'hello',
        });
    });

    // ── Scenario 3: Thinking → text transition ───────────────────────────

    it('§6.3 — thinking → text closes thinking and opens text', () => {
        const e1 = fsm.process({ thought: true, text: '...' });
        const e2 = fsm.process({ text: 'hi' });

        assert.deepStrictEqual(eventTypes(e1), ['content_block_start', 'content_block_delta']);
        assert.equal(e1[0].data.index, 0);

        assert.deepStrictEqual(eventTypes(e2), [
            'content_block_stop',
            'content_block_start',
            'content_block_delta',
        ]);
        assert.equal(e2[0].data.index, 0);
        assert.equal(e2[1].data.index, 1);
        assert.equal(e2[2].data.index, 1);
        assert.deepStrictEqual(e2[1].data.content_block, {
            type: 'text',
            text: '',
        });
    });

    // ── Scenario 4: Consecutive thinking (coalesce) ──────────────────────

    it('§6.4 — consecutive thinking parts coalesce into one block', () => {
        const e1 = fsm.process({ thought: true, text: 'a' });
        const e2 = fsm.process({ thought: true, text: 'b' });

        assert.deepStrictEqual(eventTypes(e1), ['content_block_start', 'content_block_delta']);
        assert.deepStrictEqual(eventTypes(e2), ['content_block_delta']);
        assert.equal(e2[0].data.index, 0);
        assert.deepStrictEqual(e2[0].data.delta, {
            type: 'thinking_delta',
            thinking: 'b',
        });
    });

    // ── Scenario 5: Consecutive text (coalesce) ──────────────────────────

    it('§6.5 — consecutive text parts coalesce into one block', () => {
        const e1 = fsm.process({ text: 'a' });
        const e2 = fsm.process({ text: 'b' });

        assert.deepStrictEqual(eventTypes(e1), ['content_block_start', 'content_block_delta']);
        assert.deepStrictEqual(eventTypes(e2), ['content_block_delta']);
        assert.equal(e2[0].data.index, 0);
        assert.deepStrictEqual(e2[0].data.delta, {
            type: 'text_delta',
            text: 'b',
        });
    });

    // ── Scenario 6: Tool use (always new block) ──────────────────────────

    it('§6.6 — consecutive tool_use parts always open new blocks', () => {
        const e1 = fsm.process({
            functionCall: { name: 'f1', args: {} },
        });
        const e2 = fsm.process({
            functionCall: { name: 'f2', args: {} },
        });

        assert.deepStrictEqual(eventTypes(e1), ['content_block_start', 'content_block_delta']);
        assert.equal(e1[0].data.index, 0);
        assert.equal(e1[0].data.content_block.type, 'tool_use');
        assert.equal(e1[0].data.content_block.name, 'f1');

        assert.deepStrictEqual(eventTypes(e2), [
            'content_block_stop',
            'content_block_start',
            'content_block_delta',
        ]);
        assert.equal(e2[0].data.index, 0);
        assert.equal(e2[1].data.index, 1);
        assert.equal(e2[1].data.content_block.name, 'f2');
    });

    // ── Scenario 7: Text → tool_use transition ──────────────────────────

    it('§6.7 — text → tool_use closes text and opens tool_use', () => {
        const e1 = fsm.process({ text: "I'll call" });
        const e2 = fsm.process({
            functionCall: { name: 'f', args: {} },
        });

        assert.deepStrictEqual(eventTypes(e1), ['content_block_start', 'content_block_delta']);
        assert.equal(e1[0].data.index, 0);

        assert.deepStrictEqual(eventTypes(e2), [
            'content_block_stop',
            'content_block_start',
            'content_block_delta',
        ]);
        assert.equal(e2[0].data.index, 0);
        assert.equal(e2[1].data.index, 1);
        assert.equal(e2[1].data.content_block.type, 'tool_use');
    });

    // ── Scenario 8: Thinking with signature ──────────────────────────────

    it('§6.8 — thinking with long signature emits signature_delta', () => {
        const sig = 'a'.repeat(50);
        const events = fsm.process({
            thought: true,
            text: 'x',
            thoughtSignature: sig,
        });

        assert.equal(events.length, 3);
        assert.deepStrictEqual(eventTypes(events), [
            'content_block_start',
            'content_block_delta',
            'content_block_delta',
        ]);
        assert.deepStrictEqual(deltaTypes(events), ['thinking_delta', 'signature_delta']);
        assert.equal(events[2].data.delta.signature, sig);
    });

    // ── Scenario 9: Thinking with short signature (ignored) ──────────────

    it('§6.9 — thinking with short signature does not emit signature_delta', () => {
        const events = fsm.process({
            thought: true,
            text: 'x',
            thoughtSignature: 'short',
        });

        assert.equal(events.length, 2);
        assert.deepStrictEqual(deltaTypes(events), ['thinking_delta']);
    });

    // ── Scenario 10: flush() closes final block ─────────────────────────

    it('§6.10 — flush() after processing closes the final block', () => {
        fsm.process({ text: 'hi' });
        const events = fsm.flush();

        assert.equal(events.length, 1);
        assert.equal(events[0].event, 'content_block_stop');
        assert.equal(events[0].data.index, 0);
    });

    // ── Scenario 11: flush() on idle (no-op) ────────────────────────────

    it('§6.11 — flush() with no prior input returns empty array', () => {
        const events = fsm.flush();
        assert.deepStrictEqual(events, []);
    });

    // ── Scenario 12: Unknown part type (ignored) ────────────────────────

    it('§6.12 — unknown part type returns empty array', () => {
        const events = fsm.process({ unknownField: 123 });
        assert.deepStrictEqual(events, []);
    });

    // ── Scenario 13: End-to-end full stream ─────────────────────────────

    it('§6.13 — full stream with thinking + text + tool_use produces correct event sequence', () => {
        const allEvents = [];

        allEvents.push(...fsm.process({ thought: true, text: 'Let me think...' }));
        allEvents.push(...fsm.process({ thought: true, text: 'I see.' }));
        allEvents.push(...fsm.process({ text: 'Here is my answer.' }));
        allEvents.push(
            ...fsm.process({
                functionCall: {
                    id: 'call_1',
                    name: 'read_file',
                    args: { path: '/tmp/x' },
                },
            }),
        );
        allEvents.push(
            ...fsm.process({
                functionCall: {
                    id: 'call_2',
                    name: 'write_file',
                    args: { path: '/tmp/y', content: 'data' },
                },
            }),
        );
        allEvents.push(...fsm.flush());

        const expectedSequence = [
            // thinking block 0
            'content_block_start',
            'content_block_delta',
            // coalesced thinking
            'content_block_delta',
            // transition: close thinking, open text (block 1)
            'content_block_stop',
            'content_block_start',
            'content_block_delta',
            // transition: close text, open tool_use (block 2)
            'content_block_stop',
            'content_block_start',
            'content_block_delta',
            // new tool_use (block 3)
            'content_block_stop',
            'content_block_start',
            'content_block_delta',
            // flush: close final block
            'content_block_stop',
        ];

        assert.deepStrictEqual(eventTypes(allEvents), expectedSequence);

        // Verify block indices
        assert.equal(allEvents[0].data.index, 0); // thinking start
        assert.equal(allEvents[3].data.index, 0); // thinking stop
        assert.equal(allEvents[4].data.index, 1); // text start
        assert.equal(allEvents[6].data.index, 1); // text stop
        assert.equal(allEvents[7].data.index, 2); // tool_use 1 start
        assert.equal(allEvents[9].data.index, 2); // tool_use 1 stop
        assert.equal(allEvents[10].data.index, 3); // tool_use 2 start
        assert.equal(allEvents[12].data.index, 3); // final stop

        // Verify tool_use blocks have correct names
        assert.equal(allEvents[7].data.content_block.name, 'read_file');
        assert.equal(allEvents[7].data.content_block.id, 'call_1');
        assert.equal(allEvents[10].data.content_block.name, 'write_file');
        assert.equal(allEvents[10].data.content_block.id, 'call_2');
    });

    // ── Scenario 14: hasToolUse flag ────────────────────────────────────

    it('§6.14 — hasToolUse is false when no tool_use processed', () => {
        fsm.process({ text: 'hello' });
        assert.equal(fsm.hasToolUse, false);
    });

    it('§6.14 — hasToolUse is true after processing functionCall', () => {
        fsm.process({ functionCall: { name: 'f', args: {} } });
        assert.equal(fsm.hasToolUse, true);
    });

    // ── Scenario 15: Empty parts (no FSM events) ────────────────────────

    it('§6.15 — process with empty object returns empty array', () => {
        const events = fsm.process({});
        assert.deepStrictEqual(events, []);
    });

    // ── Additional edge cases ────────────────────────────────────────────

    it('tool_use generates id when not provided', () => {
        const events = fsm.process({
            functionCall: { name: 'test_fn', args: { a: 1 } },
        });
        const startEvent = events.find((e) => e.event === 'content_block_start');
        assert.ok(startEvent.data.content_block.id.startsWith('toolu_'));
    });

    it('tool_use delta contains stringified args', () => {
        const args = { path: '/tmp/file', mode: 'write' };
        const events = fsm.process({
            functionCall: { name: 'fn', args },
        });
        const delta = events.find((e) => e.event === 'content_block_delta');
        assert.equal(delta.data.delta.partial_json, JSON.stringify(args));
    });

    it('tool_use with missing args defaults to empty object', () => {
        const events = fsm.process({
            functionCall: { name: 'fn' },
        });
        const delta = events.find((e) => e.event === 'content_block_delta');
        assert.equal(delta.data.delta.partial_json, '{}');
    });

    it('thinking with empty text skips thinking_delta', () => {
        const events = fsm.process({ thought: true });
        assert.equal(events.length, 1);
        assert.equal(events[0].event, 'content_block_start');
        assert.deepStrictEqual(events[0].data.content_block, {
            type: 'thinking',
            thinking: '',
        });
    });

    it('final thinking chunk with empty text and signature emits only signature_delta', () => {
        fsm.process({ thought: true, text: 'some thinking' });

        const sig = 'a'.repeat(50);
        const events = fsm.process({
            thought: true,
            text: '',
            thoughtSignature: sig,
        });

        assert.equal(events.length, 1);
        assert.equal(events[0].event, 'content_block_delta');
        assert.equal(events[0].data.delta.type, 'signature_delta');
        assert.equal(events[0].data.delta.signature, sig);
    });

    it('flush() is idempotent — second call returns empty', () => {
        fsm.process({ text: 'hi' });
        fsm.flush();
        const events = fsm.flush();
        assert.deepStrictEqual(events, []);
    });

    it('tool_use → text transition works correctly', () => {
        fsm.process({ functionCall: { name: 'f', args: {} } });
        const events = fsm.process({ text: 'result' });

        assert.deepStrictEqual(eventTypes(events), [
            'content_block_stop',
            'content_block_start',
            'content_block_delta',
        ]);
        assert.equal(events[1].data.content_block.type, 'text');
        assert.equal(events[1].data.index, 1);
    });

    it('tool_use → thinking transition works correctly', () => {
        fsm.process({ functionCall: { name: 'f', args: {} } });
        const events = fsm.process({ thought: true, text: 'hmm' });

        assert.deepStrictEqual(eventTypes(events), [
            'content_block_stop',
            'content_block_start',
            'content_block_delta',
        ]);
        assert.equal(events[1].data.content_block.type, 'thinking');
        assert.equal(events[1].data.index, 1);
    });

    it('text → thinking transition works correctly', () => {
        fsm.process({ text: 'hello' });
        const events = fsm.process({ thought: true, text: 'thinking...' });

        assert.deepStrictEqual(eventTypes(events), [
            'content_block_stop',
            'content_block_start',
            'content_block_delta',
        ]);
        assert.equal(events[1].data.content_block.type, 'thinking');
    });

    it('signature at exactly 50 chars is emitted', () => {
        const sig = 'x'.repeat(50);
        const events = fsm.process({
            thought: true,
            text: 'a',
            thoughtSignature: sig,
        });
        assert.equal(events.length, 3);
        assert.equal(events[2].data.delta.type, 'signature_delta');
    });

    it('signature at 49 chars is not emitted', () => {
        const sig = 'x'.repeat(49);
        const events = fsm.process({
            thought: true,
            text: 'a',
            thoughtSignature: sig,
        });
        assert.equal(events.length, 2);
    });
});
