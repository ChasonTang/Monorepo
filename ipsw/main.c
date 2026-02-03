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
 * Check if the target address falls within any segment of the Mach-O at
 * the given file offset. This handles 64-bit Mach-O format (arm64).
 * Returns 1 if found, 0 otherwise.
 */
static int check_macho_segments(const uint8_t *cache, size_t cache_size,
                                uint64_t macho_file_offset,
                                uint64_t target_addr) {
  const uint8_t *macho = cache + macho_file_offset;

  /* Bounds check */
  if (macho_file_offset + sizeof(struct mach_header_64) > cache_size) {
    return 0;
  }

  const struct mach_header_64 *mh = (const struct mach_header_64 *)macho;

  /* Verify magic - we expect 64-bit for arm64 */
  if (mh->magic != MH_MAGIC_64) {
    return 0;
  }

  /* Iterate through load commands */
  const uint8_t *cmd_ptr = macho + sizeof(struct mach_header_64);
  uint32_t ncmds = mh->ncmds;

  for (uint32_t i = 0; i < ncmds; i++) {
    /* Bounds check for load_command */
    if ((size_t)(cmd_ptr - cache) + sizeof(struct load_command) > cache_size) {
      return 0;
    }

    const struct load_command *cmd = (const struct load_command *)cmd_ptr;

    /* Validate cmdsize to prevent stuck iteration (Problem 2) */
    if (cmd->cmdsize < sizeof(struct load_command)) {
      return 0; /* Invalid cmdsize, abort */
    }

    if (cmd->cmd == LC_SEGMENT_64) {
      /* Bounds check for segment_command_64 (Problem 1) */
      if ((size_t)(cmd_ptr - cache) + sizeof(struct segment_command_64) >
          cache_size) {
        return 0;
      }

      const struct segment_command_64 *seg =
          (const struct segment_command_64 *)cmd_ptr;

      uint64_t seg_start = seg->vmaddr;
      uint64_t seg_end = seg_start + seg->vmsize;

      if (target_addr >= seg_start && target_addr < seg_end) {
        return 1;
      }
    }

    cmd_ptr += cmd->cmdsize;
  }

  return 0;
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
  }

  /* Search through all images */
  int found = 0;
  for (uint32_t i = 0; i < header->imagesCount; i++) {
    /* Validate pathFileOffset bounds (Problem 3) */
    if (images[i].pathFileOffset >= cache_size) {
      continue;
    }

    const char *dylib_path = (const char *)(cache + images[i].pathFileOffset);

    /* Ensure path string is null-terminated within bounds (Problem 4) */
    size_t max_path_len = cache_size - images[i].pathFileOffset;
    size_t path_len = strnlen(dylib_path, max_path_len);
    if (path_len == max_path_len) {
      /* Path string is not null-terminated within file bounds, skip */
      continue;
    }

    uint64_t image_addr = images[i].address;

    /* Convert image address to file offset */
    int64_t file_offset =
        addr_to_file_offset(mappings, header->mappingCount, image_addr);
    if (file_offset < 0) {
      continue;
    }

    /* Check if target address is in this image's segments */
    if (check_macho_segments(cache, cache_size, (uint64_t)file_offset,
                             target_addr)) {
      printf("%s\n", dylib_path);
      found = 1;
      /* Don't break - an address might appear in multiple segments/aliases */
    }
  }

  if (!found) {
    fprintf(stderr, "Address 0x%llx not found in any dylib\n",
            (unsigned long long)target_addr);
  }

  munmap((void *)cache, cache_size);
  return found ? 0 : 1;
}
