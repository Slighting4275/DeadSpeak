#undef NDEBUG
#define main ds_build_driver_main
#include "../tools/build.c"
#undef main
#include <assert.h>

static void expect_rejection(void (*operation)(void)) {
    fflush(NULL);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) { operation(); _exit(0); }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 1);
}

static EVP_PKEY *test_key;
static X509 *test_cert;
static void save_test_identity(void) { save_identity(test_key, test_cert); }
static char audit_fixture[PATH_MAX];
static void audit_test_fixture(void) {
    char *abis[] = {"arm64-v8a", "armeabi-v7a", "x86_64"};
    audit_apk(audit_fixture, abis, 3);
}

static void make_fixture(const char *source, unsigned mutation) {
    struct archive *reader = archive_read_new(), *writer = archive_write_new();
    assert(reader && writer);
    archive_ok(archive_read_support_format_zip(reader), reader);
    archive_ok(archive_read_open_filename(reader, source, 65536), reader);
    archive_ok(archive_write_set_format_zip(writer), writer);
    archive_ok(archive_write_set_options(writer, "zip:compression=store,zip:zip64="), writer);
    archive_ok(archive_write_open_filename(writer, audit_fixture), writer);
    struct archive_entry *entry;
    bool changed = false;
    int result;
    while ((result = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        bool selected = !strcmp(name, "lib/arm64-v8a/libdeadspeak.so");
        const char *output_name = name;
        if (selected && mutation == 1) output_name = "lib/not-an-abi/payload.bin";
        if (selected && mutation == 2) output_name = "lib/../../escape.so";
        if (selected && mutation == 3) output_name = "lib/x86_64/libdeadspeak.so";
        header(writer, output_name, archive_entry_size(entry));
        char bytes[65536];
        la_ssize_t n;
        bool first = true;
        while ((n = archive_read_data(reader, bytes, sizeof(bytes))) > 0) {
            if (selected && first && mutation == 4) bytes[18] = 40; /* Wrong ELF architecture. */
            if (selected && first && mutation == 5) memcpy(bytes, "dex\n", 4);
            if (selected && first && mutation == 6) bytes[16] = 2; /* ET_EXEC is not a shared object. */
            bytes_to_archive(writer, bytes, (size_t)n);
            first = false;
        }
        assert(n == 0);
        if (selected) changed = true;
    }
    assert(result == ARCHIVE_EOF && changed);
    archive_ok(archive_read_close(reader), reader);
    archive_read_free(reader);
    archive_ok(archive_write_close(writer), writer);
    archive_write_free(writer);
}

int main(void) {
    assert(forbidden("assets/test.DEX") && forbidden("assets/test.Class") && forbidden("assets/test.JAR"));
    assert(!allowed_resource("assets/../bad.bin") && !allowed_resource("lib/../../escape.so"));
    char source[PATH_MAX];
    assert(realpath("build/deadspeak-debug.apk", source));
    char *abis[] = {"arm64-v8a", "armeabi-v7a", "x86_64"};
    audit_apk(source, abis, 3);
    char scratch[] = "build/build-test-XXXXXX";
    assert(mkdtemp(scratch));
    char absolute_scratch[PATH_MAX];
    assert(realpath(scratch, absolute_scratch));
    assert(chdir(scratch) == 0);

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    assert(ctx && EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0 &&
        EVP_PKEY_keygen(ctx, &test_key) > 0);
    EVP_PKEY_CTX_free(ctx);
    test_cert = X509_new();
    assert(test_cert && X509_set_version(test_cert, 2) == 1 && X509_set_pubkey(test_cert, test_key) == 1 &&
        X509_gmtime_adj(X509_getm_notBefore(test_cert), 0) && X509_gmtime_adj(X509_getm_notAfter(test_cert), 86400));
    X509_NAME *name = X509_get_subject_name(test_cert);
    assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, BAD_CAST "Native build test", -1, -1, 0) == 1 &&
        X509_set_issuer_name(test_cert, name) == 1 && X509_sign(test_cert, test_key, EVP_sha256()) > 0);

    directory(".signing");
    assert(write_identity_file(".signing/cert.pem", NULL, test_cert));
    expect_rejection(save_test_identity);
    assert(access(".signing/key.pem", F_OK) != 0 && access(".signing/cert.pem", R_OK) == 0);
    assert(unlink(".signing/cert.pem") == 0 && rmdir(".signing") == 0);
    save_identity(test_key, test_cert);
    validate_identity();
    expect_rejection(save_test_identity);
    assert(chmod(".signing/key.pem", 0644) == 0);
    expect_rejection(validate_identity);
    assert(chmod(".signing/key.pem", 0600) == 0 && rename(".signing/key.pem", "key.pem") == 0);
    assert(symlink("../key.pem", ".signing/key.pem") == 0);
    expect_rejection(validate_identity);
    assert(unlink(".signing/key.pem") == 0 && rename("key.pem", ".signing/key.pem") == 0);
    validate_identity();

    path(audit_fixture, "%s/fixture.apk", absolute_scratch);
    for (unsigned i = 1; i <= 6; ++i) {
        make_fixture(source, i);
        expect_rejection(audit_test_fixture);
    }
    assert(unlink(audit_fixture) == 0);
    assert(unlink(".signing/key.pem") == 0 && unlink(".signing/cert.pem") == 0 && rmdir(".signing") == 0);
    assert(chdir("..") == 0 && rmdir(strrchr(absolute_scratch, '/') + 1) == 0);
    EVP_PKEY_free(test_key);
    X509_free(test_cert);
    puts("PASS: native build identity transactions, permissions, symlinks, strict library paths and ELF architecture guards");
    return 0;
}
