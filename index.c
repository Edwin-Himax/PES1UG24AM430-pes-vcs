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
// what will go into the next commit) and ATOMIC WRITES (temp+rename),
// not on binary format gymnastics.
//
// PROVIDED functions: index_find, index_remove
// TODO functions:     index_load, index_save, index_add, index_status

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <inttypes.h>
#include "tree.h"
#include "commit.h"

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

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// Steps:
//   1. If .pes/index does not exist, set index->count = 0 and return 0
//   2. Open the file for reading (fopen with "r")
//   3. For each line, parse the fields:
//      - Use fscanf or sscanf to read: mode, hex-hash, mtime, size, path
//      - Convert the 64-char hex hash to an ObjectID using hex_to_hash()
//   4. Populate index->entries and index->count
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (index->count >= MAX_INDEX_ENTRIES) break;
        
        char hex[100];
        char path[512];
        unsigned int mode;
        uint64_t mtime;
        unsigned int size;
        
        if (sscanf(line, "%o %64s %" SCNu64 " %u %[^\n]", &mode, hex, &mtime, &size, path) == 5) {
            index->entries[index->count].mode = mode;
            index->entries[index->count].mtime_sec = mtime;
            index->entries[index->count].size = size;
            strcpy(index->entries[index->count].path, path);
            hex_to_hash(hex, &index->entries[index->count].hash);
            index->count++;
        }
    }
    
    fclose(f);
    return 0;
}

static int compare_index_entries(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->path, eb->path);
}

// Save the index to .pes/index atomically.
//
// Steps:
//   1. Sort entries by path (use qsort with strcmp on the path field)
//   2. Open a temporary file for writing (.pes/index.tmp)
//   3. For each entry, write one line:
//      "<mode> <64-char-hex-hash> <mtime_sec> <size> <path>\n"
//      - Convert ObjectID to hex using hash_to_hex()
//   4. fflush() and fsync() the temp file to ensure data reaches disk
//   5. fclose() the temp file
//   6. rename(".pes/index.tmp", ".pes/index") — atomic replacement
//
// The rename() call is the key filesystem concept here: it is atomic
// on POSIX systems, meaning the index file is never in a half-written
// state even if the system crashes.
//
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    Index idx_copy = *index;
    qsort(idx_copy.entries, idx_copy.count, sizeof(IndexEntry), compare_index_entries);
    
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;
    
    for (int i = 0; i < idx_copy.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&idx_copy.entries[i].hash, hex);
        fprintf(f, "%06o %s %" PRIu64 " %u %s\n",
                idx_copy.entries[i].mode, hex,
                idx_copy.entries[i].mtime_sec,
                idx_copy.entries[i].size,
                idx_copy.entries[i].path);
    }
    
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    
    if (rename(tmp_path, INDEX_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }
    
    return 0;
}

// Forward declaration
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Stage a file for the next commit.
//
// Steps:
//   1. Open and read the file at `path`
//   2. Write the file contents as a blob: object_write(OBJ_BLOB, ...)
//   3. Stat the file to get mode, mtime, and size
//      - Use stat() or lstat()
//      - mtime_sec = st.st_mtime
//      - size = st.st_size
//      - mode: use 0100755 if executable (st_mode & S_IXUSR), else 0100644
//   4. Search the index for an existing entry with this path (index_find)
//      - If found: update its hash, mode, mtime, and size
//      - If not found: append a new entry (check count < MAX_INDEX_ENTRIES)
//   5. Save the index to disk (index_save)
//
// Returns 0 on success, -1 on error (file not found, etc.).
int index_add(Index *index, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "error: could not open '%s'\n", path);
        return -1;
    }
    
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    
    size_t size = st.st_size;
    void *data = malloc(size > 0 ? size : 1);
    if (size > 0 && !data) { close(fd); return -1; }
    
    if (size > 0 && read(fd, data, size) != (ssize_t)size) {
        free(data); close(fd); return -1;
    }
    close(fd);
    
    ObjectID id;
    if (object_write(OBJ_BLOB, data, size, &id) != 0) {
        free(data); return -1;
    }
    free(data);
    
    uint32_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    
    IndexEntry *entry = index_find(index, path);
    if (entry) {
        entry->hash = id;
        entry->mode = mode;
        entry->mtime_sec = st.st_mtime;
        entry->size = size;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
        entry->hash = id;
        entry->mode = mode;
        entry->mtime_sec = st.st_mtime;
        entry->size = size;
        strcpy(entry->path, path);
    }
    
    return index_save(index);
}

static void flatten_tree(const ObjectID *tree_id, const char *prefix, Index *flat) {
    ObjectType type;
    void *data;
    size_t len;
    extern int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);
    if (object_read(tree_id, &type, &data, &len) != 0) return;
    
    if (type != OBJ_TREE) {
        free(data);
        return;
    }
    
    Tree t;
    if (tree_parse(data, len, &t) != 0) {
        free(data);
        return;
    }
    free(data);
    
    for (int i = 0; i < t.count; i++) {
        char path[512];
        if (prefix[0] == '\0') {
            strcpy(path, t.entries[i].name);
        } else {
            snprintf(path, sizeof(path), "%s/%s", prefix, t.entries[i].name);
        }
        
        if (t.entries[i].mode == 0040000) {
            flatten_tree(&t.entries[i].hash, path, flat);
        } else {
            if (flat->count < MAX_INDEX_ENTRIES) {
                flat->entries[flat->count].mode = t.entries[i].mode;
                flat->entries[flat->count].hash = t.entries[i].hash;
                strcpy(flat->entries[flat->count].path, path);
                flat->count++;
            }
        }
    }
}

