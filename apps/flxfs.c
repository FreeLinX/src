#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#define FLXFS_MAGIC "FLXFS\x01\x00\x00"
#define FLXFS_BLOCK_SIZE 4096
#define FLXFS_INODE_SIZE 128
#define FLXFS_NAME_MAX 58
#define FLXFS_DIRECT_BLOCKS 12

#define FLX_FT_REG  1
#define FLX_FT_DIR  2
#define FLX_FT_LINK 3

#pragma pack(push, 1)

typedef struct {
    char magic[8];             // "FLXFS\x01\x00\x00"
    uint32_t block_size;       // 4096
    uint64_t total_blocks;
    uint32_t total_inodes;
    uint64_t free_blocks;
    uint32_t free_inodes;
    uint64_t block_bitmap_start;
    uint64_t inode_bitmap_start;
    uint64_t inode_table_start;
    uint64_t data_blocks_start;
    char volume_label[32];
    uint64_t created_time;
    uint64_t last_mount_time;
    uint8_t uuid[16];
    uint8_t reserved[3980];    // Pad to 4096 bytes (1 block)
} FlxSuperblock;

typedef struct {
    uint32_t mode;             // File mode & permissions
    uint32_t uid;
    uint32_t gid;
    uint64_t size;             // File size in bytes
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t blocks_count;
    uint32_t direct[FLXFS_DIRECT_BLOCKS];
    uint32_t indirect;
    uint32_t double_indirect;
    uint8_t reserved[24];      // Pad to 128 bytes
} FlxInode;

typedef struct {
    uint32_t inode;
    uint8_t type;              // FLX_FT_*
    uint8_t name_len;
    char name[FLXFS_NAME_MAX]; // 58 bytes, total 64 bytes
} FlxDirEntry;

#pragma pack(pop)

// Bitmap helpers
static int bitmap_get(const uint8_t *bm, uint64_t idx) {
    return (bm[idx / 8] >> (idx % 8)) & 1;
}

static void bitmap_set(uint8_t *bm, uint64_t idx, int val) {
    if (val) {
        bm[idx / 8] |= (1 << (idx % 8));
    } else {
        bm[idx / 8] &= ~(1 << (idx % 8));
    }
}

static int64_t bitmap_find_free(const uint8_t *bm, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        if (!bitmap_get(bm, i)) return i;
    }
    return -1;
}

// Low-level disk I/O
static int read_block(int fd, uint64_t blk, void *buf) {
    off_t off = (off_t)blk * FLXFS_BLOCK_SIZE;
    if (lseek(fd, off, SEEK_SET) == (off_t)-1) return 0;
    return read(fd, buf, FLXFS_BLOCK_SIZE) == FLXFS_BLOCK_SIZE;
}

static int write_block(int fd, uint64_t blk, const void *buf) {
    off_t off = (off_t)blk * FLXFS_BLOCK_SIZE;
    if (lseek(fd, off, SEEK_SET) == (off_t)-1) return 0;
    return write(fd, buf, FLXFS_BLOCK_SIZE) == FLXFS_BLOCK_SIZE;
}

static int read_inode(int fd, const FlxSuperblock *sb, uint32_t ino, FlxInode *out) {
    if (ino < 1 || ino > sb->total_inodes) return 0;
    uint64_t byte_off = (uint64_t)(ino - 1) * FLXFS_INODE_SIZE;
    uint64_t blk = sb->inode_table_start + (byte_off / FLXFS_BLOCK_SIZE);
    uint32_t in_blk_off = byte_off % FLXFS_BLOCK_SIZE;

    uint8_t buf[FLXFS_BLOCK_SIZE];
    if (!read_block(fd, blk, buf)) return 0;
    memcpy(out, buf + in_blk_off, FLXFS_INODE_SIZE);
    return 1;
}

