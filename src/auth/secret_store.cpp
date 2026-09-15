#include "secret_store.hpp"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <dpapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lanlink::auth::detail {
namespace {

constexpr std::array<std::byte, 4> identity_magic{
    std::byte{'L'},
    std::byte{'L'},
    std::byte{'I'},
    std::byte{'D'},
};
constexpr std::byte legacy_version{1};
constexpr std::byte protected_version{2};
constexpr std::size_t legacy_file_size = identity_magic.size() + 1 + secret_key_size;
#ifdef _WIN32
constexpr std::size_t protected_header_size = identity_magic.size() + 1 + sizeof(std::uint32_t);
#endif
constexpr std::size_t maximum_identity_file_size = 64U * 1024U;

void cleanse(const std::span<std::byte> bytes) noexcept {
    if (!bytes.empty()) {
        OPENSSL_cleanse(bytes.data(), bytes.size());
    }
}

[[noreturn]] void invalid_identity(const std::filesystem::path& path) {
    throw std::runtime_error("invalid device identity file: " + path.string());
}

void random_fill(const std::span<std::byte> output) {
    if (output.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        RAND_bytes(reinterpret_cast<unsigned char*>(output.data()),
                   static_cast<int>(output.size())) != 1) {
        throw std::runtime_error("secure random generation failed");
    }
}

std::string random_suffix() {
    constexpr std::string_view digits = "0123456789abcdef";
    std::array<std::byte, 8> random{};
    random_fill(random);
    std::string suffix;
    suffix.reserve(random.size() * 2);

    for (const auto byte : random) {
        const auto value = std::to_integer<unsigned char>(byte);
        suffix.push_back(digits[value >> 4U]);
        suffix.push_back(digits[value & 0x0fU]);
    }

    cleanse(random);
    return suffix;
}

void create_parent_directory(const std::filesystem::path& path) {
    const auto parent = path.parent_path();

    if (parent.empty()) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(parent, error);

    if (error) {
        throw std::runtime_error("cannot create identity directory: " + parent.string());
    }
}

std::filesystem::path temporary_path_for(const std::filesystem::path& path) {
    auto temporary = path;
    temporary += ".tmp-" + random_suffix();
    return temporary;
}

#ifdef _WIN32
void append_u32(std::vector<std::byte>& output, const std::uint32_t value) {
    output.push_back(std::byte{static_cast<unsigned char>((value >> 24U) & 0xffU)});
    output.push_back(std::byte{static_cast<unsigned char>((value >> 16U) & 0xffU)});
    output.push_back(std::byte{static_cast<unsigned char>((value >> 8U) & 0xffU)});
    output.push_back(std::byte{static_cast<unsigned char>(value & 0xffU)});
}

std::uint32_t read_u32(const std::span<const std::byte> input, const std::size_t offset) {
    return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(input[offset])) << 24U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(input[offset + 1])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(input[offset + 2])) << 8U) |
           static_cast<std::uint32_t>(std::to_integer<unsigned char>(input[offset + 3]));
}

[[noreturn]] void throw_windows_error(const std::string& message, const DWORD error) {
    throw std::system_error(static_cast<int>(error), std::system_category(), message);
}

class NativeHandle {
public:
    explicit NativeHandle(const HANDLE handle) noexcept : handle_(handle) {
    }

