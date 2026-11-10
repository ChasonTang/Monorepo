// Provide a public linker anchor; ICU symbols come from the re-exported dylibs.
// NOLINTNEXTLINE(misc-use-internal-linkage)
void catalina_icu_anchor(void) {}
