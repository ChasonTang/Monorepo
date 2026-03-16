import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import { handleRequest } from '../src/server.js';

const API_KEY = 'test-api-key-12345';
const API_KEY_HASH = crypto.createHash('sha256').update(API_KEY).digest();

function makeHeaders(token) {
  if (token === undefined) return {};
  return { authorization: token };
}

describe('handleRequest — route matching', () => {
  it('returns 404 for unknown path', () => {
    const res = handleRequest(
      { method: 'POST', url: '/v1/unknown', headers: makeHeaders(`Bearer ${API_KEY}`) },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 404);
    const body = JSON.parse(res.body);
    assert.equal(body.type, 'error');
    assert.equal(body.error.type, 'not_found_error');
  });

  it('returns 404 for root path', () => {
    const res = handleRequest(
      { method: 'POST', url: '/', headers: makeHeaders(`Bearer ${API_KEY}`) },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 404);
  });

  it('recognizes /v1/messages', () => {
    const res = handleRequest(
      { method: 'POST', url: '/v1/messages', headers: makeHeaders(`Bearer ${API_KEY}`) },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 501);
  });

  it('recognizes /v1/messages/count_tokens', () => {
    const res = handleRequest(
      { method: 'POST', url: '/v1/messages/count_tokens', headers: makeHeaders(`Bearer ${API_KEY}`) },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 501);
  });
});

describe('handleRequest — method validation', () => {
  it('returns 405 for GET on valid route', () => {
    const res = handleRequest(
      { method: 'GET', url: '/v1/messages', headers: makeHeaders(`Bearer ${API_KEY}`) },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 405);
    assert.equal(res.headers['Allow'], 'POST');
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, 'invalid_request_error');
  });

  it('returns 405 for PUT on valid route', () => {
    const res = handleRequest(
      { method: 'PUT', url: '/v1/messages', headers: {} },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 405);
    assert.equal(res.headers['Allow'], 'POST');
  });

  it('returns 404 (not 405) for GET on unknown path', () => {
    const res = handleRequest(
      { method: 'GET', url: '/v1/unknown', headers: {} },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 404);
  });
});

describe('handleRequest — authorization validation', () => {
  it('returns 401 for missing Authorization header', () => {
    const res = handleRequest(
      { method: 'POST', url: '/v1/messages', headers: {} },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 401);
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, 'authentication_error');
  });

  it('returns 401 for malformed Authorization (no Bearer prefix)', () => {
    const res = handleRequest(
      { method: 'POST', url: '/v1/messages', headers: makeHeaders(API_KEY) },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 401);
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, 'authentication_error');
  });

  it('returns 401 for empty Bearer token', () => {
    const res = handleRequest(
      { method: 'POST', url: '/v1/messages', headers: makeHeaders('Bearer ') },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 401);
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, 'authentication_error');
  });

  it('returns 401 for wrong API key', () => {
    const res = handleRequest(
      { method: 'POST', url: '/v1/messages', headers: makeHeaders('Bearer wrong-key') },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 401);
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, 'authentication_error');
  });

  it('returns 501 for correct API key', () => {
    const res = handleRequest(
      { method: 'POST', url: '/v1/messages', headers: makeHeaders(`Bearer ${API_KEY}`) },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 501);
    const body = JSON.parse(res.body);
    assert.equal(body.error.type, 'not_implemented');
  });
});

describe('handleRequest — error response format', () => {
  it('follows Anthropic error shape', () => {
    const res = handleRequest(
      { method: 'POST', url: '/v1/unknown', headers: {} },
      API_KEY_HASH, false
    );
    const body = JSON.parse(res.body);
    assert.equal(body.type, 'error');
    assert.ok(body.error);
    assert.equal(typeof body.error.type, 'string');
    assert.equal(typeof body.error.message, 'string');
  });

  it('sets Content-Type to application/json', () => {
    const res = handleRequest(
      { method: 'POST', url: '/v1/messages', headers: {} },
      API_KEY_HASH, false
    );
    assert.equal(res.headers['Content-Type'], 'application/json');
  });
});

describe('handleRequest — shutdown flag', () => {
  it('sets Connection: close when shutting down', () => {
    const res = handleRequest(
      { method: 'POST', url: '/v1/messages', headers: makeHeaders(`Bearer ${API_KEY}`) },
      API_KEY_HASH, true
    );
    assert.equal(res.headers['Connection'], 'close');
  });

  it('does not set Connection: close when not shutting down', () => {
    const res = handleRequest(
      { method: 'POST', url: '/v1/messages', headers: makeHeaders(`Bearer ${API_KEY}`) },
      API_KEY_HASH, false
    );
    assert.equal(res.headers['Connection'], undefined);
  });
});

describe('handleRequest — evaluation order', () => {
  it('returns 404 before 401 for unknown path without auth', () => {
    const res = handleRequest(
      { method: 'POST', url: '/v1/unknown', headers: {} },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 404);
  });

  it('returns 405 before 401 for wrong method without auth', () => {
    const res = handleRequest(
      { method: 'GET', url: '/v1/messages', headers: {} },
      API_KEY_HASH, false
    );
    assert.equal(res.statusCode, 405);
  });
});
