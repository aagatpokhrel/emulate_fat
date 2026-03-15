#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "jdisk.h"

typedef struct {
    void *jd;
    unsigned int total_sectors;
    unsigned int s_sectors;
    unsigned int d_sectors;
    unsigned short **fat_cache;
    int *fat_dirty;
} FAT_System;

/* Helper to calculate S and D based on disk size */
void calculate_dimensions(unsigned long size, unsigned int *S, unsigned int *D) {
    unsigned int total = size / JDISK_SECTOR_SIZE;
    for (unsigned int d = total; d > 0; d--) {
        unsigned int s = ((d + 1) * 2 + JDISK_SECTOR_SIZE - 1) / JDISK_SECTOR_SIZE;
        if (s + d <= total) {
            *S = s;
            *D = d;
            return;
        }
    }
}

/* Reads a link from the FAT cache, loading from disk if necessary */
unsigned short Read_Link(FAT_System *fs, int index) {
    int sector_idx = (index * 2) / JDISK_SECTOR_SIZE;
    int offset = (index * 2) % JDISK_SECTOR_SIZE;

    if (fs->fat_cache[sector_idx] == NULL) {
        fs->fat_cache[sector_idx] = (unsigned short *)malloc(JDISK_SECTOR_SIZE);
        jdisk_read(fs->jd, sector_idx, fs->fat_cache[sector_idx]);
    }
    
    unsigned char *ptr = (unsigned char *)fs->fat_cache[sector_idx];
    return ptr[offset] | (ptr[offset + 1] << 8);
}

/* Writes a link to the cache and marks the sector as dirty if changed */
void Write_Link(FAT_System *fs, int index, unsigned short val) {
    int sector_idx = (index * 2) / JDISK_SECTOR_SIZE;
    int offset = (index * 2) % JDISK_SECTOR_SIZE;

    if (fs->fat_cache[sector_idx] == NULL) {
        fs->fat_cache[sector_idx] = (unsigned short *)malloc(JDISK_SECTOR_SIZE);
        jdisk_read(fs->jd, sector_idx, fs->fat_cache[sector_idx]);
    }

    unsigned char *ptr = (unsigned char *)fs->fat_cache[sector_idx];
    unsigned short old_val = ptr[offset] | (ptr[offset + 1] << 8);

    if (old_val != val) {
        ptr[offset] = val & 0xFF;
        ptr[offset + 1] = (val >> 8) & 0xFF;
        fs->fat_dirty[sector_idx] = 1;
    }
}

void Flush_Links(FAT_System *fs) {
    for (int i = 0; i < fs->s_sectors; i++) {
        if (fs->fat_dirty[i]) {
            jdisk_write(fs->jd, i, fs->fat_cache[i]);
            fs->fat_dirty[i] = 0;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: FATRW diskfile import input-file\n");
        fprintf(stderr, "       FATRW diskfile export starting-block output-file\n");
        exit(1);
    }

    FAT_System fs;
    fs.jd = jdisk_attach(argv[1]);
    if (!fs.jd) { perror(argv[1]); exit(1); }

    calculate_dimensions(jdisk_size(fs.jd), &fs.s_sectors, &fs.d_sectors);
    fs.fat_cache = (unsigned short **)calloc(fs.s_sectors, sizeof(unsigned short *));
    fs.fat_dirty = (int *)calloc(fs.s_sectors, sizeof(int));

    if (strcmp(argv[2], "import") == 0) {
        struct stat st;
        if (stat(argv[3], &st) != 0) { perror(argv[3]); exit(1); }
        
        unsigned int sectors_needed = (st.st_size + JDISK_SECTOR_SIZE - 1) / JDISK_SECTOR_SIZE;
        if (st.st_size == 0) sectors_needed = 1;

        // Count free sectors
        unsigned int free_count = 0;
        unsigned short curr = Read_Link(&fs, 0);
        while (curr != 0) {
            free_count++;
            curr = Read_Link(&fs, curr);
        }

        if (free_count < sectors_needed) {
            fprintf(stderr, "Not enough free sectors (%u) for %s, which needs %u.\n", free_count, argv[3], sectors_needed);
            exit(1);
        }

        FILE *fin = fopen(argv[3], "rb");
        unsigned char buffer[JDISK_SECTOR_SIZE];
        unsigned short first_block = Read_Link(&fs, 0);
        unsigned short prev_link_idx = 0;
        unsigned short curr_link_idx = first_block;

        for (unsigned int i = 0; i < sectors_needed; i++) {
            memset(buffer, 0, JDISK_SECTOR_SIZE);
            size_t bytes_read = fread(buffer, 1, JDISK_SECTOR_SIZE, fin);
            unsigned short next_link_idx = Read_Link(&fs, curr_link_idx);

            if (i == sectors_needed - 1) { // Last sector logic
                if (bytes_read == JDISK_SECTOR_SIZE) {
                    Write_Link(&fs, curr_link_idx, 0);
                } else if (bytes_read == 1023) {
                    buffer[1023] = 0xFF;
                    Write_Link(&fs, curr_link_idx, curr_link_idx);
                } else {
                    buffer[1022] = bytes_read & 0xFF;
                    buffer[1023] = (bytes_read >> 8) & 0xFF;
                    Write_Link(&fs, curr_link_idx, curr_link_idx);
                }
                Write_Link(&fs, prev_link_idx, next_link_idx); // Point free list head to next
            } else {
                Write_Link(&fs, prev_link_idx, next_link_idx); // Bypass this block in free list
            }

            jdisk_write(fs.jd, fs.s_sectors + curr_link_idx - 1, buffer);
            
            if (i < sectors_needed - 1) {
                unsigned short next_in_file = next_link_idx;
                Write_Link(&fs, curr_link_idx, next_in_file);
                curr_link_idx = next_in_file;
                prev_link_idx = 0; // Always pull from head of free list
            }
        }
        
        Flush_Links(&fs);
        fclose(fin);
        printf("New file starts at sector %u\n", fs.s_sectors + first_block - 1);

    } else if (strcmp(argv[2], "export") == 0) {
        unsigned int start_sector = atoi(argv[3]);
        FILE *fout = fopen(argv[4], "wb");
        unsigned char buffer[JDISK_SECTOR_SIZE];
        unsigned short curr_link_idx = start_sector - fs.s_sectors + 1;

        while (1) {
            jdisk_read(fs.jd, fs.s_sectors + curr_link_idx - 1, buffer);
            unsigned short next = Read_Link(&fs, curr_link_idx);

            if (next == 0) {
                fwrite(buffer, 1, JDISK_SECTOR_SIZE, fout);
                break;
            } else if (next == curr_link_idx) {
                int bytes;
                if (buffer[1023] == 0xFF) bytes = 1023;
                else bytes = (buffer[1023] << 8) | buffer[1022];
                fwrite(buffer, 1, bytes, fout);
                break;
            } else {
                fwrite(buffer, 1, JDISK_SECTOR_SIZE, fout);
                curr_link_idx = next;
            }
        }
        fclose(fout);
    }

    printf("Reads: %ld\n", jdisk_reads(fs.jd));
    printf("Writes: %ld\n", jdisk_writes(fs.jd));

    return 0;
}