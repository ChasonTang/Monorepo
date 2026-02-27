import crypto from 'node:crypto';
import { createInterface } from 'node:readline';
import { Readable } from 'node:stream';

import { CLAUDE_THINKING_MAX_OUTPUT_TOKENS } from './constants.js';
import { validateSSEEvent } from './response-validator.js';

// ─── Tool Schema Sanitization (RFC-006: Whitelist-based Sanitizer) ──────────

/**
 * Allowed keywords (22 total) — pass through unchanged.
 * Only keywords in this set survive the whitelist filter.
 */
const ALLOWED_KEYWORDS = new Set([
    // Applicator (7 of 15)
    'allOf',
    'oneOf',
    'not',
    'prefixItems',
    'items',
    'properties',
    'additionalProperties',
    // Validation (12 of 20)
    'type',
    'enum',
    'maximum',
    'minimum',
    'maxLength',
    'minLength',
    'pattern',
    'maxItems',
    'minItems',
    'maxProperties',
    'minProperties',
    'required',
    // Meta-Data (2 of 7)
    'description',
    'default',
    // Format (1 of 1)
    'format',
]);

/**
 * Keywords that are currently stripped but COULD be converted to description
 * hints or other semantic-preserving transforms in the future.
 *
 * When any of these keywords is encountered and stripped, a warn-level log is
 * emitted with the keyword name and value. This enables us to:
 * 1. Detect when Claude Code starts sending these keywords
 * 2. Measure frequency to prioritize which transforms to implement next
 * 3. Proactively improve model accuracy before users report quality issues
 *
 * Grouped by potential future transform complexity:
 */
const MONITORABLE_KEYWORDS = new Set([
    // ── Core vocabulary — future: $ref/$defs inline resolution ──
    '$ref',
    '$defs',

    // ── Applicator — future: conditional/dependency/constraint description hints ──
    'if',
    'then',
    'else',
    'dependentSchemas',
    'contains',
    'propertyNames',
    'patternProperties',

    // ── Validation — future: constraint description hints ──
    'multipleOf',
    'uniqueItems',
    'maxContains',
    'minContains',
    'dependentRequired',

    // ── Unevaluated — future: structural constraint hints ──
    'unevaluatedItems',
    'unevaluatedProperties',

    // ── Meta-Data — future: preserve as description hints ──
    'title',
    'deprecated',
    'readOnly',
    'writeOnly',
    'examples',

    // ── Content — future: encoding/media type hints ──
    'contentEncoding',
    'contentMediaType',
    'contentSchema',

    // ── Deprecated — future: merge/rewrite if encountered ──
    'definitions',
    'dependencies',
]);

/**
 * All JSON Schema 2020-12 keywords NOT in the whitelist (35 standard + 4 deprecated = 39).
 * Keywords in this set but NOT in MONITORABLE_KEYWORDS are stripped silently
 * (they have no meaningful semantic-preserving transform potential).
 */
const KNOWN_UNSUPPORTED_KEYWORDS = new Set([
    // ── Core vocabulary (9, all unsupported) ──
    '$schema',
    '$id',
    '$ref',
    '$anchor',
    '$dynamicRef',
    '$dynamicAnchor',
    '$vocabulary',
    '$comment',
    '$defs',
    // ── Applicator (8 of 15 unsupported) ──
    'anyOf', // Converted to oneOf in Phase 1c (RFC-012)
    'if',
    'then',
    'else',
    'dependentSchemas',
    'contains',
    'patternProperties',
    'propertyNames',
    // ── Validation (8 of 20 unsupported) ──
    'const', // Converted to enum in Phase 1a
    'multipleOf',
    'exclusiveMaximum',
    'exclusiveMinimum',
    'uniqueItems',
    'maxContains',
    'minContains',
    'dependentRequired',
    // ── Unevaluated (2, all unsupported) ──
    'unevaluatedItems',
    'unevaluatedProperties',
    // ── Meta-Data (5 of 7 unsupported) ──
    'title',
    'deprecated',
    'readOnly',
    'writeOnly',
    'examples',
    // ── Content (3, all unsupported) ──
    'contentEncoding',
    'contentMediaType',
    'contentSchema',
    // ── Deprecated (4, from 2020-12 root meta-schema) ──
    'definitions',
    'dependencies',
    '$recursiveAnchor',
    '$recursiveRef',
]);

