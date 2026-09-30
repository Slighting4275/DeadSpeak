#define _POSIX_C_SOURCE 200809L

#include "custom_words.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#define DS_CUSTOM_WORDS_FILE_LIMIT DS_CUSTOM_WORDS_BLOB_MAX_SIZE
#define DS_CUSTOM_WORDS_TEMP_TRIES 64u

static const char *error_text(DSCustomWordsError error) {
    switch (error) {
        case DS_CUSTOM_WORDS_ERROR_NONE: return "No error.";
        case DS_CUSTOM_WORDS_ERROR_INVALID_ARGUMENT: return "Invalid argument or path.";
        case DS_CUSTOM_WORDS_ERROR_INVALID_WORD:
            return "Word must contain ASCII letters and may include single spaces, hyphens, or apostrophes between letters.";
        case DS_CUSTOM_WORDS_ERROR_WORD_TOO_LONG: return "Word exceeds 31 ASCII bytes.";
        case DS_CUSTOM_WORDS_ERROR_DUPLICATE: return "That word is already in the custom bank.";
        case DS_CUSTOM_WORDS_ERROR_FULL: return "Custom bank is limited to 256 words.";
        case DS_CUSTOM_WORDS_ERROR_NOT_FOUND: return "Word index is not present in the custom bank.";
        case DS_CUSTOM_WORDS_ERROR_IO: return "Could not safely read or persist the custom bank.";
        case DS_CUSTOM_WORDS_ERROR_MALFORMED: return "Custom bank file is malformed or unsupported.";
        case DS_CUSTOM_WORDS_ERROR_SYMLINK: return "Custom bank paths may not contain symbolic links.";
        case DS_CUSTOM_WORDS_ERROR_BUFFER_TOO_SMALL: return "Output buffer is too small for the custom-word blob.";
        case DS_CUSTOM_WORDS_ERROR_DURABILITY_UNCONFIRMED:
            return "The new bank was committed, but directory durability could not be confirmed.";
    }
    return "Unknown custom-word error.";
}

static bool fail(DSCustomWords *bank, DSCustomWordsError error) {
    if (bank) bank->last_error = error;
    return false;
}

static size_t blob_size_for(const DSCustomWords *bank) {
    size_t size = DS_CUSTOM_WORDS_BLOB_HEADER_SIZE;
    for (uint32_t i = 0; i < bank->count; ++i) {
        size += 2u + strlen(bank->words[i]);
    }
    return size;
}

static void put_u16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)(value >> 24);
}