    ~NativeHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    HANDLE release() noexcept {
        const auto result = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return result;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class LocalDataBlob {
public:
    ~LocalDataBlob() {
        if (value_.pbData != nullptr) {
            SecureZeroMemory(value_.pbData, value_.cbData);
            LocalFree(value_.pbData);
        }
    }

    LocalDataBlob(const LocalDataBlob&) = delete;
    LocalDataBlob& operator=(const LocalDataBlob&) = delete;
    LocalDataBlob() = default;

    [[nodiscard]] DATA_BLOB* output() noexcept {
        return &value_;
    }

    [[nodiscard]] const DATA_BLOB& value() const noexcept {
        return value_;
    }

private:
    DATA_BLOB value_{};
};

DATA_BLOB protection_entropy() noexcept {
    static constexpr char entropy[] = "lanlink-device-identity-v2";
    return DATA_BLOB{
        static_cast<DWORD>(sizeof(entropy) - 1),
        reinterpret_cast<BYTE*>(const_cast<char*>(entropy)),
    };
}

std::vector<std::byte> protect_secret(const SecretKey& secret) {
    DATA_BLOB input{
        static_cast<DWORD>(secret.size()),
        reinterpret_cast<BYTE*>(const_cast<std::byte*>(secret.data())),
    };
    auto entropy = protection_entropy();
    LocalDataBlob protected_data;

    if (CryptProtectData(&input,
                         L"LanLink device identity",
                         &entropy,
                         nullptr,
                         nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN,
                         protected_data.output()) == FALSE) {
        throw_windows_error("cannot protect device identity", GetLastError());
    }

    const auto& value = protected_data.value();
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(value.pbData),
                                  reinterpret_cast<const std::byte*>(value.pbData + value.cbData));
}

SecretKey unprotect_secret(const std::span<const std::byte> protected_data) {
    if (protected_data.empty() ||
        protected_data.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        throw std::runtime_error("invalid protected device identity payload");
    }

    DATA_BLOB input{
        static_cast<DWORD>(protected_data.size()),
        reinterpret_cast<BYTE*>(const_cast<std::byte*>(protected_data.data())),
    };
    auto entropy = protection_entropy();
    LocalDataBlob clear_data;

    if (CryptUnprotectData(&input,
                           nullptr,
                           &entropy,
                           nullptr,
                           nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN,
                           clear_data.output()) == FALSE) {
        throw_windows_error("cannot decrypt device identity", GetLastError());
    }

    const auto& value = clear_data.value();

    if (value.cbData != secret_key_size || value.pbData == nullptr) {
        throw std::runtime_error("decrypted device identity has an invalid size");
    }

    SecretKey secret{};
    std::memcpy(secret.data(), value.pbData, secret.size());
    return secret;
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    NativeHandle file(CreateFileW(path.c_str(),
                                  GENERIC_READ,
                                  FILE_SHARE_READ,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr));

    if (file.get() == INVALID_HANDLE_VALUE) {
        throw_windows_error("cannot open device identity", GetLastError());
    }

    BY_HANDLE_FILE_INFORMATION information{};

    if (GetFileInformationByHandle(file.get(), &information) == FALSE) {
        throw_windows_error("cannot inspect device identity", GetLastError());
    }

    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        GetFileType(file.get()) != FILE_TYPE_DISK) {
        throw std::runtime_error("device identity is not a regular file: " + path.string());
    }

    LARGE_INTEGER size{};

    if (GetFileSizeEx(file.get(), &size) == FALSE) {
        throw_windows_error("cannot inspect device identity size", GetLastError());
    }

    if (size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > maximum_identity_file_size) {
        throw std::runtime_error("device identity file is too large: " + path.string());
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD bytes_read = 0;

    if (!bytes.empty() &&
        (ReadFile(file.get(),
                  bytes.data(),
                  static_cast<DWORD>(bytes.size()),
                  &bytes_read,
                  nullptr) == FALSE ||
         bytes_read != bytes.size())) {
        cleanse(bytes);
        throw_windows_error("cannot read device identity", GetLastError());
    }

    return bytes;
}

void atomic_write(const std::filesystem::path& path,
                  const std::span<const std::byte> bytes) {
    create_parent_directory(path);
    const auto temporary = temporary_path_for(path);
    bool installed = false;

    try {
        NativeHandle file(CreateFileW(temporary.c_str(),
                                      GENERIC_WRITE,
                                      0,
                                      nullptr,
                                      CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr));

        if (file.get() == INVALID_HANDLE_VALUE) {
            throw_windows_error("cannot create temporary device identity", GetLastError());
        }

        DWORD bytes_written = 0;

        if (WriteFile(file.get(),
                      bytes.data(),
                      static_cast<DWORD>(bytes.size()),
                      &bytes_written,
                      nullptr) == FALSE ||
            bytes_written != bytes.size()) {
            throw_windows_error("cannot write device identity", GetLastError());
        }

        if (FlushFileBuffers(file.get()) == FALSE) {
            throw_windows_error("cannot flush device identity", GetLastError());
        }

        const auto raw_handle = file.release();

        if (CloseHandle(raw_handle) == FALSE) {
            throw_windows_error("cannot close device identity", GetLastError());
        }

        if (MoveFileExW(temporary.c_str(),
                        path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
            throw_windows_error("cannot install device identity", GetLastError());
        }

        installed = true;
    } catch (...) {
        if (!installed) {
            DeleteFileW(temporary.c_str());
        }

        throw;
    }
}

#else

[[noreturn]] void throw_errno(const std::string& message) {
    throw std::system_error(errno, std::generic_category(), message);
}

class FileDescriptor {
public:
    explicit FileDescriptor(const int descriptor) noexcept : descriptor_(descriptor) {
    }

    ~FileDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

    int release() noexcept {
        const auto result = descriptor_;
        descriptor_ = -1;
        return result;
    }

private:
    int descriptor_ = -1;
};

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    FileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));

    if (file.get() < 0) {
        throw_errno("cannot open device identity");
    }

    struct stat status {};

    if (::fstat(file.get(), &status) != 0) {
        throw_errno("cannot inspect device identity");
    }

    if (!S_ISREG(status.st_mode)) {
        throw std::runtime_error("device identity is not a regular file: " + path.string());
    }

    if (status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > maximum_identity_file_size) {
        throw std::runtime_error("device identity file is too large: " + path.string());
    }

    if (::fchmod(file.get(), S_IRUSR | S_IWUSR) != 0) {
        throw_errno("cannot protect device identity permissions");
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;

    while (offset < bytes.size()) {
        const auto result = ::read(file.get(), bytes.data() + offset, bytes.size() - offset);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            cleanse(bytes);
            throw_errno("cannot read device identity");
        }

        if (result == 0) {
            cleanse(bytes);
            throw std::runtime_error("device identity changed while reading: " + path.string());
        }

        offset += static_cast<std::size_t>(result);
    }

    return bytes;
}

