import Ajv from 'ajv';

// ─── JSON Schema Definition ─────────────────────────────────────────────────

/**
 * JSON Schema for Antigravity SSE event payloads.
 *
 * Derived from observed production traffic (response.txt, 354 events) and the
 * Antigravity API Specification. Validates all fields consumed by
 * parseGoogleSSEEvents() and the downstream ContentBlockFSM.
 *
 * Design principles:
 * - Strict on types: every consumed field has its type validated.
 * - Strict on enums: finishReason constrained to known values.
 * - Strict on critical fields, permissive on the rest.
 * - Permissive on unknown fields: additionalProperties not restricted.
 * - anyOf for part types: new part types trigger validation warnings.
 *
 * @see RFC-008 §4.2.1
 */
const antigravitySSEEventSchema = {
    type: 'object',
    required: ['response'],
    properties: {
        response: {
            type: 'object',
            required: ['candidates', 'responseId'],
            properties: {
                candidates: {
                    type: 'array',
                    maxItems: 1,
                    items: {
                        type: 'object',
                        properties: {
                            content: {
                                type: 'object',
                                required: ['role', 'parts'],
                                properties: {
                                    role: {
                                        type: 'string',
                                    },
                                    parts: {
                                        type: 'array',
                                        items: {
                                            type: 'object',
                                            anyOf: [
                                                {
                                                    required: ['text'],
                                                    properties: {
                                                        text: { type: 'string' },
                                                        thought: { type: 'boolean' },
                                                        thoughtSignature: { type: 'string' },
                                                    },
                                                },
                                                {
                                                    required: ['functionCall'],
                                                    properties: {
                                                        functionCall: {
                                                            type: 'object',
                                                            required: ['name'],
                                                            properties: {
                                                                name: {
                                                                    type: 'string',
                                                                    minLength: 1,
                                                                },
                                                                args: { type: 'object' },
                                                                id: { type: 'string' },
                                                            },
                                                        },
                                                    },
                                                },
                                            ],
                                        },
                                    },
                                },
                            },
                            finishReason: {
                                type: 'string',
                                enum: ['STOP', 'MAX_TOKENS', 'OTHER'],
                            },
                        },
                    },
                },
                usageMetadata: {
                    type: 'object',
                    required: ['promptTokenCount', 'candidatesTokenCount', 'totalTokenCount'],
                    properties: {
                        promptTokenCount: { type: 'integer', minimum: 0 },
                        candidatesTokenCount: { type: 'integer', minimum: 0 },
                        totalTokenCount: { type: 'integer', minimum: 0 },
                    },
                },
                modelVersion: { type: 'string' },
                responseId: { type: 'string' },
            },
        },
        traceId: { type: 'string' },
        metadata: { type: 'object' },
    },
};

// ─── Ajv Compilation ─────────────────────────────────────────────────────────

const ajv = new Ajv({ allErrors: true });
const validate = ajv.compile(antigravitySSEEventSchema);

// ─── Error Formatting ────────────────────────────────────────────────────────

/**
 * Format Ajv validation errors into a concise diagnostic string.
 *
 * @param {import('ajv').ErrorObject[]} errors
 * @returns {string}
 */
function formatValidationErrors(errors) {
    const messages = errors.map((err) => {
        const path = err.instancePath ? err.instancePath.slice(1).replace(/\//g, '.') : '(root)';

        return `${path} ${err.message}`;
    });

    const unique = [...new Set(messages)];

    return unique.join('; ');
}

// ─── Public API ──────────────────────────────────────────────────────────────

/**
 * Validate an Antigravity SSE event payload.
 *
 * @param {Object} data - Parsed JSON from an SSE data line
 * @returns {{ valid: true } | { valid: false, errors: string }}
 */
export function validateSSEEvent(data) {
    if (validate(data)) {
        return { valid: true };
    }

    return { valid: false, errors: formatValidationErrors(validate.errors) };
}
