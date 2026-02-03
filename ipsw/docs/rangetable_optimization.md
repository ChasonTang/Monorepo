# RangeTable Optimization for IPSW Address Lookup

**Document Version:** 1.2
**Author:** Chason Tang
**Last Updated:** 2026-02-04
**Status:** Proposed

---

## 1. Executive Summary

This document describes the technical plan for optimizing the IPSW address lookup tool using the `rangeTable` data structure from the `dyld_cache_accelerator_info`. The current implementation uses O(n×m) linear search (n = number of images, m = average segments per image). By leveraging the pre-built, sorted `rangeTable` in the accelerator info, we can achieve O(log n) binary search performance.

### 1.1 Goals

- **Primary**: Reduce address lookup time from O(n×m) to O(log n)
- **Secondary**: Simplify codebase by removing legacy linear search
- **Tertiary**: Improve code organization for future extensions

### 1.2 Scope & Minimum Supported Version

**Minimum Supported Cache Version:** iOS 9+ / macOS 10.11+

Caches older than iOS 9 do not contain `accelerateInfo` and are **not supported**. This is acceptable because:
- iOS 10 is already considered legacy (released in 2016)
- Modern reverse engineering and analysis focus on iOS 10+
- Maintaining backward compatibility adds unnecessary complexity

If a cache lacks accelerator info, the tool will report an error and exit.

### 1.3 Key Metrics

| Metric | Current | Target |
|--------|---------|--------|
| Lookup Algorithm | O(n×m) linear scan | O(log n) binary search |
| Typical Lookup Time (1170 images) | ~1170 iterations | ~10 iterations |
| Memory Overhead | None | Minimal (pointer to existing data) |
| Code Complexity | High (Mach-O parsing) | Low (simple binary search) |

---

## 2. Background

### 2.1 Current Implementation

The existing `main.c` performs address lookup as follows:

```c
// Current O(n×m) approach
for (uint32_t i = 0; i < header->imagesCount; i++) {
    // For each image, parse its Mach-O header
    // Iterate through all LC_SEGMENT_64 commands
    // Check if target_addr is within each segment's vmaddr range
}
```

**Problems with Current Approach:**
1. Must parse every image's Mach-O header
2. Must iterate through all load commands
3. No early exit optimization
4. Returns all 1170+ dylibs for `__LINKEDIT` addresses (shared segment problem)

### 2.2 dyld Shared Cache Accelerator Info

Starting with iOS 9 / macOS 10.11, Apple introduced `dyld_cache_accelerator_info` to speed up dyld operations. This structure includes a pre-computed `rangeTable` that maps address ranges directly to image indexes.

**Key Structures (from `dyld-421.2/launch-cache/dyld_cache_format.h`):**

```c
struct dyld_cache_accelerator_info {
    uint32_t version;              // currently 1
    uint32_t imageExtrasCount;     // number of images
    uint32_t imagesExtrasOffset;   // offset to image extras
    // ... other fields ...
    uint32_t rangeTableOffset;     // offset to range table
    uint32_t rangeTableCount;      // number of range entries
    uint64_t dyldSectionAddr;
};

struct dyld_cache_range_entry {
    uint64_t startAddress;         // unslid address of start of region
    uint32_t size;                 // size of region
    uint32_t imageIndex;           // index into images array
};
```

### 2.3 Reference Implementation

From `dyld-421.2/src/ImageLoaderMegaDylib.cpp`:

```cpp
bool ImageLoaderMegaDylib::addressInCache(const void* address, ...) {
    uint64_t unslidAddress = (uint64_t)address - _slide;
    
    // Current dyld implementation uses linear search
    const dyld_cache_range_entry* rangeTableEnd = &_rangeTable[_rangeTableCount];
    for (const dyld_cache_range_entry* r = _rangeTable; r < rangeTableEnd; ++r) {
        if ((r->startAddress <= unslidAddress) && 
            (unslidAddress < r->startAddress + r->size)) {
            *index = r->imageIndex;
            return true;
        }
    }
    return false;
}
```

**Key Insight:** The rangeTable is **sorted by startAddress** (see `OptimizerLinkedit.cpp` line 452-455), enabling binary search optimization.

