/*
 * IPSW CLI Tool - Address Lookup in dyld_shared_cache
 *
 * Usage: ipsw <dyld_shared_cache_path> <hex_address>
 *
 * This tool accepts a dyld_shared_cache file path and a hexadecimal address,
 * then outputs which dynamic library the address belongs to.
 *
 * Based on dyld-421.2 shared cache format.
 */

#include <fcntl.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * dyld_cache_header - Main shared cache header
 * Based on dyld-421.2/launch-cache/dyld_cache_format.h
 */
struct dyld_cache_header {
    char magic[16];           /* e.g. "dyld_v1   arm64" */
    uint32_t mappingOffset;   /* file offset to first dyld_cache_mapping_info */
    uint32_t mappingCount;    /* number of dyld_cache_mapping_info entries */
    uint32_t imagesOffset;    /* file offset to first dyld_cache_image_info */
    uint32_t imagesCount;     /* number of dyld_cache_image_info entries */
    uint64_t dyldBaseAddress; /* base address of dyld when cache was built */
    uint64_t codeSignatureOffset; /* file offset of code signature blob */
    uint64_t codeSignatureSize;   /* size of code signature blob */
    uint64_t slideInfoOffset;     /* file offset of kernel slid info */
    uint64_t slideInfoSize;       /* size of kernel slid info */
    uint64_t
        localSymbolsOffset; /* file offset of where local symbols are stored */
    uint64_t localSymbolsSize; /* size of local symbols information */
    uint8_t uuid[16];          /* unique value for each shared cache file */
    uint64_t cacheType;        /* 0 for development, 1 for production */
    uint32_t
        branchPoolsOffset; /* file offset to table of uint64_t pool addresses */
    uint32_t branchPoolsCount;   /* number of uint64_t entries */
    uint64_t accelerateInfoAddr; /* (unslid) address of optimization info */
    uint64_t accelerateInfoSize; /* size of optimization info */
    uint64_t
        imagesTextOffset; /* file offset to first dyld_cache_image_text_info */
    uint64_t imagesTextCount; /* number of dyld_cache_image_text_info entries */
};

/*
 * dyld_cache_mapping_info - Maps file regions to virtual addresses
 */
struct dyld_cache_mapping_info {
    uint64_t address;
    uint64_t size;
    uint64_t fileOffset;
    uint32_t maxProt;
    uint32_t initProt;
};

/*
 * dyld_cache_image_info - Information about each dylib in the cache
 */
struct dyld_cache_image_info {
    uint64_t address; /* unslid address of start of __TEXT */
    uint64_t modTime;
    uint64_t inode;
    uint32_t pathFileOffset; /* file offset of path string */
    uint32_t pad;
};

/*
 * dyld_cache_accelerator_info - Accelerator table header
 * Contains offsets to various optimization tables including rangeTable.
 * Based on dyld-421.2/launch-cache/dyld_cache_format.h
 */
struct dyld_cache_accelerator_info {
    uint32_t version;          /* currently 1 */
    uint32_t imageExtrasCount; /* does not include aliases */
    uint32_t
        imagesExtrasOffset; /* offset to first dyld_cache_image_info_extra */
    uint32_t
        bottomUpListOffset;   /* offset to bottom-up sorted image index list */
    uint32_t dylibTrieOffset; /* offset to dylib path trie */
    uint32_t dylibTrieSize;   /* size of dylib trie */
    uint32_t initializersOffset; /* offset to initializers list */
    uint32_t initializersCount;  /* count of initializers */
    uint32_t dofSectionsOffset;  /* offset to DOF sections */
    uint32_t dofSectionsCount;   /* count of DOF sections */
    uint32_t reExportListOffset; /* offset to re-export list */
    uint32_t reExportCount;      /* count of re-exports */
    uint32_t depListOffset;      /* offset to dependency list */
    uint32_t depListCount;       /* count of dependencies */
    uint32_t rangeTableOffset;   /* offset to range table */
    uint32_t rangeTableCount;    /* count of range entries */
    uint64_t dyldSectionAddr;    /* address of libdyld's __dyld section */
};

