import crypto from 'node:crypto';

import { ANTIGRAVITY_SYSTEM_INSTRUCTION, isThinkingModel } from './constants.js';

// ─── Tool Schema Sanitization Constants ─────────────────────────────────────
// Ported from opencode-antigravity-auth request-helpers.ts & gemini.ts

/**
 * Unsupported constraint keywords that should be moved to description hints.
 * Antigravity rejects these in VALIDATED mode.
 */
const UNSUPPORTED_CONSTRAINTS = [
    'minLength',
    'maxLength',
    'exclusiveMinimum',
    'exclusiveMaximum',
    'pattern',
    'minItems',
    'maxItems',
    'format',
    'default',
    'examples',
];

/**
 * Keywords that should be removed after hint extraction.
 * Superset of UNSUPPORTED_CONSTRAINTS.
 */
const UNSUPPORTED_KEYWORDS = new Set([
    ...UNSUPPORTED_CONSTRAINTS,
    '$schema',
    '$defs',
    'definitions',
    'const',
    '$ref',
    'additionalProperties',
    'propertyNames',
    'title',
    '$id',
    '$comment',
]);

/**
 * Additional fields that toGeminiSchema() strips.
 * These are JSON Schema features that Gemini's protobuf rejects.
 */
const UNSUPPORTED_GEMINI_FIELDS = new Set([
    'additionalProperties',
    '$schema',
    '$id',
    '$comment',
    '$ref',
    '$defs',
    'definitions',
    'const',
    'contentMediaType',
    'contentEncoding',
    'if',
    'then',
    'else',
    'not',
    'patternProperties',
    'unevaluatedProperties',
    'unevaluatedItems',
    'dependentRequired',
    'dependentSchemas',
    'propertyNames',
    'minContains',
    'maxContains',
]);

/**
 * VALIDATED mode requires at least one property in object schemas.
 */
const EMPTY_SCHEMA_PLACEHOLDER_NAME = '_placeholder';
const EMPTY_SCHEMA_PLACEHOLDER_DESCRIPTION = 'Placeholder. Always pass true.';

// ─── Tool Schema Sanitization — Stage 1: cleanSchemaForAntigravity ──────────
// Ported from opencode-antigravity-auth request-helpers.ts
// (cleanJSONSchemaForAntigravity)

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
 * Phase 1a: Converts $ref to description hints.
 * $ref: "#/$defs/Foo" → { type: "object", description: "See: Foo" }
 */
function convertRefsToHints(schema) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map((item) => convertRefsToHints(item));

    if (typeof schema.$ref === 'string') {
        const refVal = schema.$ref;
        const defName = refVal.includes('/') ? refVal.split('/').pop() : refVal;
        const hint = `See: ${defName}`;
        const existingDesc = typeof schema.description === 'string' ? schema.description : '';
        const newDescription = existingDesc ? `${existingDesc} (${hint})` : hint;
        return { type: 'object', description: newDescription };
    }

    const result = {};
    for (const [key, value] of Object.entries(schema)) {
        result[key] = convertRefsToHints(value);
    }
    return result;
}

/**
 * Phase 1b: Converts const to enum.
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
 * Phase 1c: Adds enum hints to description.
 * { enum: ["a", "b", "c"] } → adds "(Allowed: a, b, c)" to description
 * Only for enums with 2-10 items.
 */
function addEnumHints(schema) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map((item) => addEnumHints(item));

    let result = { ...schema };

    if (Array.isArray(result.enum) && result.enum.length > 1 && result.enum.length <= 10) {
        const vals = result.enum.map((v) => String(v)).join(', ');
        result = appendDescriptionHint(result, `Allowed: ${vals}`);
    }

    for (const [key, value] of Object.entries(result)) {
        if (key !== 'enum' && typeof value === 'object' && value !== null) {
            result[key] = addEnumHints(value);
        }
    }

    return result;
}

/**
 * Phase 1d: Adds additionalProperties hints.
 * { additionalProperties: false } → adds "(No extra properties allowed)"
 */
