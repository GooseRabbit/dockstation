#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "application/linux/gateway_persistence_context.hpp"
#include "platform/linux/controller_psk_credential_store.hpp"
#include "platform/linux/runtime_config_file.hpp"

namespace {

using dockstation::application::linux_app::GatewayPersistenceContext;
using dockstation::application::linux_app::kGatewayDatabasePath;
using dockstation::core::SourceRegistrationLookupStatus;
using dockstation::core::SourceRegistrationState;
using dockstation::core::SubsystemRegistrationLookupStatus;
using dockstation::platform::linux_platform::ControllerPskCredentials;
using dockstation::platform::linux_platform::ControllerPskCredentialFileStatus;
using dockstation::platform::linux_platform::load_controller_psk_credentials;
using dockstation::platform::linux_runtime::RuntimeConfig;
using dockstation::platform::linux_runtime::RuntimeConfigFileStatus;
using dockstation::platform::linux_runtime::load_runtime_config_file;
using dockstation::platform::linux_runtime::validate_runtime_config;

constexpr char kConfigFile[] = "/etc/dockstation/gateway.conf";
constexpr char kDatabaseFileName[] = "dockstation.sqlite3";
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
            (void)::close(descriptor_);
        }
        descriptor_ = descriptor;
    }
private:
    int descriptor_;
};

struct ServiceIdentity final {
    uid_t user_id{0U};
    gid_t group_id{0U};
};

enum class RegistrationKind : std::uint8_t { Source, Subsystem };

struct Options final {
    RegistrationKind kind{RegistrationKind::Source};
    std::string_view revision{};
    std::string_view subsystem_id{};
    bool apply{false};
};

[[nodiscard]] bool lookup_service_identity(ServiceIdentity* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    char user_buffer[4096]{};
    struct passwd user_entry {};
    struct passwd* resolved_user = nullptr;
    const int user_result = ::getpwnam_r(
        "dockstation", &user_entry, user_buffer, sizeof(user_buffer),
        &resolved_user);
    if (user_result != 0 || resolved_user == nullptr) {
        return false;
    }

    char group_buffer[4096]{};
    struct group group_entry {};
    struct group* resolved_group = nullptr;
    const int group_result = ::getgrnam_r(
        "dockstation", &group_entry, group_buffer, sizeof(group_buffer),
        &resolved_group);
    if (group_result != 0 || resolved_group == nullptr ||
        resolved_user->pw_gid != resolved_group->gr_gid) {
        return false;
    }
    output->user_id = resolved_user->pw_uid;
    output->group_id = resolved_group->gr_gid;
    return true;
}

[[nodiscard]] bool has_no_xattr(int descriptor, const char* name) noexcept {
    errno = 0;
    const ssize_t result = ::fgetxattr(descriptor, name, nullptr, 0U);
    if (result >= 0) {
        return false;
    }
    return errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP;
}

[[nodiscard]] bool has_no_access_acl(int descriptor) noexcept {
    return has_no_xattr(descriptor, kAccessAclAttribute);
}

[[nodiscard]] bool has_no_directory_acls(int descriptor) noexcept {
    return has_no_access_acl(descriptor) &&
           has_no_xattr(descriptor, kDefaultAclAttribute);
}

[[nodiscard]] bool safe_config_directory(
    int descriptor, gid_t service_group) noexcept {
    struct stat status {};
    return ::fstat(descriptor, &status) == 0 &&
           S_ISDIR(status.st_mode) &&
           status.st_uid == 0U &&
           status.st_gid == service_group &&
           (status.st_mode & 07777) == 0750 &&
           has_no_directory_acls(descriptor);
}

