# Symbol Lookup Tool - Address to Symbol Resolution

**Document Version:** 2.0  
**Author:** Chason Tang  
**Last Updated:** 2026-02-04  
**Status:** Implemented

---

## 1. Executive Summary

This document describes the technical design for adding symbol lookup capability to the IPSW tool. Given a hexadecimal address, the tool will output the corresponding symbol name and offset.

### 1.1 Background

The current IPSW tool can identify which dynamic library contains a given address (see `address_lookup.md`), but cannot resolve the address to a specific function name. Symbol resolution is essential for reverse engineering, crash log analysis, and performance profiling workflows.

The dyld source code provides a reference implementation through the `dladdr()` function, which internally calls `ImageLoader::findClosestSymbol()` to perform address-to-symbol resolution.

### 1.2 Goals

- **Primary**: Given an address, output the closest symbol name and offset from symbol start
- **Secondary**: Support lookup in the local symbol table stored in dyld_shared_cache
- **Non-Goals**: DWARF debug info parsing (potential future extension)

### 1.3 Key Features

| Feature | Description |
|---------|-------------|
| Symbol Lookup | Given an address, return the closest preceding symbol name |
| Offset Calculation | Return the offset from symbol start address |
| atos-Compatible Output | Output format compatible with Apple's atos tool |

---

## 2. Technical Design

### 2.1 Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    Symbol Lookup System                          │
├─────────────────────────────────────────────────────────────────┤
│  Input Layer                                                     │
│  ├── Address Parser (hex string → uint64_t)                     │
│  └── Cache Loader (mmap dyld_shared_cache)                      │
├─────────────────────────────────────────────────────────────────┤
│  Lookup Engine                                                   │
│  ├── RangeTable Lookup (O(log n) - find containing image)       │
│  └── Dual Symbol Table Search                                   │
│      ├── Dylib LC_SYMTAB (exported symbols)                     │
│      └── Local Symbols Info (local symbols)                     │
├─────────────────────────────────────────────────────────────────┤
│  Symbol Resolution                                               │
│  ├── nlist_64 Parser (symbol table entries)                     │
│  ├── String Table Access (symbol names)                         │
│  └── Best Match Selection (closest address wins)                │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Symbol Table Design Choice

The dyld_shared_cache contains multiple sources of symbol information:

| Source | Storage | Symbol Type | Best For |
|--------|---------|-------------|----------|
| **Dylib Symbol Table** | Each dylib's `LC_SYMTAB` | Exported symbols (`N_EXT`) | Public API functions like `vm_allocate` |
| **Local Symbols Info** | `localSymbolsOffset` | Local symbols (non-`N_EXT`) | Static functions, file-scope symbols |
| **Exports Trie** | `exportsTrieAddr` | Exported symbols | Symbol → Address lookup (not suitable for reverse) |

**Design Decision**: Search **both** the dylib's own symbol table (via `LC_SYMTAB`) and the centralized local symbols (`dyld_cache_local_symbols_info`). This ensures complete coverage of all symbols.

**Why Two Sources Are Needed**:
- **Local symbols** (`(n_type & N_EXT) == 0`): Stripped from individual dylibs during cache optimization, stored centrally in `localSymbolsOffset`
- **Exported symbols** (`(n_type & N_EXT) != 0`): Remain in each dylib's Mach-O `LC_SYMTAB` load command

These two symbol types are **mutually exclusive** based on the `N_EXT` flag. Both sources must be searched to find all symbols.

The exports trie is designed as a prefix tree for efficient symbol name lookup (O(k) where k = name length). It would require a full traversal to find a symbol by address, making it unsuitable for reverse lookup.

### 2.3 Data Structures

**Source**: All structures below are based on `dyld-421.2/launch-cache/dyld_cache_format.h`.

#### 2.3.1 Local Symbols Info Header

The local symbols section is located at `header->localSymbolsOffset` in the cache file:

```c
/**
 * Header for local symbols section in dyld_shared_cache.
 * Located at header->localSymbolsOffset in the cache file.
 */
struct dyld_cache_local_symbols_info {
    uint32_t nlistOffset;      // Offset to nlist entries (from this struct)
    uint32_t nlistCount;       // Total count of nlist entries
    uint32_t stringsOffset;    // Offset to string table (from this struct)
    uint32_t stringsSize;      // Size of string table in bytes
    uint32_t entriesOffset;    // Offset to entries array (from this struct)
    uint32_t entriesCount;     // Number of entries (one per dylib)
};
```