function addAdditionalPropertiesHints(schema) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map((item) => addAdditionalPropertiesHints(item));

    let result = { ...schema };

    if (result.additionalProperties === false) {
        result = appendDescriptionHint(result, 'No extra properties allowed');
    }

    for (const [key, value] of Object.entries(result)) {
        if (key !== 'additionalProperties' && typeof value === 'object' && value !== null) {
            result[key] = addAdditionalPropertiesHints(value);
        }
    }

    return result;
}

/**
 * Phase 1e: Moves unsupported constraints to description hints.
 * { minLength: 1, maxLength: 100 } → adds "(minLength: 1) (maxLength: 100)"
 */
function moveConstraintsToDescription(schema) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map((item) => moveConstraintsToDescription(item));

    let result = { ...schema };

    for (const constraint of UNSUPPORTED_CONSTRAINTS) {
        if (result[constraint] !== undefined && typeof result[constraint] !== 'object') {
            result = appendDescriptionHint(result, `${constraint}: ${result[constraint]}`);
        }
    }

    for (const [key, value] of Object.entries(result)) {
        if (typeof value === 'object' && value !== null) {
            result[key] = moveConstraintsToDescription(value);
        }
    }

    return result;
}

/**
 * Phase 2a: Merges allOf schemas into a single object.
 * { allOf: [{ properties: { a } }, { properties: { b } }] }
 * → { properties: { a, b } }
 */
function mergeAllOf(schema) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map((item) => mergeAllOf(item));

    const result = { ...schema };

    if (Array.isArray(result.allOf)) {
        const merged = {};
        const mergedRequired = [];

        for (const item of result.allOf) {
            if (!item || typeof item !== 'object') continue;

            if (item.properties && typeof item.properties === 'object') {
                merged.properties = { ...merged.properties, ...item.properties };
            }

            if (Array.isArray(item.required)) {
                for (const req of item.required) {
                    if (!mergedRequired.includes(req)) {
                        mergedRequired.push(req);
                    }
                }
            }

            for (const [key, value] of Object.entries(item)) {
                if (key !== 'properties' && key !== 'required' && merged[key] === undefined) {
                    merged[key] = value;
                }
            }
        }

        if (merged.properties) {
            result.properties = { ...result.properties, ...merged.properties };
        }
        if (mergedRequired.length > 0) {
            const existingRequired = Array.isArray(result.required) ? result.required : [];
            result.required = Array.from(new Set([...existingRequired, ...mergedRequired]));
        }

        for (const [key, value] of Object.entries(merged)) {
            if (key !== 'properties' && key !== 'required' && result[key] === undefined) {
                result[key] = value;
            }
        }

        delete result.allOf;
    }

    for (const [key, value] of Object.entries(result)) {
        if (typeof value === 'object' && value !== null) {
            result[key] = mergeAllOf(value);
        }
    }

    return result;
}

/**
 * Scores a schema option for selection in anyOf/oneOf flattening.
 * Higher score = more preferred. (object > array > other > null)
 */
function scoreSchemaOption(schema) {
    if (!schema || typeof schema !== 'object') {
        return { score: 0, typeName: 'unknown' };
    }

    const type = schema.type;

    if (type === 'object' || schema.properties) return { score: 3, typeName: 'object' };
    if (type === 'array' || schema.items) return { score: 2, typeName: 'array' };
    if (type && type !== 'null') return { score: 1, typeName: type };

    return { score: 0, typeName: type || 'null' };
}

/**
 * Checks if an anyOf/oneOf array represents enum choices.
 * Returns the merged enum values if so, otherwise null.
 */
function tryMergeEnumFromUnion(options) {
    if (!Array.isArray(options) || options.length === 0) return null;

    const enumValues = [];

    for (const option of options) {
        if (!option || typeof option !== 'object') return null;

        if (option.const !== undefined) {
            enumValues.push(String(option.const));
            continue;
        }

        if (Array.isArray(option.enum) && option.enum.length === 1) {
            enumValues.push(String(option.enum[0]));
            continue;
        }

        if (Array.isArray(option.enum) && option.enum.length > 0) {
            for (const val of option.enum) {
                enumValues.push(String(val));
            }
            continue;
        }

        if (option.properties || option.items || option.anyOf || option.oneOf || option.allOf) {
            return null;
        }

        if (option.type && !option.const && !option.enum) {
            return null;
        }
    }

    return enumValues.length > 0 ? enumValues : null;
}

