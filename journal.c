/*
 * Course: CSE321 - Operating Systems
 * Term Project: Metadata Journaling System (VSFS)
 * Group Number: 11
 *
 * Team Members:
 * 1. Md.Al-Raiyan       (ID: 22201562)
 * 2. Adnan Jaidy        (ID: 21201101)
 * 3. Tanvir Ahmed Tamim (ID: 21201623)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdbool.h>

#define VSFS_MAGIC_NUM 0x56534653U
#define LOG_MAGIC_VAL 0x4A524E4C
#define BYTES_PER_BLK 4096U
#define LOG_START_BLK_IDX 1U
#define LOG_CAPACITY_BLKS 16U

#define ENTRY_TYPE_META 1
#define ENTRY_TYPE_SEAL 2

struct superblock {
    uint32_t magic; uint32_t block_size; uint32_t total_blocks; uint32_t inode_count;
    uint32_t journal_block; uint32_t inode_bitmap; uint32_t data_bitmap;
    uint32_t inode_start; uint32_t data_start;
    uint8_t  _pad[128 - 9 * 4];
};

struct inode {
    uint16_t type; uint16_t links; uint32_t size;
    uint32_t direct[8]; uint32_t ctime; uint32_t mtime;
    uint8_t _pad[128 - (2 + 2 + 4 + 8 * 4 + 4 + 4)];
};

struct dirent {
    uint32_t inode; char name[28];
};

struct log_state_block {
    uint32_t verify_id;
    uint32_t active_bytes;
    uint8_t  align_pad[BYTES_PER_BLK - 8]; 
};

struct log_entry_head {
    uint32_t kind; 
    uint32_t span; 
};

struct meta_wrapper {
    struct log_entry_head header;
    uint32_t target_disk_idx;
};

void pull_disk_chunk(int disk_handle, uint32_t idx, void *dest) {
    off_t pos = (off_t)idx * BYTES_PER_BLK;
    if (pread(disk_handle, dest, BYTES_PER_BLK, pos) != BYTES_PER_BLK) {
        perror("IO Read Error");
        exit(1);
    }
}

void push_disk_chunk(int disk_handle, uint32_t idx, void *src) {
    off_t pos = (off_t)idx * BYTES_PER_BLK;
    if (pwrite(disk_handle, src, BYTES_PER_BLK, pos) != BYTES_PER_BLK) {
        perror("IO Write Error");
        exit(1);
    }
}

int get_unused_id_idx(uint8_t *map_ptr, uint32_t limit) {
    for (uint32_t k = 1; k < limit; k++) {
        uint8_t octet = map_ptr[k / 8];
        int status = (octet >> (k % 8)) & 1;
        if (status == 0) return k;
    }
    return -1;
}

int get_open_dir_idx(struct dirent *dir_table) {
    int capacity = BYTES_PER_BLK / sizeof(struct dirent);
    int j = 0;
    while (j < capacity) {
        if (dir_table[j].name[0] == '\0') return j;
        j++;
    }
    return -1;
}

void process_new_file(int disk_handle, char *target_name) {
    struct superblock fs_head;
    pull_disk_chunk(disk_handle, 0, &fs_head);

    uint8_t bmap_cache[BYTES_PER_BLK];
    uint8_t inode_tbl_root[BYTES_PER_BLK]; // Buffer for Block 19 (Root)
    uint8_t inode_tbl_target[BYTES_PER_BLK]; // Buffer for Target Inode
    uint8_t root_cache[BYTES_PER_BLK];

    pull_disk_chunk(disk_handle, fs_head.inode_bitmap, bmap_cache);
    pull_disk_chunk(disk_handle, fs_head.inode_start, inode_tbl_root);
    pull_disk_chunk(disk_handle, fs_head.data_start, root_cache);

    int chosen_id = get_unused_id_idx(bmap_cache, fs_head.inode_count);
    if (chosen_id < 0) {
        printf("Error: ID limit reached.\n");
        return;
    }

    struct dirent *dir_arr = (struct dirent *)root_cache;
    int chosen_slot = get_open_dir_idx(dir_arr);
    if (chosen_slot < 0) {
        printf("Error: Dir full.\n");
        return;
    }

    // --- Block Calculation ---
    uint32_t inodes_per_blk = BYTES_PER_BLK / sizeof(struct inode);
    uint32_t target_blk_offset = chosen_id / inodes_per_blk;
    uint32_t target_blk_idx = fs_head.inode_start + target_blk_offset;
    
    // Check if new inode is in a different block than Root (Inode 0)
    bool is_split_blocks = (target_blk_idx != fs_head.inode_start);
    
    if (is_split_blocks) {
        pull_disk_chunk(disk_handle, target_blk_idx, inode_tbl_target);
    } else {
        // If same block, initialize target buffer with current state of root block
        memcpy(inode_tbl_target, inode_tbl_root, BYTES_PER_BLK);
    }

    // --- Metadata Updates ---
    
    // 1. Update Bitmap
    bmap_cache[chosen_id / 8] |= (1 << (chosen_id % 8));

    // 2. Initialize New Inode (in target buffer)
    struct inode *node_arr = (struct inode *)inode_tbl_target;
    struct inode *fresh_node = &node_arr[chosen_id % inodes_per_blk];
    memset(fresh_node, 0, sizeof(struct inode));
    fresh_node->type = 1; 
    fresh_node->links = 1;
    fresh_node->ctime = fresh_node->mtime = time(NULL);

    // 3. Update Root Inode Size (in root buffer)
    struct inode *root_node_arr = (struct inode *)inode_tbl_root;
    struct inode *root_node = &root_node_arr[0];
    uint32_t req_sz = (chosen_slot + 1) * sizeof(struct dirent);
    if (root_node->size < req_sz) {
        root_node->size = req_sz;
    }
    root_node->mtime = time(NULL);

    // CRITICAL FIX: Merge buffers if they are the same block
    if (!is_split_blocks) {
        // Copy the updated Root Inode from root buffer to target buffer
        // So target buffer now has BOTH the new file info AND the root size update
        memcpy(node_arr, root_node, sizeof(struct inode));
    }

    // 4. Update Directory Entry
    dir_arr[chosen_slot].inode = chosen_id;
    strncpy(dir_arr[chosen_slot].name, target_name, 27);
    dir_arr[chosen_slot].name[27] = '\0';

    // --- Journaling ---
    struct log_state_block log_trk;
    pull_disk_chunk(disk_handle, LOG_START_BLK_IDX, &log_trk);

    if (log_trk.verify_id != LOG_MAGIC_VAL) {
        log_trk.verify_id = LOG_MAGIC_VAL;
        log_trk.active_bytes = sizeof(struct log_state_block);
    }

    int num_meta_blks = is_split_blocks ? 4 : 3;
    uint32_t size_needed = num_meta_blks * (sizeof(struct meta_wrapper) + BYTES_PER_BLK) + sizeof(struct log_entry_head);
    
    if (log_trk.active_bytes + size_needed > LOG_CAPACITY_BLKS * BYTES_PER_BLK) {
        printf("Log space exhausted. Run install.\n");
        return;
    }

    off_t write_ptr = (LOG_START_BLK_IDX * BYTES_PER_BLK) + log_trk.active_bytes;

    uint32_t blk_indices[4];
    void *mem_ptrs[4];
    
    // Order: Bitmap -> Inode Block(s) -> Directory
    blk_indices[0] = fs_head.inode_bitmap; 
    mem_ptrs[0] = bmap_cache;
    
    int current_idx = 1;

    if (is_split_blocks) {
        // Write Block 19 (Root) AND Block 20+ (Target)
        blk_indices[current_idx] = fs_head.inode_start;
        mem_ptrs[current_idx] = inode_tbl_root;
        current_idx++;
        
        blk_indices[current_idx] = target_blk_idx;
        mem_ptrs[current_idx] = inode_tbl_target;
        current_idx++;
    } else {
        // Write only Block 19 (which now contains merged updates)
        blk_indices[current_idx] = fs_head.inode_start;
        mem_ptrs[current_idx] = inode_tbl_target; // Use the merged buffer!
        current_idx++;
    }
    
    blk_indices[current_idx] = fs_head.data_start; 
    mem_ptrs[current_idx] = root_cache;
    
    for (int m = 0; m <= current_idx; m++) {
        struct meta_wrapper meta;
        meta.header.kind = ENTRY_TYPE_META;
        meta.header.span = sizeof(struct meta_wrapper) + BYTES_PER_BLK;
        meta.target_disk_idx = blk_indices[m];

        pwrite(disk_handle, &meta, sizeof(meta), write_ptr);
        write_ptr += sizeof(meta);

        pwrite(disk_handle, mem_ptrs[m], BYTES_PER_BLK, write_ptr);
        write_ptr += BYTES_PER_BLK;
    }

    struct log_entry_head seal_rec;
    seal_rec.kind = ENTRY_TYPE_SEAL;
    seal_rec.span = sizeof(seal_rec);
    pwrite(disk_handle, &seal_rec, sizeof(seal_rec), write_ptr);
    write_ptr += sizeof(seal_rec);

    log_trk.active_bytes = write_ptr - (LOG_START_BLK_IDX * BYTES_PER_BLK);
    push_disk_chunk(disk_handle, LOG_START_BLK_IDX, &log_trk);
    fsync(disk_handle);

    printf("Entry '%s' queued (ID %d).\n", target_name, chosen_id);
}

void process_sync(int disk_handle) {
    struct log_state_block log_trk;
    pull_disk_chunk(disk_handle, LOG_START_BLK_IDX, &log_trk);

    if (log_trk.verify_id != LOG_MAGIC_VAL || log_trk.active_bytes <= sizeof(log_trk)) {
        printf("Log clean.\n");
        return;
    }

    uint32_t current_ptr = sizeof(log_trk);
    uint32_t base_addr = LOG_START_BLK_IDX * BYTES_PER_BLK;

    while (current_ptr < log_trk.active_bytes) {
        uint32_t lookahead_ptr = current_ptr;
        bool valid_seal = false;

        while (lookahead_ptr < log_trk.active_bytes) {
            struct log_entry_head head;
            if (pread(disk_handle, &head, sizeof(head), base_addr + lookahead_ptr) != sizeof(head)) break;
            
            if (head.span == 0) return; 

            if (head.kind == ENTRY_TYPE_SEAL) {
                valid_seal = true;
                lookahead_ptr += head.span;
                break;
            }
            lookahead_ptr += head.span;
        }

        if (!valid_seal) break; 

        while (current_ptr < lookahead_ptr) {
            struct log_entry_head head;
            pread(disk_handle, &head, sizeof(head), base_addr + current_ptr);

            if (head.kind == ENTRY_TYPE_META) {
                struct meta_wrapper d_wrap;
                pread(disk_handle, &d_wrap, sizeof(d_wrap), base_addr + current_ptr);
                
                uint8_t payload[BYTES_PER_BLK];
                pread(disk_handle, payload, BYTES_PER_BLK, base_addr + current_ptr + sizeof(d_wrap));
                
                push_disk_chunk(disk_handle, d_wrap.target_disk_idx, payload);
            }
            current_ptr += head.span;
        }
    }

    log_trk.active_bytes = sizeof(log_trk);
    push_disk_chunk(disk_handle, LOG_START_BLK_IDX, &log_trk);
    printf("Log committed to storage.\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;

    int disk_handle = open("vsfs.img", O_RDWR);
    if (disk_handle < 0) return 1;

    if (strcmp(argv[1], "create") == 0) {
        if (argc < 3) return 1;
        process_new_file(disk_handle, argv[2]);
    } else if (strcmp(argv[1], "install") == 0) {
        process_sync(disk_handle);
    }

    close(disk_handle);
    return 0;
}