#define _POSIX_C_SOURCE 200809L
#undef NDEBUG

#include "../app/src/main/cpp/custom_words.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void path_join(char output[512], const char *directory, const char *name) {
    const int result = snprintf(output, 512, "%s/%s", directory, name);
    assert(result > 0 && result < 512);
}

static void write_bytes(const char *path, const void *bytes, size_t size) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    const uint8_t *data = bytes;
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(fd, data + offset, size - offset);
        assert(written > 0);
        offset += (size_t)written;
    }
    assert(close(fd) == 0);
}

static uint32_t read_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint16_t read_le16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void test_empty_and_blob(void) {
    DSCustomWords bank;
    ds_custom_words_init(&bank);
    assert(ds_custom_words_count(&bank) == 0);

    uint8_t blob[DS_CUSTOM_WORDS_BLOB_MAX_SIZE];
    memset(blob, 0xa5, sizeof(blob));
    size_t size = ds_custom_words_blob(&bank, blob, sizeof(blob));
    assert(size == 12 && memcmp(blob, "DSWB", 4) == 0);
    assert(read_le32(blob + 4) == 1 && read_le32(blob + 8) == 0);

    uint8_t untouched[16];
    memset(untouched, 0x5a, sizeof(untouched));
    assert(ds_custom_words_blob(&bank, untouched, 11) == 0);
    assert(ds_custom_words_error(&bank) == DS_CUSTOM_WORDS_ERROR_BUFFER_TOO_SMALL);
    for (size_t i = 0; i < sizeof(untouched); ++i) assert(untouched[i] == 0x5a);
}

static void test_word_validation_and_persistence(const char *directory) {
    char path[512];
    path_join(path, directory, "custom.bin");
    DSCustomWords bank;
    ds_custom_words_init(&bank);
    assert(ds_custom_words_load(&bank, path));
    assert(ds_custom_words_count(&bank) == 0);

    assert(ds_custom_words_add(&bank, path, "Morrow"));
    assert(strcmp(ds_custom_words_word(&bank, 0), "MORROW") == 0);
    assert(ds_custom_words_add(&bank, path, "O'Neil"));
    assert(ds_custom_words_add(&bank, path, "Mother-in-law"));
    assert(ds_custom_words_add(&bank, path, "New York"));
    assert(ds_custom_words_count(&bank) == 4);
    assert(ds_custom_words_error(&bank) == DS_CUSTOM_WORDS_ERROR_NONE);

    assert(!ds_custom_words_add(&bank, path, "MORROW"));
    assert(ds_custom_words_error(&bank) == DS_CUSTOM_WORDS_ERROR_DUPLICATE);
    assert(ds_custom_words_count(&bank) == 4);

    const char *invalid[] = {"", "-START", "END'", "A--B", "A  B", "A_B", "A\tB", "A\nB"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!ds_custom_words_add(&bank, path, invalid[i]));
        assert(ds_custom_words_error(&bank) == DS_CUSTOM_WORDS_ERROR_INVALID_WORD);
        assert(ds_custom_words_count(&bank) == 4);
    }
    const char non_ascii[] = {'C', 'A', (char)0xc3, (char)0xa9, '\0'};
    assert(!ds_custom_words_add(&bank, path, non_ascii));
    assert(ds_custom_words_error(&bank) == DS_CUSTOM_WORDS_ERROR_INVALID_WORD);

    char max_word[DS_CUSTOM_WORDS_MAX_LENGTH + 1u];
    memset(max_word, 'Z', DS_CUSTOM_WORDS_MAX_LENGTH);
    max_word[DS_CUSTOM_WORDS_MAX_LENGTH] = '\0';
    assert(ds_custom_words_add(&bank, path, max_word));
    char too_long[DS_CUSTOM_WORDS_MAX_LENGTH + 2u];
    memset(too_long, 'Q', sizeof(too_long) - 1u);
    too_long[sizeof(too_long) - 1u] = '\0';
    assert(!ds_custom_words_add(&bank, path, too_long));
    assert(ds_custom_words_error(&bank) == DS_CUSTOM_WORDS_ERROR_WORD_TOO_LONG);

    struct stat status;
    assert(stat(path, &status) == 0 && S_ISREG(status.st_mode));
    assert((status.st_mode & 0777) == 0600);

    uint8_t blob[DS_CUSTOM_WORDS_BLOB_MAX_SIZE];
    const size_t blob_size = ds_custom_words_blob(&bank, blob, sizeof(blob));
    assert(blob_size > 12 && memcmp(blob, "DSWB", 4) == 0);
    assert(read_le32(blob + 4) == 1 && read_le32(blob + 8) == 5);
    size_t offset = 12;
    assert(read_le16(blob + offset) == 6 && memcmp(blob + offset + 2, "MORROW", 6) == 0);
    offset += 8;
    assert(read_le16(blob + offset) == 6 && memcmp(blob + offset + 2, "O'NEIL", 6) == 0);

    DSCustomWords reloaded;
    ds_custom_words_init(&reloaded);
    assert(ds_custom_words_load(&reloaded, path));
    assert(ds_custom_words_count(&reloaded) == 5);
    for (size_t i = 0; i < ds_custom_words_count(&bank); ++i)
        assert(strcmp(ds_custom_words_word(&bank, i), ds_custom_words_word(&reloaded, i)) == 0);
    assert(!ds_custom_words_remove(&reloaded, path, 99));
    assert(ds_custom_words_error(&reloaded) == DS_CUSTOM_WORDS_ERROR_NOT_FOUND);
    assert(ds_custom_words_remove(&reloaded, path, 1));
    assert(strcmp(ds_custom_words_word(&reloaded, 1), "MOTHER-IN-LAW") == 0);
    assert(ds_custom_words_count(&reloaded) == 4);
    DSCustomWords after_remove;
    ds_custom_words_init(&after_remove);
    assert(ds_custom_words_load(&after_remove, path));
    assert(ds_custom_words_count(&after_remove) == 4);
    assert(strcmp(ds_custom_words_word(&after_remove, 1), "MOTHER-IN-LAW") == 0);
}