/**
 * Phase 2b: Flattens anyOf/oneOf to the best option with type hints.
 * Special handling for enum patterns:
 * { anyOf: [{ const: "a" }, { const: "b" }] } → { type: "string", enum: ["a", "b"] }
 */
function flattenAnyOfOneOf(schema) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map((item) => flattenAnyOfOneOf(item));

    let result = { ...schema };

    for (const unionKey of ['anyOf', 'oneOf']) {
        if (Array.isArray(result[unionKey]) && result[unionKey].length > 0) {
            const options = result[unionKey];
            const parentDesc = typeof result.description === 'string' ? result.description : '';

            // Check if this is an enum pattern
            const mergedEnum = tryMergeEnumFromUnion(options);
            if (mergedEnum !== null) {
                const { [unionKey]: _, ...rest } = result;
                result = { ...rest, type: 'string', enum: mergedEnum };
                if (parentDesc) result.description = parentDesc;
                continue;
            }

            // Standard flattening: score each option and pick the best
            let bestIdx = 0;
            let bestScore = -1;
            const allTypes = [];

            for (let i = 0; i < options.length; i++) {
                const { score, typeName } = scoreSchemaOption(options[i]);
                if (typeName) allTypes.push(typeName);
                if (score > bestScore) {
                    bestScore = score;
                    bestIdx = i;
                }
            }

            let selected = flattenAnyOfOneOf(options[bestIdx]) || { type: 'string' };

            if (parentDesc) {
                const childDesc =
                    typeof selected.description === 'string' ? selected.description : '';
                if (childDesc && childDesc !== parentDesc) {
                    selected = { ...selected, description: `${parentDesc} (${childDesc})` };
                } else if (!childDesc) {
                    selected = { ...selected, description: parentDesc };
                }
            }

            if (allTypes.length > 1) {
                const uniqueTypes = Array.from(new Set(allTypes));
                const hint = `Accepts: ${uniqueTypes.join(' | ')}`;
                selected = appendDescriptionHint(selected, hint);
            }

            const { [unionKey]: _u, description: _d, ...rest } = result;
            result = { ...rest, ...selected };
        }
    }

    for (const [key, value] of Object.entries(result)) {
        if (typeof value === 'object' && value !== null) {
            result[key] = flattenAnyOfOneOf(value);
        }
    }

    return result;
}

/**
 * Phase 2c: Flattens type arrays to single type with nullable hint.
 * { type: ["string", "null"] } → { type: "string", description: "(nullable)" }
 */
function flattenTypeArrays(schema, nullableFields, currentPath) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) {
        return schema.map((item, idx) =>
            flattenTypeArrays(item, nullableFields, `${currentPath || ''}[${idx}]`),
        );
    }

    let result = { ...schema };
    const localNullableFields = nullableFields || new Map();

    if (Array.isArray(result.type)) {
        const types = result.type;
        const hasNull = types.includes('null');
        const nonNullTypes = types.filter((t) => t !== 'null' && t);

        const firstType = nonNullTypes.length > 0 ? nonNullTypes[0] : 'string';
        result.type = firstType;

        if (nonNullTypes.length > 1) {
            result = appendDescriptionHint(result, `Accepts: ${nonNullTypes.join(' | ')}`);
        }

        if (hasNull) {
            result = appendDescriptionHint(result, 'nullable');
        }
    }

    if (result.properties && typeof result.properties === 'object') {
        const newProps = {};
        for (const [propKey, propValue] of Object.entries(result.properties)) {
            const propPath = currentPath
                ? `${currentPath}.properties.${propKey}`
                : `properties.${propKey}`;
            const processed = flattenTypeArrays(propValue, localNullableFields, propPath);
            newProps[propKey] = processed;

            if (
                processed &&
                typeof processed === 'object' &&
                typeof processed.description === 'string' &&
                processed.description.includes('nullable')
            ) {
                const objectPath = currentPath || '';
                const existing = localNullableFields.get(objectPath) || [];
                existing.push(propKey);
                localNullableFields.set(objectPath, existing);
            }
        }
        result.properties = newProps;
    }

    // Remove nullable fields from required array at root level
    if (Array.isArray(result.required) && !nullableFields) {
        const nullableAtRoot = localNullableFields.get('') || [];
        if (nullableAtRoot.length > 0) {
            result.required = result.required.filter((r) => !nullableAtRoot.includes(r));
            if (result.required.length === 0) {
                delete result.required;
            }
        }
    }

    for (const [key, value] of Object.entries(result)) {
        if (key !== 'properties' && typeof value === 'object' && value !== null) {
            result[key] = flattenTypeArrays(
                value,
                localNullableFields,
                `${currentPath || ''}.${key}`,
            );
        }
    }

    return result;
}

