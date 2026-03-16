# RFC-002: Code Formatting and Linting

**Version:** 1.0
**Author:** Chason Tang
**Date:** 2026-03-16
**Status:** Implemented

---

## 1. Summary

Introduce ESLint and Prettier to the frigga project to enforce consistent code formatting and catch common JavaScript errors through static analysis. ESLint handles code-quality rules (bug detection, best practices), Prettier handles formatting (indentation, quotes, line length), and `eslint-config-prettier` eliminates rule conflicts between the two.

## 2. Motivation

Frigga currently has zero linting or formatting tooling — no ESLint, no Prettier, no `.editorconfig`. All code style enforcement relies on manual review, which is error-prone and non-scalable.

Specific problems this addresses:

- **No automated bug detection.** Common JavaScript pitfalls — unused variables, accidental `==` instead of `===`, `eval()` usage.
- **No formatting standard.** Without an enforced style, formatting divergence is inevitable as the codebase grows and new contributors join. Reviewing formatting in PRs wastes time.

Adding automated linting and formatting eliminates an entire class of review comments, catches bugs earlier in the development cycle, and establishes a baseline for code quality as frigga grows.

## 3. Goals and Non-Goals

**Goals:**

- All `.js` files in `src/` and `tests/` pass ESLint with zero errors and zero warnings
- All `.js` files in `src/` and `tests/` conform to Prettier formatting with zero diff
- npm scripts available for both checking and auto-fixing lint/format issues

**Non-Goals:**

- TypeScript support — frigga uses plain JavaScript and has no plans to migrate
- CI/CD integration — pipeline configuration is a separate concern; this RFC covers the local tooling only
- Pre-commit hooks — git hooks (e.g., via husky + lint-staged) can be added later without altering the core configuration
- Monorepo-wide shared config — frigga is independent services; sharing an ESLint/Prettier config package adds coupling that is not justified at this scale

## 4. Design

### 4.1 Overview

```
Developer writes code
        │
        ▼
┌───────────────┐     ┌──────────────────┐
│   Prettier    │────▶│     ESLint       │
│  (Formatting) │     │ (Code Quality)   │
└───────────────┘     └──────────────────┘
        │                      │
        ▼                      ▼
  Consistent style       Bug-free code
        │                      │
        └──────────┬───────────┘
                   ▼
          Clean, quality code
```

The workflow is two-stage: Prettier formats first, then ESLint checks quality. `eslint-config-prettier` disables all ESLint rules that would conflict with Prettier, ensuring the two tools complement rather than fight each other.

### 4.2 Detailed Design

#### 4.2.1 Tool Selection Analysis

Three alternatives were evaluated before confirming ESLint + Prettier:

| Criterion | ESLint + Prettier | Biome | ESLint + @stylistic |
|-----------|------------------|-------|---------------------|
| Ecosystem maturity | Highest — decade of production use | Growing but smaller rule catalog | Mature (ESLint core) |
| Formatting quality | Prettier is the gold standard | Good, but fewer formatting options | Less comprehensive than Prettier |
| Performance | Adequate (JS-based) | Fastest (Rust-based) | Adequate (JS-based) |
| Configuration complexity | Two configs + bridge | Single config | Single config |
| devDependencies | 5 packages | 1 package | 4 packages |

**Decision: ESLint + Prettier.** The ecosystem maturity, and Prettier's formatting superiority outweigh Biome's speed advantage and lower dependency count. For a project with 6 source files, execution speed is negligible.

#### 4.2.2 Prettier Configuration

File: `.prettierrc.json`

```json
{}
```

#### 4.2.3 ESLint Configuration

File: `eslint.config.js`

Uses ESLint v9+ flat config format.

```js
import js from '@eslint/js';
import globals from 'globals';
import eslintConfigPrettier from 'eslint-config-prettier';

export default [
  js.configs.recommended,

  {
    files: ['src/**/*.js', 'tests/**/*.js'],
    languageOptions: {
      ecmaVersion: 'latest',
      sourceType: 'module',
      globals: {
        ...globals.node,
      },
    },
    rules: {
      // ── Possible Errors ──────────────────────────────────────────
      'no-constant-binary-expression': 'error',
      'no-constructor-return': 'error',
      'no-promise-executor-return': 'error',
      'no-template-curly-in-string': 'warn',
      'no-unreachable-loop': 'error',

      // ── Best Practices ───────────────────────────────────────────
      'eqeqeq': ['error', 'always'],
      'no-eval': 'error',
      'no-implied-eval': 'error',
      'curly': ['error', 'all'],
      'no-shadow': 'error',
      'no-throw-literal': 'error',
      'prefer-promise-reject-errors': 'error',
      'no-return-assign': ['error', 'always'],
      'no-self-compare': 'error',
      'no-sequences': 'error',
      'no-unused-expressions': ['error', {
        allowShortCircuit: true,
        allowTernary: true,
      }],

      // ── Variables ────────────────────────────────────────────────
      'no-var': 'error',
      'prefer-const': ['error', { destructuring: 'all' }],
      'no-unused-vars': ['error', {
        argsIgnorePattern: '^_',
        varsIgnorePattern: '^_',
      }],

      // ── ES6+ ─────────────────────────────────────────────────────
      'no-useless-rename': 'error',
      'object-shorthand': ['error', 'always'],
      'prefer-arrow-callback': ['error', { allowNamedFunctions: true }],
      'prefer-template': 'warn',
      'symbol-description': 'error',
    },
  },

  eslintConfigPrettier,
];
```