#### 2.3.2 Per-Dylib Symbol Entry

Each dynamic library has an entry that maps to its symbols in the shared nlist array:

```c
/**
 * Per-dylib entry in local symbols table.
 * Maps a dylib to its range of symbols in the shared nlist array.
 */
struct dyld_cache_local_symbols_entry {
    uint32_t dylibOffset;       // File offset of dylib's mach_header in cache
    uint32_t nlistStartIndex;   // First symbol index for this dylib
    uint32_t nlistCount;        // Number of symbols for this dylib
};
```

#### 2.3.3 Symbol Table Entry (nlist_64)

Standard Mach-O symbol table entry structure:

```c
/**
 * 64-bit symbol table entry (from <mach-o/nlist.h>).
 */
struct nlist_64 {
    uint32_t n_strx;   // Index into string table
    uint8_t  n_type;   // Type flags (N_EXT, N_TYPE, etc.)
    uint8_t  n_sect;   // Section number (1-based) or NO_SECT
    uint16_t n_desc;   // Description field
    uint64_t n_value;  // Symbol value (address for defined symbols)
};

/* n_type masks */
#define N_STAB  0xe0  /* Stabs debugging symbol */
#define N_PEXT  0x10  /* Private external symbol */
#define N_TYPE  0x0e  /* Type mask */
#define N_EXT   0x01  /* External symbol */

/* n_type values for N_TYPE bits */
#define N_UNDF  0x00  /* Undefined */
#define N_ABS   0x02  /* Absolute */
#define N_SECT  0x0e  /* Defined in section n_sect */
```

### 2.4 Core Algorithms

#### 2.4.1 Main Lookup Algorithm

```c
/**
 * Find the closest symbol for a given address.
 * 
 * @param cache             Pointer to mmap'd cache file
 * @param cache_size        Size of cache file
 * @param header            Cache header pointer
 * @param mappings          Mapping info array
 * @param mapping_count     Number of mappings
 * @param local_info        Local symbols info pointer (may be NULL)
 * @param rangeTable        Range table pointer
 * @param range_table_count Number of range entries
 * @param target_addr       Target address to look up
 * @param symbol_name       [out] Symbol name (NULL if not found)
 * @param symbol_addr       [out] Symbol address (0 if not found)
 * @param image_index_out   [out] Image index of containing dylib (-1 if not found)
 * @return                  0 on success, -1 on failure
 *
 * Algorithm:
 *   1. Use rangeTable binary search to find containing image (O(log n))
 *   2. Convert imageIndex to dylibOffset via mapping table
 *   3. Search BOTH symbol sources for closest match:
 *      a. Dylib's own LC_SYMTAB (exported symbols) - O(d)
 *      b. Local symbols from dyld_cache_local_symbols_info - O(l)
 *   4. Select the symbol with largest n_value <= target_addr
 *
 * Time Complexity: O(log n + d + l)
 *   where n = rangeTableCount, d = dylib symbols, l = local symbols
 */
static int find_symbol_for_address(
    const uint8_t *cache, size_t cache_size,
    const struct dyld_cache_header *header,
    const struct dyld_cache_mapping_info *mappings, uint32_t mapping_count,
    const struct dyld_cache_local_symbols_info *local_info,
    const struct dyld_cache_range_entry *rangeTable, uint32_t range_table_count,
    uint64_t target_addr, const char **symbol_name, uint64_t *symbol_addr,
    int32_t *image_index_out)
{
    /* Initialize output parameters */
    *symbol_name = NULL;
    *symbol_addr = 0;
    *image_index_out = -1;

    /* Step 1: Binary search rangeTable for containing image */
    const struct dyld_cache_range_entry *range_entry =
        binary_search_range_table(rangeTable, range_table_count, target_addr);
    if (range_entry == NULL)
        return -1;  /* Address not in any dylib */

    uint32_t image_index = range_entry->imageIndex;
    *image_index_out = (int32_t)image_index;

    /* Step 2: Convert imageIndex to dylibOffset */
    int64_t dylib_offset = image_index_to_dylib_offset(
        cache, header, mappings, mapping_count, image_index);
    if (dylib_offset < 0)
        return -1;

    int found_symbol = 0;

    /* Step 3a: Search dylib's own symbol table (exported symbols) */
    if (search_dylib_symbol_table(cache, cache_size, mappings, mapping_count,
                                  (uint64_t)dylib_offset, target_addr,
                                  symbol_name, symbol_addr)) {
        found_symbol = 1;
    }

    /* Step 3b: Search local symbols from dyld_cache_local_symbols_info */
    if (local_info != NULL) {
        const struct dyld_cache_local_symbols_entry *entries = /* ... */;
        const struct dyld_cache_local_symbols_entry *sym_entry =
            find_local_symbols_entry(entries, local_info->entriesCount,
                                     (uint64_t)dylib_offset);

        if (sym_entry != NULL) {
            const char *local_name = NULL;
            uint64_t local_addr = 0;

            if (search_symbol_table(nlist_base, string_table,
                                    sym_entry->nlistStartIndex, sym_entry->nlistCount,
                                    target_addr, &local_name, &local_addr)) {
                /* Step 4: Use local symbol if it's closer than current best */
                if (local_addr > *symbol_addr) {
                    *symbol_name = local_name;
                    *symbol_addr = local_addr;
                    found_symbol = 1;
                }
            }
        }
    }

    return found_symbol ? 0 : -1;
}
```

