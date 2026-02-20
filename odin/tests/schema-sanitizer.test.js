/**
 * Tests for RFC-006: JSON Schema Sanitization Overhaul
 *
 * Covers:
 * - Keyword registries (ALLOWED_KEYWORDS, MONITORABLE_KEYWORDS, KNOWN_UNSUPPORTED_KEYWORDS)
 * - Phase 1: Semantic transforms (const→enum, exclusiveBounds→min/max+hint)
 * - Phase 2: Whitelist filter with three-tier logging
 * - Orchestration (sanitizeSchemaForAntigravity)
 * - All 15 test scenarios from RFC §6
 */

import { describe, it, beforeEach } from 'node:test';
import assert from 'node:assert/strict';
import { sanitizeSchemaForAntigravity } from '../src/converter.js';

// ─── Test Logger ────────────────────────────────────────────────────────────

/**
 * Creates a mock logger that captures log calls for assertion.
 */
function createMockLogger() {
    const logs = { warn: [], info: [] };

    return {
        warn: (msg) => logs.warn.push(msg),
        info: (msg) => logs.info.push(msg),
        logs,
    };
}

// ─── RFC §6 Test Scenarios ──────────────────────────────────────────────────

describe('sanitizeSchemaForAntigravity', () => {
    let logger;

    beforeEach(() => {
        logger = createMockLogger();
    });

    // ── Scenario 1: Supported schema passthrough ────────────────────────

    it('Scenario 1: supported schema passes through unchanged', () => {
        const input = {
            type: 'object',
            properties: {
                name: { type: 'string' },
            },
            required: ['name'],
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result, input);
        assert.equal(logger.logs.warn.length, 0);
        assert.equal(logger.logs.info.length, 0);
    });

    // ── Scenario 2: $ref/$defs stripping + monitoring log ───────────────

    it('Scenario 2: $ref/$defs stripped with warn log', () => {
        const input = {
            $defs: { Addr: { type: 'object', properties: { street: { type: 'string' } } } },
            properties: {
                home: { $ref: '#/$defs/Addr' },
            },
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        // $defs should be stripped
        assert.equal(result.$defs, undefined);
        // $ref inside home should be stripped, leaving empty object
        assert.deepStrictEqual(result.properties.home, {});
        // Both $defs and $ref should emit warn logs
        assert.ok(logger.logs.warn.some((m) => m.includes('"$defs"')));
        assert.ok(logger.logs.warn.some((m) => m.includes('"$ref"')));
    });

    // ── Scenario 3: const → enum ────────────────────────────────────────

    it('Scenario 3: const converted to enum', () => {
        const input = {
            properties: {
                status: { const: 'active' },
            },
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result, {
            properties: {
                status: { enum: ['active'] },
            },
        });
    });

    // ── Scenario 4: exclusiveMinimum on integer ─────────────────────────

    it('Scenario 4: exclusiveMinimum on integer → minimum + hint', () => {
        const input = {
            type: 'integer',
            exclusiveMinimum: 0,
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result, {
            type: 'integer',
            minimum: 1,
            description: 'exclusiveMinimum: 0',
        });
    });

    // ── Scenario 5: exclusiveMaximum on number ──────────────────────────

    it('Scenario 5: exclusiveMaximum on number → hint only, no maximum', () => {
        const input = {
            type: 'number',
            exclusiveMaximum: 100,
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result, {
            type: 'number',
            description: 'exclusiveMaximum: 100',
        });
        // No maximum should be set for type: "number"
        assert.equal(result.maximum, undefined);
    });

    // ── Scenario 6: if/then/else stripping + monitoring ─────────────────

    it('Scenario 6: if/then/else stripped with warn log', () => {
        const input = {
            type: 'object',
            if: { properties: { type: { const: 'a' } } },
            then: { required: ['a_field'] },
            else: { required: ['b_field'] },
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.equal(result.if, undefined);
        assert.equal(result.then, undefined);
        assert.equal(result.else, undefined);
        assert.equal(result.type, 'object');
        // Each should emit a warn log
        assert.ok(logger.logs.warn.some((m) => m.includes('"if"')));
        assert.ok(logger.logs.warn.some((m) => m.includes('"then"')));
        assert.ok(logger.logs.warn.some((m) => m.includes('"else"')));
    });

    // ── Scenario 7: dependentRequired stripping + monitoring ────────────

    it('Scenario 7: dependentRequired stripped with warn log', () => {
        const input = {
            type: 'object',
            dependentRequired: { cc: ['billing'] },
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.equal(result.dependentRequired, undefined);
        assert.ok(logger.logs.warn.some((m) => m.includes('"dependentRequired"')));
    });

    // ── Scenario 8: Meta-data stripping + monitoring ────────────────────

    it('Scenario 8: title/deprecated stripped, description preserved', () => {
        const input = {
            title: 'User',
            deprecated: true,
            description: 'The user',
            type: 'object',
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.equal(result.title, undefined);
        assert.equal(result.deprecated, undefined);
        assert.equal(result.description, 'The user');
        assert.equal(result.type, 'object');
        // Warn logs for monitorable keywords
        assert.ok(logger.logs.warn.some((m) => m.includes('"title"')));
        assert.ok(logger.logs.warn.some((m) => m.includes('"deprecated"')));
    });

    // ── Scenario 9: examples stripping + monitoring ─────────────────────

    it('Scenario 9: examples stripped with warn log', () => {
        const input = {
            type: 'string',
            examples: ['a', 'b'],
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.equal(result.examples, undefined);
        assert.deepStrictEqual(result, { type: 'string' });
        assert.ok(logger.logs.warn.some((m) => m.includes('"examples"')));
    });

    // ── Scenario 10: Unknown keyword logging ────────────────────────────

    it('Scenario 10: unknown keywords stripped with info log', () => {
        const input = {
            type: 'string',
            $futureKeyword: true,
            'x-custom': 'foo',
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result, { type: 'string' });
        // Unknown keywords should emit info logs (not warn)
        assert.ok(logger.logs.info.some((m) => m.includes('"$futureKeyword"')));
        assert.ok(logger.logs.info.some((m) => m.includes('"x-custom"')));
        assert.equal(logger.logs.warn.length, 0);
    });

    // ── Scenario 11: Known unsupported (silent) ─────────────────────────

    it('Scenario 11: known unsupported keywords stripped silently', () => {
        const input = {
            type: 'string',
            $schema: 'https://json-schema.org/draft/2020-12/schema',
            $id: 'urn:example:test',
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result, { type: 'string' });
        // Should be stripped silently — no warn or info logs
        assert.equal(logger.logs.warn.length, 0);
        assert.equal(logger.logs.info.length, 0);
    });

    // ── Scenario 12: Deeply nested unsupported ──────────────────────────

    it('Scenario 12: deeply nested unsupported keywords stripped', () => {
        const input = {
            properties: {
                tags: {
                    type: 'array',
                    items: {
                        contentEncoding: 'base64',
                    },
                    uniqueItems: true,
                },
            },
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.equal(result.properties.tags.uniqueItems, undefined);
        assert.equal(result.properties.tags.items.contentEncoding, undefined);
        // Both are monitorable — warn logs
        assert.ok(logger.logs.warn.some((m) => m.includes('"contentEncoding"')));
        assert.ok(logger.logs.warn.some((m) => m.includes('"uniqueItems"')));
    });

    // ── Scenario 13: Deprecated definitions ─────────────────────────────

    it('Scenario 13: deprecated definitions stripped with warn log', () => {
        const input = {
            definitions: { Foo: { type: 'string' } },
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.equal(result.definitions, undefined);
        assert.ok(logger.logs.warn.some((m) => m.includes('"definitions"')));
    });

    // ── Scenario 14: allOf/anyOf/oneOf passthrough ──────────────────────

    it('Scenario 14: allOf/anyOf/oneOf pass through with recursive sanitization', () => {
        const input = {
            anyOf: [{ type: 'string' }, { type: 'integer' }],
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result, {
            anyOf: [{ type: 'string' }, { type: 'integer' }],
        });
    });

    it('Scenario 14b: sub-schemas within anyOf are recursively sanitized', () => {
        const input = {
            anyOf: [
                { type: 'string', title: 'Name' },
                { type: 'integer', $comment: 'age field' },
            ],
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result, {
            anyOf: [{ type: 'string' }, { type: 'integer' }],
        });
        // title is monitorable → warn
        assert.ok(logger.logs.warn.some((m) => m.includes('"title"')));
    });

    // ── Scenario 15: propertyNames stripping + monitoring ───────────────

    it('Scenario 15: propertyNames stripped with warn log', () => {
        const input = {
            type: 'object',
            propertyNames: { pattern: '^[a-z]+$' },
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.equal(result.propertyNames, undefined);
        assert.deepStrictEqual(result, { type: 'object' });
        assert.ok(logger.logs.warn.some((m) => m.includes('"propertyNames"')));
    });
});

// ─── Additional Edge Case Tests ─────────────────────────────────────────────

describe('sanitizeSchemaForAntigravity — edge cases', () => {
    let logger;

    beforeEach(() => {
        logger = createMockLogger();
    });

    it('handles null/undefined input gracefully', () => {
        assert.equal(sanitizeSchemaForAntigravity(null), null);
        assert.equal(sanitizeSchemaForAntigravity(undefined), undefined);
    });

    it('handles primitive input gracefully', () => {
        assert.equal(sanitizeSchemaForAntigravity('string'), 'string');
        assert.equal(sanitizeSchemaForAntigravity(42), 42);
        assert.equal(sanitizeSchemaForAntigravity(true), true);
    });

    it('works without logger parameter', () => {
        const input = {
            type: 'object',
            title: 'Test',
            $schema: 'https://json-schema.org/draft/2020-12/schema',
            $futureKeyword: true,
        };

        // Should not throw when logger is undefined
        const result = sanitizeSchemaForAntigravity(input);

        assert.deepStrictEqual(result, { type: 'object' });
    });

    it('all 23 allowed keywords pass through', () => {
        // Build a schema with every allowed keyword (where semantically valid)
        const input = {
            type: 'object',
            enum: ['a', 'b'],
            maximum: 100,
            minimum: 0,
            maxLength: 255,
            minLength: 1,
            pattern: '^[a-z]+$',
            maxItems: 10,
            minItems: 1,
            maxProperties: 5,
            minProperties: 1,
            required: ['name'],
            description: 'test',
            default: 'hello',
            format: 'email',
            properties: { name: { type: 'string' } },
            additionalProperties: false,
            items: { type: 'string' },
            allOf: [{ type: 'object' }],
            anyOf: [{ type: 'string' }],
            oneOf: [{ type: 'integer' }],
            not: { type: 'null' },
            prefixItems: [{ type: 'string' }],
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        // All 23 keywords should be present in the result
        const expectedKeys = [
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
            'description',
            'default',
            'format',
            'properties',
            'additionalProperties',
            'items',
            'allOf',
            'anyOf',
            'oneOf',
            'not',
            'prefixItems',
        ];

        for (const key of expectedKeys) {
            assert.ok(result[key] !== undefined, `Expected keyword "${key}" to be preserved`);
        }

        assert.equal(logger.logs.warn.length, 0);
        assert.equal(logger.logs.info.length, 0);
    });

    it('const → enum does not override existing enum', () => {
        const input = {
            const: 'x',
            enum: ['a', 'b'],
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        // When both const and enum exist, keep enum (const is stripped by whitelist)
        assert.deepStrictEqual(result.enum, ['a', 'b']);
    });

    it('exclusiveMinimum on integer with existing minimum — no override', () => {
        const input = {
            type: 'integer',
            exclusiveMinimum: 5,
            minimum: 10,
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        // Should keep existing minimum, not set minimum = 6
        assert.equal(result.minimum, 10);
        assert.equal(result.description, 'exclusiveMinimum: 5');
    });

    it('both exclusiveMinimum and exclusiveMaximum on integer', () => {
        const input = {
            type: 'integer',
            exclusiveMinimum: 0,
            exclusiveMaximum: 10,
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.equal(result.minimum, 1);
        assert.equal(result.maximum, 9);
        assert.ok(result.description.includes('exclusiveMinimum: 0'));
        assert.ok(result.description.includes('exclusiveMaximum: 10'));
    });

    it('deeply nested const → enum transform', () => {
        const input = {
            properties: {
                nested: {
                    properties: {
                        deep: { const: 'value' },
                    },
                },
            },
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result.properties.nested.properties.deep, {
            enum: ['value'],
        });
    });

    it('deprecated $recursiveAnchor/$recursiveRef stripped silently', () => {
        const input = {
            type: 'object',
            $recursiveAnchor: true,
            $recursiveRef: '#',
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result, { type: 'object' });
        // These are in KNOWN_UNSUPPORTED but not in MONITORABLE — silent
        assert.equal(logger.logs.warn.length, 0);
        assert.equal(logger.logs.info.length, 0);
    });

    it('all monitorable keywords emit warn logs', () => {
        const monitorableKeywords = [
            '$ref',
            '$defs',
            'if',
            'then',
            'else',
            'dependentSchemas',
            'contains',
            'propertyNames',
            'patternProperties',
            'multipleOf',
            'uniqueItems',
            'maxContains',
            'minContains',
            'dependentRequired',
            'unevaluatedItems',
            'unevaluatedProperties',
            'title',
            'deprecated',
            'readOnly',
            'writeOnly',
            'examples',
            'contentEncoding',
            'contentMediaType',
            'contentSchema',
            'definitions',
            'dependencies',
        ];

        const input = { type: 'string' };
        for (const kw of monitorableKeywords) {
            input[kw] = 'test-value';
        }

        sanitizeSchemaForAntigravity(input, logger);

        for (const kw of monitorableKeywords) {
            assert.ok(
                logger.logs.warn.some((m) => m.includes(`"${kw}"`)),
                `Expected warn log for monitorable keyword "${kw}"`,
            );
        }
    });

    it('allOf sub-schemas are recursively sanitized', () => {
        const input = {
            allOf: [
                { type: 'object', title: 'Schema1' },
                { type: 'object', $comment: 'internal' },
            ],
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result, {
            allOf: [{ type: 'object' }, { type: 'object' }],
        });
    });

    it('prefixItems sub-schemas are recursively sanitized', () => {
        const input = {
            type: 'array',
            prefixItems: [
                { type: 'string', title: 'First' },
                { type: 'integer', $id: 'second' },
            ],
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result, {
            type: 'array',
            prefixItems: [{ type: 'string' }, { type: 'integer' }],
        });
    });

    it('additionalProperties (schema object) is recursively sanitized', () => {
        const input = {
            type: 'object',
            additionalProperties: {
                type: 'string',
                title: 'Extra',
                $comment: 'allow extras',
            },
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result, {
            type: 'object',
            additionalProperties: { type: 'string' },
        });
    });

    it('additionalProperties (boolean false) passes through', () => {
        const input = {
            type: 'object',
            properties: { name: { type: 'string' } },
            additionalProperties: false,
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.equal(result.additionalProperties, false);
    });

    it('not sub-schema is recursively sanitized', () => {
        const input = {
            not: { type: 'null', title: 'NotNull' },
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        assert.deepStrictEqual(result, {
            not: { type: 'null' },
        });
    });

    it('complex real-world MCP tool schema', () => {
        const input = {
            $schema: 'https://json-schema.org/draft/2020-12/schema',
            type: 'object',
            title: 'FileReadParams',
            description: 'Parameters for reading a file',
            properties: {
                path: {
                    type: 'string',
                    description: 'File path to read',
                    minLength: 1,
                    examples: ['/home/user/file.txt'],
                },
                encoding: {
                    type: 'string',
                    description: 'File encoding',
                    enum: ['utf-8', 'ascii', 'binary'],
                    default: 'utf-8',
                },
                maxBytes: {
                    type: 'integer',
                    description: 'Maximum bytes to read',
                    exclusiveMinimum: 0,
                    exclusiveMaximum: 10485760,
                },
            },
            required: ['path'],
            additionalProperties: false,
        };

        const result = sanitizeSchemaForAntigravity(input, logger);

        // $schema stripped silently
        assert.equal(result.$schema, undefined);
        // title stripped with warn
        assert.equal(result.title, undefined);
        // description preserved
        assert.equal(result.description, 'Parameters for reading a file');
        // properties preserved and recursively sanitized
        assert.equal(result.properties.path.type, 'string');
        assert.equal(result.properties.path.minLength, 1);
        assert.equal(result.properties.path.examples, undefined); // stripped
        // enum and default preserved
        assert.deepStrictEqual(result.properties.encoding.enum, ['utf-8', 'ascii', 'binary']);
        assert.equal(result.properties.encoding.default, 'utf-8');
        // exclusiveBounds converted for integer
        assert.equal(result.properties.maxBytes.minimum, 1);
        assert.equal(result.properties.maxBytes.maximum, 10485759);
        assert.ok(result.properties.maxBytes.description.includes('exclusiveMinimum: 0'));
        assert.ok(result.properties.maxBytes.description.includes('exclusiveMaximum: 10485760'));
        // required and additionalProperties preserved
        assert.deepStrictEqual(result.required, ['path']);
        assert.equal(result.additionalProperties, false);
    });
});
