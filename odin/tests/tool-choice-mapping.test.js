/**
 * Tests for RFC-016: ToolChoice-to-ToolConfig Mapping
 *
 * Covers §6.2 scenarios:
 * - tool_choice auto → toolConfig.functionCallingConfig.mode: "AUTO"
 * - tool_choice any → toolConfig.functionCallingConfig.mode: "ANY"
 * - tool_choice none → toolConfig.functionCallingConfig.mode: "NONE"
 * - Absent tool_choice → no toolConfig on Google request
 * - tool_choice with tools coexistence
 * - Unrecognized tool_choice.type → AJV rejection
 */

import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { anthropicToGoogle } from '../src/converter.js';
import { validateMessagesRequest } from '../src/validator.js';

function baseRequest(overrides = {}) {
    return {
        messages: [{ role: 'user', content: 'hello' }],
        system: 'You are helpful.',
        max_tokens: 4096,
        ...overrides,
    };
}

function validBase(overrides = {}) {
    return {
        model: 'claude-sonnet-4-20250514',
        messages: [{ role: 'user', content: 'hello' }],
        max_tokens: 4096,
        stream: true,
        ...overrides,
    };
}

describe('RFC-016 Converter — ToolChoice-to-ToolConfig Mapping', () => {
    // §6.2 #1: tool_choice auto → AUTO
    it('maps tool_choice type "auto" to mode "AUTO"', () => {
        const req = baseRequest({
            tool_choice: { type: 'auto' },
        });
        const result = anthropicToGoogle(req);

        assert.deepStrictEqual(result.toolConfig, {
            functionCallingConfig: {
                mode: 'AUTO',
            },
        });
    });

    // §6.2 #2: tool_choice any → ANY
    it('maps tool_choice type "any" to mode "ANY"', () => {
        const req = baseRequest({
            tool_choice: { type: 'any' },
        });
        const result = anthropicToGoogle(req);

        assert.deepStrictEqual(result.toolConfig, {
            functionCallingConfig: {
                mode: 'ANY',
            },
        });
    });

    // §6.2 #3: tool_choice none → NONE
    it('maps tool_choice type "none" to mode "NONE"', () => {
        const req = baseRequest({
            tool_choice: { type: 'none' },
        });
        const result = anthropicToGoogle(req);

        assert.deepStrictEqual(result.toolConfig, {
            functionCallingConfig: {
                mode: 'NONE',
            },
        });
    });

    // §6.2 #4: absent tool_choice → no toolConfig
    it('omits toolConfig when tool_choice is absent', () => {
        const req = baseRequest();
        const result = anthropicToGoogle(req);

        assert.strictEqual(result.toolConfig, undefined);
    });

    // §6.2 #5: tool_choice with tools coexistence
    it('emits both toolConfig and tools when both are present', () => {
        const req = baseRequest({
            tool_choice: { type: 'any' },
            tools: [
                {
                    name: 'get_weather',
                    description: 'Get weather info',
                    input_schema: {
                        type: 'object',
                        properties: {
                            location: { type: 'string' },
                        },
                        required: ['location'],
                    },
                },
            ],
        });
        const result = anthropicToGoogle(req);

        assert.deepStrictEqual(result.toolConfig, {
            functionCallingConfig: {
                mode: 'ANY',
            },
        });
        assert.ok(result.tools, 'tools should be present');
        assert.equal(result.tools[0].functionDeclarations.length, 1);
        assert.equal(result.tools[0].functionDeclarations[0].name, 'get_weather');
    });
});

describe('RFC-016 Validator — tool_choice AJV Rejection', () => {
    // §6.2 #6: Unrecognized type → AJV rejection
    it('rejects tool_choice with unrecognized type "tool"', () => {
        const body = validBase({
            tool_choice: { type: 'tool' },
        });
        const result = validateMessagesRequest(body);

        assert.equal(result.valid, false);
        assert.ok(result.message, 'should have an error message');
    });

    it('accepts tool_choice with type "auto"', () => {
        const body = validBase({
            tool_choice: { type: 'auto' },
        });
        const result = validateMessagesRequest(body);

        assert.equal(result.valid, true);
    });

    it('accepts tool_choice with type "any"', () => {
        const body = validBase({
            tool_choice: { type: 'any' },
        });
        const result = validateMessagesRequest(body);

        assert.equal(result.valid, true);
    });

    it('accepts tool_choice with type "none"', () => {
        const body = validBase({
            tool_choice: { type: 'none' },
        });
        const result = validateMessagesRequest(body);

        assert.equal(result.valid, true);
    });
});