#### 2.4.2 ImageIndex to DylibOffset Conversion

```c
/**
 * Convert imageIndex (from rangeTable) to dylibOffset (for local symbols entry).
 *
 * @param cache         Pointer to mmap'd cache file
 * @param header        Cache header pointer
 * @param mappings      Mapping info array
 * @param mapping_count Number of mappings
 * @param image_index   Index from rangeTable lookup
 * @return              File offset of dylib's mach_header, or -1 on error
 *
 * The rangeTable returns an imageIndex into dyld_cache_image_info array.
 * The local_symbols_entry uses dylibOffset (file offset of mach_header).
 * This function bridges the two by:
 *   1. Looking up the image's virtual address from images[imageIndex].address
 *   2. Converting that virtual address to a file offset via mapping table
 */
static int64_t image_index_to_dylib_offset(
    const uint8_t* cache,
    const struct dyld_cache_header* header,
    const struct dyld_cache_mapping_info* mappings,
    uint32_t mapping_count,
    uint32_t image_index)
{
    /* Get image info array */
    const struct dyld_cache_image_info* images = 
        (const struct dyld_cache_image_info*)(cache + header->imagesOffset);
    
    /* Bounds check */
    if (image_index >= header->imagesCount)
        return -1;
    
    /* Get dylib's __TEXT segment virtual address */
    uint64_t image_addr = images[image_index].address;
    
    /* Convert virtual address to file offset using mapping table */
    return addr_to_file_offset(mappings, mapping_count, image_addr);
}
```

#### 2.4.3 Find Local Symbols Entry

```c
/**
 * Find the local symbols entry for a given dylib file offset.
 *
 * @param entries       Pointer to entries array
 * @param entries_count Number of entries
 * @param dylib_offset  File offset of dylib's mach_header (from image_index_to_dylib_offset)
 * @return              Pointer to matching entry, or NULL if not found
 *
 * Time Complexity: O(n) where n = entries_count
 *
 * Note: The entries array order may not match the images array order.
 * A linear search is required to find the matching dylibOffset.
 * The caller must ensure dylib_offset is valid (>= 0) before calling.
 */
static const struct dyld_cache_local_symbols_entry*
find_local_symbols_entry(
    const struct dyld_cache_local_symbols_entry* entries,
    uint32_t entries_count,
    uint64_t dylib_offset)
{
    for (uint32_t i = 0; i < entries_count; i++) {
        if ((uint64_t)entries[i].dylibOffset == dylib_offset) {
            return &entries[i];
        }
    }
    return NULL;
}
```

#### 2.4.4 Dylib Symbol Table Search

Search the dylib's own Mach-O symbol table for exported symbols:

