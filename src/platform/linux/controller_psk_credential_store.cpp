#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "platform/linux/controller_psk_credential_store.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace dockstation::platform::linux_platform {
namespace {

constexpr mode_t kCredentialDirectoryMode = 0750;
constexpr mode_t kCredentialFileMode = 0640;
constexpr mode_t kForbiddenWritableBits = S_IWGRP | S_IWOTH;
constexpr char kAccessAclAttribute[] = "system.posix_acl_access";
constexpr char kDefaultAclAttribute[] = "system.posix_acl_default";

struct SensitiveFileBuffer final {
    std::array<std::uint8_t, kControllerCredentialFileSize> bytes{};

    ~SensitiveFileBuffer() {
        OPENSSL_cleanse(bytes.data(), bytes.size());
    }
};

[[nodiscard]] bool is_zero_filled(
    const std::uint8_t* bytes, std::size_t size) noexcept {
    std::uint8_t combined = 0U;
    for (std::size_t index = 0U; index < size; ++index) {
        combined = static_cast<std::uint8_t>(combined | bytes[index]);
    }
    return combined == 0U;
}

[[nodiscard]] bool has_no_acl(int file_descriptor,
                              const char* attribute) noexcept {
    errno = 0;
    const ssize_t result = fgetxattr(file_descriptor, attribute, nullptr, 0U);
    if (result >= 0) {
        return false;
    }
    return errno == ENODATA || errno == ENOTSUP;
}

[[nodiscard]] bool directory_has_no_acl(int file_descriptor) noexcept {
    return has_no_acl(file_descriptor, kAccessAclAttribute) &&
           has_no_acl(file_descriptor, kDefaultAclAttribute);
}

[[nodiscard]] bool file_has_no_acl(int file_descriptor) noexcept {
    return has_no_acl(file_descriptor, kAccessAclAttribute);
}

[[nodiscard]] bool lookup_service_group(gid_t* group_id) noexcept {
    if (group_id == nullptr) {
        return false;
    }
    char buffer[4096]{};
    struct group entry {};
    struct group* resolved = nullptr;
    const int result = getgrnam_r(
        "dockstation", &entry, buffer, sizeof(buffer), &resolved);
    if (result != 0 || resolved == nullptr) {
        return false;
    }
    *group_id = resolved->gr_gid;
    return true;
}

[[nodiscard]] bool stat_is_unchanged(
    const struct stat& before, const struct stat& after) noexcept {
    return before.st_dev == after.st_dev &&
           before.st_ino == after.st_ino &&
           before.st_mode == after.st_mode &&
           before.st_uid == after.st_uid &&
           before.st_gid == after.st_gid &&
           before.st_nlink == after.st_nlink &&
           before.st_size == after.st_size &&
           before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

[[nodiscard]] ControllerPskCredentialFileStatus open_verified_directory(
    const char* path, gid_t service_group, int* directory_fd) noexcept {
    if (path == nullptr || directory_fd == nullptr || path[0] != '/') {
        return ControllerPskCredentialFileStatus::InvalidArgument;
    }
    const std::size_t path_length = std::strlen(path);
    if (path_length < 2U || path_length >= static_cast<std::size_t>(PATH_MAX) ||
        path[path_length - 1U] == '/') {
        return ControllerPskCredentialFileStatus::InvalidArgument;
    }

    char path_copy[PATH_MAX]{};
    std::memcpy(path_copy, path, path_length + 1U);

    int current_fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (current_fd < 0) {
        return ControllerPskCredentialFileStatus::DirectoryUnavailable;
    }

    struct stat root_status {};
    if (fstat(current_fd, &root_status) != 0 ||
        !S_ISDIR(root_status.st_mode) || root_status.st_uid != 0U ||
        (root_status.st_mode & kForbiddenWritableBits) != 0 ||
        !directory_has_no_acl(current_fd)) {
        close(current_fd);
        return ControllerPskCredentialFileStatus::DirectoryPermissionsInvalid;
    }

    std::size_t cursor = 1U;
    bool found_component = false;
    while (cursor < path_length) {
        const std::size_t component_start = cursor;
        while (cursor < path_length && path_copy[cursor] != '/') {
            ++cursor;
        }
        const std::size_t component_length = cursor - component_start;
        if (component_length == 0U || component_length > NAME_MAX) {
            close(current_fd);
            return ControllerPskCredentialFileStatus::InvalidArgument;
        }

        char component[NAME_MAX + 1U]{};
        std::memcpy(component, path_copy + component_start, component_length);
        if (std::strcmp(component, ".") == 0 ||
            std::strcmp(component, "..") == 0) {
            close(current_fd);
            return ControllerPskCredentialFileStatus::InvalidArgument;
        }

        const bool is_final_component = cursor == path_length;
        const int next_fd = openat(
            current_fd, component,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (next_fd < 0) {
            close(current_fd);
            return ControllerPskCredentialFileStatus::DirectoryUnavailable;
        }

        struct stat status {};
        const bool metadata_read = fstat(next_fd, &status) == 0;
        const mode_t permission_bits = status.st_mode & 07777;
        const bool common_policy =
            metadata_read && S_ISDIR(status.st_mode) &&
            status.st_uid == 0U &&
            (status.st_mode & kForbiddenWritableBits) == 0 &&
            directory_has_no_acl(next_fd);
        const bool final_policy =
            !is_final_component ||
            (status.st_gid == service_group &&
             permission_bits == kCredentialDirectoryMode);

        if (!common_policy || !final_policy) {
            close(next_fd);
            close(current_fd);
            return ControllerPskCredentialFileStatus::DirectoryPermissionsInvalid;
        }

        close(current_fd);
        current_fd = next_fd;
        found_component = true;
        if (cursor < path_length) {
            ++cursor;
            if (cursor == path_length || path_copy[cursor] == '/') {
                close(current_fd);
                return ControllerPskCredentialFileStatus::InvalidArgument;
            }
        }
    }

    if (!found_component) {
        close(current_fd);
        return ControllerPskCredentialFileStatus::InvalidArgument;
    }
    *directory_fd = current_fd;
    return ControllerPskCredentialFileStatus::Success;
}

[[nodiscard]] ControllerPskCredentialFileStatus read_credentials_file(
    int directory_fd, gid_t service_group,
    ControllerPskCredentials* credentials) noexcept {
    const int file_fd = openat(
        directory_fd, kControllerCredentialFileName,
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (file_fd < 0) {
        return (errno == ELOOP || errno == EACCES)
            ? ControllerPskCredentialFileStatus::CredentialFilePermissionsInvalid
            : ControllerPskCredentialFileStatus::CredentialFileUnavailable;
    }

    struct stat before {};
    if (fstat(file_fd, &before) != 0) {
        close(file_fd);
        return ControllerPskCredentialFileStatus::CredentialFileReadFailed;
    }

    const bool file_policy =
        S_ISREG(before.st_mode) && before.st_uid == 0U &&
        before.st_gid == service_group &&
        (before.st_mode & 07777) == kCredentialFileMode &&
        before.st_nlink == 1U &&
        before.st_size == static_cast<off_t>(kControllerCredentialFileSize) &&
        file_has_no_acl(file_fd);
    if (!file_policy) {
        close(file_fd);
        return (before.st_size != static_cast<off_t>(kControllerCredentialFileSize))
            ? ControllerPskCredentialFileStatus::CredentialFileFormatInvalid
            : ControllerPskCredentialFileStatus::CredentialFilePermissionsInvalid;
    }

    SensitiveFileBuffer file_buffer{};
    std::size_t offset = 0U;
    while (offset < file_buffer.bytes.size()) {
        const ssize_t count = pread(
            file_fd, file_buffer.bytes.data() + offset,
            file_buffer.bytes.size() - offset, static_cast<off_t>(offset));
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        close(file_fd);
        return count == 0
            ? ControllerPskCredentialFileStatus::CredentialFileFormatInvalid
            : ControllerPskCredentialFileStatus::CredentialFileReadFailed;
    }

    std::uint8_t extra_byte = 0U;
    ssize_t extra_count = 0;
    do {
        extra_count = pread(file_fd, &extra_byte, 1U,
                            static_cast<off_t>(file_buffer.bytes.size()));
    } while (extra_count < 0 && errno == EINTR);

    struct stat after {};
    const bool final_metadata_read = fstat(file_fd, &after) == 0;
    close(file_fd);

    if (extra_count != 0 || !final_metadata_read ||
        !stat_is_unchanged(before, after)) {
        return extra_count > 0
            ? ControllerPskCredentialFileStatus::CredentialFileFormatInvalid
            : ControllerPskCredentialFileStatus::CredentialFileReadFailed;
    }

    std::memcpy(credentials->psk.data(),
                file_buffer.bytes.data(), credentials->psk.size());
    std::memcpy(credentials->identity.data(),
                file_buffer.bytes.data() + credentials->psk.size(),
                credentials->identity.size());

    if (is_zero_filled(credentials->psk.data(), credentials->psk.size()) ||
        is_zero_filled(credentials->identity.data(), credentials->identity.size())) {
        credentials->clear();
        return ControllerPskCredentialFileStatus::CredentialFileFormatInvalid;
    }
    return ControllerPskCredentialFileStatus::Success;
}

}  // namespace

ControllerPskCredentialFileStatus load_controller_psk_credentials(
    ControllerPskCredentials* credentials) noexcept {
    return load_controller_psk_credentials_from_directory(
        kControllerCredentialDirectory, credentials);
}

ControllerPskCredentialFileStatus load_controller_psk_credentials_from_directory(
    const char* absolute_directory,
    ControllerPskCredentials* credentials) noexcept {
    if (credentials == nullptr) {
        return ControllerPskCredentialFileStatus::InvalidArgument;
    }
    credentials->clear();
    if (absolute_directory == nullptr || absolute_directory[0] != '/') {
        return ControllerPskCredentialFileStatus::InvalidArgument;
    }

    gid_t service_group = 0U;
    if (!lookup_service_group(&service_group)) {
        return ControllerPskCredentialFileStatus::DirectoryPermissionsInvalid;
    }

    int directory_fd = -1;
    const ControllerPskCredentialFileStatus directory_status =
        open_verified_directory(absolute_directory, service_group, &directory_fd);
    if (directory_status != ControllerPskCredentialFileStatus::Success) {
        return directory_status;
    }

    const ControllerPskCredentialFileStatus result =
        read_credentials_file(directory_fd, service_group, credentials);
    close(directory_fd);
    if (result != ControllerPskCredentialFileStatus::Success) {
        credentials->clear();
    }
    return result;
}

}  // namespace dockstation::platform::linux_platform