/*
 * dyld_cache_range_entry - Maps an address range to an image index
 * Entries are sorted by startAddress for binary search.
 */
struct dyld_cache_range_entry {
    uint64_t startAddress; /* unslid address of region start */
    uint32_t size;         /* size of region in bytes */
    uint32_t imageIndex;   /* index into dyld_cache_image_info array */
};

/**
 * Header for local symbols section in dyld_shared_cache.
 * Located at header->localSymbolsOffset in the cache file.
 * Based on dyld-421.2/launch-cache/dyld_cache_format.h
 */
struct dyld_cache_local_symbols_info {
    uint32_t nlistOffset;   /* Offset to nlist entries (from this struct) */
    uint32_t nlistCount;    /* Total count of nlist entries */
    uint32_t stringsOffset; /* Offset to string table (from this struct) */
    uint32_t stringsSize;   /* Size of string table in bytes */
    uint32_t entriesOffset; /* Offset to entries array (from this struct) */
    uint32_t entriesCount;  /* Number of entries (one per dylib) */
};

/**
 * Per-dylib entry in local symbols table.
 * Maps a dylib to its range of symbols in the shared nlist array.
 */
struct dyld_cache_local_symbols_entry {
    uint32_t dylibOffset;     /* File offset of dylib's mach_header in cache */
    uint32_t nlistStartIndex; /* First symbol index for this dylib */
    uint32_t nlistCount;      /* Number of symbols for this dylib */
};

/**
 * 64-bit symbol table entry (from <mach-o/nlist.h>).
 */
struct nlist_64 {
    uint32_t n_strx;  /* Index into string table */
    uint8_t n_type;   /* Type flags (N_EXT, N_TYPE, etc.) */
    uint8_t n_sect;   /* Section number (1-based) or NO_SECT */
    uint16_t n_desc;  /* Description field */
    uint64_t n_value; /* Symbol value (address for defined symbols) */
};

/* n_type masks */
#define N_STAB 0xe0 /* Stabs debugging symbol */
#define N_PEXT 0x10 /* Private external symbol */
#define N_TYPE 0x0e /* Type mask */
#define N_EXT 0x01  /* External symbol */

/* n_type values for N_TYPE bits */
#define N_UNDF 0x00 /* Undefined */
#define N_ABS 0x02  /* Absolute */
#define N_SECT 0x0e /* Defined in section n_sect */

/* Note: LC_SYMTAB, LC_SEGMENT_64, struct symtab_command, and
 * struct segment_command_64 are defined in <mach-o/loader.h> */

/*
 * Convert a virtual address to a file offset using the mapping table.
 * Returns -1 if the address is not in any mapping.
 */
static int64_t
addr_to_file_offset(const struct dyld_cache_mapping_info *mappings,
                    uint32_t mapping_count, uint64_t addr) {
    for (uint32_t i = 0; i < mapping_count; i++) {
        uint64_t start = mappings[i].address;
        uint64_t end = start + mappings[i].size;
        if (addr >= start && addr < end) {
            return (int64_t)(mappings[i].fileOffset + (addr - start));
        }
    }
    return -1;
}

/*
 * Get the accelerator info from the cache.
 * Returns pointer to accelerator info, or NULL if not available.
 *
 * The accelerator info requires:
 * - mappingOffset >= 0x78 (header has accelerateInfo fields)
 * - accelerateInfoAddr != 0
 * - accelerateInfoSize != 0
 */