```c
/**
 * Search dylib's own symbol table from its Mach-O header.
 *
 * In the dyld_shared_cache, each dylib's symbol table offsets (from LC_SYMTAB)
 * are relative to the __LINKEDIT segment. We need to:
 * 1. Find LC_SEGMENT_64(__LINKEDIT) to get linkedit_base
 * 2. Find LC_SYMTAB to get symbol table info
 * 3. Calculate actual pointers using the cache's __LINKEDIT mapping
 *
 * @param cache         Pointer to mmap'd cache file
 * @param cache_size    Size of cache file
 * @param mappings      Mapping info array
 * @param mapping_count Number of mappings
 * @param dylib_offset  File offset of dylib's mach_header in cache
 * @param target_addr   Target address to look up
 * @param best_name     [in/out] Best matching symbol name
 * @param best_addr     [in/out] Best matching symbol address
 * @return              true if a better symbol was found
 *
 * Symbol Table Location Calculation:
 *   1. Get linkedit_vmaddr from LC_SEGMENT_64(__LINKEDIT)
 *   2. Convert to cache file offset via addr_to_file_offset()
 *   3. symtab_cache_offset = linkedit_cache_offset + symoff - linkedit_fileoff
 *   4. strtab_cache_offset = linkedit_cache_offset + stroff - linkedit_fileoff
 */
static int search_dylib_symbol_table(
    const uint8_t *cache, size_t cache_size,
    const struct dyld_cache_mapping_info *mappings, uint32_t mapping_count,
    uint64_t dylib_offset, uint64_t target_addr,
    const char **best_name, uint64_t *best_addr);
```

#### 2.4.5 Symbol Table Search

Reference implementation based on `dyld-421.2/src/ImageLoaderMachO.cpp`:

**Note on Symbol Table Ordering**: The nlist entries in `dyld_cache_local_symbols_info` are **not guaranteed to be sorted by address** (n_value). They are stored in the order they appear in each dylib's original symbol table. Therefore, a linear scan O(m) is required to find the closest symbol. Binary search optimization is not applicable here.

```c
/**
 * Search symbol table for the closest match to target address.
 * 
 * @param nlist_base    Base pointer to nlist_64 array
 * @param string_table  Base pointer to string table
 * @param start_index   First nlist index for this image
 * @param count         Number of nlist entries for this image
 * @param target_addr   Target address (unslid)
 * @param best_name     [out] Best matching symbol name (unchanged if not found)
 * @param best_addr     [out] Best matching symbol address (unchanged if not found)
 * @return              true if a symbol was found, false otherwise
 *
 * Time Complexity: O(n) where n = count
 * Space Complexity: O(1)
 */
static bool search_symbol_table(
    const struct nlist_64* nlist_base,
    const char* string_table,
    uint32_t start_index,
    uint32_t count,
    uint64_t target_addr,
    const char** best_name,
    uint64_t* best_addr)
{
    const struct nlist_64* best_symbol = NULL;
    
    for (uint32_t i = 0; i < count; i++) {
        const struct nlist_64* sym = &nlist_base[start_index + i];
        
        /* Skip stabs debugging symbols */
        if ((sym->n_type & N_STAB) != 0)
            continue;
        
        /* Only consider symbols defined in a section */
        if ((sym->n_type & N_TYPE) != N_SECT)
            continue;
        
        /* Symbol must not be past the target address */
        if (sym->n_value > target_addr)
            continue;
        
        /* Select if this is the closest so far */
        if (best_symbol == NULL || sym->n_value > best_symbol->n_value) {
            best_symbol = sym;
        }
    }
    
    if (best_symbol != NULL) {
        *best_name = &string_table[best_symbol->n_strx];
        *best_addr = best_symbol->n_value;
        return true;
    }
    return false;
}
```

### 2.5 Flow Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          Symbol Lookup Flow                                  │
└─────────────────────────────────────────────────────────────────────────────┘

    ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
    │ Parse hex    │────▶│ Binary search│────▶│ Get imageIdx │
    │ address      │     │ rangeTable   │     │ from result  │
    └──────────────┘     └──────────────┘     └──────┬───────┘
                                                     │
                                                     ▼
                                              ┌──────────────┐
                                              │ Convert to   │
                                              │ dylibOffset  │
                                              │ via mapping  │
                                              └──────┬───────┘
                                                     │
                         ┌───────────────────────────┴───────────────────────────┐
                         │                                                       │
                         ▼                                                       ▼
                  ┌──────────────┐                                        ┌──────────────┐
                  │ Search dylib │                                        │ Search local │
                  │ LC_SYMTAB    │                                        │ symbols info │
                  │ (exported)   │                                        │ (local)      │
                  └──────┬───────┘                                        └──────┬───────┘
                         │                                                       │
                         └───────────────────────────┬───────────────────────────┘
                                                     │
                                                     ▼
                                              ┌──────────────┐
                                              │ Select best  │
                                              │ match (addr  │
                                              │ closest)     │
                                              └──────┬───────┘
                                                     │
                                                     ▼
                                              ┌──────────────┐
                                              │ Output:      │
                                              │ name + offset│
                                              └──────────────┘