// ─── Phase 1: Semantic Transforms ───────────────────────────────────────────

/**
 * Appends a hint to a schema's description field.
 *
 * @param {Object} schema
 * @param {string} hint
 * @returns {Object}
 */
function appendDescriptionHint(schema, hint) {
    if (!schema || typeof schema !== 'object') return schema;
    const existing = typeof schema.description === 'string' ? schema.description : '';
    const newDescription = existing ? `${existing} (${hint})` : hint;

    return { ...schema, description: newDescription };
}

/**
 * Phase 1a: Converts const to enum.
 * { const: "foo" } → { enum: ["foo"] }
 */
function convertConstToEnum(schema) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map((item) => convertConstToEnum(item));

    const result = {};
    for (const [key, value] of Object.entries(schema)) {
        if (key === 'const' && !schema.enum) {
            result.enum = [value];
        } else {
            result[key] = convertConstToEnum(value);
        }
    }

    return result;
}

/**
 * Phase 1b: Converts exclusiveMinimum/exclusiveMaximum to min/max + description hint.
 *
 * - For type: "integer": also sets minimum = exclusiveMinimum + 1 (or maximum - 1)
 * - For type: "number": only adds description hint (conversion not exact for continuous values)
 * - Phase 1 transforms do NOT delete original keywords — Phase 2 handles all deletion.
 */
function convertExclusiveBounds(schema) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map(convertExclusiveBounds);

    let result = { ...schema };

    if (result.exclusiveMinimum !== undefined) {
        const excMin = result.exclusiveMinimum;
        result = appendDescriptionHint(result, `exclusiveMinimum: ${excMin}`);
        if (result.type === 'integer' && result.minimum === undefined) {
            result.minimum = excMin + 1;
        }
    }

    if (result.exclusiveMaximum !== undefined) {
        const excMax = result.exclusiveMaximum;
        result = appendDescriptionHint(result, `exclusiveMaximum: ${excMax}`);
        if (result.type === 'integer' && result.maximum === undefined) {
            result.maximum = excMax - 1;
        }
    }

    for (const [key, value] of Object.entries(result)) {
        if (typeof value === 'object' && value !== null) {
            result[key] = convertExclusiveBounds(value);
        }
    }

    return result;
}

/**
 * Phase 1c: Convert anyOf → oneOf.
 *
 * oneOf is strictly more restrictive than anyOf (exclusive vs. inclusive OR).
 * For type-union patterns — the dominant anyOf use case in tool schemas,
 * where branches are mutually exclusive by type — the two are equivalent.
 * For the model generation use case (not validation), both keywords carry
 * identical guidance: "produce a value conforming to one of these schemas."
 *
 * oneOf is natively supported by Antigravity; anyOf is not.
 *
 * If a schema node has both anyOf and oneOf (extremely rare), anyOf is
 * dropped to avoid conflict — Phase 2 whitelist strips the residual.
 */
function convertAnyOfToOneOf(schema) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map(convertAnyOfToOneOf);

    const result = {};
    for (const [key, value] of Object.entries(schema)) {
        if (key === 'anyOf') {
            if (!schema.oneOf) {
                result.oneOf = Array.isArray(value) ? value.map(convertAnyOfToOneOf) : value;
            }
        } else {
            result[key] =
                typeof value === 'object' && value !== null ? convertAnyOfToOneOf(value) : value;
        }
    }

    return result;
}

// ─── Phase 2: Whitelist Filter with Monitoring ──────────────────────────────

/**
 * Phase 2: Whitelist filter — only permit known-safe JSON Schema keywords.
 * All keywords not in ALLOWED_KEYWORDS are stripped. Logging tiers:
 *   - MONITORABLE_KEYWORDS:      warn-level  (future transform potential)
 *   - unknown (not in any set):  info-level  (upstream schema changes)
 *   - other KNOWN_UNSUPPORTED:   silent      (routine, no action needed)
 */