void sync_parent_directory(const std::filesystem::path& path) {
    auto parent = path.parent_path();

    if (parent.empty()) {
        parent = ".";
    }

    FileDescriptor directory(::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));

    if (directory.get() < 0) {
        throw_errno("cannot open identity directory");
    }

    if (::fsync(directory.get()) != 0) {
        throw_errno("cannot flush identity directory");
    }
}

void atomic_write(const std::filesystem::path& path,
                  const std::span<const std::byte> bytes) {
    create_parent_directory(path);
    const auto temporary = temporary_path_for(path);
    bool installed = false;

    try {
        FileDescriptor file(::open(temporary.c_str(),
                                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                                   S_IRUSR | S_IWUSR));

        if (file.get() < 0) {
            throw_errno("cannot create temporary device identity");
        }

        std::size_t offset = 0;

        while (offset < bytes.size()) {
            const auto result = ::write(file.get(), bytes.data() + offset, bytes.size() - offset);

            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }

                throw_errno("cannot write device identity");
            }

            if (result == 0) {
                throw std::runtime_error("cannot write complete device identity");
            }

            offset += static_cast<std::size_t>(result);
        }

        if (::fsync(file.get()) != 0) {
            throw_errno("cannot flush device identity");
        }

        const auto descriptor = file.release();

        if (::close(descriptor) != 0) {
            throw_errno("cannot close device identity");
        }

        if (::rename(temporary.c_str(), path.c_str()) != 0) {
            throw_errno("cannot install device identity");
        }

        installed = true;
        sync_parent_directory(path);
    } catch (...) {
        if (!installed) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }

        throw;
    }
}

#endif

bool decode_secret(const std::filesystem::path& path,
                   const std::span<const std::byte> bytes,
                   SecretKey& secret) {
    if (bytes.size() < identity_magic.size() + 1 ||
        !std::equal(identity_magic.begin(), identity_magic.end(), bytes.begin())) {
        invalid_identity(path);
    }

    if (bytes[identity_magic.size()] == legacy_version) {
        if (bytes.size() != legacy_file_size) {
            invalid_identity(path);
        }

        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(identity_magic.size() + 1),
                    secret.size(),
                    secret.begin());
#ifdef _WIN32
        return true;
#else
        return false;
#endif
    }

    if (bytes[identity_magic.size()] != protected_version) {
        invalid_identity(path);
    }

#ifdef _WIN32
    if (bytes.size() < protected_header_size) {
        invalid_identity(path);
    }

    const auto payload_size = read_u32(bytes, identity_magic.size() + 1);

    if (payload_size == 0 || payload_size > maximum_identity_file_size ||
        static_cast<std::size_t>(payload_size) != bytes.size() - protected_header_size) {
        invalid_identity(path);
    }

    auto clear_secret = unprotect_secret(bytes.subspan(protected_header_size));
    secret = clear_secret;
    cleanse(clear_secret);
    return false;
#else
    throw std::runtime_error("Windows-protected device identity cannot be used on this platform: " +
                             path.string());
#endif
}

std::vector<std::byte> encode_secret(const SecretKey& secret) {
    std::vector<std::byte> output;
    output.insert(output.end(), identity_magic.begin(), identity_magic.end());

#ifdef _WIN32
    auto protected_data = protect_secret(secret);

    if (protected_data.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("protected device identity is too large");
    }

    output.reserve(protected_header_size + protected_data.size());
    output.push_back(protected_version);
    append_u32(output, static_cast<std::uint32_t>(protected_data.size()));
    output.insert(output.end(), protected_data.begin(), protected_data.end());
#else
    output.reserve(legacy_file_size);
    output.push_back(legacy_version);
    output.insert(output.end(), secret.begin(), secret.end());
#endif

    return output;
}

}

bool load_device_secret(const std::filesystem::path& path, SecretKey& secret) {
    cleanse(secret);
    auto bytes = read_file(path);

    try {
        const auto requires_migration = decode_secret(path, bytes, secret);
        cleanse(bytes);
        return requires_migration;
    } catch (...) {
        cleanse(bytes);
        cleanse(secret);
        throw;
    }
}

void store_device_secret(const std::filesystem::path& path, const SecretKey& secret) {
    auto bytes = encode_secret(secret);

    try {
        atomic_write(path, bytes);
        cleanse(bytes);
    } catch (...) {
        cleanse(bytes);
        throw;
    }
}

}