---

## 3. Technical Design

### 3.1 Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         IPSW CLI Tool (Optimized)                       │
├─────────────────────────────────────────────────────────────────────────┤
│  main()                                                                 │
│  ├── Argument Parsing                                                   │
│  ├── File Mapping (mmap)                                                │
│  ├── Cache Header Validation                                            │
│  │    └── Require accelerateInfoAddr != 0 (exit with error otherwise)   │
│  └── Address Lookup Engine                                              │
│       ├── Locate Accelerator Info                                       │
│       │    └── Convert accelerateInfoAddr to file offset                │
│       └── RangeTable Binary Search                                      │
│            └── O(log n) lookup using sorted range entries               │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3.2 Data Structures

Add the following structures to align with dyld definitions:

```c
/*
 * dyld_cache_accelerator_info - Accelerator table header
 * Contains offsets to various optimization tables including rangeTable.
 */
struct dyld_cache_accelerator_info {
    uint32_t version;               // currently 1
    uint32_t imageExtrasCount;      // does not include aliases
    uint32_t imagesExtrasOffset;    // offset to first dyld_cache_image_info_extra
    uint32_t bottomUpListOffset;    // offset to bottom-up sorted image index list
    uint32_t dylibTrieOffset;       // offset to dylib path trie
    uint32_t dylibTrieSize;         // size of dylib trie
    uint32_t initializersOffset;    // offset to initializers list
    uint32_t initializersCount;     // count of initializers
    uint32_t dofSectionsOffset;     // offset to DOF sections
    uint32_t dofSectionsCount;      // count of DOF sections
    uint32_t reExportListOffset;    // offset to re-export list
    uint32_t reExportCount;         // count of re-exports
    uint32_t depListOffset;         // offset to dependency list
    uint32_t depListCount;          // count of dependencies
    uint32_t rangeTableOffset;      // offset to range table
    uint32_t rangeTableCount;       // count of range entries
    uint64_t dyldSectionAddr;       // address of libdyld's __dyld section
};

/*
 * dyld_cache_range_entry - Maps an address range to an image index
 * Entries are sorted by startAddress for binary search.
 */
struct dyld_cache_range_entry {
    uint64_t startAddress;          // unslid address of region start
    uint32_t size;                  // size of region in bytes
    uint32_t imageIndex;            // index into dyld_cache_image_info array
};
```

### 3.3 Algorithm: Binary Search on RangeTable

```c
/**
 * Binary search for address in sorted rangeTable.
 * 
 * @param rangeTable Pointer to sorted range entry array
 * @param count      Number of entries in rangeTable
 * @param addr       Target address to find (unslid)
 * @return           Pointer to matching entry, or NULL if not found
 *
 * Time Complexity: O(log n) where n = rangeTableCount
 */
static const struct dyld_cache_range_entry*
binary_search_range_table(const struct dyld_cache_range_entry* rangeTable,
                          uint32_t count, uint64_t addr) {
    uint32_t low = 0;
    uint32_t high = count;
    
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        const struct dyld_cache_range_entry* entry = &rangeTable[mid];
        
        if (addr < entry->startAddress) {
            high = mid;
        } else if (addr >= entry->startAddress + entry->size) {
            low = mid + 1;
        } else {
            // addr is within [startAddress, startAddress + size)
            return entry;
        }
    }
    return NULL;
}
```

### 3.4 Lookup Flow