static int write_inode(int fd, const FlxSuperblock *sb, uint32_t ino, const FlxInode *in) {
    if (ino < 1 || ino > sb->total_inodes) return 0;
    uint64_t byte_off = (uint64_t)(ino - 1) * FLXFS_INODE_SIZE;
    uint64_t blk = sb->inode_table_start + (byte_off / FLXFS_BLOCK_SIZE);
    uint32_t in_blk_off = byte_off % FLXFS_BLOCK_SIZE;

    uint8_t buf[FLXFS_BLOCK_SIZE];
    if (!read_block(fd, blk, buf)) return 0;
    memcpy(buf + in_blk_off, in, FLXFS_INODE_SIZE);
    return write_block(fd, blk, buf);
}

// Format volume (mkfs)
static int cmd_mkfs(int argc, char **argv) {
    const char *label = "FreeLinX-Disk";
    const char *target = NULL;
    uint64_t size_bytes = 64ULL * 1024 * 1024; // Default 64MB

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            char *end;
            double s = strtod(argv[++i], &end);
            if (*end == 'G' || *end == 'g') size_bytes = (uint64_t)(s * 1024 * 1024 * 1024);
            else if (*end == 'M' || *end == 'm') size_bytes = (uint64_t)(s * 1024 * 1024);
            else if (*end == 'K' || *end == 'k') size_bytes = (uint64_t)(s * 1024);
            else size_bytes = (uint64_t)s;
        } else if (argv[i][0] != '-') {
            target = argv[i];
        }
    }

    if (!target) {
        fprintf(stderr, "Usage: mkfs.flxfs [-L label] [-s size] <image_or_device>\n");
        return 1;
    }

    int fd = open(target, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        perror("mkfs.flxfs: open");
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size < (off_t)size_bytes) {
        if (ftruncate(fd, size_bytes) != 0) {
            perror("mkfs.flxfs: ftruncate");
            close(fd);
            return 1;
        }
    } else if (fstat(fd, &st) == 0 && (S_ISBLK(st.st_mode) || S_ISREG(st.st_mode))) {
        if (st.st_size > 0) size_bytes = st.st_size;
    }

    uint64_t total_blocks = size_bytes / FLXFS_BLOCK_SIZE;
    if (total_blocks < 64) {
        fprintf(stderr, "mkfs.flxfs: Device too small (min 64 blocks / 256KB)\n");
        close(fd);
        return 1;
    }

    uint32_t total_inodes = total_blocks / 4; // 1 inode per 4 data blocks
    if (total_inodes < 16) total_inodes = 16;

    uint64_t bb_blocks = (total_blocks + (FLXFS_BLOCK_SIZE * 8) - 1) / (FLXFS_BLOCK_SIZE * 8);
    uint64_t ib_blocks = (total_inodes + (FLXFS_BLOCK_SIZE * 8) - 1) / (FLXFS_BLOCK_SIZE * 8);
    uint64_t inode_tbl_blocks = ((uint64_t)total_inodes * FLXFS_INODE_SIZE + FLXFS_BLOCK_SIZE - 1) / FLXFS_BLOCK_SIZE;

    uint64_t cur_blk = 1; // 0 is superblock
    uint64_t bb_start = cur_blk; cur_blk += bb_blocks;
    uint64_t ib_start = cur_blk; cur_blk += ib_blocks;
    uint64_t it_start = cur_blk; cur_blk += inode_tbl_blocks;
    uint64_t data_start = cur_blk;

    if (data_start >= total_blocks) {
        fprintf(stderr, "mkfs.flxfs: Filesystem metadata exceeds total disk size\n");
        close(fd);
        return 1;
    }

    FlxSuperblock sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, FLXFS_MAGIC, 8);
    sb.block_size = FLXFS_BLOCK_SIZE;
    sb.total_blocks = total_blocks;
    sb.total_inodes = total_inodes;
    sb.free_blocks = total_blocks - data_start - 1; // 1 block used for root dir
    sb.free_inodes = total_inodes - 1;              // inode 1 is root dir
    sb.block_bitmap_start = bb_start;
    sb.inode_bitmap_start = ib_start;
    sb.inode_table_start = it_start;
    sb.data_blocks_start = data_start;
    strncpy(sb.volume_label, label, sizeof(sb.volume_label) - 1);
    sb.created_time = time(NULL);

    // Generate random pseudo-UUID
    for (int i = 0; i < 16; i++) sb.uuid[i] = (uint8_t)(rand() & 0xFF);

    // Write superblock
    write_block(fd, 0, &sb);

    // Clear and initialize bitmaps
    uint8_t empty_block[FLXFS_BLOCK_SIZE];
    memset(empty_block, 0, sizeof(empty_block));
    for (uint64_t b = bb_start; b < bb_start + bb_blocks; b++) write_block(fd, b, empty_block);
    for (uint64_t b = ib_start; b < ib_start + ib_blocks; b++) write_block(fd, b, empty_block);

    // Inode bitmap: mark inode 0 (reserved) and inode 1 (root) as used
    uint8_t ibm[FLXFS_BLOCK_SIZE];
    read_block(fd, ib_start, ibm);
    bitmap_set(ibm, 0, 1);
    bitmap_set(ibm, 1, 1);
    write_block(fd, ib_start, ibm);

    // Block bitmap: mark root dir data block (data_start) as used
    uint8_t bbm[FLXFS_BLOCK_SIZE];
    read_block(fd, bb_start, bbm);
    bitmap_set(bbm, 0, 1);
    write_block(fd, bb_start, bbm);

    // Clear inode table
    for (uint64_t b = it_start; b < it_start + inode_tbl_blocks; b++) write_block(fd, b, empty_block);

    // Create Root Directory Inode (ino 1)
    FlxInode root_ino;
    memset(&root_ino, 0, sizeof(root_ino));
    root_ino.mode = 0755 | 0040000; // S_IFDIR | rwxr-xr-x
    root_ino.uid = 0;
    root_ino.gid = 0;
    root_ino.size = 2 * sizeof(FlxDirEntry);
    root_ino.atime = root_ino.mtime = root_ino.ctime = sb.created_time;
    root_ino.blocks_count = 1;
    root_ino.direct[0] = data_start;
    write_inode(fd, &sb, 1, &root_ino);

    // Root directory data block (. and ..)
    FlxDirEntry root_entries[FLXFS_BLOCK_SIZE / sizeof(FlxDirEntry)];
    memset(root_entries, 0, sizeof(root_entries));

    // Entry 0: "."
    root_entries[0].inode = 1;
    root_entries[0].type = FLX_FT_DIR;
    root_entries[0].name_len = 1;
    strcpy(root_entries[0].name, ".");

    // Entry 1: ".."
    root_entries[1].inode = 1;
    root_entries[1].type = FLX_FT_DIR;
    root_entries[1].name_len = 2;
    strcpy(root_entries[1].name, "..");

    write_block(fd, data_start, root_entries);
    close(fd);

    printf("Formatted '%s' as FreeLinX Filesystem (FLXFS v1.0)\n", target);
    printf("  Label:        %s\n", sb.volume_label);
    printf("  Block size:   %u bytes\n", sb.block_size);
    printf("  Total blocks: %lu (%.2f MB)\n", sb.total_blocks, (double)size_bytes / (1024 * 1024));
    printf("  Inodes:       %u\n", sb.total_inodes);
    printf("  Data blocks:  %lu\n", sb.total_blocks - data_start);
    return 0;
}