function filterToAllowedKeywords(schema, logger) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map((s) => filterToAllowedKeywords(s, logger));

    const result = {};

    for (const [key, value] of Object.entries(schema)) {
        if (ALLOWED_KEYWORDS.has(key)) {
            // ── Allowed: recurse into sub-schemas ──
            if (key === 'properties' && typeof value === 'object' && value !== null) {
                const props = {};
                for (const [propName, propSchema] of Object.entries(value)) {
                    props[propName] = filterToAllowedKeywords(propSchema, logger);
                }
                result[key] = props;
            } else if (
                (key === 'items' || key === 'not' || key === 'additionalProperties') &&
                typeof value === 'object' &&
                value !== null
            ) {
                result[key] = filterToAllowedKeywords(value, logger);
            } else if (
                (key === 'allOf' || key === 'oneOf' || key === 'prefixItems') &&
                Array.isArray(value)
            ) {
                result[key] = value.map((item) => filterToAllowedKeywords(item, logger));
            } else {
                result[key] = value;
            }
            continue;
        }

        // ── Not allowed: three-tier logging ──
        if (MONITORABLE_KEYWORDS.has(key)) {
            // Keyword with future transform potential — log for prioritization
            if (logger) {
                logger.warn(
                    `[schema-sanitizer] Monitorable keyword stripped: "${key}" ` +
                        `(value: ${JSON.stringify(value)?.slice(0, 200)})`,
                );
            }
        } else if (!KNOWN_UNSUPPORTED_KEYWORDS.has(key)) {
            // Unknown keyword — may indicate new upstream pattern or Antigravity support
            if (logger) {
                logger.info(
                    `[schema-sanitizer] Unknown keyword stripped: "${key}" ` +
                        `(value: ${JSON.stringify(value)?.slice(0, 100)})`,
                );
            }
        }
        // Known unsupported (not monitorable): strip silently
    }

    return result;
}

// ─── Orchestration ──────────────────────────────────────────────────────────

/**
 * Single-stage pure function that sanitizes a JSON Schema for Antigravity
 * compatibility. Replaces the previous two-stage pipeline
 * (cleanSchemaForAntigravity → toGeminiSchema).
 *
 * Phase 1: Semantic transforms (only output-producing transforms)
 *   1a. const → enum
 *   1b. exclusiveMin/Max → min/max + description hint
 *   1c. anyOf → oneOf (RFC-012)
 *
 * Phase 2: Whitelist filter — strip all non-allowed keywords with
 *   three-tier logging (monitorable/unknown/silent).
 *
 * @param {Object} schema - JSON Schema object (JSON Schema 2020-12)
 * @param {Object} [logger] - Logger with .warn() and .info() methods
 * @returns {Object} Antigravity-safe schema (supported subset only)
 */
export function sanitizeSchemaForAntigravity(schema, logger) {
    if (!schema || typeof schema !== 'object') return schema;

    let result = schema;

    // ── Phase 1: Semantic Transforms (only output-producing transforms) ──
    result = convertConstToEnum(result); // 1a: const → enum
    result = convertExclusiveBounds(result); // 1b: exclusiveMin/Max → min/max + hint
    result = convertAnyOfToOneOf(result); // 1c: anyOf → oneOf (RFC-012)

    // ── Phase 2: Whitelist Filter ──
    result = filterToAllowedKeywords(result, logger); // 2a-c: strip + log (monitorable/unknown)

    return result;
}

// ─── Utilities ──────────────────────────────────────────────────────────────

/**
 * Generate a random hex string.
 *
 * @param {number} bytes - Number of random bytes
 * @returns {string} Hex-encoded random string
 */
function randomHex(bytes) {
    return crypto.randomBytes(bytes).toString('hex');
}

// ─── Content Block Conversion (Anthropic → Google) ──────────────────────────

/**
 * Convert Anthropic content blocks to Google parts.
 * Extracts only known fields, ignoring cache_control etc.
 *
 * @param {string|Array|*} content - Anthropic content (string or block array)
 * @returns {Array} Google parts array
 */
