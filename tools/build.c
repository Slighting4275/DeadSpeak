#define _GNU_SOURCE
#include "build_config.h"
#include <archive.h>
#include <archive_entry.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Native-only orchestrator. Tools are executed directly, never via a shell. */
[[noreturn]] static void fail(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("DeadSpeak build: ", stderr);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static void path(char *out, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(out, PATH_MAX, format, args);
    va_end(args);
    if (n < 0 || n >= PATH_MAX) fail("path is too long");
}

static void directory(const char *name) {
    if (mkdir(name, 0750) != 0 && errno != EEXIST) fail("mkdir %s: %s", name, strerror(errno));
    struct stat st;
    if (stat(name, &st) != 0 || !S_ISDIR(st.st_mode)) fail("not a directory: %s", name);
}

static void run(char *const argv[]) {
    printf("+ %s", argv[0]);
    for (size_t i = 1; argv[i]; ++i) printf(" %s", argv[i]);
    putchar('\n');
    fflush(stdout);
    pid_t child = fork();
    if (child < 0) fail("fork: %s", strerror(errno));
    if (child == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "Cannot execute %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) fail("waitpid: %s", strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) fail("%s failed", argv[0]);
}

#define RUN(...) do { char *const command[] = {__VA_ARGS__, NULL}; run(command); } while (0)

static bool property(const char *key, char value[PATH_MAX]) {
    FILE *file = fopen("local.properties", "r");
    if (!file) return false;
    char line[PATH_MAX + 128];
    bool found = false;
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, key, strlen(key)) == 0 && line[strlen(key)] == '=') {
            line[strcspn(line, "\r\n")] = '\0';
            path(value, "%s", line + strlen(key) + 1);
            found = true;
            break;
        }
    }
    if (ferror(file)) fail("reading local.properties failed");
    fclose(file);
    return found;
}

static bool write_identity_file(const char *name, EVP_PKEY *key, X509 *cert) {
    int fd = open(name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    FILE *out = fdopen(fd, "w");
    if (!out) { close(fd); return false; }
    bool written = key ? PEM_write_PrivateKey(out, key, NULL, NULL, 0, NULL, NULL) == 1 : PEM_write_X509(out, cert) == 1;
    bool ok = written && fflush(out) == 0 && fsync(fd) == 0;
    if (fclose(out) != 0) ok = false;
    return ok;
}

static void save_identity(EVP_PKEY *key, X509 *cert) {
    if (!key || !cert || X509_check_private_key(cert, key) != 1) fail("signing key and certificate do not match");
    int bits = EVP_PKEY_get_bits(key);
    if (!EVP_PKEY_is_a(key, "RSA") || (bits != 2048 && bits != 4096 && bits != 8192 && bits != 16384))
        fail("signer requires an Android-supported RSA size: 2048, 4096, 8192, or 16384 bits");
    struct stat st;
    if (lstat(".signing", &st) == 0) fail(".signing already exists; identity creation never alters an existing directory");
    if (errno != ENOENT) fail("inspect signing directory: %s", strerror(errno));
    char staging[] = ".signing-stage-XXXXXX", key_path[PATH_MAX], cert_path[PATH_MAX];
    if (!mkdtemp(staging)) fail("create private identity stage: %s", strerror(errno));
    path(key_path, "%s/key.pem", staging);
    path(cert_path, "%s/cert.pem", staging);
    bool ok = write_identity_file(key_path, key, NULL) && write_identity_file(cert_path, NULL, cert);
    if (ok) ok = renameat2(AT_FDCWD, staging, AT_FDCWD, ".signing", RENAME_NOREPLACE) == 0;
    if (!ok) {
        int saved_error = errno;
        /* These exact files belong to the private stage created above. */
        unlink(key_path);
        unlink(cert_path);
        rmdir(staging);
        fail("publishing signing identity failed; existing identity unchanged: %s", strerror(saved_error));
    }
    puts("Saved native PEM signing identity in ignored .signing/; back it up privately.");
}

static void validate_identity(void) {
    struct stat st;
    if (lstat(".signing", &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022))
        fail(".signing must be a real owned directory, not writable by group or others");
    const char *names[] = {".signing/key.pem", ".signing/cert.pem"};
    for (size_t i = 0; i < 2; ++i) {
        int fd = open(names[i], O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) fail("open signing identity without symlinks: %s", strerror(errno));
        bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0077);
        close(fd);
        if (!valid) fail("signing identity must be an owned regular file with private permissions: %s", names[i]);
    }
}