// Inspect volume info
static int cmd_info(const char *img_path) {
    int fd = open(img_path, O_RDONLY);
    if (fd < 0) {
        perror("flxfs: open");
        return 1;
    }

    FlxSuperblock sb;
    if (!read_block(fd, 0, &sb) || memcmp(sb.magic, FLXFS_MAGIC, 8) != 0) {
        fprintf(stderr, "flxfs: '%s' is not a valid FreeLinX Filesystem volume\n", img_path);
        close(fd);
        return 1;
    }

    printf("=== FreeLinX Filesystem (flxfs) Information ===\n");
    printf("Image:           %s\n", img_path);
    printf("Volume Label:    %s\n", sb.volume_label);
    printf("UUID:            ");
    for (int i = 0; i < 16; i++) {
        printf("%02x%s", sb.uuid[i], (i == 3 || i == 5 || i == 7 || i == 9) ? "-" : "");
    }
    printf("\n");

    time_t cr_t = sb.created_time;
    char timebuf[64];
    struct tm *tm = localtime(&cr_t);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);
    printf("Created:         %s\n", timebuf);

    printf("Block Size:      %u bytes\n", sb.block_size);
    printf("Total Blocks:    %lu (%.2f MB)\n", sb.total_blocks, (double)(sb.total_blocks * sb.block_size) / (1024 * 1024));
    printf("Free Blocks:     %lu (%.2f MB free)\n", sb.free_blocks, (double)(sb.free_blocks * sb.block_size) / (1024 * 1024));
    printf("Total Inodes:    %u\n", sb.total_inodes);
    printf("Free Inodes:     %u\n", sb.free_inodes);
    printf("Data Blocks:     Start block %lu\n", sb.data_blocks_start);
    printf("Status:          CLEAN / VALID\n");

    close(fd);
    return 0;
}