The rule set is divided into four categories:

1. **Possible Errors** — catches code patterns that are almost certainly bugs (e.g., constant binary expressions, unreachable loops, constructor returns)
2. **Best Practices** — enforces patterns that prevent subtle bugs (strict equality, no eval, proper error objects in promise rejections)
3. **Variables** — prevents `var`, enforces `const` where possible, catches unused declarations with `_` prefix exemption for intentional discard
4. **ES6+** — promotes modern syntax (template literals, object shorthand, arrow callbacks)

#### 4.2.4 Package.json Scripts

```json
{
  "scripts": {
    "start": "node src/index.js",
    "test": "node --test tests/**/*.test.js",
    "check": "prettier --check src/ tests/ && eslint src/ tests/",
    "fix": "prettier --write src/ tests/ && eslint --fix src/ tests/"
  }
}
```

| Script | Purpose | Typical use |
|--------|---------|-------------|
| `check` | Check for ESLint violations and formatting without modifying files | CI gate, pre-commit check |
| `fix` | Auto-fix ESLint violations and format all files with Prettier | Developer workflow |

### 4.3 Design Rationale

**Why two tools instead of one?** ESLint and Prettier each solve a distinct problem. Prettier is opinionated and deterministic — given the same input, it always produces the same output regardless of the original formatting. ESLint cannot match this guarantee because formatting is not its primary concern. Combining them with `eslint-config-prettier` as a bridge gives the best of both: zero-config formatting from Prettier and deep code-quality analysis from ESLint.

## 5. Testing Strategy

This proposal involves pure configuration changes with no unit-testable logic. Verification relies on running the tools against the existing codebase and confirming existing tests remain green.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Auto-fix formats all files and resolves lint violations | `npm run fix` | All files reformatted; zero errors, zero warnings |
| 2 | Existing tests pass after auto-fix | `npm test` (after `npm run fix`) | All tests pass unchanged |
| 3 | Check passes on already-clean codebase | `npm run check` (after `npm run fix`) | Exit code 0, no output |
| 4 | ESLint catches an intentional code-quality violation | Add `var x = 1;` to a source file, then `npm run check` | Non-zero exit code; ESLint reports `no-var` error with file path and line number |
| 5 | Prettier catches an intentional formatting violation | Add inconsistent indentation to a source file, then `npm run check` | Non-zero exit code; Prettier reports the file as unformatted |
| 6 | Fix resolves intentionally introduced violations | Introduce violations from scenarios 4–5, then `npm run fix`, then `npm run check` | `fix` exits 0; subsequent `check` exits 0 |
| 7 | No rule conflict between ESLint and Prettier | `npm run fix` twice consecutively | Second run produces zero changes (idempotent) |
| 8 | Linting scope is limited to `src/` and `tests/` | Place a file with lint violations in the project root, then `npm run check` | Exit code 0; the root file is not checked |

## 6. Implementation Plan

Unit test phase is omitted per template guidance — this proposal involves no unit-testable logic (pure configuration changes).

### Phase 1: Prettier Setup — 10 min

- [x] Install Prettier: `npm install --save-dev prettier`
- [x] Create `.prettierrc.json` with configuration from Section 4.2.2
- [x] Run `npx prettier --write src/ tests/` to format existing codebase
- [x] Verify `npm test` passes after formatting

**Done when:** All files match Prettier formatting (`npx prettier --check src/ tests/` exits 0) and existing tests pass.

### Phase 2: ESLint Setup — 15 min

- [x] Install ESLint and dependencies: `npm install --save-dev eslint @eslint/js globals eslint-config-prettier`
- [x] Create `eslint.config.js` with configuration from Section 4.2.3
- [x] Run `npx eslint src/ tests/` and fix any violations in existing code
- [x] Verify `npm test` passes after fixes

**Done when:** `npx eslint src/ tests/` reports zero errors and zero warnings, and existing tests pass.

### Phase 3: Script Integration — 5 min

- [x] Add `check` and `fix` scripts to `package.json`
- [x] Verify all two scripts execute correctly

**Done when:** All scripts in Section 4.2.4 run successfully.

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Formatting changes pollute `git blame` | High | Low | Use a dedicated formatting commit; configure `git blame --ignore-rev` with a `.git-blame-ignore-revs` file |
| ESLint rule triggers false positive on existing code | Low | Low | Fix or adjust rule severity on a case-by-case basis |
| Dependency version conflicts with future tooling | Low | Low | Pin major versions in `package.json`; devDependencies do not affect runtime |

## 8. Future Work

- Pre-commit hooks via `husky` + `lint-staged` — automate lint/format checks before each commit
- CI/CD pipeline integration — add lint and format-check as required gates

## 9. References

- [ESLint v9 Flat Config documentation](https://eslint.org/docs/latest/use/configure/configuration-files)
- [Prettier documentation](https://prettier.io/docs/en/)
- [eslint-config-prettier](https://github.com/prettier/eslint-config-prettier)
- frigga RFC-001: `frigga/docs/rfc_001_api_proxy_service.md`

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-03-16 | Chason Tang | Initial version |
