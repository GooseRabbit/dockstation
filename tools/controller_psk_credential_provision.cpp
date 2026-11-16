#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <linux/fs.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <string_view>

#include "hardware/interfaces/controller_stable_identity.hpp"
#include "platform/linux/controller_psk_credential_store.hpp"

namespace {

using dockstation::platform::linux_platform::ControllerPskCredentials;
using dockstation::platform::linux_platform::ControllerPskCredentialFileStatus;
using dockstation::platform::linux_platform::kControllerCredentialDirectory;
using dockstation::platform::linux_platform::kControllerCredentialFileName;
using dockstation::platform::linux_platform::kControllerCredentialFileSize;
using dockstation::platform::linux_platform::load_controller_psk_credentials;

constexpr mode_t kDirectoryMode = 0750;
constexpr mode_t kFileMode = 0640;
constexpr char kAccessAclAttribute[] = "system.posix_acl_access";
constexpr char kDefaultAclAttribute[] = "system.posix_acl_default";

class ScopedFd final {
public:
    explicit ScopedFd(int descriptor = -1) noexcept : descriptor_(descriptor) {}
    ~ScopedFd() { reset(); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    [[nodiscard]] int get() const noexcept { return descriptor_; }
    [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }
    void reset(int descriptor = -1) noexcept {
        if (descriptor_ >= 0) {
            (void)close(descriptor_);
        }
        descriptor_ = descriptor;
    }
private:
    int descriptor_;
};

class SensitiveBytes final {
public:
    std::array<std::uint8_t, kControllerCredentialFileSize> value{};
    ~SensitiveBytes() { OPENSSL_cleanse(value.data(), value.size()); }
    SensitiveBytes() = default;
    SensitiveBytes(const SensitiveBytes&) = delete;
    SensitiveBytes& operator=(const SensitiveBytes&) = delete;
};

[[nodiscard]] bool has_no_xattr(int descriptor, const char* name) noexcept {
    errno = 0;
    const ssize_t size = fgetxattr(descriptor, name, nullptr, 0U);
    if (size >= 0) {
        return false;
    }
    return errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP;
}

[[nodiscard]] bool has_no_directory_acls(int descriptor) noexcept {
    return has_no_xattr(descriptor, kAccessAclAttribute) &&
           has_no_xattr(descriptor, kDefaultAclAttribute);
}

[[nodiscard]] bool has_no_file_acl(int descriptor) noexcept {
    return has_no_xattr(descriptor, kAccessAclAttribute);
}

[[nodiscard]] bool safe_parent_directory(int descriptor) noexcept {
    struct stat status {};
    return fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode) &&
           status.st_uid == 0U &&
           (status.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
           has_no_directory_acls(descriptor);
}

[[nodiscard]] bool exact_credentials_directory(
    int descriptor, gid_t service_group) noexcept {
    struct stat status {};
    return fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode) &&
           status.st_uid == 0U && status.st_gid == service_group &&
           (status.st_mode & 07777) == kDirectoryMode &&
           has_no_directory_acls(descriptor);
}

[[nodiscard]] bool lookup_service_group(gid_t* group_id) noexcept {
    if (group_id == nullptr) {
        return false;
    }
    char buffer[4096]{};
    struct group entry {};
    struct group* resolved = nullptr;
    const int result = getgrnam_r("dockstation", &entry, buffer,
                                 sizeof(buffer), &resolved);
    if (result != 0 || resolved == nullptr) {
        return false;
    }
    *group_id = resolved->gr_gid;
    return true;
}

[[nodiscard]] bool open_or_create_directory(
    int parent_fd, const char* name, gid_t service_group,
    ScopedFd* directory_fd) noexcept {
    if (directory_fd == nullptr) {
        return false;
    }
    int descriptor = openat(parent_fd, name,
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 && errno == ENOENT) {
        if (mkdirat(parent_fd, name, kDirectoryMode) != 0 && errno != EEXIST) {
            return false;
        }
        descriptor = openat(parent_fd, name,
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0) {
            return false;
        }
        if (fchown(descriptor, 0U, service_group) != 0 ||
            fchmod(descriptor, kDirectoryMode) != 0 ||
            fsync(descriptor) != 0 || fsync(parent_fd) != 0) {
            (void)close(descriptor);
            return false;
        }
    }
    if (descriptor < 0) {
        return false;
    }
    if (!exact_credentials_directory(descriptor, service_group)) {
        (void)close(descriptor);
        return false;
    }
    directory_fd->reset(descriptor);
    return true;
}

[[nodiscard]] bool open_credentials_directory(
    gid_t service_group, ScopedFd* directory_fd) noexcept {
    ScopedFd root_fd(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!root_fd.valid() || !safe_parent_directory(root_fd.get())) {
        return false;
    }

    ScopedFd etc_fd(openat(root_fd.get(), "etc",
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!etc_fd.valid() || !safe_parent_directory(etc_fd.get())) {
        return false;
    }

    ScopedFd dockstation_fd;
    if (!open_or_create_directory(etc_fd.get(), "dockstation", service_group,
                                  &dockstation_fd)) {
        return false;
    }
    return open_or_create_directory(dockstation_fd.get(), "credentials",
                                    service_group, directory_fd);
}

[[nodiscard]] bool write_all(int descriptor,
                             const std::uint8_t* bytes,
                             std::size_t size) noexcept {
    std::size_t offset = 0U;
    while (offset < size) {
        const ssize_t count = write(descriptor, bytes + offset, size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool write_random_temp_file(
    int directory_fd, gid_t service_group, const SensitiveBytes& bytes,
    char* temporary_name, std::size_t temporary_name_capacity) noexcept {
    std::array<unsigned char, 8U> suffix{};
    if (RAND_bytes(suffix.data(), static_cast<int>(suffix.size())) != 1) {
        OPENSSL_cleanse(suffix.data(), suffix.size());
        return false;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    char suffix_text[17]{};
    for (std::size_t index = 0U; index < suffix.size(); ++index) {
        suffix_text[index * 2U] = kHex[suffix[index] >> 4U];
        suffix_text[index * 2U + 1U] = kHex[suffix[index] & 0x0fU];
    }
    OPENSSL_cleanse(suffix.data(), suffix.size());

    char candidate_name[64]{};
    const int name_length = std::snprintf(
        candidate_name, sizeof(candidate_name),
        ".repka-esp32.psk.tmp.%ld.%s", static_cast<long>(getpid()), suffix_text);
    OPENSSL_cleanse(suffix_text, sizeof(suffix_text));
    if (name_length <= 0 ||
        static_cast<std::size_t>(name_length) >= sizeof(candidate_name) ||
        static_cast<std::size_t>(name_length) + 1U > temporary_name_capacity) {
        OPENSSL_cleanse(candidate_name, sizeof(candidate_name));
        return false;
    }

    ScopedFd file_fd(openat(directory_fd, candidate_name,
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                            0600));
    if (!file_fd.valid()) {
        OPENSSL_cleanse(candidate_name, sizeof(candidate_name));
        return false;
    }
    std::memcpy(temporary_name, candidate_name,
                static_cast<std::size_t>(name_length) + 1U);
    OPENSSL_cleanse(candidate_name, sizeof(candidate_name));

    // Keep the temporary file root-only while its secret bytes are being
    // written. Grant the dockstation group read access only after fsync.
    if (!write_all(file_fd.get(), bytes.value.data(), bytes.value.size()) ||
        fsync(file_fd.get()) != 0 ||
        fchown(file_fd.get(), 0U, service_group) != 0 ||
        fchmod(file_fd.get(), kFileMode) != 0 ||
        fsync(file_fd.get()) != 0) {
        return false;
    }

    struct stat status {};
    if (fstat(file_fd.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != 0U || status.st_gid != service_group ||
        (status.st_mode & 07777) != kFileMode || status.st_nlink != 1U ||
        status.st_size != static_cast<off_t>(kControllerCredentialFileSize) ||
        !has_no_file_acl(file_fd.get())) {
        return false;
    }
    return true;
}

[[nodiscard]] bool install_atomically(int directory_fd,
                                      const char* temporary_name,
                                      bool replace_existing) noexcept {
    if (replace_existing) {
        return renameat(directory_fd, temporary_name, directory_fd,
                        kControllerCredentialFileName) == 0;
    }
#if defined(SYS_renameat2)
    return syscall(SYS_renameat2, directory_fd, temporary_name,
                   directory_fd, kControllerCredentialFileName,
                   RENAME_NOREPLACE) == 0;
#else
    errno = ENOSYS;
    return false;
#endif
}

void print_usage() noexcept {
    (void)std::puts(
        "Usage:\n"
        "  dockstation-credential-provision [--replace]\n"
        "  dockstation-credential-provision --check --local-controller-id ID --peer-controller-id ID\n"
        "\n"
        "Generate a unique TLS 1.3 PSK and identity for the local Repka side.\n"
        "The 48-byte binary file is installed at the fixed credentials path.\n"
        "By default an existing file is never overwritten. --replace explicitly\n"
        "installs a new local credential pair after validating the existing file.\n"
        "--check validates the existing Repka credential file and both stable\n"
        "controller IDs. It is read-only: no files, ESP32 NVS or eFuses are changed.\n"
        "Secrets are never printed. ESP32 provisioning is a separate procedure.");
}

[[nodiscard]] int check_preflight(
    const char* local_controller_id,
    const char* peer_controller_id) noexcept {
    using dockstation::hardware::interfaces::is_valid_controller_stable_id;

    if (local_controller_id == nullptr || peer_controller_id == nullptr) {
        (void)std::fputs("ERROR: both controller IDs are required.\\n", stderr);
        return 2;
    }

    const std::string_view local_id{local_controller_id};
    const std::string_view peer_id{peer_controller_id};
    if (!is_valid_controller_stable_id(local_id) ||
        !is_valid_controller_stable_id(peer_id)) {
        (void)std::fputs(
            "ERROR: controller IDs must be 1-128 ASCII characters using letters, digits, '_', '-', '.' or ':'.\\n",
            stderr);
        return 2;
    }
    if (local_id == peer_id) {
        (void)std::fputs(
            "ERROR: local and peer controller IDs must be different.\\n",
            stderr);
        return 2;
    }

    if (geteuid() != 0U) {
        (void)std::fputs(
            "ERROR: run this read-only check as root to inspect the protected Repka credential file.\\n",
            stderr);
        return 1;
    }

    ControllerPskCredentials credentials{};
    const ControllerPskCredentialFileStatus status =
        load_controller_psk_credentials(&credentials);
    credentials.clear();
    if (status != ControllerPskCredentialFileStatus::Success) {
        (void)std::fputs(
            "ERROR: the existing Repka credential file is missing or failed its security/format checks.\\n",
            stderr);
        return 1;
    }

    (void)std::puts(
        "Preflight passed: the Repka credential file is valid and both controller IDs satisfy the shared format. No files were changed; ESP32 remains unprovisioned.");
    return 0;
}

[[nodiscard]] int provision(bool replace_existing) noexcept {
    if (geteuid() != 0U) {
        (void)std::fputs(
            "ERROR: run this utility as root; credentials are installed root-owned.\n",
            stderr);
        return 1;
    }

    gid_t service_group = 0U;
    if (!lookup_service_group(&service_group)) {
        (void)std::fputs(
            "ERROR: the dockstation service group does not exist.\n", stderr);
        return 1;
    }

    if (replace_existing) {
        ControllerPskCredentials previous{};
        const ControllerPskCredentialFileStatus status =
            load_controller_psk_credentials(&previous);
        previous.clear();
        if (status != ControllerPskCredentialFileStatus::Success) {
            (void)std::fputs(
                "ERROR: --replace requires an existing credential file that passes the credential-store checks.\n",
                stderr);
            return 1;
        }
    }

    ScopedFd directory_fd;
    if (!open_credentials_directory(service_group, &directory_fd)) {
        (void)std::fputs(
            "ERROR: cannot safely create or verify /etc/dockstation/credentials. Check root ownership, dockstation group, mode 0750, and ACLs.\n",
            stderr);
        return 1;
    }

    SensitiveBytes generated{};
    if (RAND_bytes(generated.value.data(), 32) != 1 ||
        RAND_bytes(generated.value.data() + 32U, 16) != 1) {
        (void)std::fputs("ERROR: cryptographic random generation failed.\n", stderr);
        return 1;
    }

    char temporary_name[64]{};
    if (!write_random_temp_file(directory_fd.get(), service_group,
                                generated, temporary_name,
                                sizeof(temporary_name))) {
        if (temporary_name[0] != '\0') {
            (void)unlinkat(directory_fd.get(), temporary_name, 0);
        }
        (void)std::fputs(
            "ERROR: could not create and verify the temporary credential file.\n",
            stderr);
        return 1;
    }
    generated.value.fill(0U);

    if (!install_atomically(directory_fd.get(), temporary_name,
                            replace_existing)) {
        (void)unlinkat(directory_fd.get(), temporary_name, 0);
        if (!replace_existing && errno == EEXIST) {
            (void)std::fputs(
                "ERROR: a credential file already exists; use --replace only after verifying that replacement is intended.\n",
                stderr);
        } else {
            (void)std::fputs(
                "ERROR: atomic installation failed; the existing credential file was not intentionally removed.\n",
                stderr);
        }
        return 1;
    }
    if (fsync(directory_fd.get()) != 0) {
        (void)std::fputs(
            "ERROR: credentials were atomically installed, but directory fsync failed; durability is uncertain.\n",
            stderr);
        return 1;
    }

    (void)std::puts(replace_existing
        ? "Local Repka credentials replaced successfully. ESP32 was not updated; do not enable the link until matching credentials are installed there."
        : "Local Repka credentials created successfully. ESP32 was not provisioned. Secret values were not displayed.");
    (void)std::printf("File: %s/%s\n", kControllerCredentialDirectory,
                      kControllerCredentialFileName);
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    (void)umask(0077);
    if (argc == 2 && argv[1] != nullptr &&
        (std::strcmp(argv[1], "--help") == 0 ||
         std::strcmp(argv[1], "-h") == 0)) {
        print_usage();
        return 0;
    }
    if (argc == 6 && argv[1] != nullptr &&
        std::strcmp(argv[1], "--check") == 0 &&
        argv[2] != nullptr &&
        std::strcmp(argv[2], "--local-controller-id") == 0 &&
        argv[3] != nullptr &&
        argv[4] != nullptr &&
        std::strcmp(argv[4], "--peer-controller-id") == 0 &&
        argv[5] != nullptr) {
        return check_preflight(argv[3], argv[5]);
    }
    if (argc == 1) {
        return provision(false);
    }
    if (argc == 2 && argv[1] != nullptr &&
        std::strcmp(argv[1], "--replace") == 0) {
        return provision(true);
    }
    print_usage();
    (void)std::fputs("ERROR: unsupported arguments.\n", stderr);
    return 2;
}
