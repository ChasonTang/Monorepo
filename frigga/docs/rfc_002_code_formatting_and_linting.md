# RFC-002: Code Formatting and Linting

**Version:** 1.0
**Author:** Chason Tang
**Date:** 2026-03-16
**Status:** Proposed

---

## 1. Summary

Introduce ESLint and Prettier to the frigga project to enforce consistent code formatting and catch common JavaScript errors through static analysis. ESLint handles code-quality rules (bug detection, best practices), Prettier handles formatting (indentation, quotes, line length), and `eslint-config-prettier` eliminates rule conflicts between the two. This mirrors the proven setup already in use by the sibling odin project.

## 2. Motivation

Frigga currently has zero linting or formatting tooling — no ESLint, no Prettier, no `.editorconfig`. All code style enforcement relies on manual review, which is error-prone and non-scalable.

Specific problems this addresses:

- **No automated bug detection.** Common JavaScript pitfalls — unused variables, accidental `==` instead of `===`, `eval()` usage, unhandled promise rejections — are invisible until they cause runtime failures.
- **No formatting standard.** Without an enforced style, formatting divergence is inevitable as the codebase grows and new contributors join. Reviewing formatting in PRs wastes time.
- **Inconsistency with sibling project.** The odin project in the same monorepo already uses ESLint + Prettier. Frigga lacks parity, creating friction for developers working across both projects.

Adding automated linting and formatting eliminates an entire class of review comments, catches bugs earlier in the development cycle, and establishes a baseline for code quality as frigga grows.

## 3. Goals and Non-Goals

**Goals:**

- All `.js` files in `src/` and `tests/` pass ESLint with zero errors and zero warnings
- All `.js` files in `src/` and `tests/` conform to Prettier formatting with zero diff
- npm scripts available for both checking and auto-fixing lint/format issues
- Configuration based on the proven odin project setup, adapted for frigga

**Non-Goals:**

- TypeScript support — frigga uses plain JavaScript and has no plans to migrate
- CI/CD integration — pipeline configuration is a separate concern; this RFC covers the local tooling only
- Pre-commit hooks — git hooks (e.g., via husky + lint-staged) can be added later without altering the core configuration
- Monorepo-wide shared config — frigga and odin are independent services; sharing an ESLint/Prettier config package adds coupling that is not justified at this scale

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
| Monorepo consistency | Matches odin project | No existing adoption | Partial (no Prettier) |
| Configuration complexity | Two configs + bridge | Single config | Single config |
| devDependencies | 6 packages | 1 package | 4 packages |

**Decision: ESLint + Prettier.** The ecosystem maturity, monorepo consistency with odin, and Prettier's formatting superiority outweigh Biome's speed advantage and lower dependency count. For a project with 6 source files, execution speed is negligible.

#### 4.2.2 Prettier Configuration

File: `.prettierrc.json`

```json
{
  "singleQuote": true,
  "tabWidth": 2,
  "semi": true,
  "trailingComma": "all",
  "printWidth": 100,
  "arrowParens": "always",
  "endOfLine": "lf"
}
```

| Setting | Value | Rationale |
|---------|-------|-----------|
| `singleQuote` | `true` | Matches existing frigga code style |
| `tabWidth` | `2` | Matches existing frigga indentation; 2-space is the dominant JS convention (Node.js core, npm, Google, Airbnb style guides) |
| `semi` | `true` | Matches existing code; avoids ASI-related pitfalls |
| `trailingComma` | `"all"` | Cleaner git diffs; safe on Node.js >= 24 |
| `printWidth` | `100` | Matches odin; balances readability vs horizontal space |
| `arrowParens` | `"always"` | Consistent style; easier to add parameters later |
| `endOfLine` | `"lf"` | Standard for Linux deployment target |

Note: `tabWidth` differs from odin's `4`. This is intentional — frigga's existing codebase consistently uses 2-space indentation, and the two projects share no source files. Keeping `2` minimizes formatting diff noise and follows the more prevalent JavaScript convention.

File: `.prettierignore`

```
node_modules/
```

#### 4.2.3 ESLint Configuration

File: `eslint.config.js`

Based on odin's proven ruleset, adapted for frigga. Uses ESLint v9+ flat config format.

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

Key differences from odin's config:

| Aspect | odin | frigga | Reason |
|--------|------|--------|--------|
| `@stylistic` plugin | Included (padding rules) | Omitted | Minimal first iteration; can be added later if desired |
| Rule set | Full set | Identical rules | Same project archetype (Node.js ES module service) — consistency aids developer experience |

#### 4.2.4 Package.json Scripts

```json
{
  "scripts": {
    "start": "node src/index.js",
    "test": "node --test tests/**/*.test.js",
    "lint": "eslint .",
    "lint:fix": "eslint --fix .",
    "format": "prettier --write .",
    "format:check": "prettier --check ."
  }
}
```

