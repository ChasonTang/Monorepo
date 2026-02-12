import js from '@eslint/js';
import globals from 'globals';
import eslintConfigPrettier from 'eslint-config-prettier';
import stylistic from '@stylistic/eslint-plugin';

export default [
    // ── Base: ESLint recommended rules ──────────────────────────────────
    js.configs.recommended,

    // ── Project configuration ───────────────────────────────────────────
    {
        files: ['src/**/*.js'],
        languageOptions: {
            ecmaVersion: 'latest',
            sourceType: 'module',
            globals: {
                ...globals.node,
            },
        },
        plugins: {
            '@stylistic': stylistic,
        },
        rules: {
            // ── Possible Errors ─────────────────────────────────────────
            'no-constant-binary-expression': 'error',
            'no-constructor-return': 'error',
            'no-promise-executor-return': 'error',
            'no-template-curly-in-string': 'warn',
            'no-unreachable-loop': 'error',

            // ── Best Practices ──────────────────────────────────────────
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

            // ── Variables ───────────────────────────────────────────────
            'no-var': 'error',
            'prefer-const': ['error', { destructuring: 'all' }],
            'no-unused-vars': ['error', {
                argsIgnorePattern: '^_',
                varsIgnorePattern: '^_',
            }],

            // ── ES6+ ───────────────────────────────────────────────────
            'no-useless-rename': 'error',
            'object-shorthand': ['error', 'always'],
            'prefer-arrow-callback': ['error', { allowNamedFunctions: true }],
            'prefer-template': 'warn',
            'symbol-description': 'error',
        },
    },

    // ── Prettier conflict resolution ─────────────────────────────────────
    eslintConfigPrettier,

    // ── Padding rules (after Prettier config – no actual conflict) ─────
    {
        files: ['src/**/*.js'],
        rules: {
            '@stylistic/padding-line-between-statements': [
                'error',
                { blankLine: 'always', prev: '*', next: 'return' },
            ],
        },
    },
];