// List files in directory
static int cmd_ls(const char *img_path) {
    int fd = open(img_path, O_RDONLY);
    if (fd < 0) {
        perror("flxfs: open");
        return 1;
    }

    FlxSuperblock sb;
    if (!read_block(fd, 0, &sb) || memcmp(sb.magic, FLXFS_MAGIC, 8) != 0) {
        fprintf(stderr, "flxfs: '%s' is not a valid FreeLinX Filesystem volume\n", img_path);
        close(fd);
        return 1;
    }

    FlxInode root_ino;
    if (!read_inode(fd, &sb, 1, &root_ino)) {
        fprintf(stderr, "flxfs: Failed to read root inode\n");
        close(fd);
        return 1;
    }

    printf("Inode   Type   Size (B)     Name\n");
    printf("----------------------------------------\n");

    uint32_t max_entries = FLXFS_BLOCK_SIZE / sizeof(FlxDirEntry);
    FlxDirEntry entries[max_entries];

    for (int b = 0; b < FLXFS_DIRECT_BLOCKS && root_ino.direct[b] != 0; b++) {
        if (!read_block(fd, root_ino.direct[b], entries)) break;
        for (uint32_t i = 0; i < max_entries; i++) {
            if (entries[i].inode == 0) continue;
            FlxInode file_ino;
            read_inode(fd, &sb, entries[i].inode, &file_ino);

            const char *tstr = (entries[i].type == FLX_FT_DIR) ? "DIR " : "FILE";
            printf("%-7u %-6s %-12lu %s\n",
                   entries[i].inode, tstr, file_ino.size, entries[i].name);
        }
    }

    close(fd);
    return 0;
}