static const struct dyld_cache_accelerator_info *
get_accelerator_info(const uint8_t *cache, size_t cache_size,
                     const struct dyld_cache_header *header,
                     const struct dyld_cache_mapping_info *mappings,
                     uint32_t mapping_count) {
    /* Check if header has accelerateInfo fields (mappingOffset >= 0x78) */
    if (header->mappingOffset < 0x78) {
        return NULL;
    }

    /* Check if accelerateInfo is present */
    if (header->accelerateInfoAddr == 0 || header->accelerateInfoSize == 0) {
        return NULL;
    }

    /* Convert accelerateInfoAddr to file offset */
    int64_t file_offset = addr_to_file_offset(mappings, mapping_count,
                                              header->accelerateInfoAddr);
    if (file_offset < 0) {
        return NULL;
    }

    /* Bounds check for accelerator info */
    if ((uint64_t)file_offset + sizeof(struct dyld_cache_accelerator_info) >
        cache_size) {
        return NULL;
    }

    const struct dyld_cache_accelerator_info *accel_info =
        (const struct dyld_cache_accelerator_info *)(cache + file_offset);

    /* Validate version (currently 1) */
    if (accel_info->version != 1) {
        return NULL;
    }

    /* Validate rangeTable bounds */
    if (accel_info->rangeTableCount == 0) {
        return NULL;
    }

    uint64_t range_table_end = (uint64_t)file_offset +
                               accel_info->rangeTableOffset +
                               (uint64_t)accel_info->rangeTableCount *
                                   sizeof(struct dyld_cache_range_entry);
    if (range_table_end > cache_size) {
        return NULL;
    }

    return accel_info;
}

/**
 * Get the local symbols info from the cache.
 * Returns pointer to local symbols info, or NULL if not available.
 *
 * @param cache       Pointer to mmap'd cache file
 * @param cache_size  Size of cache file
 * @param header      Cache header pointer
 * @return            Pointer to local symbols info, or NULL if not available
 */
static const struct dyld_cache_local_symbols_info *
get_local_symbols_info(const uint8_t *cache, size_t cache_size,
                       const struct dyld_cache_header *header) {
    /* Check if localSymbols is present */
    if (header->localSymbolsOffset == 0 || header->localSymbolsSize == 0) {
        return NULL;
    }

    /* Bounds check for local symbols info */
    if (header->localSymbolsOffset +
            sizeof(struct dyld_cache_local_symbols_info) >
        cache_size) {
        return NULL;
    }

    const struct dyld_cache_local_symbols_info *local_info =
        (const struct dyld_cache_local_symbols_info
             *)(cache + header->localSymbolsOffset);

    /* Validate offsets within local symbols section */
    uint64_t nlist_end =
        (uint64_t)local_info->nlistOffset +
        (uint64_t)local_info->nlistCount * sizeof(struct nlist_64);
    uint64_t strings_end =
        (uint64_t)local_info->stringsOffset + local_info->stringsSize;
    uint64_t entries_end = (uint64_t)local_info->entriesOffset +
                           (uint64_t)local_info->entriesCount *
                               sizeof(struct dyld_cache_local_symbols_entry);

    /* All offsets are relative to local_info, check they fit in
     * localSymbolsSize */
    if (nlist_end > header->localSymbolsSize ||
        strings_end > header->localSymbolsSize ||
        entries_end > header->localSymbolsSize) {
        return NULL;
    }

    return local_info;
}

/**
 * Convert imageIndex (from rangeTable) to dylibOffset (for local symbols
 * entry).
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
static int64_t
image_index_to_dylib_offset(const uint8_t *cache,
                            const struct dyld_cache_header *header,
                            const struct dyld_cache_mapping_info *mappings,
                            uint32_t mapping_count, uint32_t image_index) {
    /* Get image info array */
    const struct dyld_cache_image_info *images =
        (const struct dyld_cache_image_info *)(cache + header->imagesOffset);

    /* Bounds check */
    if (image_index >= header->imagesCount)
        return -1;

    /* Get dylib's __TEXT segment virtual address */
    uint64_t image_addr = images[image_index].address;

    /* Convert virtual address to file offset using mapping table */
    return addr_to_file_offset(mappings, mapping_count, image_addr);
}

/**
 * Find the local symbols entry for a given dylib file offset.
 *
 * @param entries       Pointer to entries array
 * @param entries_count Number of entries
 * @param dylib_offset  File offset of dylib's mach_header (from
 * image_index_to_dylib_offset)
 * @return              Pointer to matching entry, or NULL if not found
 *
 * Time Complexity: O(n) where n = entries_count
 *
 * Note: The entries array order may not match the images array order.
 * A linear search is required to find the matching dylibOffset.
 * The caller must ensure dylib_offset is valid (>= 0) before calling.
 */