static void import_identity(const char *source) {
    FILE *file = fopen(source, "rb");
    if (!file) fail("open PKCS#12: %s", strerror(errno));
    PKCS12 *store = d2i_PKCS12_fp(file, NULL);
    fclose(file);
    if (!store) fail("not a PKCS#12 container; Java keystore formats are not supported");
    const char *password = getenv("DS_PKCS12_PASSWORD");
    if (!password) fail("set DS_PKCS12_PASSWORD to import the PKCS#12 identity");
    EVP_PKEY *key = NULL;
    X509 *cert = NULL;
    if (PKCS12_parse(store, password, &key, &cert, NULL) != 1) fail("PKCS#12 password or container is invalid");
    save_identity(key, cert);
    EVP_PKEY_free(key);
    X509_free(cert);
    PKCS12_free(store);
}

static void create_identity(void) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    EVP_PKEY *key = NULL;
    if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 4096) <= 0 ||
        EVP_PKEY_keygen(ctx, &key) <= 0) fail("RSA key generation failed");
    EVP_PKEY_CTX_free(ctx);
    X509 *cert = X509_new();
    if (!cert || X509_set_version(cert, 2) != 1 || ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(cert), -86400) ||
        !X509_gmtime_adj(X509_getm_notAfter(cert), 30L * 365 * 86400) || X509_set_pubkey(cert, key) != 1)
        fail("creating signing certificate failed");
    X509_NAME *name = X509_NAME_new();
    if (!name || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"DeadSpeak local build", -1, -1, 0) != 1 ||
        X509_set_subject_name(cert, name) != 1 || X509_set_issuer_name(cert, name) != 1 || X509_sign(cert, key, EVP_sha256()) <= 0) {
        X509_NAME_free(name);
        fail("signing certificate failed");
    }
    X509_NAME_free(name);
    save_identity(key, cert);
    EVP_PKEY_free(key);
    X509_free(cert);
}

static bool allowed_resource(const char *name) {
    if (!name || strstr(name, "..") || name[0] == '/') return false;
    return strcmp(name, "AndroidManifest.xml") == 0 || strcmp(name, "resources.arsc") == 0 ||
        strncmp(name, "res/", 4) == 0 || strncmp(name, "assets/", 7) == 0;
}

static bool forbidden(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".dex") == 0 || strcasecmp(dot, ".class") == 0 || strcasecmp(dot, ".jar") == 0 ||
        strcasecmp(dot, ".java") == 0 || strcasecmp(dot, ".kt") == 0 || strcasecmp(dot, ".kts") == 0);
}

static void archive_ok(int result, struct archive *a) {
    if (result != ARCHIVE_OK) fail("archive: %s", archive_error_string(a));
}

static void bytes_to_archive(struct archive *writer, const char *bytes, size_t length) {
    while (length) {
        la_ssize_t n = archive_write_data(writer, bytes, length);
        if (n <= 0 || (size_t)n > length) fail("writing archive data: %s", archive_error_string(writer));
        bytes += (size_t)n;
        length -= (size_t)n;
    }
}

static void header(struct archive *writer, const char *name, la_int64_t size) {
    struct archive_entry *entry = archive_entry_new();
    if (!entry) fail("out of memory");
    archive_entry_set_pathname(entry, name);
    archive_entry_set_size(entry, size);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_mtime(entry, 315532800, 0); /* Fixed 1980 ZIP timestamp. */
    archive_ok(archive_write_header(writer, entry), writer);
    archive_entry_free(entry);
}