Step Details:
  1. rangeTable binary search → imageIndex (O(log n))
  2. images[imageIndex].address → addr_to_file_offset() → dylibOffset (O(1))
  3. Search BOTH symbol sources in parallel:
     a. Dylib LC_SYMTAB: Parse mach_header → LC_SYMTAB → nlist scan (O(d))
     b. Local symbols: Find entry by dylibOffset (O(e)) → nlist scan (O(l))
  4. Select symbol with largest address <= target_addr
```

---

## 3. Interface Design

### 3.1 Command Line Interface

Symbol lookup is the default behavior. The existing dylib-only lookup will be performed as a fallback when symbol resolution is unavailable.

**Address Convention**: All addresses in this tool are **unslid** (file-based) addresses. The dyld_shared_cache file contains unslid addresses; ASLR slide is only applied at runtime. When analyzing crash logs, subtract the slide value before querying.

```
Usage: ipsw [options] <dyld_shared_cache_path> <hex_address>

Options:
  -h, --help        Show help message
  -v, --verbose     Verbose output (show cache and symbol details)

Examples:
  # Symbol lookup (default)
  ipsw dyld_shared_cache_arm64 0x180625848
  
  # Verbose symbol lookup
  ipsw -v dyld_shared_cache_arm64 0x180625848
```

### 3.2 Output Format

**Standard Output** (compatible with atos):

```
vm_allocate (in libsystem_kernel.dylib) + 0x0
```

**Verbose Output** (`-v` flag):

```
Cache magic: dyld_v1   arm64
Image count: 1170
Target address: 0x180625848