static const struct dyld_cache_local_symbols_entry *
find_local_symbols_entry(const struct dyld_cache_local_symbols_entry *entries,
                         uint32_t entries_count, uint64_t dylib_offset) {
    for (uint32_t i = 0; i < entries_count; i++) {
        if ((uint64_t)entries[i].dylibOffset == dylib_offset) {
            return &entries[i];
        }
    }
    return NULL;
}

/**
 * Search symbol table for the closest match to target address.
 *
 * @param nlist_base    Base pointer to nlist_64 array
 * @param string_table  Base pointer to string table
 * @param start_index   First nlist index for this image
 * @param count         Number of nlist entries for this image
 * @param target_addr   Target address (unslid)
 * @param best_name     [out] Best matching symbol name (unchanged if not found)
 * @param best_addr     [out] Best matching symbol address (unchanged if not
 * found)
 * @return              true if a symbol was found, false otherwise
 *
 * Time Complexity: O(n) where n = count
 * Space Complexity: O(1)
 */
static int search_symbol_table(const struct nlist_64 *nlist_base,
                               const char *string_table, uint32_t start_index,
                               uint32_t count, uint64_t target_addr,
                               const char **best_name, uint64_t *best_addr,
                               int verbose) {
    const struct nlist_64 *best_symbol = NULL;

    (void)verbose; /* Reserved for future use */

    for (uint32_t i = 0; i < count; i++) {
        const struct nlist_64 *sym = &nlist_base[start_index + i];

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
        return 1;
    }
    return 0;
}

/**
 * Search dylib's own symbol table from its Mach-O header.
 *
 * In the dyld_shared_cache, each dylib's symbol table offsets (from LC_SYMTAB)
 * are relative to the __LINKEDIT segment base of the dylib. We need to:
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
 * @param verbose       Verbose output flag
 * @return              true if a better symbol was found, false otherwise
 */