| Script | Purpose | Typical use |
|--------|---------|-------------|
| `lint` | Check for ESLint violations | CI gate, pre-commit check |
| `lint:fix` | Auto-fix ESLint violations | Developer workflow |
| `format` | Format all files with Prettier | Developer workflow |
| `format:check` | Check formatting without modifying files | CI gate |

### 4.3 Design Rationale

**Why two tools instead of one?** ESLint and Prettier each solve a distinct problem. Prettier is opinionated and deterministic — given the same input, it always produces the same output regardless of the original formatting. ESLint cannot match this guarantee because formatting is not its primary concern. Combining them with `eslint-config-prettier` as a bridge gives the best of both: zero-config formatting from Prettier and deep code-quality analysis from ESLint.

**Why match odin's ESLint rules?** Both frigga and odin are Node.js ES module services written by the same team. Identical lint rules reduce context-switching cost and ensure the same quality bar across both projects. The rules were already validated against a production codebase (odin), reducing the risk of false positives.

**Why `tabWidth: 2` instead of odin's `tabWidth: 4`?** Frigga's existing 6 files consistently use 2-space indentation. Since the two projects share no source code and are independently deployed, per-project style is acceptable. Matching existing style produces a minimal formatting diff and keeps `git blame` history cleaner.

**Why omit `@stylistic/eslint-plugin`?** Odin uses it for a single padding rule (`padding-line-between-statements`). For frigga's initial setup, the core ESLint + Prettier combination provides the highest value with the lowest complexity. The `@stylistic` plugin can be added in a follow-up if padding enforcement proves valuable.

## 5. Testing Strategy

This proposal involves pure configuration changes with no unit-testable logic. Verification relies on running the tools against the existing codebase and confirming existing tests remain green.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Prettier formats existing files | `npm run format` | All files reformatted; changes limited to whitespace and style |
| 2 | ESLint passes on formatted code | `npm run lint` | Zero errors, zero warnings |
| 3 | Existing tests pass after formatting | `npm test` | All tests pass unchanged |
| 4 | Prettier check detects unformatted code | Add unformatted file, run `npm run format:check` | Non-zero exit code |
| 5 | ESLint detects code quality issue | Introduce `==` comparison, run `npm run lint` | `eqeqeq` error reported |

## 6. Implementation Plan

Unit test phase is omitted per template guidance — this proposal involves no unit-testable logic (pure configuration changes).

### Phase 1: Prettier Setup — 10 min

- [ ] Install Prettier: `npm install --save-dev prettier`
- [ ] Create `.prettierrc.json` with configuration from Section 4.2.2
- [ ] Create `.prettierignore`
- [ ] Run `npx prettier --write .` to format existing codebase
- [ ] Verify `npm test` passes after formatting

**Done when:** All files match Prettier formatting (`npx prettier --check .` exits 0) and existing tests pass.

### Phase 2: ESLint Setup — 15 min

- [ ] Install ESLint and dependencies: `npm install --save-dev eslint @eslint/js globals eslint-config-prettier`
- [ ] Create `eslint.config.js` with configuration from Section 4.2.3
- [ ] Run `npx eslint .` and fix any violations in existing code
- [ ] Verify `npm test` passes after fixes

**Done when:** `npx eslint .` reports zero errors and zero warnings, and existing tests pass.

### Phase 3: Script Integration — 5 min

- [ ] Add `lint`, `lint:fix`, `format`, and `format:check` scripts to `package.json`
- [ ] Verify all four scripts execute correctly

**Done when:** All scripts in Section 4.2.4 run successfully.

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Formatting changes pollute `git blame` | High | Low | Use a dedicated formatting commit; configure `git blame --ignore-rev` with a `.git-blame-ignore-revs` file |
| ESLint rule triggers false positive on existing code | Low | Low | Rules are validated against odin's codebase; fix or adjust rule severity on a case-by-case basis |
| Dependency version conflicts with future tooling | Low | Low | Pin major versions in `package.json`; devDependencies do not affect runtime |

## 8. Future Work

- Pre-commit hooks via `husky` + `lint-staged` — automate lint/format checks before each commit
- CI/CD pipeline integration — add lint and format-check as required gates
- `@stylistic/eslint-plugin` — add padding rules for blank-line enforcement, matching odin
- Shared ESLint/Prettier config package — if a third Node.js project joins the monorepo, extract a shared config to reduce duplication

## 9. References

- [ESLint v9 Flat Config documentation](https://eslint.org/docs/latest/use/configure/configuration-files)
- [Prettier documentation](https://prettier.io/docs/en/)
- [eslint-config-prettier](https://github.com/prettier/eslint-config-prettier)
- odin project configuration: `odin/eslint.config.js`, `odin/.prettierrc.json`
- frigga RFC-001: `frigga/docs/rfc_001_api_proxy_service.md`

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-03-16 | Chason Tang | Initial version |
