// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/sha.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(id_out->hash, &ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

#include <errno.h>

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_out
//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str = type == OBJ_BLOB ? "blob" : (type == OBJ_TREE ? "tree" : "commit");
    char header[128];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1; // +1 to include \0

    size_t full_len = header_len + len;
    void *full_data = malloc(full_len);
    if (!full_data) return -1;

    memcpy(full_data, header, header_len);
    memcpy((char *)full_data + header_len, data, len);

    compute_hash(full_data, full_len, id_out);

    if (object_exists(id_out)) {
        free(full_data);
        return 0; // Already exists
    }

    char shard_dir[512];
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);

    if (mkdir(shard_dir, 0755) != 0 && errno != EEXIST) {
        free(full_data);
        return -1;
    }

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/temp_XXXXXX", shard_dir);
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        free(full_data);
        return -1;
    }

    if (write(fd, full_data, full_len) != (ssize_t)full_len) {
        close(fd);
        unlink(tmp_path);
        free(full_data);
        return -1;
    }
    fsync(fd);
    close(fd);
    free(full_data);

    char final_path[512];
    object_path(id_out, final_path, sizeof(final_path));

    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    int dir_fd = open(shard_dir, O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    return 0;
}

// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }

    size_t full_len = st.st_size;
    void *full_data = malloc(full_len);
    if (!full_data) {
        close(fd);
        return -1;
    }

    if (read(fd, full_data, full_len) != (ssize_t)full_len) {
        free(full_data);
        close(fd);
        return -1;
    }
    close(fd);

    ObjectID recomputed;
    compute_hash(full_data, full_len, &recomputed);
    if (memcmp(id->hash, recomputed.hash, HASH_SIZE) != 0) {
        free(full_data);
        return -1;
    }

    char type_str[32];
    size_t size = 0;
    if (sscanf((const char *)full_data, "%31s %zu", type_str, &size) != 2) {
        free(full_data);
        return -1;
    }

    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else {
        free(full_data);
        return -1;
    }

    char *data_start = memchr(full_data, '\0', full_len);
    if (!data_start) {
        free(full_data);
        return -1;
    }
    data_start++; // Skip the null byte

    *data_out = malloc(size);
    if (!*data_out) {
        free(full_data);
        return -1;
    }
    memcpy(*data_out, data_start, size);
    *len_out = size;

    free(full_data);
    return 0;
}