static int
search_dylib_symbol_table(const uint8_t *cache, size_t cache_size,
                          const struct dyld_cache_mapping_info *mappings,
                          uint32_t mapping_count, uint64_t dylib_offset,
                          uint64_t target_addr, const char **best_name,
                          uint64_t *best_addr, int verbose) {
    /* Get mach_header_64 at dylib_offset */
    if (dylib_offset + sizeof(struct mach_header_64) > cache_size) {
        return 0;
    }

    const struct mach_header_64 *mh =
        (const struct mach_header_64 *)(cache + dylib_offset);

    /* Validate magic */
    if (mh->magic != MH_MAGIC_64) {
        return 0;
    }

    /* Find LC_SYMTAB and LC_SEGMENT_64(__LINKEDIT) */
    const struct symtab_command *symtab_cmd = NULL;
    uint64_t linkedit_vmaddr = 0;
    uint64_t linkedit_fileoff = 0;

    const uint8_t *lc_ptr = (const uint8_t *)(mh + 1);
    const uint8_t *lc_end = lc_ptr + mh->sizeofcmds;

    if ((uint64_t)(lc_end - cache) > cache_size) {
        return 0;
    }

    for (uint32_t i = 0; i < mh->ncmds && lc_ptr < lc_end; i++) {
        const struct load_command *lc = (const struct load_command *)lc_ptr;

        if (lc->cmdsize < sizeof(struct load_command) ||
            lc_ptr + lc->cmdsize > lc_end) {
            break;
        }

        if (lc->cmd == LC_SYMTAB) {
            symtab_cmd = (const struct symtab_command *)lc;
        } else if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg =
                (const struct segment_command_64 *)lc;
            if (strncmp(seg->segname, "__LINKEDIT", 16) == 0) {
                linkedit_vmaddr = seg->vmaddr;
                linkedit_fileoff = seg->fileoff;
            }
        }

        lc_ptr += lc->cmdsize;
    }

    if (symtab_cmd == NULL || linkedit_vmaddr == 0) {
        return 0;
    }

    /* In shared cache, symbol/string offsets are relative to __LINKEDIT file
     * offset But the actual data is in the cache's __LINKEDIT mapping. We need
     * to convert: symoff (file-based) -> cache file offset
     *
     * The formula is:
     *   actual_offset = linkedit_fileoff + (symoff - (linkedit_vmaddr -
     * linkedit_vmaddr)) But in shared cache, all dylibs share the same
     * __LINKEDIT segment from the cache. So we need to use the cache's
     * __LINKEDIT mapping to find the data.
     *
     * symoff and stroff are offsets from the original dylib's __LINKEDIT base.
     * We need to add them to linkedit_fileoff to get the cache file offset.
     */

    /* Convert linkedit_vmaddr to file offset in cache */
    int64_t linkedit_cache_offset =
        addr_to_file_offset(mappings, mapping_count, linkedit_vmaddr);
    if (linkedit_cache_offset < 0) {
        return 0;
    }

    /* Calculate symbol table and string table pointers */
    uint64_t symtab_offset =
        (uint64_t)linkedit_cache_offset + symtab_cmd->symoff - linkedit_fileoff;
    uint64_t strtab_offset =
        (uint64_t)linkedit_cache_offset + symtab_cmd->stroff - linkedit_fileoff;

    /* Bounds check */
    if (symtab_offset + symtab_cmd->nsyms * sizeof(struct nlist_64) >
        cache_size) {
        return 0;
    }
    if (strtab_offset + symtab_cmd->strsize > cache_size) {
        return 0;
    }

    const struct nlist_64 *nlist =
        (const struct nlist_64 *)(cache + symtab_offset);
    const char *strtab = (const char *)(cache + strtab_offset);

    (void)verbose; /* Reserved for future use */

    int found_better = 0;

    for (uint32_t i = 0; i < symtab_cmd->nsyms; i++) {
        const struct nlist_64 *sym = &nlist[i];

        /* Skip stabs debugging symbols */
        if ((sym->n_type & N_STAB) != 0)
            continue;

        /* Only consider symbols defined in a section */
        if ((sym->n_type & N_TYPE) != N_SECT)
            continue;

        /* Symbol must not be past the target address */
        if (sym->n_value > target_addr)
            continue;

        /* Bounds check for string index */
        if (sym->n_strx >= symtab_cmd->strsize)
            continue;

        const char *sym_name = &strtab[sym->n_strx];

        /* Select if this is closer than current best */
        if (*best_addr == 0 || sym->n_value > *best_addr) {
            *best_name = sym_name;
            *best_addr = sym->n_value;
            found_better = 1;
        }
    }

    return found_better;
}

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
static const struct dyld_cache_range_entry *
binary_search_range_table(const struct dyld_cache_range_entry *rangeTable,
                          uint32_t count, uint64_t addr) {
    uint32_t low = 0;
    uint32_t high = count;

    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        const struct dyld_cache_range_entry *entry = &rangeTable[mid];

        if (addr < entry->startAddress) {
            high = mid;
        } else if (addr >= entry->startAddress + entry->size) {
            low = mid + 1;
        } else {
            /* addr is within [startAddress, startAddress + size) */
            return entry;
        }
    }
    return NULL;
}

/**
 * Find the closest symbol for a given address.
 *
 * @param cache             Pointer to mmap'd cache file
 * @param cache_size        Size of cache file
 * @param header            Cache header pointer
 * @param mappings          Mapping info array
 * @param mapping_count     Number of mappings
 * @param local_info        Local symbols info pointer
 * @param rangeTable        Range table pointer
 * @param range_table_count Number of range entries
 * @param target_addr       Target address to look up
 * @param symbol_name       [out] Symbol name (NULL if not found)
 * @param symbol_addr       [out] Symbol address (0 if not found)
 * @param image_index       [out] Image index of containing dylib (-1 if not
 * found)
 * @return                  0 on success, -1 on failure (address not in cache or
 * no symbols)
 *
 * Algorithm:
 *   1. Use rangeTable binary search to find containing image (O(log n))
 *      - Returns imageIndex into dyld_cache_image_info array
 *   2. Convert imageIndex to dylibOffset:
 *      a. Access images[imageIndex].address to get dylib's __TEXT vmaddr
 *      b. Use addr_to_file_offset() to convert vmaddr to file offset
 *      - This file offset equals dylibOffset in local_symbols_entry
 *   3. Find matching entry in local_symbols_entry array by dylibOffset (O(e))
 *      - Linear search through entries array comparing dylibOffset
 *   4. Iterate through the dylib's nlist entries (O(m))
 *   5. Find symbol with largest n_value <= target_addr
 *
 * Time Complexity: O(log n + e + m)
 *   where n = rangeTableCount, e = entriesCount, m = symbols per dylib
 */