[[nodiscard]] bool safe_config_file(gid_t service_group) noexcept {
    ScopedFd etc_fd(::open(
        "/etc", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!etc_fd.valid()) {
        return false;
    }
    struct stat etc_status {};
    if (::fstat(etc_fd.get(), &etc_status) != 0 ||
        !S_ISDIR(etc_status.st_mode) || etc_status.st_uid != 0U ||
        (etc_status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        !has_no_directory_acls(etc_fd.get())) {
        return false;
    }

    ScopedFd config_directory(::openat(
        etc_fd.get(), "dockstation",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!config_directory.valid() ||
        !safe_config_directory(config_directory.get(), service_group)) {
        return false;
    }

    ScopedFd config_file(::openat(
        config_directory.get(), "gateway.conf",
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!config_file.valid()) {
        return false;
    }
    struct stat file_status {};
    return ::fstat(config_file.get(), &file_status) == 0 &&
           S_ISREG(file_status.st_mode) &&
           file_status.st_uid == 0U &&
           file_status.st_gid == service_group &&
           (file_status.st_mode & 07777) == 0640 &&
           file_status.st_nlink == 1U &&
           file_status.st_size >= 0 &&
           file_status.st_size <= 65536 &&
           has_no_access_acl(config_file.get());
}

[[nodiscard]] bool safe_database_file(
    uid_t service_user, gid_t service_group) noexcept {
    ScopedFd var_fd(::open(
        "/var", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!var_fd.valid()) {
        return false;
    }
    struct stat var_status {};
    if (::fstat(var_fd.get(), &var_status) != 0 ||
        !S_ISDIR(var_status.st_mode) || var_status.st_uid != 0U ||
        (var_status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        !has_no_directory_acls(var_fd.get())) {
        return false;
    }

    ScopedFd lib_fd(::openat(
        var_fd.get(), "lib", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!lib_fd.valid()) {
        return false;
    }
    struct stat lib_status {};
    if (::fstat(lib_fd.get(), &lib_status) != 0 ||
        !S_ISDIR(lib_status.st_mode) || lib_status.st_uid != 0U ||
        (lib_status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        !has_no_directory_acls(lib_fd.get())) {
        return false;
    }

    ScopedFd state_directory(::openat(
        lib_fd.get(), "dockstation",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!state_directory.valid()) {
        return false;
    }
    struct stat directory_status {};
    if (::fstat(state_directory.get(), &directory_status) != 0 ||
        !S_ISDIR(directory_status.st_mode) ||
        directory_status.st_uid != service_user ||
        directory_status.st_gid != service_group ||
        (directory_status.st_mode & 07777) != 0700 ||
        !has_no_directory_acls(state_directory.get())) {
        return false;
    }

    ScopedFd database_file(::openat(
        state_directory.get(), kDatabaseFileName,
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!database_file.valid()) {
        // A missing DB is safe to create because the parent StateDirectory is
        // already verified as private and owned by the service account.
        return errno == ENOENT;
    }
    struct stat database_status {};
    return ::fstat(database_file.get(), &database_status) == 0 &&
           S_ISREG(database_status.st_mode) &&
           database_status.st_uid == service_user &&
           database_status.st_gid == service_group &&
           (database_status.st_mode & 07777) == 0600 &&
           database_status.st_nlink == 1U &&
           has_no_access_acl(database_file.get());
}

[[nodiscard]] bool valid_revision(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128U) {
        return false;
    }
    for (const unsigned char ch : value) {
        const bool alpha = (ch >= 'A' && ch <= 'Z') ||
                           (ch >= 'a' && ch <= 'z');
        const bool digit = ch >= '0' && ch <= '9';
        if (!alpha && !digit && ch != '_' && ch != '-' &&
            ch != '.' && ch != ':') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_subsystem_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128U) {
        return false;
    }
    std::size_t offset = 0U;
    while (offset < value.size()) {
        const std::size_t separator = value.find('/', offset);
        const std::size_t end = separator == std::string_view::npos
            ? value.size() : separator;
        const std::string_view component = value.substr(offset, end - offset);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        for (const unsigned char ch : component) {
            const bool alpha = (ch >= 'A' && ch <= 'Z') ||
                               (ch >= 'a' && ch <= 'z');
            const bool digit = ch >= '0' && ch <= '9';
            if (!alpha && !digit && ch != '_' && ch != '-' &&
                ch != '.' && ch != ':') {
                return false;
            }
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        offset = separator + 1U;
    }
    return false;
}

[[nodiscard]] bool parse_options(
    int argc, char* argv[], Options* options) noexcept {
    if (options == nullptr || argc < 2 || argv == nullptr ||
        argv[1] == nullptr) {
        return false;
    }
    const std::string_view command{argv[1]};
    if (command == "source") {
        options->kind = RegistrationKind::Source;
    } else if (command == "subsystem") {
        options->kind = RegistrationKind::Subsystem;
    } else {
        return false;
    }

    bool revision_seen = false;
    bool subsystem_seen = false;
    for (int index = 2; index < argc; ++index) {
        if (argv[index] == nullptr) {
            return false;
        }
        const std::string_view argument{argv[index]};
        if (argument == "--apply") {
            if (options->apply) {
                return false;
            }
            options->apply = true;
            continue;
        }
        if (argument == "--revision" || argument == "--subsystem-id") {
            if (index + 1 >= argc || argv[index + 1] == nullptr) {
                return false;
            }
            const std::string_view value{argv[++index]};
            if (argument == "--revision") {
                if (revision_seen || !valid_revision(value)) {
                    return false;
                }
                options->revision = value;
                revision_seen = true;
            } else {
                if (subsystem_seen || !valid_subsystem_id(value)) {
                    return false;
                }
                options->subsystem_id = value;
                subsystem_seen = true;
            }
            continue;
        }
        return false;
    }

    if (!revision_seen) {
        return false;
    }
    return options->kind == RegistrationKind::Source
        ? !subsystem_seen : subsystem_seen;
}

void print_usage() noexcept {
    (void)std::puts(
        "Usage:\n"
        "  dockstation-registry-provision source --revision <revision> [--apply]\n"
        "  dockstation-registry-provision subsystem --subsystem-id <id> --revision <revision> [--apply]\n"
        "\n"
        "Station ID and source are read from /etc/dockstation/gateway.conf.\n"
        "The 16-byte PSK identity is read from the protected local credential file.\n"
        "The PSK secret is never displayed or stored in the registry.\n"
        "Without --apply the utility validates and previews the operation only.\n"
        "Registrations are create-only: duplicates are rejected, never overwritten.\n"
        "Run as the dockstation service account, for example:\n"
        "  sudo -u dockstation -- /usr/local/bin/dockstation-registry-provision source --revision r1");
}

void print_identity_hex(const ControllerPskCredentials& credentials) noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    char output[33]{};
    for (std::size_t index = 0U; index < credentials.identity.size(); ++index) {
        const std::uint8_t byte = credentials.identity[index];
        output[index * 2U] = kHex[byte >> 4U];
        output[index * 2U + 1U] = kHex[byte & 0x0fU];
    }
    (void)std::printf("%s", output);
}

[[nodiscard]] int run(const Options& options) noexcept {
    ServiceIdentity service{};
    if (!lookup_service_identity(&service)) {
        (void)std::fputs(
            "ERROR: the dockstation service account/group is unavailable.\n",
            stderr);
        return 1;
    }
    if (::geteuid() != service.user_id) {
        (void)std::fputs(
            "ERROR: run this utility as the dockstation service account, not as root or another user.\n"
            "Use: sudo -u dockstation -- /usr/local/bin/dockstation-registry-provision ...\n",
            stderr);
        return 1;
    }
    if (!safe_config_file(service.group_id)) {
        (void)std::fputs(
            "ERROR: /etc/dockstation/gateway.conf must be a root:dockstation regular file, mode 0640, without links or ACLs.\n",
            stderr);
        return 1;
    }

    const auto config_result = load_runtime_config_file(kConfigFile);
    if (config_result.status != RuntimeConfigFileStatus::Loaded) {
        (void)std::fprintf(
            stderr, "ERROR: cannot load %s: %s (field=%s, line=%zu).\n",
            kConfigFile, config_result.reason.c_str(),
            config_result.field.c_str(), config_result.error_line);
        return 1;
    }
    const RuntimeConfig& config = config_result.config;
    const auto config_validation = validate_runtime_config(config);
    if (config_validation.status !=
        dockstation::platform::linux_runtime::RuntimeConfigValidationStatus::Valid) {
        (void)std::fprintf(
            stderr, "ERROR: gateway configuration is invalid: %s: %s.\n",
            config_validation.field.c_str(), config_validation.reason.c_str());
        return 1;
    }
    if (config.station_id.empty() || config.controller_source.empty()) {
        (void)std::fputs(
            "ERROR: station_id and controller_source must both be configured in gateway.conf before provisioning.\n",
            stderr);
        return 1;
    }

    if (!safe_database_file(service.user_id, service.group_id)) {
        (void)std::fputs(
            "ERROR: /var/lib/dockstation must be an existing dockstation-owned 0700 StateDirectory; an existing database must be a dockstation-owned 0600 regular file without links or ACLs.\n"
            "Run the standard Gateway installer to create the private StateDirectory before provisioning.\n",
            stderr);
        return 1;
    }

    ControllerPskCredentials credentials{};
    const ControllerPskCredentialFileStatus credential_status =
        load_controller_psk_credentials(&credentials);
    if (credential_status != ControllerPskCredentialFileStatus::Success) {
        (void)std::fprintf(
            stderr, "ERROR: cannot load the protected local PSK identity (credential-store status=%u).\n",
            static_cast<unsigned>(credential_status));
        return 1;
    }

    const std::string_view identity{
        reinterpret_cast<const char*>(credentials.identity.data()),
        credentials.identity.size()};

    GatewayPersistenceContext persistence{};
    const auto database_result = persistence.open(kGatewayDatabasePath);
    if (!database_result) {
        (void)std::fprintf(
            stderr, "ERROR: cannot open the local DockStation SQLite database (status=%u, sqlite=%d).\n",
            static_cast<unsigned>(database_result.status),
            database_result.sqlite_status);
        return 1;
    }
    if (!safe_database_file(service.user_id, service.group_id)) {
        (void)std::fputs(
            "ERROR: the opened/created SQLite database failed the required dockstation owner, 0600 mode, no-link and no-ACL checks.\n",
            stderr);
        return 1;
    }

    bool source_identity_conflict = false;
    bool source_identity_error = false;
    bool source_binding_valid = false;
    bool subsystem_conflict = false;
    bool subsystem_lookup_error = false;
    bool write_succeeded = false;
    bool write_conflict = false;
    bool write_error = false;
    bool source_registration_missing = false;
    bool source_registration_mismatch = false;
    bool source_registration_inactive = false;
    unsigned write_status = 0U;
    int sqlite_status = 0;

    const bool stores_available = persistence.with_serialized_stores(
        [&](auto& source_store, auto& subsystem_store) {
            const auto source_record =
                source_store.find_by_peer_identity(identity);

            if (options.kind == RegistrationKind::Source) {
                if (source_record.status == SourceRegistrationLookupStatus::Found) {
                    source_identity_conflict = true;
                    return;
                }
                if (source_record.status != SourceRegistrationLookupStatus::NotFound) {
                    source_identity_error = true;
                    return;
                }
                source_binding_valid = true;
                if (!options.apply) {
                    return;
                }
                const auto result = source_store.create_source_registration(
                    config.station_id, config.controller_source,
                    options.revision, identity);
                write_status = static_cast<unsigned>(result.status);
                sqlite_status = result.sqlite_status;
                if (result.status ==
                    dockstation::platform::persistence::SqliteSourceRegistrationWriteStatus::Created) {
                    write_succeeded = true;
                } else if (result.status ==
                           dockstation::platform::persistence::SqliteSourceRegistrationWriteStatus::Conflict) {
                    write_conflict = true;
                } else {
                    write_error = true;
                }
                return;
            }

            if (source_record.status == SourceRegistrationLookupStatus::NotFound) {
                source_registration_missing = true;
                return;
            }
            if (source_record.status != SourceRegistrationLookupStatus::Found) {
                source_identity_error = true;
                return;
            }
            if (source_record.record.station_id != config.station_id ||
                source_record.record.source != config.controller_source) {
                source_registration_mismatch = true;
                return;
            }
            if (source_record.record.state != SourceRegistrationState::Active) {
                source_registration_inactive = true;
                return;
            }

            const auto subsystem_record = subsystem_store.find_subsystem(
                config.station_id, options.subsystem_id);
            if (subsystem_record.status == SubsystemRegistrationLookupStatus::Found) {
                subsystem_conflict = true;
                return;
            }
            if (subsystem_record.status != SubsystemRegistrationLookupStatus::NotFound) {
                subsystem_lookup_error = true;
                return;
            }
            source_binding_valid = true;
            if (!options.apply) {
                return;
            }
            const auto result = subsystem_store.create_subsystem_registration(
                config.station_id, options.subsystem_id,
                config.controller_source, options.revision);
            write_status = static_cast<unsigned>(result.status);
            sqlite_status = result.sqlite_status;
            if (result.status ==
                dockstation::platform::persistence::SqliteSubsystemRegistrationWriteStatus::Created) {
                write_succeeded = true;
            } else if (result.status ==
                       dockstation::platform::persistence::SqliteSubsystemRegistrationWriteStatus::Conflict) {
                write_conflict = true;
            } else {
                write_error = true;
            }
        });

    if (!stores_available) {
        (void)std::fputs("ERROR: SQLite persistence is not open; no registration was written.\n", stderr);
        return 1;
    }

    (void)std::puts("Validated local registration request:");
    (void)std::printf("  Station ID: %s\n", config.station_id.c_str());
    (void)std::printf("  Source: %s\n", config.controller_source.c_str());
    if (options.kind == RegistrationKind::Subsystem) {
        (void)std::printf("  Subsystem ID: %.*s\n",
                          static_cast<int>(options.subsystem_id.size()),
                          options.subsystem_id.data());
    }
    (void)std::printf("  Registration revision: %.*s\n",
                      static_cast<int>(options.revision.size()),
                      options.revision.data());
    (void)std::printf("  Local PSK identity (16-byte hex): ");
    print_identity_hex(credentials);
    (void)std::puts("\n  Requested initial state: ACTIVE");

    if (source_identity_conflict) {
        (void)std::fputs(
            "ERROR: this PSK identity is already registered. Existing registrations are never overwritten.\n",
            stderr);
        return 3;
    }
    if (source_identity_error || subsystem_lookup_error) {
        (void)std::fputs(
            "ERROR: the existing source registry could not be read reliably; refusing to continue.\n",
            stderr);
        return 1;
    }
    if (source_registration_missing) {
        (void)std::fputs(
            "ERROR: register the source first using the source command; subsystem registration never creates a source implicitly.\n",
            stderr);
        return 3;
    }
    if (source_registration_mismatch) {
        (void)std::fputs(
            "ERROR: the local PSK identity is registered to a different Station ID or source than gateway.conf.\n",
            stderr);
        return 3;
    }
    if (source_registration_inactive) {
        (void)std::fputs(
            "ERROR: the source registration is not ACTIVE; disabled/revoked entries are not reactivated by this utility.\n",
            stderr);
        return 3;
    }
    if (subsystem_conflict) {
        (void)std::fputs(
            "ERROR: this Station ID/subsystem ID already exists. Existing registrations are never overwritten.\n",
            stderr);
        return 3;
    }
    if (!source_binding_valid) {
        (void)std::fputs("ERROR: registration preconditions were not satisfied.\n", stderr);
        return 1;
    }

    if (!options.apply) {
        (void)std::puts(
            "PREVIEW ONLY: no source/subsystem registration was created. Add --apply to commit this exact request.");
        return 0;
    }
    if (write_conflict) {
        (void)std::fprintf(
            stderr, "ERROR: a source or subsystem registration already exists (status=%u, sqlite=%d); nothing was overwritten.\n",
            write_status, sqlite_status);
        return 3;
    }
    if (write_error) {
        (void)std::fprintf(
            stderr, "ERROR: SQLite refused the registration (status=%u, sqlite=%d); no partial registration was committed.\n",
            write_status, sqlite_status);
        return 1;
    }
    if (!write_succeeded) {
        (void)std::fputs("ERROR: registration result was not confirmed.\n", stderr);
        return 1;
    }
    (void)std::puts(
        "SUCCESS: registration created atomically in the local SQLite registry. Existing records were not modified.");
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    (void)::umask(0077);
    if (argc == 2 && argv[1] != nullptr &&
        (std::strcmp(argv[1], "--help") == 0 ||
         std::strcmp(argv[1], "-h") == 0)) {
        print_usage();
        return 0;
    }

    Options options{};
    if (!parse_options(argc, argv, &options)) {
        print_usage();
        (void)std::fputs("ERROR: invalid or incomplete arguments.\n", stderr);
        return 2;
    }
    return run(options);
}