Image: /usr/lib/system/libsystem_kernel.dylib
Symbol: vm_allocate
Symbol address: 0x180625848
Offset: +0x0
```

**Fallback Output** (when no symbol found):

```
(in libSystem.B.dylib) + 0x0
```

**Note**: In fallback mode, the offset is relative to the dylib's `__TEXT` segment base address, not a symbol address.

### 3.3 Error Handling

| Exit Code | Condition | Message |
|-----------|-----------|---------|
| 0 | Symbol found | Standard output |
| 0 | No symbol, but dylib found | Fallback output with dylib name (prints warning to stderr) |
| 0 | Cache lacks local symbols | Fallback to dylib-only output (prints "Note: No local symbols available" to stderr) |
| 1 | Address not in any dylib | "Error: Address 0x%llx not found in any dylib" |

**Note**: When local symbols are unavailable, the tool falls back to dylib-only output and returns exit code 0 (success with degraded functionality). This allows scripts to continue processing while being notified via stderr.

---

## 4. Implementation Plan

### Phase 1: Symbol Infrastructure ✅ Completed

**Task 1.1: Parse dyld_cache_local_symbols_info**
- [x] Add `get_local_symbols_info()` function
- [x] Validate localSymbolsOffset and localSymbolsSize
- [x] Bounds check all table accesses

**Task 1.2: Build dylibOffset lookup**
- [x] Parse `dyld_cache_local_symbols_entry` array
- [x] Implement `find_local_symbols_entry()` for linear search

**Task 1.3: Implement local symbol search**
- [x] Add `search_symbol_table()` function
- [x] Handle nlist_64 parsing with proper filtering (N_STAB, N_SECT)
- [x] Access string table for symbol names

**Task 1.4: Implement dylib symbol search** (Added during implementation)
- [x] Add `search_dylib_symbol_table()` function
- [x] Parse LC_SYMTAB and LC_SEGMENT_64(__LINKEDIT) load commands
- [x] Calculate symbol/string table offsets in shared cache
- [x] Search exported symbols from dylib's Mach-O header

**Acceptance Criteria:** ✅
- Resolve 0x180625848 to "vm_allocate + 0x0"
- Graceful fallback when symbols unavailable
- Handle edge cases: dylib base address, no matching symbol

### Phase 2: Output Formatting ✅ Completed

**Task 2.1: atos-compatible output**
- [x] Format: "symbol (in dylib) + offset"
- [x] Strip leading underscore from symbol names (C convention)

**Task 2.2: Verbose mode enhancement**
- [x] Display cache and image information
- [x] Show symbol address and offset details

---

## 5. Testing

### 5.1 Test Cases

Test data based on iOS 10.0.2 `dyld_shared_cache_arm64`.

| Test Scenario | Input Address | Expected Output | Notes |
|---------------|---------------|-----------------|-------|
| Function start | 0x180625848 | vm_allocate (in libsystem_kernel.dylib) + 0x0 | Exact start of vm_allocate (exported symbol) |
| Dylib base | 0x180028000 | (in libSystem.B.dylib) + 0x0 | Dylib start, no symbol before this address |
| Invalid address | 0x100000000 | Error: Address 0x100000000 not found in any dylib | Outside cache range |

**Note**: Test addresses may vary between cache versions. Use `ipsw -v` to verify actual addresses.

### 5.2 Performance Benchmarks

| Metric | Target | Notes |
|--------|--------|-------|
| Single lookup latency | < 10ms | Including mmap overhead |
| Memory usage | Minimal | mmap with lazy loading |

---

## 6. Risk Assessment

### 6.1 Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Cache lacks local symbols | Medium | Medium | Fallback to dylib-only output |
| Corrupted symbol table | Low | Medium | Bounds check all accesses |
| Large symbol tables | Low | Low | Linear scan is acceptable for single lookups |

---

## 7. Future Considerations

### 7.1 Potential Extensions

| Feature | Status | Description |
|---------|--------|-------------|
| DWARF debug info | 💡 Idea | Parse __DWARF segment for source file and line numbers |
| Batch lookup mode | 💡 Idea | Process multiple addresses from stdin |
| Symbol demangling | 📋 Planned | Demangle C++ and Swift symbol names |

---

## 8. Appendix

### 8.1 References

All references are from Apple's open source dyld-421.2:

1. `dyld-421.2/src/dyldAPIs.cpp` - `dladdr()` implementation
2. `dyld-421.2/src/ImageLoaderMachO.cpp` - `findClosestSymbol()` implementation
3. `dyld-421.2/launch-cache/dyld_cache_format.h` - Cache format structures
4. `dyld-421.2/launch-cache/dsc_iterator.cpp` - Cache iteration and symbol access

Source: https://opensource.apple.com/tarballs/dyld/

### 8.2 Related Documents

| Document | Description |
|----------|-------------|
| `address_lookup.md` | Address to dylib lookup implementation |
| `rangetable_optimization.md` | RangeTable binary search optimization |

### 8.3 Glossary

| Term | Definition |
|------|------------|
| nlist | Symbol table entry structure in Mach-O format |
| Stabs | Legacy debugging symbol format (filtered via N_STAB mask) |
| n_value | Symbol address field in nlist structure |
| n_strx | String table index for symbol name |
| N_EXT | External (exported) symbol flag in n_type |
| LC_SYMTAB | Mach-O load command containing symbol table info |
| __LINKEDIT | Mach-O segment containing symbol/string tables |
| Local symbol | Symbol with `(n_type & N_EXT) == 0`, not externally visible |
| Exported symbol | Symbol with `(n_type & N_EXT) != 0`, can be linked externally |

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 2.0 | 2026-02-04 | Chason Tang | **Implementation complete**: Added dual symbol source search (dylib LC_SYMTAB + local symbols); added `search_dylib_symbol_table()` for exported symbols; updated architecture diagram and flow chart; updated test cases to reflect actual behavior; marked all implementation tasks as completed; changed status to Implemented |
| 1.3 | 2026-02-04 | Chason Tang | Added ASLR slide convention note; added phase time estimates; fixed test case addresses and error message format; clarified fallback output offset semantics |
| 1.2 | 2026-02-04 | Chason Tang | Fixed: search_symbol_table returns bool; fixed type consistency (uint64_t for dylib_offset); unified N_TYPE/N_SECT format (0x0e); added complete find_symbol_for_address implementation; fixed error handling table consistency; added dyld-421.2 version to all references; added data structures source annotation; improved test cases with notes |
| 1.1 | 2026-02-04 | Chason Tang | Added imageIndex→dylibOffset conversion details; added find_local_symbols_entry algorithm; clarified symbol table ordering; unified references |
| 1.0 | 2026-02-04 | Chason Tang | Initial version |

---

*End of Technical Design Document*
