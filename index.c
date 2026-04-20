// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
int hash_file(const char *path, ObjectID *hash);
// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.

int index_load(Index *index) {
    index->count = 0;

    FILE *fp = fopen(".pes/index", "r");
    if (!fp) {
        return 0; // Fresh repo, return success with empty index
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned int mode = 0;
        char hash_str[65] = {0};
        unsigned long long mtime = 0;
        unsigned int size = 0;
        char path[512] = {0};

        // Expect: mode hash mtime size path
        int items = sscanf(line, "%o %64s %llu %u %511s", &mode, hash_str, &mtime, &size, path);
        if (items != 5) continue; // skip malformed lines

        index->entries[index->count].mode = mode;
        hex_to_hash(hash_str, &index->entries[index->count].hash);
        index->entries[index->count].mtime_sec = (uint64_t)mtime;
        index->entries[index->count].size = (uint32_t)size;
        strncpy(index->entries[index->count].path, path, sizeof(index->entries[index->count].path) - 1);
        index->entries[index->count].path[sizeof(index->entries[index->count].path) - 1] = '\0';
        index->count++;
        if (index->count >= MAX_INDEX_ENTRIES) break;
    }

    fclose(fp);
    return 0;
}

// Ensure this helper function is outside of other functions
int compare_entries(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->path, eb->path);
}// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    // 1. Sort the entries by path
    qsort((void *)index->entries, index->count, sizeof(IndexEntry), compare_entries);

    // 2. Open a temporary file for writing
    FILE *fp = fopen(".pes/index.tmp", "w");
    if (!fp) return -1;

    // 3. Loop through entries and write them to the temp file
    for (int i = 0; i < index->count; i++) {
        char hash_str[65];
        hash_to_hex(&index->entries[i].hash, hash_str);
        // Write: mode hash mtime size path
        fprintf(fp, "%o %s %llu %u %s\n",
                index->entries[i].mode,
                hash_str,
                (unsigned long long)index->entries[i].mtime_sec,
                (unsigned int)index->entries[i].size,
                index->entries[i].path);
    }

    // 4. Flush and sync to disk
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    // 5. Atomically rename the temp file to the actual index
    if (rename(".pes/index.tmp", ".pes/index") != 0) {
        return -1;
    }

    return 0;
}

// Stage a file for the next commit.
//
// HINTS - Useful functions and syscalls:
//   - fopen, fread, fclose             : reading the target file's contents
//   - object_write                     : saving the contents as OBJ_BLOB
//   - stat / lstat                     : getting file metadata (size, mtime, mode)
//   - index_find                       : checking if the file is already staged
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    struct stat st;
    // Get metadata for the file
    if (stat(path, &st) != 0) return -1; 

    // Assuming you have a hash_file function elsewhere in your project
    ObjectID hash;
    if (hash_file(path, &hash) != 0) return -1;

    // Check if the file is already in the index
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            // Update existing entry
            index->entries[i].mode = st.st_mode;
            index->entries[i].hash = hash;
            index->entries[i].mtime_sec = st.st_mtime;
            index->entries[i].size = st.st_size;
            return index_save(index);
        }
    }

    // Add new entry if space exists
    if (index->count >= MAX_INDEX_ENTRIES) return -1;

    IndexEntry *e = &index->entries[index->count];
    e->mode = st.st_mode;
    e->hash = hash;
    e->mtime_sec = st.st_mtime;
    e->size = st.st_size;
    strncpy(e->path, path, 511);
    index->count++;

    return index_save(index);
}
