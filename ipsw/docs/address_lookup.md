# IPSW Address Lookup Tool - Technical Design Document

## 1. Overview

### 1.1 Background

iOS uses the `dyld_shared_cache` file to pre-link all system dynamic libraries together, improving app launch performance and reducing memory footprint. In scenarios such as reverse engineering, performance analysis, and crash log parsing, it is often necessary to map a virtual address back to its corresponding dynamic library.

This project implements a command-line tool that queries which dynamic library a given address belongs to within a `dyld_shared_cache` file.

### 1.2 Goals

- Accept a `dyld_shared_cache` file path and a hexadecimal address as input
- Output the path of the dynamic library containing that address
- Pure C implementation with no external dependencies
- Based on Apple dyld-421.2 open source code

## 2. Technical Architecture

### 2.1 System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         IPSW CLI Tool                           │
├─────────────────────────────────────────────────────────────────┤
│  main()                                                         │
│  ├── Argument Parsing                                           │
│  ├── File Mapping (mmap)                                        │
│  ├── Cache Header Validation                                    │
│  └── Address Lookup Engine                                      │
│       ├── Mapping Table Parser (dyld_cache_mapping_info)        │
│       ├── Image Table Iterator (dyld_cache_image_info)          │
│       └── Mach-O Segment Parser (LC_SEGMENT_64)                 │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Core Data Structures

#### 2.2.1 dyld_cache_header

The main header structure of the shared cache file, defining the basic layout:

```c
struct dyld_cache_header {
  char magic[16];           // Magic bytes, e.g. "dyld_v1   arm64"
  uint32_t mappingOffset;   // File offset to mapping info table
  uint32_t mappingCount;    // Number of mapping info entries
  uint32_t imagesOffset;    // File offset to image info table
  uint32_t imagesCount;     // Number of image info entries
  uint64_t dyldBaseAddress; // dyld base address
  // ... additional fields
};
```

#### 2.2.2 dyld_cache_mapping_info

Describes the mapping between virtual addresses and file offsets:

```c
struct dyld_cache_mapping_info {
  uint64_t address;     // Virtual address start
  uint64_t size;        // Region size
  uint64_t fileOffset;  // Corresponding file offset
  uint32_t maxProt;     // Maximum protection attributes
  uint32_t initProt;    // Initial protection attributes
};
```

A typical iOS arm64 shared cache contains 3 mapping regions:

| Region | Description | Protection |
|--------|-------------|------------|
| __TEXT | Code segment | r-x |
| __DATA | Data segment | rw- |
| __LINKEDIT | Link information | r-- |

#### 2.2.3 dyld_cache_image_info

Describes information about each dynamic library in the cache:

```c
struct dyld_cache_image_info {
  uint64_t address;        // Virtual address of __TEXT segment
  uint64_t modTime;        // Modification time
  uint64_t inode;          // Inode number
  uint32_t pathFileOffset; // File offset to library path string
  uint32_t pad;            // Padding
};
```

## 3. Core Algorithms

### 3.1 Address Translation Algorithm

Converts a virtual address to a file offset:

```c
// Input: virtual address addr, mapping table mappings[], mapping count
// Output: file offset or -1 (not found)

for (uint32_t i = 0; i < mapping_count; i++) {
    uint64_t start = mappings[i].address;
    uint64_t end = start + mappings[i].size;
    if (addr >= start && addr < end) {
        return mappings[i].fileOffset + (addr - start);
    }
}
return -1;
```

Time Complexity: O(n), where n is the number of mappings (typically 3)

### 3.2 Address Lookup Algorithm

Finds the dynamic library containing the target address:

```c
// Input: cache file data, target address
// Output: list of matching dynamic library paths

for (uint32_t i = 0; i < header->imagesCount; i++) {
    int64_t file_offset = addr_to_file_offset(mappings, mapping_count, images[i].address);
    if (file_offset < 0)
        continue;

    struct mach_header_64 *mh = (struct mach_header_64 *)(cache + file_offset);
    uint8_t *cmd_ptr = (uint8_t *)mh + sizeof(struct mach_header_64);

    for (uint32_t j = 0; j < mh->ncmds; j++) {
        struct load_command *cmd = (struct load_command *)cmd_ptr;
        if (cmd->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *seg = (struct segment_command_64 *)cmd_ptr;
            uint64_t seg_start = seg->vmaddr;
            uint64_t seg_end = seg_start + seg->vmsize;
            if (target_addr >= seg_start && target_addr < seg_end) {
                printf("%s\n", image_path);
                break;
            }
        }
        cmd_ptr += cmd->cmdsize;
    }
}
```