```
┌──────────────────────────────────────────────────────────────────────────┐
│                          Address Lookup Flow                              │
└──────────────────────────────────────────────────────────────────────────┘

                              ┌─────────────┐
                              │ Start       │
                              └──────┬──────┘
                                     │
                                     ▼
                    ┌────────────────────────────────┐
                    │ Parse dyld_cache_header        │
                    │ Validate magic & bounds        │
                    └───────────────┬────────────────┘
                                    │
                                    ▼
                    ┌────────────────────────────────┐
                    │ Check: mappingOffset >= 0x78   │
                    │    && accelerateInfoAddr != 0  │
                    │    && accelerateInfoSize != 0  │
                    └───────────────┬────────────────┘
                                    │
                  ┌─────────────────┴─────────────────┐
                  │ YES                               │ NO
                  ▼                                   ▼
    ┌─────────────────────────┐         ┌─────────────────────────┐
    │ Locate accelerateInfo   │         │ ERROR: Unsupported      │
    │ via addr_to_file_offset │         │ cache format (too old)  │
    └───────────────┬─────────┘         │ Exit with error code    │
                    │                   └─────────────────────────┘
                    ▼
    ┌─────────────────────────┐
    │ Validate accelerateInfo │
    │ - version == 1          │
    │ - rangeTableCount > 0   │
    └───────────────┬─────────┘
                    │
                    ▼
    ┌─────────────────────────┐
    │ Binary Search rangeTable│
    │ O(log n) lookup         │
    └───────────────┬─────────┘
                    │
          ┌─────────┴─────────┐
          │ FOUND             │ NOT FOUND
          ▼                   ▼
    ┌──────────────────┐  ┌──────────────┐
    │ Get imageIndex   │  │ Report       │
    │ Lookup path from │  │ not found    │
    │ image_info array │  │ Exit code 1  │
    └────────┬─────────┘  └──────────────┘
             │
             ▼
    ┌────────────────────────────────┐
    │ Output matching dylib path     │
    │ Exit code 0                    │
    └────────────────────────────────┘
```

### 3.5 LINKEDIT Segment Exclusion

The `rangeTable` **intentionally excludes `__LINKEDIT` segments**. This is by design in Apple's cache builder (`OptimizerLinkedit.cpp` line 443-444):

```cpp
for ( const macho_segment_command<P>* segCmd : op->segCmds() ) {
    if ( strcmp(segCmd->segname(), "__LINKEDIT") == 0 )
        continue;  // Skipped!
    // Only __TEXT and __DATA segments are indexed
}
```

**Why?** In the shared cache, all dylibs share a single merged `__LINKEDIT` region. Each dylib's `LC_SEGMENT_64(__LINKEDIT)` command points to the **same** virtual address range. Using the legacy segment-walking approach, a `__LINKEDIT` address would match **all 1170+ dylibs** — which is meaningless.

The `rangeTable` design solves this by only indexing `__TEXT` and `__DATA` segments, which remain unique per dylib. For addresses in `__LINKEDIT`, the tool will report "not found" — this is the correct behavior since such addresses cannot be uniquely attributed to any single dylib.

---

## 4. Implementation Plan

### Phase 1: Infrastructure (Estimated: 2 hours)

**Task 1.1: Add New Data Structures**
- [ ] Add `struct dyld_cache_accelerator_info` definition to `main.c`
- [ ] Add `struct dyld_cache_range_entry` definition to `main.c`

**Task 1.2: Implement Accelerator Info Validation**
- [ ] Check `mappingOffset >= 0x78` (header has accelerateInfo fields)
- [ ] Check `accelerateInfoAddr != 0` and `accelerateInfoSize != 0`
- [ ] If missing, exit with error: "Error: Cache lacks accelerator info (iOS 9+ required)"
- [ ] Implement `get_accelerator_info()` function to locate and validate accelerator info

**Acceptance Criteria:**
- Verbose mode (`-v`) displays accelerator info details
- Tool exits with clear error message for unsupported caches

### Phase 2: Binary Search Implementation (Estimated: 2 hours)

**Task 2.1: Implement Binary Search Function**
- [ ] Implement `binary_search_range_table()` as specified in Section 3.3
- [ ] Add comprehensive bounds checking

**Task 2.2: Replace Main Lookup Logic**
- [ ] Remove existing linear search code (`check_macho_segments()` function)
- [ ] Replace with rangeTable binary search
- [ ] Output single matching path (no alias handling needed)

**Acceptance Criteria:**
- Correct results for all test addresses in `__TEXT` and `__DATA` segments
- `__LINKEDIT` addresses correctly report "not found"
- Verbose mode shows rangeTable statistics

### Phase 3: Code Cleanup & Testing (Estimated: 2 hours)