static int find_symbol_for_address(
    const uint8_t *cache, size_t cache_size,
    const struct dyld_cache_header *header,
    const struct dyld_cache_mapping_info *mappings, uint32_t mapping_count,
    const struct dyld_cache_local_symbols_info *local_info,
    const struct dyld_cache_range_entry *rangeTable, uint32_t range_table_count,
    uint64_t target_addr, const char **symbol_name, uint64_t *symbol_addr,
    int32_t *image_index_out, int verbose) {
    /* Initialize output parameters */
    *symbol_name = NULL;
    *symbol_addr = 0;
    *image_index_out = -1;

    /* Step 1: Binary search rangeTable for containing image */
    const struct dyld_cache_range_entry *range_entry =
        binary_search_range_table(rangeTable, range_table_count, target_addr);
    if (range_entry == NULL)
        return -1; /* Address not in any dylib */

    uint32_t image_index = range_entry->imageIndex;
    *image_index_out = (int32_t)image_index;

    /* Step 2: Convert imageIndex to dylibOffset */
    int64_t dylib_offset = image_index_to_dylib_offset(
        cache, header, mappings, mapping_count, image_index);
    if (dylib_offset < 0)
        return -1;

    int found_symbol = 0;

    /* Search source 1: dylib's own symbol table (exported symbols) */
    if (search_dylib_symbol_table(cache, cache_size, mappings, mapping_count,
                                  (uint64_t)dylib_offset, target_addr,
                                  symbol_name, symbol_addr, verbose)) {
        found_symbol = 1;
    }

    /* Search source 2: local symbols from dyld_cache_local_symbols_info */
    if (local_info != NULL) {
        /* Get entries array */
        const struct dyld_cache_local_symbols_entry *entries =
            (const struct dyld_cache_local_symbols_entry
                 *)((const uint8_t *)local_info + local_info->entriesOffset);

        /* Find matching local symbols entry */
        const struct dyld_cache_local_symbols_entry *sym_entry =
            find_local_symbols_entry(entries, local_info->entriesCount,
                                     (uint64_t)dylib_offset);

        if (sym_entry != NULL) {
            /* Get nlist and string table pointers */
            const struct nlist_64 *nlist_base =
                (const struct nlist_64 *)((const uint8_t *)local_info +
                                          local_info->nlistOffset);
            const char *string_table =
                (const char *)((const uint8_t *)local_info +
                               local_info->stringsOffset);

            /* Search local symbol table - may find a closer match */
            const char *local_name = NULL;
            uint64_t local_addr = 0;

            if (search_symbol_table(nlist_base, string_table,
                                    sym_entry->nlistStartIndex,
                                    sym_entry->nlistCount, target_addr,
                                    &local_name, &local_addr, verbose)) {
                /* Use local symbol if it's closer than current best */
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

/**
 * Get the basename of a path (last component after /).
 */
static const char *get_basename(const char *path) {
    const char *last_slash = strrchr(path, '/');
    return last_slash ? last_slash + 1 : path;
}

/**
 * Strip leading underscore from symbol name (C convention).
 * Returns the original name if it doesn't start with underscore.
 */
static const char *strip_leading_underscore(const char *name) {
    if (name && name[0] == '_')
        return name + 1;
    return name;
}

static void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s [-v] <dyld_shared_cache_path> <hex_address>\n",
            prog_name);
    fprintf(stderr, "\n");
    fprintf(stderr, "Arguments:\n");
    fprintf(stderr,
            "  -v                      Verbose mode (show cache info)\n");
    fprintf(stderr,
            "  dyld_shared_cache_path  Path to the dyld shared cache file\n");
    fprintf(stderr, "  hex_address             Hexadecimal address (with or "
                    "without 0x prefix)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  %s dyld_shared_cache_arm64 0x180028000\n", prog_name);
    fprintf(stderr, "  %s -v dyld_shared_cache_arm64 0x180028000\n", prog_name);
}

int main(int argc, const char *argv[]) {
    int verbose = 0;
    int arg_offset = 1;

    /* Check for -v flag */
    if (argc >= 2 && strcmp(argv[1], "-v") == 0) {
        verbose = 1;
        arg_offset = 2;
    }

    if (argc - arg_offset != 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cache_path = argv[arg_offset];
    const char *addr_str = argv[arg_offset + 1];

    /* Parse the hex address */
    char *endptr;
    uint64_t target_addr = strtoull(addr_str, &endptr, 16);
    if (*endptr != '\0' || endptr == addr_str) {
        fprintf(stderr, "Error: Invalid hexadecimal address '%s'\n", addr_str);
        return 1;
    }

    /* Open the cache file */
    int fd = open(cache_path, O_RDONLY);
    if (fd < 0) {
        perror("Error opening cache file");
        return 1;
    }

    /* Get file size */
    struct stat st;
    if (fstat(fd, &st) != 0) {
        perror("Error getting file size");
        close(fd);
        return 1;
    }
    size_t cache_size = (size_t)st.st_size;

    /* Memory map the file */
    const uint8_t *cache =
        mmap(NULL, cache_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (cache == MAP_FAILED) {
        perror("Error mapping cache file");
        close(fd);
        return 1;
    }
    close(fd);

    /* Verify file is large enough for header */
    if (cache_size < sizeof(struct dyld_cache_header)) {
        fprintf(stderr, "Error: File too small for dyld shared cache header\n");
        munmap((void *)cache, cache_size);
        return 1;
    }

    /* Verify the cache header */
    const struct dyld_cache_header *header =
        (const struct dyld_cache_header *)cache;

    /* Check magic - should start with "dyld_v1" */
    if (strncmp(header->magic, "dyld_v1", 7) != 0) {
        fprintf(stderr, "Error: Invalid dyld shared cache magic: %.16s\n",
                header->magic);
        munmap((void *)cache, cache_size);
        return 1;
    }

    /* Validate mappingOffset and imagesOffset bounds with overflow protection.
     * Note: On 64-bit systems, uint32_t cannot overflow size_t multiplication.
     * The bounds checks below prevent buffer overruns when accessing the
     * tables.
     */
    size_t mappings_size =
        (size_t)header->mappingCount * sizeof(struct dyld_cache_mapping_info);
    if (header->mappingOffset > cache_size ||
        mappings_size > cache_size - header->mappingOffset) {
        fprintf(stderr, "Error: Invalid mapping offset or count\n");
        munmap((void *)cache, cache_size);
        return 1;
    }

    size_t images_size =
        (size_t)header->imagesCount * sizeof(struct dyld_cache_image_info);
    if (header->imagesOffset > cache_size ||
        images_size > cache_size - header->imagesOffset) {
        fprintf(stderr, "Error: Invalid images offset or count\n");
        munmap((void *)cache, cache_size);
        return 1;
    }

    /* Get mappings and images */
    const struct dyld_cache_mapping_info *mappings =
        (const struct dyld_cache_mapping_info *)(cache + header->mappingOffset);
    const struct dyld_cache_image_info *images =
        (const struct dyld_cache_image_info *)(cache + header->imagesOffset);

    /* Validate each mapping's file range */
    for (uint32_t i = 0; i < header->mappingCount; i++) {
        if (mappings[i].fileOffset > cache_size ||
            mappings[i].size > cache_size - mappings[i].fileOffset) {
            fprintf(stderr, "Error: Mapping %u has invalid file range\n", i);
            munmap((void *)cache, cache_size);
            return 1;
        }
    }

    /* Get accelerator info - required for iOS 9+ / macOS 10.11+ */
    const struct dyld_cache_accelerator_info *accel_info = get_accelerator_info(
        cache, cache_size, header, mappings, header->mappingCount);
    if (accel_info == NULL) {
        fprintf(stderr, "Error: This cache lacks accelerator info. "
                        "Only iOS 9+ / macOS 10.11+ caches are supported.\n");
        munmap((void *)cache, cache_size);
        return 1;
    }

    /* Get the rangeTable pointer */
    int64_t accel_file_offset = addr_to_file_offset(
        mappings, header->mappingCount, header->accelerateInfoAddr);
    const struct dyld_cache_range_entry *rangeTable =
        (const struct dyld_cache_range_entry *)(cache + accel_file_offset +
                                                accel_info->rangeTableOffset);

    /* Get local symbols info (may be NULL if not available) */
    const struct dyld_cache_local_symbols_info *local_info =
        get_local_symbols_info(cache, cache_size, header);

    if (verbose) {
        printf("Cache magic: %.16s\n", header->magic);
        printf("Image count: %u\n", header->imagesCount);
        printf("Target address: 0x%llx\n", (unsigned long long)target_addr);
        printf("\n");
    }

    /* Perform symbol lookup */
    const char *symbol_name = NULL;
    uint64_t symbol_addr = 0;
    int32_t image_index = -1;

    int symbol_found = find_symbol_for_address(
        cache, cache_size, header, mappings, header->mappingCount, local_info,
        rangeTable, accel_info->rangeTableCount, target_addr, &symbol_name,
        &symbol_addr, &image_index, verbose);

    /* Check if we at least found the containing dylib */
    if (image_index < 0) {
        /* Address not in any dylib */
        fprintf(stderr, "Error: Address 0x%llx not found in any dylib\n",
                (unsigned long long)target_addr);
        munmap((void *)cache, cache_size);
        return 1;
    }

    /* Get the dylib path */
    const struct dyld_cache_image_info *image = &images[image_index];
    if (image->pathFileOffset >= cache_size) {
        fprintf(stderr, "Error: Invalid path offset for image %d\n",
                image_index);
        munmap((void *)cache, cache_size);
        return 1;
    }

    const char *dylib_path = (const char *)(cache + image->pathFileOffset);

    /* Ensure path string is null-terminated within bounds */
    size_t max_path_len = cache_size - image->pathFileOffset;
    size_t path_len = strnlen(dylib_path, max_path_len);
    if (path_len == max_path_len) {
        fprintf(stderr, "Error: Path string not null-terminated for image %d\n",
                image_index);
        munmap((void *)cache, cache_size);
        return 1;
    }

    const char *dylib_basename = get_basename(dylib_path);

    if (symbol_found == 0 && symbol_name != NULL) {
        /* Symbol found - output in atos-compatible format */
        uint64_t offset = target_addr - symbol_addr;
        const char *display_name = strip_leading_underscore(symbol_name);

        if (verbose) {
            printf("Image: %s\n", dylib_path);
            printf("Symbol: %s\n", display_name);
            printf("Symbol address: 0x%llx\n", (unsigned long long)symbol_addr);
            printf("Offset: +0x%llx\n", (unsigned long long)offset);
        } else {
            /* atos-compatible output: symbol (in dylib) + offset */
            printf("%s (in %s) + 0x%llx\n", display_name, dylib_basename,
                   (unsigned long long)offset);
        }
    } else {
        /* No symbol found - fallback to dylib-only output */
        if (local_info == NULL) {
            fprintf(stderr, "Note: No local symbols available\n");
        }

        /* Calculate offset from dylib's __TEXT base */
        uint64_t dylib_base = image->address;
        uint64_t offset = target_addr - dylib_base;

        if (verbose) {
            printf("Image: %s\n", dylib_path);
            printf("Symbol: (not found)\n");
            printf("Dylib base: 0x%llx\n", (unsigned long long)dylib_base);
            printf("Offset: +0x%llx\n", (unsigned long long)offset);
        } else {
            /* Fallback output: (in dylib) + offset */
            printf("(in %s) + 0x%llx\n", dylib_basename,
                   (unsigned long long)offset);
        }
    }

    munmap((void *)cache, cache_size);
    return 0;
}