/**
 * Phase 3: Removes unsupported keywords after hints have been extracted.
 *
 * @param {Object} schema
 * @param {boolean} insideProperties - When true, keys are property NAMES (preserve)
 * @returns {Object}
 */
function removeUnsupportedKeywords(schema, insideProperties = false) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map((item) => removeUnsupportedKeywords(item, false));

    const result = {};
    for (const [key, value] of Object.entries(schema)) {
        if (!insideProperties && UNSUPPORTED_KEYWORDS.has(key)) continue;

        if (typeof value === 'object' && value !== null) {
            if (key === 'properties') {
                const propertiesResult = {};
                for (const [propName, propSchema] of Object.entries(value)) {
                    propertiesResult[propName] = removeUnsupportedKeywords(propSchema, false);
                }
                result[key] = propertiesResult;
            } else {
                result[key] = removeUnsupportedKeywords(value, false);
            }
        } else {
            result[key] = value;
        }
    }
    return result;
}

/**
 * Phase 3b: Cleans up required fields — removes entries that don't exist in properties.
 */
function cleanupRequiredFields(schema) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map((item) => cleanupRequiredFields(item));

    const result = { ...schema };

    if (
        Array.isArray(result.required) &&
        result.properties &&
        typeof result.properties === 'object'
    ) {
        const validRequired = result.required.filter((req) =>
            Object.prototype.hasOwnProperty.call(result.properties, req),
        );
        if (validRequired.length === 0) {
            delete result.required;
        } else if (validRequired.length !== result.required.length) {
            result.required = validRequired;
        }
    }

    for (const [key, value] of Object.entries(result)) {
        if (typeof value === 'object' && value !== null) {
            result[key] = cleanupRequiredFields(value);
        }
    }

    return result;
}

/**
 * Phase 4: Adds placeholder property for empty object schemas.
 * VALIDATED mode requires at least one property.
 */
function addEmptySchemaPlaceholder(schema) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map((item) => addEmptySchemaPlaceholder(item));

    const result = { ...schema };

    if (result.type === 'object') {
        const hasProperties =
            result.properties &&
            typeof result.properties === 'object' &&
            Object.keys(result.properties).length > 0;

        if (!hasProperties) {
            result.properties = {
                [EMPTY_SCHEMA_PLACEHOLDER_NAME]: {
                    type: 'boolean',
                    description: EMPTY_SCHEMA_PLACEHOLDER_DESCRIPTION,
                },
            };
            result.required = [EMPTY_SCHEMA_PLACEHOLDER_NAME];
        }
    }

    for (const [key, value] of Object.entries(result)) {
        if (typeof value === 'object' && value !== null) {
            result[key] = addEmptySchemaPlaceholder(value);
        }
    }

    return result;
}

/**
 * Clean a JSON Schema for Antigravity API compatibility.
 * Transforms unsupported features into description hints while preserving
 * semantic information.
 *
 * Ported from opencode-antigravity-auth request-helpers.ts
 * (cleanJSONSchemaForAntigravity)
 *
 * @param {Object} schema - JSON Schema object
 * @returns {Object} Cleaned schema
 */