static uint16_t get_u16(const uint8_t *in) {
    return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t get_u32(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static size_t encode_blob(const DSCustomWords *bank, uint8_t *out) {
    memcpy(out, "DSWB", 4);
    put_u32(out + 4, 1u);
    put_u32(out + 8, bank->count);
    size_t offset = DS_CUSTOM_WORDS_BLOB_HEADER_SIZE;
    for (uint32_t i = 0; i < bank->count; ++i) {
        const size_t length = strlen(bank->words[i]);
        put_u16(out + offset, (uint16_t)length);
        offset += 2;
        memcpy(out + offset, bank->words[i], length);
        offset += length;
    }
    return offset;
}

static bool normalize_word(const char *text, char normalized[DS_CUSTOM_WORDS_MAX_LENGTH + 1u],
                           DSCustomWordsError *error) {
    if (!text) {
        *error = DS_CUSTOM_WORDS_ERROR_INVALID_ARGUMENT;
        return false;
    }

    size_t length = 0;
    bool has_letter = false;
    bool previous_separator = true;
    for (; length <= DS_CUSTOM_WORDS_MAX_LENGTH; ++length) {
        const unsigned char value = (unsigned char)text[length];
        if (value == '\0') break;
        if (length == DS_CUSTOM_WORDS_MAX_LENGTH) {
            *error = DS_CUSTOM_WORDS_ERROR_WORD_TOO_LONG;
            return false;
        }

        if (value >= 'a' && value <= 'z') {
            normalized[length] = (char)(value - 'a' + 'A');
            has_letter = true;
            previous_separator = false;
        } else if (value >= 'A' && value <= 'Z') {
            normalized[length] = (char)value;
            has_letter = true;
            previous_separator = false;
        } else if (value == ' ' || value == '-' || value == '\'') {
            if (previous_separator) {
                *error = DS_CUSTOM_WORDS_ERROR_INVALID_WORD;
                return false;
            }
            normalized[length] = (char)value;
            previous_separator = true;
        } else {
            *error = DS_CUSTOM_WORDS_ERROR_INVALID_WORD;
            return false;
        }
    }
    if (length == 0 || !has_letter || previous_separator) {
        *error = DS_CUSTOM_WORDS_ERROR_INVALID_WORD;
        return false;
    }
    normalized[length] = '\0';
    *error = DS_CUSTOM_WORDS_ERROR_NONE;
    return true;
}

static bool normalize_loaded_word(const uint8_t *bytes, size_t length,
                                  char normalized[DS_CUSTOM_WORDS_MAX_LENGTH + 1u]) {
    if (length == 0 || length > DS_CUSTOM_WORDS_MAX_LENGTH) return false;
    char input[DS_CUSTOM_WORDS_MAX_LENGTH + 1u];
    memcpy(input, bytes, length);
    input[length] = '\0';
    DSCustomWordsError error;
    if (!normalize_word(input, normalized, &error)) return false;
    return strlen(normalized) == length && memcmp(normalized, bytes, length) == 0;
}

static int symlink_at(int directory_fd, const char *name) {
    struct stat status;
    return fstatat(directory_fd, name, &status, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(status.st_mode);
}

/* Walk each directory component with O_NOFOLLOW; reject dot traversal. */
static int open_parent_directory(const char *path, char *basename, size_t basename_capacity,
                                 DSCustomWords *bank) {
    if (!path || !path[0] || !basename || basename_capacity == 0) {
        fail(bank, DS_CUSTOM_WORDS_ERROR_INVALID_ARGUMENT);
        return -1;
    }
    const size_t path_length = strnlen(path, PATH_MAX);
    if (path_length == 0 || path_length >= PATH_MAX || path[path_length - 1] == '/') {
        fail(bank, DS_CUSTOM_WORDS_ERROR_INVALID_ARGUMENT);
        return -1;
    }
    const char *last_slash = strrchr(path, '/');
    char parent_dir[PATH_MAX];
    const char *base = NULL;
    if (!last_slash) {
        snprintf(parent_dir, sizeof(parent_dir), ".");
        base = path;
    } else if (last_slash == path) {
        snprintf(parent_dir, sizeof(parent_dir), "/");
        base = path + 1;
    } else {
        const size_t dir_len = (size_t)(last_slash - path);
        if (dir_len >= sizeof(parent_dir)) {
            fail(bank, DS_CUSTOM_WORDS_ERROR_INVALID_ARGUMENT);
            return -1;
        }
        memcpy(parent_dir, path, dir_len);
        parent_dir[dir_len] = '\0';
        base = last_slash + 1;
    }
    if (!base[0] || strcmp(base, ".") == 0 || strcmp(base, "..") == 0 ||
        strlen(base) >= basename_capacity) {
        fail(bank, DS_CUSTOM_WORDS_ERROR_INVALID_ARGUMENT);
        return -1;
    }
    snprintf(basename, basename_capacity, "%s", base);

    struct stat st;
    if (lstat(parent_dir, &st) == 0 && S_ISLNK(st.st_mode)) {
        fail(bank, DS_CUSTOM_WORDS_ERROR_SYMLINK);
        return -1;
    }

    int directory_fd = open(parent_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd < 0) {
        const DSCustomWordsError error = (errno == ELOOP || errno == EMLINK)
            ? DS_CUSTOM_WORDS_ERROR_SYMLINK : DS_CUSTOM_WORDS_ERROR_IO;
        fail(bank, error);
        return -1;
    }
    return directory_fd;
}

static bool parse_blob(const uint8_t *bytes, size_t size, DSCustomWords *candidate) {
    if (!bytes || size < DS_CUSTOM_WORDS_BLOB_HEADER_SIZE || memcmp(bytes, "DSWB", 4) != 0 ||
        get_u32(bytes + 4) != 1u) return false;
    const uint32_t count = get_u32(bytes + 8);
    if (count > DS_CUSTOM_WORDS_MAX_COUNT) return false;

    DSCustomWords parsed;
    ds_custom_words_init(&parsed);
    size_t offset = DS_CUSTOM_WORDS_BLOB_HEADER_SIZE;
    for (uint32_t i = 0; i < count; ++i) {
        if (offset > size || size - offset < 2) return false;
        const size_t length = get_u16(bytes + offset);
        offset += 2;
        if (length == 0 || length > DS_CUSTOM_WORDS_MAX_LENGTH || length > size - offset) return false;
        char normalized[DS_CUSTOM_WORDS_MAX_LENGTH + 1u];
        if (!normalize_loaded_word(bytes + offset, length, normalized)) return false;
        for (uint32_t previous = 0; previous < parsed.count; ++previous) {
            if (strcmp(parsed.words[previous], normalized) == 0) return false;
        }
        memcpy(parsed.words[parsed.count], normalized, length + 1u);
        ++parsed.count;
        offset += length;
    }
    if (offset != size) return false;
    parsed.last_error = DS_CUSTOM_WORDS_ERROR_NONE;
    *candidate = parsed;
    return true;
}

void ds_custom_words_init(DSCustomWords *bank) {
    if (bank) memset(bank, 0, sizeof(*bank));
}

static bool read_bank_file(DSCustomWords *bank, const char *path, DSCustomWords *candidate) {
    char basename[NAME_MAX + 1u];
    const int directory_fd = open_parent_directory(path, basename, sizeof(basename), bank);
    if (directory_fd < 0) return false;
    const int file_fd = openat(directory_fd, basename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file_fd < 0) {
        const int saved_errno = errno;
        const bool symlink = symlink_at(directory_fd, basename) || saved_errno == ELOOP;
        close(directory_fd);
        if (saved_errno == ENOENT && !symlink) {
            ds_custom_words_init(candidate);
            return true;
        }
        fail(bank, symlink ? DS_CUSTOM_WORDS_ERROR_SYMLINK : DS_CUSTOM_WORDS_ERROR_IO);
        return false;
    }

    struct stat status;
    if (fstat(file_fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        (uintmax_t)status.st_size > DS_CUSTOM_WORDS_FILE_LIMIT) {
        close(file_fd);
        close(directory_fd);
        fail(bank, DS_CUSTOM_WORDS_ERROR_MALFORMED);
        return false;
    }
    uint8_t bytes[DS_CUSTOM_WORDS_FILE_LIMIT];
    size_t used = 0;
    bool read_ok = true;
    for (;;) {
        if (used == sizeof(bytes)) {
            uint8_t extra;
            ssize_t amount;
            do amount = read(file_fd, &extra, 1); while (amount < 0 && errno == EINTR);
            if (amount != 0) read_ok = false;
            break;
        }
        ssize_t amount;
        do amount = read(file_fd, bytes + used, sizeof(bytes) - used); while (amount < 0 && errno == EINTR);
        if (amount < 0) {
            read_ok = false;
            break;
        }
        if (amount == 0) break;
        used += (size_t)amount;
    }
    const int close_file_result = close(file_fd);
    close(directory_fd);
    if (!read_ok || close_file_result != 0) {
        fail(bank, DS_CUSTOM_WORDS_ERROR_IO);
        return false;
    }
    if ((off_t)used != status.st_size || !parse_blob(bytes, used, candidate)) {
        fail(bank, DS_CUSTOM_WORDS_ERROR_MALFORMED);
        return false;
    }
    return true;
}

bool ds_custom_words_load(DSCustomWords *bank, const char *path) {
    if (!bank || !path) return fail(bank, DS_CUSTOM_WORDS_ERROR_INVALID_ARGUMENT);
    DSCustomWords candidate;
    if (!read_bank_file(bank, path, &candidate)) return false;
    *bank = candidate;
    return true;
}

static bool write_all(int fd, const uint8_t *bytes, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t amount = write(fd, bytes + offset, length - offset);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) return false;
        offset += (size_t)amount;
    }
    return true;
}

static bool persist_bank(DSCustomWords *bank, const char *path) {
    char basename[NAME_MAX + 1u];
    const int directory_fd = open_parent_directory(path, basename, sizeof(basename), bank);
    if (directory_fd < 0) return false;
    if (symlink_at(directory_fd, basename)) {
        close(directory_fd);
        return fail(bank, DS_CUSTOM_WORDS_ERROR_SYMLINK);
    }

    uint8_t bytes[DS_CUSTOM_WORDS_BLOB_MAX_SIZE];
    const size_t size = encode_blob(bank, bytes);
    char temporary[64];
    int temporary_fd = -1;
    for (unsigned attempt = 0; attempt < DS_CUSTOM_WORDS_TEMP_TRIES; ++attempt) {
        const unsigned long serial = (unsigned long)attempt;
        const int written = snprintf(temporary, sizeof(temporary),
                                     ".deadspeak-custom-%ld-%lu.tmp", (long)getpid(), serial);
        if (written < 0 || (size_t)written >= sizeof(temporary)) {
            close(directory_fd);
            return fail(bank, DS_CUSTOM_WORDS_ERROR_IO);
        }
        temporary_fd = openat(directory_fd, temporary,
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (temporary_fd >= 0 || errno != EEXIST) break;
    }
    if (temporary_fd < 0) {
        close(directory_fd);
        return fail(bank, DS_CUSTOM_WORDS_ERROR_IO);
    }

    bool written_ok = fchmod(temporary_fd, 0600) == 0 && write_all(temporary_fd, bytes, size) &&
                      fsync(temporary_fd) == 0;
    const int close_result = close(temporary_fd);
    if (!written_ok || close_result != 0) {
        unlinkat(directory_fd, temporary, 0);
        close(directory_fd);
        return fail(bank, DS_CUSTOM_WORDS_ERROR_IO);
    }
    if (renameat(directory_fd, temporary, directory_fd, basename) != 0) {
        const DSCustomWordsError error = symlink_at(directory_fd, basename)
            ? DS_CUSTOM_WORDS_ERROR_SYMLINK : DS_CUSTOM_WORDS_ERROR_IO;
        unlinkat(directory_fd, temporary, 0);
        close(directory_fd);
        return fail(bank, error);
    }
    if (fsync(directory_fd) != 0) {
        close(directory_fd);
        bank->last_error = DS_CUSTOM_WORDS_ERROR_DURABILITY_UNCONFIRMED;
        return true;
    }
    close(directory_fd);
    bank->last_error = DS_CUSTOM_WORDS_ERROR_NONE;
    return true;
}

bool ds_custom_words_add(DSCustomWords *bank, const char *path, const char *text) {
    if (!bank || !path) return fail(bank, DS_CUSTOM_WORDS_ERROR_INVALID_ARGUMENT);
    if (bank->count > DS_CUSTOM_WORDS_MAX_COUNT) return fail(bank, DS_CUSTOM_WORDS_ERROR_MALFORMED);
    char normalized[DS_CUSTOM_WORDS_MAX_LENGTH + 1u];
    DSCustomWordsError validation_error;
    if (!normalize_word(text, normalized, &validation_error)) return fail(bank, validation_error);
    for (uint32_t i = 0; i < bank->count; ++i) {
        if (strcmp(bank->words[i], normalized) == 0) return fail(bank, DS_CUSTOM_WORDS_ERROR_DUPLICATE);
    }
    if (bank->count == DS_CUSTOM_WORDS_MAX_COUNT) return fail(bank, DS_CUSTOM_WORDS_ERROR_FULL);

    DSCustomWords candidate = *bank;
    memcpy(candidate.words[candidate.count], normalized, strlen(normalized) + 1u);
    ++candidate.count;
    candidate.last_error = DS_CUSTOM_WORDS_ERROR_NONE;
    if (!persist_bank(&candidate, path)) {
        bank->last_error = candidate.last_error;
        return false;
    }
    *bank = candidate;
    return true;
}

bool ds_custom_words_remove(DSCustomWords *bank, const char *path, size_t index) {
    if (!bank || !path) return fail(bank, DS_CUSTOM_WORDS_ERROR_INVALID_ARGUMENT);
    if (bank->count > DS_CUSTOM_WORDS_MAX_COUNT) return fail(bank, DS_CUSTOM_WORDS_ERROR_MALFORMED);
    if (index >= bank->count) return fail(bank, DS_CUSTOM_WORDS_ERROR_NOT_FOUND);

    DSCustomWords candidate = *bank;
    for (size_t i = index; i + 1 < candidate.count; ++i) {
        memcpy(candidate.words[i], candidate.words[i + 1], sizeof(candidate.words[i]));
    }
    memset(candidate.words[candidate.count - 1u], 0, sizeof(candidate.words[0]));
    --candidate.count;
    candidate.last_error = DS_CUSTOM_WORDS_ERROR_NONE;
    if (!persist_bank(&candidate, path)) {
        bank->last_error = candidate.last_error;
        return false;
    }
    *bank = candidate;
    return true;
}

size_t ds_custom_words_count(const DSCustomWords *bank) {
    return bank && bank->count <= DS_CUSTOM_WORDS_MAX_COUNT ? bank->count : 0;
}

const char *ds_custom_words_word(const DSCustomWords *bank, size_t index) {
    if (!bank || index >= bank->count || bank->count > DS_CUSTOM_WORDS_MAX_COUNT) return NULL;
    return bank->words[index];
}

DSCustomWordsError ds_custom_words_error(const DSCustomWords *bank) {
    return bank ? bank->last_error : DS_CUSTOM_WORDS_ERROR_INVALID_ARGUMENT;
}

const char *ds_custom_words_error_message(const DSCustomWords *bank) {
    return error_text(ds_custom_words_error(bank));
}

size_t ds_custom_words_blob(DSCustomWords *bank, uint8_t *output, size_t capacity) {
    if (!bank || !output) {
        fail(bank, DS_CUSTOM_WORDS_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    if (bank->count > DS_CUSTOM_WORDS_MAX_COUNT) {
        fail(bank, DS_CUSTOM_WORDS_ERROR_MALFORMED);
        return 0;
    }
    const size_t required = blob_size_for(bank);
    if (capacity < required) {
        fail(bank, DS_CUSTOM_WORDS_ERROR_BUFFER_TOO_SMALL);
        return 0;
    }
    uint8_t encoded[DS_CUSTOM_WORDS_BLOB_MAX_SIZE];
    const size_t size = encode_blob(bank, encoded);
    memcpy(output, encoded, size);
    bank->last_error = DS_CUSTOM_WORDS_ERROR_NONE;
    return size;
}
