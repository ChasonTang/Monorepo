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
      localSymbolsOffset;    /* file offset of where local symbols are stored */
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
  uint32_t version;            /* currently 1 */
  uint32_t imageExtrasCount;   /* does not include aliases */
  uint32_t imagesExtrasOffset; /* offset to first dyld_cache_image_info_extra */
  uint32_t bottomUpListOffset; /* offset to bottom-up sorted image index list */
  uint32_t dylibTrieOffset;    /* offset to dylib path trie */
  uint32_t dylibTrieSize;      /* size of dylib trie */
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
  int64_t file_offset =
      addr_to_file_offset(mappings, mapping_count, header->accelerateInfoAddr);
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

static void print_usage(const char *prog_name) {
  fprintf(stderr, "Usage: %s [-v] <dyld_shared_cache_path> <hex_address>\n",
          prog_name);
  fprintf(stderr, "\n");
  fprintf(stderr, "Arguments:\n");
  fprintf(stderr, "  -v                      Verbose mode (show cache info)\n");
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
  const uint8_t *cache = mmap(NULL, cache_size, PROT_READ, MAP_PRIVATE, fd, 0);
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
   * The bounds checks below prevent buffer overruns when accessing the tables.
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

  if (verbose) {
    printf("Cache magic: %.16s\n", header->magic);
    printf("Image count: %u\n", header->imagesCount);
    printf("Target address: 0x%llx\n\n", (unsigned long long)target_addr);

    /* Print mapping info */
    printf("Mappings (%u):\n", header->mappingCount);
    for (uint32_t i = 0; i < header->mappingCount; i++) {
      printf("  [%u] address: 0x%llx - 0x%llx, fileOffset: 0x%llx\n", i,
             (unsigned long long)mappings[i].address,
             (unsigned long long)(mappings[i].address + mappings[i].size),
             (unsigned long long)mappings[i].fileOffset);
    }
    printf("\n");

    /* Print accelerator info */
    printf("Accelerator info:\n");
    printf("  version: %u\n", accel_info->version);
    printf("  rangeTableCount: %u\n", accel_info->rangeTableCount);
    printf("  imageExtrasCount: %u\n", accel_info->imageExtrasCount);
    printf("\n");
  }

  /* Binary search for the address in rangeTable - O(log n) */
  const struct dyld_cache_range_entry *found_entry = binary_search_range_table(
      rangeTable, accel_info->rangeTableCount, target_addr);

  if (found_entry != NULL) {
    uint32_t image_index = found_entry->imageIndex;

    /* Validate image index */
    if (image_index >= header->imagesCount) {
      fprintf(stderr, "Error: Invalid image index %u (max: %u)\n", image_index,
              header->imagesCount - 1);
      munmap((void *)cache, cache_size);
      return 1;
    }

    /* Get the dylib path */
    const struct dyld_cache_image_info *image = &images[image_index];
    if (image->pathFileOffset >= cache_size) {
      fprintf(stderr, "Error: Invalid path offset for image %u\n", image_index);
      munmap((void *)cache, cache_size);
      return 1;
    }

    const char *dylib_path = (const char *)(cache + image->pathFileOffset);

    /* Ensure path string is null-terminated within bounds */
    size_t max_path_len = cache_size - image->pathFileOffset;
    size_t path_len = strnlen(dylib_path, max_path_len);
    if (path_len == max_path_len) {
      fprintf(stderr, "Error: Path string not null-terminated for image %u\n",
              image_index);
      munmap((void *)cache, cache_size);
      return 1;
    }

    printf("%s\n", dylib_path);
    munmap((void *)cache, cache_size);
    return 0;
  }

  /* Address not found - this is expected for __LINKEDIT addresses */
  fprintf(stderr, "Address 0x%llx not found in any dylib\n",
          (unsigned long long)target_addr);
  munmap((void *)cache, cache_size);
  return 1;
}