function convertContentToParts(content) {
    if (typeof content === 'string') {
        return [{ text: content }];
    }

    if (!Array.isArray(content)) {
        return [{ text: String(content) }];
    }

    const parts = [];

    for (const block of content) {
        switch (block.type) {
            case 'text':
                if (block.text?.trim()) {
                    parts.push({ text: block.text });
                }
                break;

            case 'tool_use':
                parts.push({
                    functionCall: {
                        id: block.id,
                        name: block.name,
                        args: block.input,
                    },
                });
                break;

            case 'tool_result':
                // NOTE: name uses tool_use_id as a simplification. Claude matches
                // by id field so this works. See Future Improvements (§6.1) for
                // proper function name lookup.
                parts.push({
                    functionResponse: {
                        id: block.tool_use_id,
                        name: block.tool_use_id,
                        response: block.content,
                    },
                });
                break;

            case 'thinking':
                parts.push({
                    text: block.thinking,
                    thought: true,
                    thoughtSignature: block.signature,
                });
                break;

            default:
                console.error(
                    `[Odin] Warning: unhandled content block type "${block.type}" — skipped`,
                );
                break;
        }
    }

    return parts;
}

// ─── Request Conversion (Anthropic → Google) ────────────────────────────────

/**
 * Convert Anthropic Messages API request to Google Generative AI format.
 * Only extracts known fields, naturally ignoring cache_control and other
 * Anthropic-specific fields.
 *
 * @param {Object} anthropicRequest - Anthropic format request
 * @returns {Object} Google format request
 */
export function anthropicToGoogle(anthropicRequest) {
    const {
        messages,
        system,
        max_tokens,
        temperature,
        top_p,
        top_k,
        stop_sequences,
        tools,
        thinking,
    } = anthropicRequest;

    const googleRequest = {
        contents: [],
        generationConfig: {},
    };

    if (system) {
        googleRequest.systemInstruction = {
            parts:
                typeof system === 'string'
                    ? [{ text: system }]
                    : system.map((b) => ({ text: b.text })),
        };
    }

    // Convert messages to contents
    for (const msg of messages) {
        const parts = convertContentToParts(msg.content);
        googleRequest.contents.push({
            role: msg.role === 'assistant' ? 'model' : 'user',
            parts,
        });
    }

    // Generation config
    if (max_tokens) {
        googleRequest.generationConfig.maxOutputTokens = max_tokens;
    }
    if (temperature !== undefined) {
        googleRequest.generationConfig.temperature = temperature;
    }
    if (top_p !== undefined) {
        googleRequest.generationConfig.topP = top_p;
    }
    if (top_k !== undefined) {
        googleRequest.generationConfig.topK = top_k;
    }
    if (stop_sequences?.length) {
        googleRequest.generationConfig.stopSequences = stop_sequences;
    }

    // Thinking config for thinking models
    if (thinking) {
        const thinkingBudget = thinking.budget_tokens;
        googleRequest.generationConfig.thinkingConfig = {
            include_thoughts: thinking.type !== 'disabled',
            thinking_budget: thinkingBudget,
        };

        // Validate max_tokens > thinking_budget (API requirement)
        if (thinkingBudget !== undefined && max_tokens <= thinkingBudget) {
            googleRequest.generationConfig.maxOutputTokens = CLAUDE_THINKING_MAX_OUTPUT_TOKENS;
        }
    }

    // Tools — sanitize schemas through the single-stage whitelist-based sanitizer (RFC-006)
    if (tools?.length) {
        googleRequest.tools = [
            {
                functionDeclarations: tools.map((tool) => ({
                    name: tool.name,
                    description: tool.description,
                    parameters: sanitizeSchemaForAntigravity(tool.input_schema),
                })),
            },
        ];
    }

    return googleRequest;
}

// ─── SSE Helpers ─────────────────────────────────────────────────────────────

/**
 * Format an SSE event string.
 *
 * @param {string} event - Event type (e.g. "message_start")
 * @param {Object} data - Event data object
 * @returns {string} Formatted SSE event string
 */