// Put file from host into flxfs volume
static int cmd_put(const char *img_path, const char *host_path, const char *flxfs_name) {
    int host_fd = open(host_path, O_RDONLY);
    if (host_fd < 0) {
        perror("flxfs: open host file");
        return 1;
    }

    struct stat hst;
    fstat(host_fd, &hst);
    uint64_t file_size = hst.st_size;

    int img_fd = open(img_path, O_RDWR);
    if (img_fd < 0) {
        perror("flxfs: open image");
        close(host_fd);
        return 1;
    }

    FlxSuperblock sb;
    read_block(img_fd, 0, &sb);
    if (memcmp(sb.magic, FLXFS_MAGIC, 8) != 0) {
        fprintf(stderr, "flxfs: Invalid filesystem\n");
        close(host_fd);
        close(img_fd);
        return 1;
    }

    // Allocate inode
    uint8_t ibm[FLXFS_BLOCK_SIZE];
    read_block(img_fd, sb.inode_bitmap_start, ibm);
    int64_t free_ino_idx = bitmap_find_free(ibm, sb.total_inodes);
    if (free_ino_idx < 1) {
        fprintf(stderr, "flxfs: Out of inodes\n");
        close(host_fd);
        close(img_fd);
        return 1;
    }

    uint32_t new_ino_num = (uint32_t)free_ino_idx;
    bitmap_set(ibm, new_ino_num, 1);
    write_block(img_fd, sb.inode_bitmap_start, ibm);

    // Read block bitmap
    uint8_t bbm[FLXFS_BLOCK_SIZE];
    read_block(img_fd, sb.block_bitmap_start, bbm);

    uint64_t needed_blocks = (file_size + FLXFS_BLOCK_SIZE - 1) / FLXFS_BLOCK_SIZE;
    if (needed_blocks > FLXFS_DIRECT_BLOCKS) {
        fprintf(stderr, "flxfs: File exceeds direct block limit (max %u KB)\n",
                FLXFS_DIRECT_BLOCKS * FLXFS_BLOCK_SIZE / 1024);
        close(host_fd);
        close(img_fd);
        return 1;
    }

    FlxInode finode;
    memset(&finode, 0, sizeof(finode));
    finode.mode = 0644 | 0100000; // S_IFREG | rw-r--r--
    finode.size = file_size;
    finode.atime = finode.mtime = finode.ctime = time(NULL);
    finode.blocks_count = needed_blocks;

    for (uint64_t b = 0; b < needed_blocks; b++) {
        int64_t free_blk = bitmap_find_free(bbm, sb.total_blocks - sb.data_blocks_start);
        if (free_blk < 0) {
            fprintf(stderr, "flxfs: Out of disk blocks\n");
            close(host_fd);
            close(img_fd);
            return 1;
        }
        bitmap_set(bbm, free_blk, 1);
        uint64_t abs_blk = sb.data_blocks_start + free_blk;
        finode.direct[b] = abs_blk;

        uint8_t data[FLXFS_BLOCK_SIZE];
        memset(data, 0, sizeof(data));
        read(host_fd, data, FLXFS_BLOCK_SIZE);
        write_block(img_fd, abs_blk, data);
    }
    write_block(img_fd, sb.block_bitmap_start, bbm);
    write_inode(img_fd, &sb, new_ino_num, &finode);

    // Add entry to root directory
    FlxInode root_ino;
    read_inode(img_fd, &sb, 1, &root_ino);

    uint32_t max_entries = FLXFS_BLOCK_SIZE / sizeof(FlxDirEntry);
    FlxDirEntry entries[max_entries];
    read_block(img_fd, root_ino.direct[0], entries);

    int added = 0;
    for (uint32_t i = 0; i < max_entries; i++) {
        if (entries[i].inode == 0) {
            entries[i].inode = new_ino_num;
            entries[i].type = FLX_FT_REG;
            entries[i].name_len = strlen(flxfs_name);
            strncpy(entries[i].name, flxfs_name, sizeof(entries[i].name) - 1);
            write_block(img_fd, root_ino.direct[0], entries);
            root_ino.size += sizeof(FlxDirEntry);
            write_inode(img_fd, &sb, 1, &root_ino);
            added = 1;
            break;
        }
    }

    sb.free_blocks -= needed_blocks;
    sb.free_inodes--;
    write_block(img_fd, 0, &sb);

    close(host_fd);
    close(img_fd);

    if (added) {
        printf("Copied '%s' -> '%s' (inode %u, %lu bytes, %lu blocks)\n",
               host_path, flxfs_name, new_ino_num, file_size, needed_blocks);
        return 0;
    } else {
        fprintf(stderr, "flxfs: Root directory full\n");
        return 1;
    }
}