Time Complexity: O(n × m), where n is the number of images and m is the average number of load commands per image

### 3.3 Performance Considerations

1. **Memory Mapping (mmap)**: Uses `mmap` instead of `read` to leverage the operating system's page cache mechanism
2. **Early Exit**: Optionally continue searching for aliases after finding a match
3. **Future Optimization Directions**:
   - Utilize the `rangeTable` in `accelerateInfo` for binary search
   - Build an address range index

## 4. Interface Design

### 4.1 Command Line Interface

```
Usage: ipsw [-v] <dyld_shared_cache_path> <hex_address>

Arguments:
  -v                      Verbose mode (display cache info)
  dyld_shared_cache_path  Path to the shared cache file
  hex_address             Hexadecimal address (with or without 0x prefix)

Examples:
  ipsw dyld_shared_cache_arm64 0x180028000
  ipsw -v dyld_shared_cache_arm64 180028000
```

### 4.2 Exit Codes

| Exit Code | Meaning |
|-----------|---------|
| 0 | Successfully found matching dynamic library |
| 1 | No match found or error occurred |

### 4.3 Output Format

**Normal Mode**: One matching dynamic library path per line
```
/usr/lib/libSystem.B.dylib
/usr/lib/libSystem.dylib
```

**Verbose Mode (-v)**: Displays additional cache metadata
```
Cache magic: dyld_v1   arm64
Image count: 1170
Target address: 0x180028000

Mappings (3):
  [0] address: 0x180000000 - 0x19db08000, fileOffset: 0x0
  [1] address: 0x19fb08000 - 0x1a5b44000, fileOffset: 0x1db08000
  [2] address: 0x1a7b44000 - 0x1ac460000, fileOffset: 0x23b44000

/usr/lib/libSystem.B.dylib
```

## 5. Compatibility

### 5.1 Supported Cache Formats

This tool **supports 64-bit architectures only**. 32-bit architectures (armv7, i386) are not currently needed and therefore not supported.

| Magic | Architecture | Status |
|-------|-------------|--------|
| `dyld_v1   arm64` | ARM64 (64-bit) | ✅ Supported |
| `dyld_v1  x86_64` | x86_64 (64-bit) | ⚠️ Untested |
| `dyld_v1 x86_64h` | x86_64h (Haswell) | ⚠️ Untested |
| `dyld_v1   armv7` | ARM (32-bit) | ❌ Not Supported |
| `dyld_v1    i386` | x86 (32-bit) | ❌ Not Supported |

### 5.2 Tested Versions

- iOS 10.0.2 (`dyld_shared_cache_arm64`)
- dyld source version: 421.2

## 6. Building and Testing

### 6.1 Build Instructions

```bash
cd /path/to/Monorepo
./tool/gn gen out/default
./tool/ninja -C out/default ipsw
```

### 6.2 Test Cases

| Test Scenario | Input Address | Expected Output |
|---------------|---------------|-----------------|
| vm_allocate | 0x180625848 | /usr/lib/system/libsystem_kernel.dylib |

## 7. Future Extensions

### 7.1 Planned Features

- [ ] Symbol lookup (output function name for a given address)
- [ ] Optimize queries using accelerateInfo.rangeTable
- [ ] Batch query mode

### 7.2 Code Structure Plan

```
ipsw/
├── docs/
│   └── address_lookup.md # This document
├── main.c                # Entry point (current single-file implementation)
├── BUILD.gn
│
├── [Future Extensions]
│   ├── src/
│   │   ├── cache_parser.c    # Cache parsing (future split)
│   │   └── macho_parser.c    # Mach-O parsing (future split)
│   ├── include/
│   │   └── dyld_cache.h      # Public header (future)
│   └── tests/
│       └── test_lookup.c     # Unit tests (future)
```

## 8. References

1. [Apple dyld Open Source Code](https://opensource.apple.com/tarballs/dyld/)
2. `dyld-421.2/launch-cache/dyld_cache_format.h` - Cache format definitions
3. `dyld-421.2/launch-cache/dsc_iterator.cpp` - Cache iteration implementation
4. `dyld-421.2/launch-cache/dyld_shared_cache_util.cpp` - Official tool implementation

---

*Document Version: 1.0*  
*Last Updated: 2026-02-03*