function cleanSchemaForAntigravity(schema) {
    if (!schema || typeof schema !== 'object') return schema;

    let result = schema;

    // Phase 1: Convert and add hints (information-preserving transforms)
    result = convertRefsToHints(result);
    result = convertConstToEnum(result);
    result = addEnumHints(result);
    result = addAdditionalPropertiesHints(result);
    result = moveConstraintsToDescription(result);

    // Phase 2: Flatten complex structures
    result = mergeAllOf(result);
    result = flattenAnyOfOneOf(result);
    result = flattenTypeArrays(result);

    // Phase 3: Remove unsupported keywords
    result = removeUnsupportedKeywords(result);
    result = cleanupRequiredFields(result);

    // Phase 4: Add placeholder for empty object schemas
    result = addEmptySchemaPlaceholder(result);

    return result;
}

// ─── Tool Schema Sanitization — Stage 2: toGeminiSchema ─────────────────────
// Ported from opencode-antigravity-auth gemini.ts (toGeminiSchema)

/**
 * Transform a JSON Schema to Gemini-compatible format.
 *
 * Key transformations:
 * - Converts type values to uppercase (object → OBJECT)
 * - Removes unsupported fields (additionalProperties, $schema, etc.)
 * - Recursively processes nested schemas (properties, items, anyOf, etc.)
 * - Ensures arrays have items (Gemini API requirement)
 * - Filters required to only existing properties
 *
 * @param {*} schema - A JSON Schema object or primitive
 * @returns {*} Gemini-compatible schema
 */