static void scan_untracked(const char *dir_path, const Index *index, int *untracked_count) {
    DIR *dir = opendir(dir_path[0] == '\0' ? "." : dir_path);
    if (!dir) return;
    
    struct dirent *dp;
    while ((dp = readdir(dir)) != NULL) {
        if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0 || 
            strcmp(dp->d_name, ".pes") == 0 || strcmp(dp->d_name, ".git") == 0) {
            continue;
        }
        
        char path[512];
        if (dir_path[0] == '\0') {
            strcpy(path, dp->d_name);
        } else {
            snprintf(path, sizeof(path), "%s/%s", dir_path, dp->d_name);
        }
        
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                scan_untracked(path, index, untracked_count);
            } else if (S_ISREG(st.st_mode)) {
                if (!index_find((Index *)index, path)) {
                    printf("    %s\n", path);
                    (*untracked_count)++;
                }
            }
        }
    }
    closedir(dir);
}

// Print the status of the working directory.
//
// This involves THREE comparisons:
//
// 1. Index vs HEAD (staged changes):
//    - Load the HEAD commit's tree (if any commits exist)
//    - For each index entry, check if it exists in HEAD's tree with the same hash
//    - New in index but not in HEAD:       "new file:   <path>"
//    - In both but different hash:          "modified:   <path>"
//
// 2. Working directory vs index (unstaged changes):
//    - For each index entry, check the working directory file
//    - If file is missing:                  "deleted:    <path>"
//    - If file's mtime or size changed, recompute its hash:
//      - If hash differs from index:        "modified:   <path>"
//    - (If mtime+size unchanged, skip — assume file is unmodified)
//
// 3. Untracked files:
//    - Scan the working directory (skip .pes/)
//    - Any file not in the index:           "<path>"
//
// Expected output:
//   Staged changes:
//       new file:   hello.txt
//
//   Unstaged changes:
//       modified:   README.md
//
//   Untracked files:
//       notes.txt
//
// If a section has no entries, print the header followed by
//   (nothing to show)
//
// Returns 0.
int index_status(const Index *index) {
    Index head_flat;
    head_flat.count = 0;
    
    ObjectID head_id;
    extern int head_read(ObjectID *id_out);
    extern int commit_parse(const void *data, size_t len, Commit *commit_out);
    extern int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);

    if (head_read(&head_id) == 0) {
        ObjectType type;
        void *data;
        size_t len;
        if (object_read(&head_id, &type, &data, &len) == 0 && type == OBJ_COMMIT) {
            Commit c;
            if (commit_parse(data, len, &c) == 0) {
                flatten_tree(&c.tree, "", &head_flat);
            }
            free(data);
        }
    }
    
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        IndexEntry *head_entry = index_find(&head_flat, index->entries[i].path);
        if (!head_entry) {
            printf("    new file:   %s\n", index->entries[i].path);
            staged_count++;
        } else if (memcmp(head_entry->hash.hash, index->entries[i].hash.hash, HASH_SIZE) != 0) {
            printf("    modified:   %s\n", index->entries[i].path);
            staged_count++;
        }
    }
    if (staged_count == 0) printf("    (nothing to show)\n");
    printf("\n");
    
    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (lstat(index->entries[i].path, &st) != 0) {
            printf("    deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != index->entries[i].mtime_sec || st.st_size != index->entries[i].size) {
                int fd = open(index->entries[i].path, O_RDONLY);
                if (fd >= 0) {
                    void *data = malloc(st.st_size > 0 ? st.st_size : 1);
                    if (st.st_size == 0 || (data && read(fd, data, st.st_size) == (ssize_t)st.st_size)) {
                        ObjectID id;
                        char header[64];
                        int hlen = snprintf(header, sizeof(header), "blob %u", (unsigned)st.st_size) + 1;
                        void *full = malloc(hlen + st.st_size);
                        if (full) {
                            memcpy(full, header, hlen);
                            if (st.st_size > 0 && data) memcpy((char*)full + hlen, data, st.st_size);
                            compute_hash(full, hlen + st.st_size, &id);
                            if (memcmp(id.hash, index->entries[i].hash.hash, HASH_SIZE) != 0) {
                                printf("    modified:   %s\n", index->entries[i].path);
                                unstaged_count++;
                            }
                            free(full);
                        }
                    }
                    if (data) free(data);
                    close(fd);
                }
            }
        }
    }
    if (unstaged_count == 0) printf("    (nothing to show)\n");
    printf("\n");
    
    printf("Untracked files:\n");
    int untracked_count = 0;
    scan_untracked("", index, &untracked_count);
    if (untracked_count == 0) printf("    (nothing to show)\n");
    
    return 0;
}