function formatSSE(event, data) {
    return `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
}

// ─── SSE Stream Layer 2: Event Parser ────────────────────────────────────────

/**
 * Parse SSE data lines into structured Google events.
 *
 * @param {AsyncIterable<string>} lines - Line stream from readline.createInterface()
 * @param {boolean} debug - Enable debug logging
 * @yields {{ parts: Array, usage: Object|null, finishReason: string|null }}
 */
async function* parseGoogleSSEEvents(lines, debug) {
    for await (const line of lines) {
        if (!line.startsWith('data:')) continue;

        const jsonText = line.slice(5).trim();
        if (!jsonText) continue;

        if (debug) {
            console.error(`[Odin:debug] ← SSE:`, line.trimEnd());
        }

        try {
            const data = JSON.parse(jsonText);

            try {
                const validation = validateSSEEvent(data);
                if (!validation.valid) {
                    const rawSuffix = debug ? ` | raw: ${jsonText.slice(0, 500)}` : '';

                    console.error(
                        `[Odin] SSE response validation warning: ${validation.errors}${rawSuffix}`,
                    );
                }
            } catch (validationError) {
                console.error(`[Odin] SSE validator internal error:`, validationError.message);
            }

            const inner = data.response || data;

            yield {
                parts: inner.candidates?.[0]?.content?.parts || [],
                usage: inner.usageMetadata || null,
                finishReason: inner.candidates?.[0]?.finishReason || null,
                responseId: inner.responseId || null,
            };
        } catch (e) {
            if (debug) {
                console.error(`[Odin:debug] ← SSE parse error:`, e.message);
            }
        }
    }
}

// ─── SSE Stream Layer 3: Content Block FSM ───────────────────────────────────

/**
 * Block-type definitions: maps input type to block start/delta constructors.
 *
 * Each entry defines how to create the content_block for block_start,
 * and how to create the delta for block_delta. This table replaces
 * the triplicated if/else branches.
 */
const BLOCK_TYPES = {
    thinking: {
        makeStartBlock: () => ({ type: 'thinking', thinking: '' }),
        makeDelta: (part) => ({ type: 'thinking_delta', thinking: part.text || '' }),
        alwaysNewBlock: false,
    },
    text: {
        makeStartBlock: () => ({ type: 'text', text: '' }),
        makeDelta: (part) => ({ type: 'text_delta', text: part.text }),
        alwaysNewBlock: false,
    },
    tool_use: {
        makeStartBlock: (part) => ({
            type: 'tool_use',
            id: part.functionCall.id || `toolu_${randomHex(12)}`,
            name: part.functionCall.name,
            input: {},
        }),
        makeDelta: (part) => ({
            type: 'input_json_delta',
            partial_json: JSON.stringify(part.functionCall.args || {}),
        }),
        alwaysNewBlock: true,
    },
};

/**
 * Classify a Google part into a block type key.
 *
 * @param {Object} part - A Google content part
 * @returns {string|null} One of 'thinking', 'text', 'tool_use', or null
 */
export function classifyPart(part) {
    if (part.thought === true) return 'thinking';
    if (part.text !== undefined) return 'text';
    if (part.functionCall) return 'tool_use';

    return null;
}

/**
 * Content Block FSM.
 *
 * Manages content block lifecycle (start/delta/stop) with a
 * table-driven transition model. Replaces the triplicated
 * if/else branches in the original streamSSEResponse.
 */
export class ContentBlockFSM {
    #state = null;
    #blockIndex = 0;
    #hasToolUse = false;

    /**
     * Process a single Google part and return the Anthropic SSE events to emit.
     *
     * @param {Object} part - Google content part
     * @returns {Array<{event: string, data: Object}>} Events to emit
     */
    process(part) {
        const type = classifyPart(part);
        if (!type) return [];

        const def = BLOCK_TYPES[type];
        const events = [];

        const needsNewBlock = this.#state !== type || def.alwaysNewBlock;

        if (needsNewBlock) {
            if (this.#state !== null) {
                events.push({
                    event: 'content_block_stop',
                    data: { type: 'content_block_stop', index: this.#blockIndex },
                });
                this.#blockIndex++;
            }

            this.#state = type;
            events.push({
                event: 'content_block_start',
                data: {
                    type: 'content_block_start',
                    index: this.#blockIndex,
                    content_block: def.makeStartBlock(part),
                },
            });
        }

        if (type !== 'thinking' || part.text) {
            events.push({
                event: 'content_block_delta',
                data: {
                    type: 'content_block_delta',
                    index: this.#blockIndex,
                    delta: def.makeDelta(part),
                },
            });
        }

        if (type === 'thinking' && part.thoughtSignature?.length >= 50) {
            events.push({
                event: 'content_block_delta',
                data: {
                    type: 'content_block_delta',
                    index: this.#blockIndex,
                    delta: {
                        type: 'signature_delta',
                        signature: part.thoughtSignature,
                    },
                },
            });
        }

        if (type === 'tool_use') {
            this.#hasToolUse = true;
        }

        return events;
    }

    /**
     * Flush: close the final open block (if any).
     *
     * @returns {Array<{event: string, data: Object}>} Final close event(s)
     */
    flush() {
        if (this.#state === null) return [];
        const events = [
            {
                event: 'content_block_stop',
                data: { type: 'content_block_stop', index: this.#blockIndex },
            },
        ];
        this.#state = null;

        return events;
    }

    get hasToolUse() {
        return this.#hasToolUse;
    }
}

// ─── SSE Debug Helper ────────────────────────────────────────────────────────

/**
 * Format and optionally log an SSE event.
 *
 * @param {string} event - Event type
 * @param {Object} data - Event data
 * @param {boolean} debug - Whether to log
 * @returns {string} Formatted SSE string
 */
function formatAndLog(event, data, debug) {
    const sse = formatSSE(event, data);
    if (debug) {
        console.error(`[Odin:debug] → SSE:`, sse.trimEnd());
    }

    return sse;
}

// ─── SSE Stream Conversion (Google → Anthropic) ─────────────────────────────

/**
 * Stream and convert Google SSE events to Anthropic format.
 *
 * Composes three layers (stream framer, event parser, content block FSM)
 * and handles the protocol envelope (message_start/delta/stop) inline.
 *
 * @param {ReadableStream} stream - Response body stream from Cloud Code
 * @param {string} model - Model name for the response
 * @param {boolean} [debug=false] - Enable debug logging of SSE chunks
 * @yields {string} Anthropic SSE event lines
 */
export async function* streamSSEResponse(stream, model, debug = false) {
    let messageId = null;
    let inputTokens = 0;
    let outputTokens = 0;
    let cacheReadTokens = 0;
    let stopReason = null;
    let hasEmittedStart = false;

    const fsm = new ContentBlockFSM();

    const lines = createInterface({ input: Readable.fromWeb(stream), crlfDelay: Infinity });
    const events = parseGoogleSSEEvents(lines, debug);

    for await (const { parts, usage, finishReason, responseId } of events) {
        if (responseId && !messageId) {
            messageId = responseId;
        }
        if (usage) {
            inputTokens = usage.promptTokenCount || inputTokens;
            outputTokens = usage.candidatesTokenCount || outputTokens;
            cacheReadTokens = usage.cachedContentTokenCount ?? cacheReadTokens;
        }

        if (!hasEmittedStart && parts.length > 0) {
            hasEmittedStart = true;
            if (!messageId) {
                throw new Error('Antigravity response missing required responseId field');
            }
            yield formatAndLog(
                'message_start',
                {
                    type: 'message_start',
                    message: {
                        id: messageId,
                        type: 'message',
                        role: 'assistant',
                        content: [],
                        model,
                        stop_reason: null,
                        stop_sequence: null,
                        usage: {
                            input_tokens: inputTokens - cacheReadTokens,
                            output_tokens: 0,
                            cache_read_input_tokens: cacheReadTokens,
                            cache_creation_input_tokens: 0,
                        },
                    },
                },
                debug,
            );
        }

        for (const part of parts) {
            for (const fsmEvent of fsm.process(part)) {
                yield formatAndLog(fsmEvent.event, fsmEvent.data, debug);
            }
        }

        if (finishReason && !stopReason) {
            stopReason = finishReason === 'MAX_TOKENS' ? 'max_tokens' : 'end_turn';
        }
    }

    for (const fsmEvent of fsm.flush()) {
        yield formatAndLog(fsmEvent.event, fsmEvent.data, debug);
    }

    if (fsm.hasToolUse && !stopReason) {
        stopReason = 'tool_use';
    }

    yield formatAndLog(
        'message_delta',
        {
            type: 'message_delta',
            delta: { stop_reason: stopReason || 'end_turn', stop_sequence: null },
            usage: {
                output_tokens: outputTokens,
                cache_read_input_tokens: cacheReadTokens,
                cache_creation_input_tokens: 0,
            },
        },
        debug,
    );

    yield formatAndLog('message_stop', { type: 'message_stop' }, debug);
}