static void test_full_bank(const char *directory) {
    char path[512];
    path_join(path, directory, "full.bin");
    DSCustomWords bank;
    ds_custom_words_init(&bank);
    for (unsigned i = 0; i < DS_CUSTOM_WORDS_MAX_COUNT; ++i) {
        char word[DS_CUSTOM_WORDS_MAX_LENGTH + 1u] = {
            'W',
            (char)('A' + (i / (26u * 26u)) % 26u),
            (char)('A' + (i / 26u) % 26u),
            (char)('A' + i % 26u)
        };
        memset(word + 4, 'Z', DS_CUSTOM_WORDS_MAX_LENGTH - 4u);
        word[DS_CUSTOM_WORDS_MAX_LENGTH] = '\0';
        assert(ds_custom_words_add(&bank, path, word));
    }
    assert(ds_custom_words_count(&bank) == DS_CUSTOM_WORDS_MAX_COUNT);
    assert(!ds_custom_words_add(&bank, path, "OVERFLOW"));
    assert(ds_custom_words_error(&bank) == DS_CUSTOM_WORDS_ERROR_FULL);
    uint8_t blob[DS_CUSTOM_WORDS_BLOB_MAX_SIZE];
    const size_t size = ds_custom_words_blob(&bank, blob, sizeof(blob));
    assert(size == DS_CUSTOM_WORDS_BLOB_MAX_SIZE);
    assert(read_le32(blob + 8) == DS_CUSTOM_WORDS_MAX_COUNT);
}

static void test_malformed_load_preserves_bank(const char *directory) {
    char source_path[512], malformed_path[512];
    path_join(source_path, directory, "preserve.bin");
    path_join(malformed_path, directory, "malformed.bin");
    DSCustomWords bank;
    ds_custom_words_init(&bank);
    assert(ds_custom_words_add(&bank, source_path, "SAFE WORD"));
    const uint8_t malformed[] = {'D', 'S', 'W', 'B', 1, 0, 0, 0, 1, 0, 0, 0, 99, 0, 'X'};
    write_bytes(malformed_path, malformed, sizeof(malformed));
    assert(!ds_custom_words_load(&bank, malformed_path));
    assert(ds_custom_words_error(&bank) == DS_CUSTOM_WORDS_ERROR_MALFORMED);
    assert(ds_custom_words_count(&bank) == 1);
    assert(strcmp(ds_custom_words_word(&bank, 0), "SAFE WORD") == 0);

    char empty_path[512];
    path_join(empty_path, directory, "missing.bin");
    assert(ds_custom_words_load(&bank, empty_path));
    assert(ds_custom_words_count(&bank) == 0);
    assert(ds_custom_words_error(&bank) == DS_CUSTOM_WORDS_ERROR_NONE);
}

static void test_symlink_rejection_and_write_failure(const char *directory) {
    char target[512], link_path[512], link_parent[512], parent_child[512];
    path_join(target, directory, "target.bin");
    path_join(link_path, directory, "link.bin");
    path_join(link_parent, directory, "linked-directory");
    path_join(parent_child, link_parent, "custom.bin");

    DSCustomWords bank;
    ds_custom_words_init(&bank);
    assert(ds_custom_words_add(&bank, target, "KEEP ME"));
    assert(symlink(target, link_path) == 0);
    assert(!ds_custom_words_load(&bank, link_path));
    assert(ds_custom_words_error(&bank) == DS_CUSTOM_WORDS_ERROR_SYMLINK);
    assert(ds_custom_words_count(&bank) == 1 && strcmp(ds_custom_words_word(&bank, 0), "KEEP ME") == 0);

    assert(symlink(directory, link_parent) == 0);
    assert(!ds_custom_words_add(&bank, parent_child, "NO LINK"));
    assert(ds_custom_words_error(&bank) == DS_CUSTOM_WORDS_ERROR_SYMLINK);
    assert(ds_custom_words_count(&bank) == 1 && strcmp(ds_custom_words_word(&bank, 0), "KEEP ME") == 0);

    char directory_target[512];
    path_join(directory_target, directory, "is-a-directory");
    assert(mkdir(directory_target, 0700) == 0);
    assert(!ds_custom_words_add(&bank, directory_target, "NO PARTIAL"));
    assert(ds_custom_words_error(&bank) == DS_CUSTOM_WORDS_ERROR_IO);
    assert(ds_custom_words_count(&bank) == 1 && strcmp(ds_custom_words_word(&bank, 0), "KEEP ME") == 0);
}

int main(void) {
    char directory[] = "/tmp/deadspeak-custom-words-XXXXXX";
    assert(mkdtemp(directory));
    test_empty_and_blob();
    test_word_validation_and_persistence(directory);
    test_full_bank(directory);
    test_malformed_load_preserves_bank(directory);
    test_symlink_rejection_and_write_failure(directory);

    char path[512];
    const char *files[] = {"custom.bin", "full.bin", "preserve.bin", "malformed.bin",
                           "target.bin", "link.bin", "linked-directory", "is-a-directory"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        path_join(path, directory, files[i]);
        struct stat status;
        if (lstat(path, &status) == 0) {
            if (S_ISDIR(status.st_mode)) assert(rmdir(path) == 0);
            else assert(unlink(path) == 0);
        }
    }
    assert(rmdir(directory) == 0);
    puts("PASS: custom words validation, persistence, atomic safety, symlinks, bounds, and DSWB blob");
    return 0;
}