**Task 3.1: Remove Legacy Code**
- [ ] Remove `check_macho_segments()` function
- [ ] Remove `addr_to_file_offset()` if no longer needed (may still be needed for accelerator info location)
- [ ] Simplify main() control flow

**Task 3.2: Testing**
- [ ] Test with iOS 10 dyld_shared_cache_arm64 (has accelerator info)
- [ ] Test edge cases: boundary addresses, invalid addresses, __LINKEDIT region
- [ ] Verify error handling for unsupported caches

**Task 3.3: Documentation Update**
- [ ] Update `address_lookup.md` with new algorithm section
- [ ] Document minimum supported cache version
- [ ] Document new verbose output fields

**Acceptance Criteria:**
- All test cases pass
- Code is cleaner and simpler
- Documentation is complete

---

## 5. Code Changes Summary

### 5.1 Files to Modify

| File | Changes |
|------|---------|
| `ipsw/main.c` | Add accelerator structures, binary search, remove legacy code |

### 5.2 New Functions

| Function | Description |
|----------|-------------|
| `get_accelerator_info()` | Locate and validate accelerator info in cache |
| `binary_search_range_table()` | O(log n) address lookup in sorted range table |

### 5.3 Functions to Remove

| Function | Reason |
|----------|--------|
| `check_macho_segments()` | Replaced by rangeTable lookup |

### 5.4 Estimated Lines of Code

| Category | Lines |
|----------|-------|
| New structures | ~30 |
| New functions | ~60 |
| Removed functions | ~-60 |
| Refactored main() | ~20 |
| Comments & documentation | ~20 |
| **Net Change** | **~+70** |

---

## 6. Risk Assessment

### 6.1 Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| RangeTable format differs in newer dyld versions | Low | Medium | Validate version field; add version checks if needed |
| Binary search edge case bugs | Low | High | Thorough testing with boundary cases |
| Memory alignment issues on different platforms | Low | Medium | Use portable data access patterns |

### 6.2 Error Handling

For caches without accelerator info:
- Clear error message: `Error: This cache lacks accelerator info. Only iOS 9+ / macOS 10.11+ caches are supported.`
- Exit code: 1

---

## 7. Future Considerations

### 7.1 Additional Optimizations

1. **Symbol Lookup via Export Trie**: The accelerator info also contains export trie data that could be used for future symbol lookup feature

2. **Parallel Lookup**: For batch mode, multiple addresses could be looked up in parallel

3. **Caching**: For repeated lookups, cache the accelerator info pointer

### 7.2 Related Work

- Task from `address_lookup.md` Section 7.1: "Optimize queries using accelerateInfo.rangeTable" - this document addresses this item

---

## 8. Appendix

### 8.1 Accelerator Info Field Offsets

For reference, the dyld_cache_header layout requires:
- `mappingOffset >= 0x78` for accelerateInfo fields to be present
- This corresponds to header versions with full accelerator support

### 8.2 Test Data

Using iOS 10.0.2 dyld_shared_cache_arm64:

| Address | Segment | Expected Result |
|---------|---------|-----------------|
| 0x180625848 | __TEXT | `/usr/lib/system/libsystem_kernel.dylib` |
| 0x1a0000000 | __DATA | Single dylib path |
| 0x1a9000000 | __LINKEDIT | "not found" (correct behavior) |

- Expected rangeTableCount: ~3000+ entries (multiple segments per image, excluding __LINKEDIT)

### 8.3 References

1. `dyld-421.2/launch-cache/dyld_cache_format.h` - Structure definitions
2. `dyld-421.2/src/ImageLoaderMegaDylib.cpp` - Reference implementation
3. `dyld-421.2/interlinked-dylibs/OptimizerLinkedit.cpp` - RangeTable construction
4. `dyld-421.2/launch-cache/dyld_shared_cache_util.cpp` - Accelerator info dumping

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.2 | 2026-02-04 | - | Removed alias handling; added LINKEDIT exclusion rationale (Section 3.5) |
| 1.1 | 2026-02-03 | - | Removed legacy fallback path; simplified to require accelerator info (iOS 9+ minimum) |
| 1.0 | 2026-02-03 | - | Initial version |

---

*End of Technical Design Document*
