// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode
// TODO functions:     tree_parse, tree_serialize, tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

#include "index.h"

// Forward declaration
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Parse binary tree data into a Tree struct.
//
// The input `data` contains concatenated entries, each formatted as:
//   "<mode> <name>\0<32-byte-hash>"
// where <mode> is an ASCII octal string (e.g., "100644").
//
// Steps:
//   1. Start a pointer at the beginning of data
//   2. For each entry:
//      a. Read the mode string up to the space character
//      b. Read the name from after the space up to the null byte
//      c. Read the next 32 bytes as the raw binary hash
//      d. Store in tree_out->entries[tree_out->count++]
//   3. Stop when you've consumed all `len` bytes
//
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const char *p = (const char *)data;
    const char *end = p + len;

    while (p < end && tree_out->count < MAX_TREE_ENTRIES) {
        const char *space = memchr(p, ' ', end - p);
        if (!space) return -1;
        
        char mode_str[32] = {0};
        size_t mode_len = space - p;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, p, mode_len);
        tree_out->entries[tree_out->count].mode = strtoul(mode_str, NULL, 8);
        
        p = space + 1;
        
        const char *null_byte = memchr(p, '\0', end - p);
        if (!null_byte) return -1;
        
        size_t name_len = null_byte - p;
        if (name_len >= sizeof(tree_out->entries[0].name)) return -1;
        memcpy(tree_out->entries[tree_out->count].name, p, name_len);
        tree_out->entries[tree_out->count].name[name_len] = '\0';
        
        p = null_byte + 1;
        
        if (end - p < HASH_SIZE) return -1;
        memcpy(tree_out->entries[tree_out->count].hash.hash, p, HASH_SIZE);
        p += HASH_SIZE;
        
        tree_out->count++;
    }

    return p == end ? 0 : -1;
}

static int compare_entries(const void *a, const void *b) {
    const TreeEntry *ea = (const TreeEntry *)a;
    const TreeEntry *eb = (const TreeEntry *)b;
    return strcmp(ea->name, eb->name);
}

// Serialize a Tree struct into binary format for storage.
//
// Steps:
//   1. Sort entries by name (required for deterministic hashing —
//      same directory contents must always produce the same tree hash)
//   2. Calculate total output size
//   3. Allocate output buffer
//   4. For each entry, write: "<mode> <name>\0<32-byte-hash>"
//   5. Set *data_out and *len_out
//
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    Tree t_copy = *tree;
    qsort(t_copy.entries, t_copy.count, sizeof(TreeEntry), compare_entries);

    size_t total = 0;
    for (int i = 0; i < t_copy.count; i++) {
        char mode_str[16];
        int n = snprintf(mode_str, sizeof(mode_str), "%06o", t_copy.entries[i].mode);
        total += n + 1 + strlen(t_copy.entries[i].name) + 1 + HASH_SIZE;
    }

    *data_out = malloc(total);
    if (!*data_out) return -1;

    char *p = (char *)*data_out;
    for (int i = 0; i < t_copy.count; i++) {
        char mode_str[16];
        int n = snprintf(mode_str, sizeof(mode_str), "%06o", t_copy.entries[i].mode);
        memcpy(p, mode_str, n);
        p += n;
        *p++ = ' ';

        size_t name_len = strlen(t_copy.entries[i].name);
        memcpy(p, t_copy.entries[i].name, name_len);
        p += name_len;
        *p++ = '\0';

        memcpy(p, t_copy.entries[i].hash.hash, HASH_SIZE);
        p += HASH_SIZE;
    }

    *len_out = total;
    return 0;
}

static int build_tree(IndexEntry *entries, int count, int prefix_len, ObjectID *out_id) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        char *path = entries[i].path + prefix_len;
        char *slash = strchr(path, '/');
        
        if (!slash) {
            strcpy(tree.entries[tree.count].name, path);
            tree.entries[tree.count].mode = entries[i].mode;
            tree.entries[tree.count].hash = entries[i].hash;
            tree.count++;
            i++;
        } else {
            int dir_len = slash - path;
            char dir_name[256];
            strncpy(dir_name, path, dir_len);
            dir_name[dir_len] = '\0';

            int j = i;
            while (j < count) {
                char *p = entries[j].path + prefix_len;
                if (strncmp(p, dir_name, dir_len) == 0 && p[dir_len] == '/') {
                    j++;
                } else {
                    break;
                }
            }

            ObjectID sub_id;
            if (build_tree(entries + i, j - i, prefix_len + dir_len + 1, &sub_id) != 0) return -1;

            strcpy(tree.entries[tree.count].name, dir_name);
            tree.entries[tree.count].mode = MODE_DIR;
            tree.entries[tree.count].hash = sub_id;
            tree.count++;

            i = j;
        }
    }

    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;
    if (object_write(OBJ_TREE, data, len, out_id) != 0) {
        free(data);
        return -1;
    }
    free(data);
    return 0;
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// The index contains flat paths like:
//   "README.md"
//   "src/main.c"
//   "src/util/helper.c"
//
// You must construct a tree hierarchy:
//   root tree:
//     100644 blob <hash> README.md
//     040000 tree <hash> src
//   src tree:
//     100644 blob <hash> main.c
//     040000 tree <hash> util
//   util tree:
//     100644 blob <hash> helper.c
//
// Steps:
//   1. Load the index (use index_load from index.h)
//   2. Group entries by their top-level directory component
//   3. For files at the current level, add blob entries to the tree
//   4. For files in subdirectories, recursively build subtrees
//   5. Serialize and write each tree object using object_write(OBJ_TREE, ...)
//   6. Return the root tree's hash in *id_out
//
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    Index idx;
    if (index_load(&idx) != 0) return -1;
    return build_tree(idx.entries, idx.count, 0, id_out);
}