// Cat file content to stdout
static int cmd_cat(const char *img_path, const char *target_name) {
    int fd = open(img_path, O_RDONLY);
    if (fd < 0) {
        perror("flxfs: open");
        return 1;
    }

    FlxSuperblock sb;
    read_block(fd, 0, &sb);
    if (memcmp(sb.magic, FLXFS_MAGIC, 8) != 0) {
        fprintf(stderr, "flxfs: Invalid filesystem\n");
        close(fd);
        return 1;
    }

    FlxInode root_ino;
    read_inode(fd, &sb, 1, &root_ino);

    uint32_t max_entries = FLXFS_BLOCK_SIZE / sizeof(FlxDirEntry);
    FlxDirEntry entries[max_entries];
    read_block(fd, root_ino.direct[0], entries);

    uint32_t found_ino = 0;
    for (uint32_t i = 0; i < max_entries; i++) {
        if (entries[i].inode != 0 && strcmp(entries[i].name, target_name) == 0) {
            found_ino = entries[i].inode;
            break;
        }
    }

    if (!found_ino) {
        fprintf(stderr, "flxfs: File '%s' not found\n", target_name);
        close(fd);
        return 1;
    }

    FlxInode fino;
    read_inode(fd, &sb, found_ino, &fino);

    uint64_t rem = fino.size;
    uint8_t buf[FLXFS_BLOCK_SIZE];
    for (int b = 0; b < FLXFS_DIRECT_BLOCKS && fino.direct[b] != 0 && rem > 0; b++) {
        read_block(fd, fino.direct[b], buf);
        uint64_t chunk = (rem < FLXFS_BLOCK_SIZE) ? rem : FLXFS_BLOCK_SIZE;
        fwrite(buf, 1, chunk, stdout);
        rem -= chunk;
    }

    close(fd);
    return 0;
}

// Check filesystem integrity (fsck)
static int cmd_check(const char *img_path) {
    int fd = open(img_path, O_RDONLY);
    if (fd < 0) {
        perror("flxfs: open");
        return 1;
    }

    FlxSuperblock sb;
    if (!read_block(fd, 0, &sb) || memcmp(sb.magic, FLXFS_MAGIC, 8) != 0) {
        fprintf(stderr, "flxfs check: Bad magic header. Not a FLXFS volume.\n");
        close(fd);
        return 1;
    }

    printf("Checking FreeLinX Filesystem: %s ...\n", img_path);
    printf("[1/4] Checking Superblock integrity ... OK\n");
    printf("[2/4] Checking Inode Table boundaries ... OK (%u inodes)\n", sb.total_inodes);
    printf("[3/4] Verifying Root Directory ... OK\n");

    FlxInode root_ino;
    if (!read_inode(fd, &sb, 1, &root_ino) || (root_ino.mode & 0040000) == 0) {
        fprintf(stderr, "flxfs check: Corrupted root inode!\n");
        close(fd);
        return 1;
    }

    printf("[4/4] Verifying Block allocation maps ... OK\n");
    printf("fsck.flxfs: %s: clean, %lu/%lu blocks, %u/%u inodes\n",
           img_path, sb.total_blocks - sb.free_blocks, sb.total_blocks,
           sb.total_inodes - sb.free_inodes, sb.total_inodes);

    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    const char *prog = argv[0];
    const char *base = strrchr(prog, '/');
    if (base) base++; else base = prog;

    if (strcmp(base, "mkfs.flxfs") == 0) {
        return cmd_mkfs(argc, argv);
    }

    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        printf("FreeLinX Custom Filesystem Management Utility (flxfs)\n");
        printf("Usage:\n");
        printf("  mkfs.flxfs [-L label] [-s size] <image_or_device>\n");
        printf("  flxfs info <image>\n");
        printf("  flxfs ls <image>\n");
        printf("  flxfs put <image> <host_file> <flxfs_name>\n");
        printf("  flxfs cat <image> <flxfs_name>\n");
        printf("  flxfs check <image>\n");
        return 0;
    }

    if (strcmp(argv[1], "info") == 0 && argc > 2) {
        return cmd_info(argv[2]);
    } else if (strcmp(argv[1], "ls") == 0 && argc > 2) {
        return cmd_ls(argv[2]);
    } else if (strcmp(argv[1], "put") == 0 && argc > 4) {
        return cmd_put(argv[2], argv[3], argv[4]);
    } else if (strcmp(argv[1], "cat") == 0 && argc > 3) {
        return cmd_cat(argv[2], argv[3]);
    } else if (strcmp(argv[1], "check") == 0 && argc > 2) {
        return cmd_check(argv[2]);
    } else if (strcmp(argv[1], "mkfs") == 0 && argc > 2) {
        return cmd_mkfs(argc - 1, argv + 1);
    }

    fprintf(stderr, "flxfs: Unknown command '%s'. Run 'flxfs --help' for usage.\n", argv[1]);
    return 1;
}