static void add_file(struct archive *writer, const char *source, const char *name) {
    FILE *file = fopen(source, "rb");
    if (!file) fail("open %s: %s", source, strerror(errno));
    struct stat st;
    if (fstat(fileno(file), &st) != 0 || !S_ISREG(st.st_mode)) fail("not a regular file: %s", source);
    header(writer, name, st.st_size);
    char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) != 0) bytes_to_archive(writer, buffer, n);
    if (ferror(file)) fail("read %s failed", source);
    fclose(file);
}


static void package_apk(char *const abis[], size_t abi_count) {
    struct archive *reader = archive_read_new(), *writer = archive_write_new();
    if (!reader || !writer) fail("out of memory");
    archive_ok(archive_read_support_format_zip(reader), reader);
    archive_ok(archive_read_open_filename(reader, "build/resources.apk", 65536), reader);
    archive_ok(archive_write_set_format_zip(writer), writer);
    archive_ok(archive_write_set_options(writer, "zip:compression=store,zip:zip64="), writer);
    archive_ok(archive_write_open_filename(writer, "build/unsigned.apk"), writer);
    struct archive_entry *entry;
    unsigned manifests = 0, tables = 0, banks = 0;
    int result;
    while ((result = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (!allowed_resource(name) || forbidden(name) || archive_entry_filetype(entry) != AE_IFREG)
            fail("unexpected or executable resource entry: %s", name ? name : "(null)");
        manifests += strcmp(name, "AndroidManifest.xml") == 0;
        tables += strcmp(name, "resources.arsc") == 0;
        banks += strcmp(name, "assets/words.bin") == 0;
        header(writer, name, archive_entry_size(entry));
        char buffer[65536];
        la_ssize_t n;
        while ((n = archive_read_data(reader, buffer, sizeof(buffer))) > 0) bytes_to_archive(writer, buffer, (size_t)n);
        if (n < 0) fail("reading resources: %s", archive_error_string(reader));
    }
    if (result != ARCHIVE_EOF || manifests != 1 || tables != 1 || banks != 1) fail("resource package is incomplete");
    archive_ok(archive_read_close(reader), reader);
    archive_read_free(reader);
    for (size_t i = 0; i < abi_count; ++i) {
        char source[PATH_MAX], name[PATH_MAX];
        path(source, "build/native/%s/libdeadspeak.so", abis[i]);
        path(name, "lib/%s/libdeadspeak.so", abis[i]);
        add_file(writer, source, name);
    }
    archive_ok(archive_write_close(writer), writer);
    archive_write_free(writer);
}

static void audit_apk(const char *filename, char *const abis[], size_t abi_count) {
    struct archive *reader = archive_read_new();
    if (!reader) fail("out of memory");
    archive_ok(archive_read_support_format_zip(reader), reader);
    archive_ok(archive_read_open_filename(reader, filename, 65536), reader);
    struct archive_entry *entry;
    if (abi_count > 3) fail("invalid ABI count");
    bool seen[3] = {false};
    unsigned manifests = 0, tables = 0, banks = 0;
    size_t libs = 0;
    int result;
    while ((result = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (!name || forbidden(name) || archive_entry_filetype(entry) != AE_IFREG)
            fail("forbidden APK entry: %s", name ? name : "(null)");
        size_t library_index = abi_count;
        if (!allowed_resource(name)) {
            for (size_t i = 0; i < abi_count; ++i) {
                char expected[PATH_MAX];
                path(expected, "lib/%s/libdeadspeak.so", abis[i]);
                if (!strcmp(name, expected)) { library_index = i; break; }
            }
            if (library_index == abi_count || seen[library_index]) fail("unexpected or duplicate native library: %s", name);
            seen[library_index] = true;
            ++libs;
        }
        manifests += !strcmp(name, "AndroidManifest.xml");
        tables += !strcmp(name, "resources.arsc");
        banks += !strcmp(name, "assets/words.bin");
        unsigned char magic[64];
        la_ssize_t n = archive_read_data(reader, magic, sizeof(magic));
        if (n < 0) fail("APK entry read failed");
        if ((n >= 4 && memcmp(magic, "\xca\xfe\xba\xbe", 4) == 0) ||
            (n >= 4 && memcmp(magic, "dex\n", 4) == 0) ||
            (n >= 4 && memcmp(magic, "PK\003\004", 4) == 0))
            fail("bytecode or nested archive disguised as %s", name);
        if (library_index != abi_count) {
            const char *abi = abis[library_index];
            unsigned expected_machine = !strcmp(abi, "arm64-v8a") ? 183 : !strcmp(abi, "armeabi-v7a") ? 40 : 62;
            unsigned expected_class = !strcmp(abi, "armeabi-v7a") ? 1 : 2;
            size_t minimum_header = expected_class == 1 ? 52 : 64;
            if (n < (la_ssize_t)minimum_header || memcmp(magic, "\177ELF", 4) || magic[4] != expected_class ||
                magic[5] != 1 || magic[6] != 1 || magic[16] != 3 || magic[17] != 0 ||
                (unsigned)(magic[18] | (unsigned)magic[19] << 8) != expected_machine ||
                magic[20] != 1 || magic[21] || magic[22] || magic[23])
                fail("native library is not an ELF shared object for %s: %s", abi, name);
        }
        archive_ok(archive_read_data_skip(reader), reader);
    }
    if (result != ARCHIVE_EOF || libs != abi_count || manifests != 1 || tables != 1 || banks != 1) fail("APK audit failed");
    archive_ok(archive_read_close(reader), reader);
    archive_read_free(reader);
    puts("APK audit passed: native libraries and data only; no DEX, Java classes, or JARs.");
}

#include "platform_symbols.h"

int main(int argc, char **argv) {
    if (access("tools/build.c", R_OK) != 0) fail("run from the repository root using sh tools/build.sh");
    if (argc == 3 && strcmp(argv[1], "--import-pkcs12") == 0) { import_identity(argv[2]); return 0; }
    if (argc == 2 && strcmp(argv[1], "--init-key") == 0) { create_identity(); return 0; }
    char *all_abis[] = {"arm64-v8a", "armeabi-v7a", "x86_64"};
    char *const *abis = all_abis;
    size_t abi_count = 3;
    if (argc == 3 && strcmp(argv[1], "--abi") == 0) {
        bool valid = false;
        for (size_t i = 0; i < 3; ++i) valid |= strcmp(argv[2], all_abis[i]) == 0;
        if (!valid) fail("unsupported ABI: %s", argv[2]);
        abis = &argv[2];
        abi_count = 1;
    } else if (argc != 1) {
        fail("usage: sh tools/build.sh [--abi arm64-v8a|armeabi-v7a|x86_64 | --import-pkcs12 FILE | --init-key]");
    }
    if (access(".signing/key.pem", R_OK) != 0 || access(".signing/cert.pem", R_OK) != 0)
        fail("configure a signing identity with --import-pkcs12 FILE or --init-key; no identity is chosen silently");
    validate_identity();
    char sdk[PATH_MAX], zig_lib[PATH_MAX], tools[PATH_MAX], ndk[PATH_MAX], aapt[PATH_MAX], zipalign[PATH_MAX];
    const char *sdk_env = getenv("DS_SDK_DIR");
    if (sdk_env && *sdk_env) path(sdk, "%s", sdk_env);
    else if (!property("sdk.dir", sdk)) fail("configure sdk.dir in local.properties or DS_SDK_DIR");
    const char *zig_env = getenv("ZIG_LIB_DIR");
    if (zig_env && *zig_env) path(zig_lib, "%s", zig_env);
    else if (!property("deadspeak.zigLibDir", zig_lib)) fail("configure the matching Zig library explicitly");
    path(tools, "%s/build-tools/%s", sdk, DS_BUILD_TOOLS_VERSION);
    path(ndk, "%s/ndk/%s", sdk, DS_NDK_VERSION);
    path(aapt, "%s/aapt2", tools);
    path(zipalign, "%s/zipalign", tools);
    if (access(aapt, X_OK) != 0 || access(zipalign, X_OK) != 0) fail("installed native SDK build tools are required");
    directory("build");
    directory("build/host");
    directory("build/native");
    directory("build/native-platform");

    ds_platform_symbols(sdk);
    RUN(aapt, "compile", "--dir", "build/native-platform/res", "-o", "build/native-platform/public-resources.zip");
    RUN(aapt, "link", "-x", "--allow-reserved-package-id", "--no-compile-sdk-metadata", "--manifest", "tools/platform/AndroidManifest.xml",
        "--min-sdk-version", DS_MIN_API, "--target-sdk-version", "37", "-o", "build/native-platform/framework-res.apk",
        "build/native-platform/public-resources.zip");
    RUN("zig", "cc", "-std=c23", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIE", "-pie", "-fstack-protector-strong", "-Wl,-z,relro,-z,now", "tools/make_words.c", "-o", "build/host/make-words");
    RUN("build/host/make-words");
    RUN("zig", "cc", "-std=c23", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIE", "-pie", "-fstack-protector-strong", "-Wl,-z,relro,-z,now", "tools/apk_sign.c", "-lcrypto", "-o", "build/host/apk-sign");
    for (size_t i = 0; i < abi_count; ++i) {
        char output[PATH_MAX], toolchain[PATH_MAX], abi[PATH_MAX], android_ndk[PATH_MAX], library[PATH_MAX];
        path(output, "build/native/%s", abis[i]);
        path(toolchain, "-DCMAKE_TOOLCHAIN_FILE=%s/build/cmake/android.toolchain.cmake", ndk);
        path(abi, "-DANDROID_ABI=%s", abis[i]);
        path(android_ndk, "-DANDROID_NDK=%s", ndk);
        path(library, "-DDS_ZIG_LIB_DIR=%s", zig_lib);
        RUN("cmake", "-S", "app/src/main/cpp", "-B", output, "-G", "Ninja", toolchain, abi, android_ndk, library,
            "-DANDROID_PLATFORM=android-30", "-DCMAKE_BUILD_TYPE=Release");
        RUN("cmake", "--build", output, "--parallel", "2");
    }
    RUN(aapt, "compile", "--dir", "app/src/main/res", "-o", "build/app-resources.zip");
    RUN(aapt, "link", "--manifest", "app/src/main/AndroidManifest.xml", "-I", "build/native-platform/framework-res.apk",
        "-A", "app/src/main/assets", "--debug-mode", "--no-compile-sdk-metadata", "-o", "build/resources.apk", "build/app-resources.zip");
    package_apk((char *const *)abis, abi_count);
    RUN(zipalign, "-P", "16", "-f", "4", "build/unsigned.apk", "build/aligned.apk");
    /* Never replace the last verified APK with partial/failed build output. */
    char staging_dir[] = "build/signing-XXXXXX", signed_apk[PATH_MAX];
    if (!mkdtemp(staging_dir)) fail("create signing stage: %s", strerror(errno));
    path(signed_apk, "%s/deadspeak.apk", staging_dir);
    RUN("build/host/apk-sign", "sign", "build/aligned.apk", signed_apk, ".signing/key.pem", ".signing/cert.pem");
    RUN("build/host/apk-sign", "verify", signed_apk);
    RUN(zipalign, "-c", "-P", "16", "4", signed_apk);
    audit_apk(signed_apk, (char *const *)abis, abi_count);
    if (rename(signed_apk, "build/deadspeak-debug.apk") != 0) fail("publish verified APK: %s", strerror(errno));
    if (rmdir(staging_dir) != 0) fail("remove empty signing stage: %s", strerror(errno));
    puts("Built build/deadspeak-debug.apk without a JVM, Java/Kotlin compiler, Gradle, D8, or Java archive dependencies.");
    return 0;
}