function toGeminiSchema(schema) {
    if (!schema || typeof schema !== 'object' || Array.isArray(schema)) {
        return schema;
    }

    const result = {};
    const propertyNames = new Set();

    // Collect property names for required validation
    if (schema.properties && typeof schema.properties === 'object') {
        for (const name of Object.keys(schema.properties)) {
            propertyNames.add(name);
        }
    }

    for (const [key, value] of Object.entries(schema)) {
        if (UNSUPPORTED_GEMINI_FIELDS.has(key)) continue;

        if (key === 'type' && typeof value === 'string') {
            result[key] = value.toUpperCase();
        } else if (key === 'properties' && typeof value === 'object' && value !== null) {
            const props = {};
            for (const [propName, propSchema] of Object.entries(value)) {
                props[propName] = toGeminiSchema(propSchema);
            }
            result[key] = props;
        } else if (key === 'items' && typeof value === 'object') {
            result[key] = toGeminiSchema(value);
        } else if (
            (key === 'anyOf' || key === 'oneOf' || key === 'allOf') &&
            Array.isArray(value)
        ) {
            result[key] = value.map((item) => toGeminiSchema(item));
        } else if (key === 'required' && Array.isArray(value)) {
            if (propertyNames.size > 0) {
                const valid = value.filter((p) => typeof p === 'string' && propertyNames.has(p));
                if (valid.length > 0) result[key] = valid;
            } else {
                result[key] = value;
            }
        } else {
            result[key] = value;
        }
    }

    // Arrays must have items (Gemini API requirement)
    if (result.type === 'ARRAY' && !result.items) {
        result.items = { type: 'STRING' };
    }

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
 * Extract text content from a tool_result content field.
 *
 * @param {string|Array|*} content
 * @returns {string}
 */
function extractTextContent(content) {
    if (typeof content === 'string') return content;
    if (Array.isArray(content)) {
        return content
            .filter((c) => c.type === 'text')
            .map((c) => c.text)
            .join('\n');
    }
    return '';
}

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

            case 'image':
                if (block.source?.type === 'base64') {
                    parts.push({
                        inlineData: {
                            mimeType: block.source.media_type,
                            data: block.source.data,
                        },
                    });
                }
                break;

            case 'tool_use':
                parts.push({
                    functionCall: {
                        id: block.id,
                        name: block.name,
                        args: block.input || {},
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
                        response: { result: extractTextContent(block.content) },
                    },
                });
                break;

            case 'thinking':
                // Only include thinking blocks with valid signatures
                if (block.signature?.length >= 50) {
                    parts.push({
                        text: block.thinking,
                        thought: true,
                        thoughtSignature: block.signature,
                    });
                }
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
        model,
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

    // Convert system instruction with Antigravity identity injection
    // Both reference implementations require this for CLIProxyAPI v6.6.89 compatibility
    const systemParts = [{ text: ANTIGRAVITY_SYSTEM_INSTRUCTION }];

    if (system) {
        const userSystemParts =
            typeof system === 'string'
                ? [{ text: system }]
                : system.filter((b) => b.type === 'text').map((b) => ({ text: b.text }));
        systemParts.push(...userSystemParts);
    }

    googleRequest.systemInstruction = {
        role: 'user',
        parts: systemParts,
    };

    // Convert messages to contents
    for (const msg of messages) {
        const parts = convertContentToParts(msg.content);
        // Google API requires at least one part per content message.
        // This can happen when all thinking blocks are filtered out (unsigned).
        if (parts.length === 0) {
            parts.push({ text: '.' });
        }
        googleRequest.contents.push({
            role: msg.role === 'assistant' ? 'model' : 'user',
            parts,
        });
    }

    // Generation config
    if (max_tokens) googleRequest.generationConfig.maxOutputTokens = max_tokens;
    if (temperature !== undefined) googleRequest.generationConfig.temperature = temperature;
    if (top_p !== undefined) googleRequest.generationConfig.topP = top_p;
    if (top_k !== undefined) googleRequest.generationConfig.topK = top_k;
    if (stop_sequences?.length) googleRequest.generationConfig.stopSequences = stop_sequences;

    // Thinking config for thinking models
    if (isThinkingModel(model)) {
        const thinkingBudget = thinking?.budget_tokens;
        googleRequest.generationConfig.thinkingConfig = {
            include_thoughts: true,
            ...(thinkingBudget ? { thinking_budget: thinkingBudget } : {}),
        };

        // Validate max_tokens > thinking_budget (API requirement)
        if (thinkingBudget && max_tokens && max_tokens <= thinkingBudget) {
            googleRequest.generationConfig.maxOutputTokens = thinkingBudget + 8192;
        }
    }

    // Tools — sanitize schemas through the two-stage pipeline
    if (tools?.length) {
        googleRequest.tools = [
            {
                functionDeclarations: tools.map((tool) => ({
                    name: tool.name,
                    description: tool.description || '',
                    parameters: toGeminiSchema(
                        cleanSchemaForAntigravity(tool.input_schema || { type: 'object' }),
                    ),
                })),
            },
        ];
        googleRequest.toolConfig = {
            functionCallingConfig: { mode: 'VALIDATED' },
        };
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

// ─── SSE Stream Conversion (Google → Anthropic) ─────────────────────────────

/**
 * Stream and convert Google SSE events to Anthropic format.
 *
 * Reads the response body stream from Cloud Code, parses each SSE data line,
 * and yields Anthropic-format SSE event strings. Handles thinking blocks,
 * text blocks, and tool use blocks with correct block indexing.
 *
 * @param {ReadableStream} stream - Response body stream from Cloud Code
 * @param {string} model - Model name for the response
 * @param {boolean} [debug=false] - Enable debug logging of SSE chunks
 * @yields {string} Anthropic SSE event lines
 */
export async function* streamSSEResponse(stream, model, debug = false) {
    const messageId = `msg_${randomHex(16)}`;
    let blockIndex = 0;
    let currentBlockType = null;
    let inputTokens = 0;
    let outputTokens = 0;
    let cacheReadTokens = 0;
    let stopReason = null;
    let hasEmittedStart = false;

    const reader = stream.getReader();
    const decoder = new TextDecoder();
    let buffer = '';

    while (true) {
        const { done, value } = await reader.read();
        if (done) break;

        buffer += decoder.decode(value, { stream: true });
        const lines = buffer.split('\n');
        buffer = lines.pop() || '';

        for (const line of lines) {
            if (!line.startsWith('data:')) continue;

            const jsonText = line.slice(5).trim();
            if (!jsonText) continue;

            if (debug) {
                console.error(`[Odin:debug] ← SSE:`, line.trimEnd());
            }

            try {
                const data = JSON.parse(jsonText);
                const innerResponse = data.response || data;

                // Update usage
                const usage = innerResponse.usageMetadata;
                if (usage) {
                    inputTokens = usage.promptTokenCount || inputTokens;
                    outputTokens = usage.candidatesTokenCount || outputTokens;
                    cacheReadTokens = usage.cachedContentTokenCount || cacheReadTokens;
                }

                const parts = innerResponse.candidates?.[0]?.content?.parts || [];

                // Emit message_start on first content
                if (!hasEmittedStart && parts.length > 0) {
                    hasEmittedStart = true;
                    const event = formatSSE('message_start', {
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
                    });
                    if (debug) {
                        console.error(`[Odin:debug] → SSE:`, event.trimEnd());
                    }
                    yield event;
                }

                // Process parts
                for (const part of parts) {
                    if (part.thought === true) {
                        // Thinking block
                        if (currentBlockType !== 'thinking') {
                            if (currentBlockType !== null) {
                                const stopEvent = formatSSE('content_block_stop', {
                                    type: 'content_block_stop',
                                    index: blockIndex,
                                });
                                if (debug)
                                    console.error(`[Odin:debug] → SSE:`, stopEvent.trimEnd());
                                yield stopEvent;
                                blockIndex++;
                            }
                            currentBlockType = 'thinking';
                            const startEvent = formatSSE('content_block_start', {
                                type: 'content_block_start',
                                index: blockIndex,
                                content_block: { type: 'thinking', thinking: '' },
                            });
                            if (debug) console.error(`[Odin:debug] → SSE:`, startEvent.trimEnd());
                            yield startEvent;
                        }
                        const deltaEvent = formatSSE('content_block_delta', {
                            type: 'content_block_delta',
                            index: blockIndex,
                            delta: { type: 'thinking_delta', thinking: part.text || '' },
                        });
                        if (debug) console.error(`[Odin:debug] → SSE:`, deltaEvent.trimEnd());
                        yield deltaEvent;

                        // Emit signature if present
                        if (part.thoughtSignature?.length >= 50) {
                            const sigEvent = formatSSE('content_block_delta', {
                                type: 'content_block_delta',
                                index: blockIndex,
                                delta: {
                                    type: 'signature_delta',
                                    signature: part.thoughtSignature,
                                },
                            });
                            if (debug) console.error(`[Odin:debug] → SSE:`, sigEvent.trimEnd());
                            yield sigEvent;
                        }
                    } else if (part.text !== undefined) {
                        // Text block
                        if (currentBlockType !== 'text') {
                            if (currentBlockType !== null) {
                                const stopEvent = formatSSE('content_block_stop', {
                                    type: 'content_block_stop',
                                    index: blockIndex,
                                });
                                if (debug)
                                    console.error(`[Odin:debug] → SSE:`, stopEvent.trimEnd());
                                yield stopEvent;
                                blockIndex++;
                            }
                            currentBlockType = 'text';
                            const startEvent = formatSSE('content_block_start', {
                                type: 'content_block_start',
                                index: blockIndex,
                                content_block: { type: 'text', text: '' },
                            });
                            if (debug) console.error(`[Odin:debug] → SSE:`, startEvent.trimEnd());
                            yield startEvent;
                        }
                        const deltaEvent = formatSSE('content_block_delta', {
                            type: 'content_block_delta',
                            index: blockIndex,
                            delta: { type: 'text_delta', text: part.text },
                        });
                        if (debug) console.error(`[Odin:debug] → SSE:`, deltaEvent.trimEnd());
                        yield deltaEvent;
                    } else if (part.functionCall) {
                        // Tool use block — each tool call gets its own block
                        if (currentBlockType !== null) {
                            const stopEvent = formatSSE('content_block_stop', {
                                type: 'content_block_stop',
                                index: blockIndex,
                            });
                            if (debug) console.error(`[Odin:debug] → SSE:`, stopEvent.trimEnd());
                            yield stopEvent;
                            blockIndex++;
                        }
                        currentBlockType = 'tool_use';
                        stopReason = 'tool_use';

                        const toolId = part.functionCall.id || `toolu_${randomHex(12)}`;
                        const startEvent = formatSSE('content_block_start', {
                            type: 'content_block_start',
                            index: blockIndex,
                            content_block: {
                                type: 'tool_use',
                                id: toolId,
                                name: part.functionCall.name,
                                input: {},
                            },
                        });
                        if (debug) console.error(`[Odin:debug] → SSE:`, startEvent.trimEnd());
                        yield startEvent;

                        const deltaEvent = formatSSE('content_block_delta', {
                            type: 'content_block_delta',
                            index: blockIndex,
                            delta: {
                                type: 'input_json_delta',
                                partial_json: JSON.stringify(part.functionCall.args || {}),
                            },
                        });
                        if (debug) console.error(`[Odin:debug] → SSE:`, deltaEvent.trimEnd());
                        yield deltaEvent;
                    }
                }

                // Check finish reason
                const finishReason = innerResponse.candidates?.[0]?.finishReason;
                if (finishReason && !stopReason) {
                    stopReason = finishReason === 'MAX_TOKENS' ? 'max_tokens' : 'end_turn';
                }
            } catch (e) {
                // Skip malformed JSON
                if (debug) {
                    console.error(`[Odin:debug] ← SSE parse error:`, e.message);
                }
            }
        }
    }

    // Close final block
    if (currentBlockType !== null) {
        const stopEvent = formatSSE('content_block_stop', {
            type: 'content_block_stop',
            index: blockIndex,
        });
        if (debug) console.error(`[Odin:debug] → SSE:`, stopEvent.trimEnd());
        yield stopEvent;
    }

    // Emit message_delta and message_stop
    const msgDelta = formatSSE('message_delta', {
        type: 'message_delta',
        delta: { stop_reason: stopReason || 'end_turn', stop_sequence: null },
        usage: {
            output_tokens: outputTokens,
            cache_read_input_tokens: cacheReadTokens,
            cache_creation_input_tokens: 0,
        },
    });
    if (debug) console.error(`[Odin:debug] → SSE:`, msgDelta.trimEnd());
    yield msgDelta;

    const msgStop = formatSSE('message_stop', { type: 'message_stop' });
    if (debug) console.error(`[Odin:debug] → SSE:`, msgStop.trimEnd());
    yield msgStop;
}

// ─── Response Conversion (Google → Anthropic) ───────────────────────────────

/**
 * Convert Google response to Anthropic format.
 *
 * @param {Object} googleResponse - Google Generative AI response
 * @param {string} model - Model name for the response
 * @returns {Object} Anthropic Messages API response
 */
export function googleToAnthropic(googleResponse, model) {
    const response = googleResponse.response || googleResponse;
    const candidates = response.candidates || [];
    const firstCandidate = candidates[0] || {};
    const parts = firstCandidate.content?.parts || [];

    const content = [];
    let hasToolCalls = false;

    for (const part of parts) {
        if (part.text !== undefined) {
            if (part.thought === true) {
                content.push({
                    type: 'thinking',
                    thinking: part.text,
                    signature: part.thoughtSignature || '',
                });
            } else {
                content.push({ type: 'text', text: part.text });
            }
        } else if (part.functionCall) {
            content.push({
                type: 'tool_use',
                id: part.functionCall.id || `toolu_${randomHex(12)}`,
                name: part.functionCall.name,
                input: part.functionCall.args || {},
            });
            hasToolCalls = true;
        }
    }

    // Determine stop reason
    const finishReason = firstCandidate.finishReason;
    let stopReason = 'end_turn';
    if (finishReason === 'MAX_TOKENS') stopReason = 'max_tokens';
    else if (hasToolCalls) stopReason = 'tool_use';

    // Usage calculation
    const usage = response.usageMetadata || {};
    const cachedTokens = usage.cachedContentTokenCount || 0;

    return {
        id: `msg_${randomHex(16)}`,
        type: 'message',
        role: 'assistant',
        content: content.length > 0 ? content : [{ type: 'text', text: '' }],
        model,
        stop_reason: stopReason,
        stop_sequence: null,
        usage: {
            input_tokens: (usage.promptTokenCount || 0) - cachedTokens,
            output_tokens: usage.candidatesTokenCount || 0,
            cache_read_input_tokens: cachedTokens,
            cache_creation_input_tokens: 0,
        },
    };
}
