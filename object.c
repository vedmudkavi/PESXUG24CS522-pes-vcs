#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED HELPERS ────────────────────────────────────────────────────────

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
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

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

// ─── IMPLEMENTED FUNCTIONS ──────────────────────────────────────────────────
int hash_file(const char *path, ObjectID *hash_out) {
    // Open the file
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    // Get the file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // Read the file contents into a buffer
    char *buffer = malloc(file_size);
    if (!buffer) { fclose(fp); return -1; }
    fread(buffer, 1, file_size, fp);
    fclose(fp);

    // Use your existing compute_hash function
    compute_hash(buffer, file_size, hash_out);

    // Cleanup
    free(buffer);
    return 0;
}
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str = (type == OBJ_BLOB) ? "blob" : (type == OBJ_TREE) ? "tree" : "commit";
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;

    size_t full_len = header_len + len;
    char *buffer = malloc(full_len);
    memcpy(buffer, header, header_len);
    memcpy(buffer + header_len, data, len);

    compute_hash(buffer, full_len, id_out);

    if (object_exists(id_out)) {
        free(buffer);
        return 0;
    }

    char path[512];
    object_path(id_out, path, sizeof(path));

    char dir[512];
    strncpy(dir, path, strrchr(path, '/') - path);
    dir[strrchr(path, '/') - path] = '\0';
    mkdir(dir, 0755);

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(buffer); return -1; }
    write(fd, buffer, full_len);
    fsync(fd);
    close(fd);
    rename(tmp, path);

    free(buffer);
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buffer = malloc(file_size);
    if (!buffer) { fclose(fp); return -1; }
    fread(buffer, 1, file_size, fp);
    fclose(fp);

    // --- INTEGRITY CHECK ---
    ObjectID actual_hash;
    compute_hash(buffer, file_size, &actual_hash);
    
    // Compare the calculated hash with the requested ID
    if (memcmp(id->hash, actual_hash.hash, HASH_SIZE) != 0) {
        free(buffer);
        return -1; // Hash mismatch means data is corrupted
    }
    // -----------------------

    char *null_byte = memchr(buffer, '\0', file_size);
    if (!null_byte) { free(buffer); return -1; }

    char type_str[16];
    size_t data_len;
    sscanf(buffer, "%15s %zu", type_str, &data_len);

    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else *type_out = OBJ_COMMIT;

    *len_out = data_len;
    *data_out = malloc(data_len);
    memcpy(*data_out, null_byte + 1, data_len);

    free(buffer);
    return 0;
}
