// System-only DSU update. Never creates, formats, deletes or writes userdata_gsi.
#include <android/gsi/IGsiService.h>
#include <android/gsi/IImageService.h>
#include <android/gsi/MappedImage.h>
#include <binder/ParcelFileDescriptor.h>
#include <android-base/parseint.h>
#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <libgsi/libgsid.h>
#include <openssl/sha.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

using android::gsi::IGsiService;
using android::gsi::IImageService;

static bool valid_digest(const char* s) {
    if (std::strlen(s) != 64) return false;
    return std::strspn(s, "0123456789abcdef") == 64;
}

static std::string hash_image(const android::sp<IGsiService>& service, const std::string& slot, const char* name,
                              uint64_t expected_bytes) {
    android::sp<IImageService> images;
    if (!service->openImageService("dsu/"+slot+"/", &images).isOk() || !images) return {};
    bool was_mapped = true;
    if (!images->isImageMapped(name, &was_mapped).isOk()) return {};
    android::gsi::MappedImage mapping;
    auto status = was_mapped ? images->getMappedImageDevice(name, &mapping.path)
                             : images->mapImageDevice(name, 10000, &mapping);
    if (!status.isOk() || mapping.path.empty()) return {};
    std::string result;
    android::base::unique_fd fd(open(mapping.path.c_str(), O_RDONLY | O_CLOEXEC));
    uint64_t device_bytes = 0;
    if (fd >= 0 && ioctl(fd, BLKGETSIZE64, &device_bytes) == 0 && device_bytes == expected_bytes) {
        SHA256_CTX hash;
        SHA256_Init(&hash);
        std::array<unsigned char, 262144> buffer;
        uint64_t read_bytes = 0;
        ssize_t count;
        while ((count = TEMP_FAILURE_RETRY(read(fd, buffer.data(), buffer.size()))) > 0) {
            SHA256_Update(&hash, buffer.data(), count);
            read_bytes += count;
        }
        if (count == 0 && read_bytes == expected_bytes) {
            unsigned char digest[SHA256_DIGEST_LENGTH];
            SHA256_Final(digest, &hash);
            char hex[65];
            for (int i=0; i<SHA256_DIGEST_LENGTH; i++) std::sprintf(hex+i*2, "%02x", digest[i]);
            result = hex;
        }
    }
    fd.reset();
    if (!was_mapped && !images->unmapImageDevice(name).isOk()) return {};
    return result;
}

static void phase(const char* message) {
    std::fprintf(stderr, "DSU_SYSTEM_ONLY: %s\n", message);
    std::fflush(stderr);
}

int main(int argc, char** argv) {
    const bool check_only = (argc == 5 || argc == 6) && std::strcmp(argv[1], "--check-system-update") == 0;
    if ((argc != 5 && argc != 6) || (!check_only && std::strcmp(argv[1], "--replace-system")) || getuid() != 0 ||
        !valid_digest(argv[3]) || !valid_digest(argv[4]) ||
        android::base::GetProperty("ro.product.vendor.device", "") != "frost") return 2;
    int64_t size = 0;
    if (!android::base::ParseInt(argv[2], &size) || size < 512*1024*1024LL ||
        size > 3284570112LL || size % 4096) return 3;
    uint64_t userdata_bytes = 2147483648ULL;
    if (argc == 6 && (!android::base::ParseUint(argv[5], &userdata_bytes) ||
        (userdata_bytes != 2147483648ULL && userdata_bytes != 8589934592ULL))) return 3;
    auto service = android::gsi::GetGsiService();
    if (!service) return 4;
    bool running = true, installing = true;
    std::string slot;
    if (!service->isGsiRunning(&running).isOk() || running ||
        !service->isGsiInstallInProgress(&installing).isOk() || installing ||
        !service->getActiveDsuSlot(&slot).isOk() || (slot != "dsu" && slot != "c40grow")) return 5;
    phase("verifying inactive userdata against the offline host backup");
    const std::string before = hash_image(service, slot, "userdata_gsi", userdata_bytes);
    if (before.empty() || before != argv[4]) return 6;
    android::sp<IImageService> images;
    if (!service->openImageService("dsu/"+slot+"/", &images).isOk() || !images) return 7;
    bool mapped = true;
    if (!images->isImageMapped("system_gsi", &mapped).isOk() || mapped) return 8;
    if (check_only) {
        std::printf("{\"preflight_passed\":true,\"dsu_slot\":\"%s\",\"userdata_sha256\":\"%s\",\"userdata_bytes\":%llu,\"partitions_written\":false}\n",slot.c_str(),before.c_str(),static_cast<unsigned long long>(userdata_bytes));
        return 0;
    }
    bool ok = false;
    if (!service->disableGsi(&ok).isOk() || !ok) return 9;
    // Direct image-service operations keep the selected slot. Android11
    // openInstall() only permits the default internal directory and would
    // silently choose the wrong slot for a separately registered DSU.
    bool exists = false;
    if (!images->backingImageExists("system_gsi", &exists).isOk()) return 11;
    phase("replacing system_gsi only; userdata will not be created or formatted");
    if (exists && !images->deleteBackingImage("system_gsi").isOk()) return 12;
    if (!images->createBackingImage("system_gsi", size, IImageService::CREATE_IMAGE_READONLY, nullptr).isOk()) return 13;
    // Allocation must reach disk before writes bypass the backing filesystem.
    sync();
    android::gsi::MappedImage target;
    if (!images->mapImageDevice("system_gsi", 10000, &target).isOk()) return 14;
    struct Cleanup {
        android::sp<IImageService> images;
        bool owned = true;
        bool close() {
            if (!owned) return true;
            if (!images->unmapImageDevice("system_gsi").isOk()) return false;
            owned = false;
            return true;
        }
        ~Cleanup() { close(); }
    } cleanup{images};
    android::base::unique_fd output(open(target.path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
    uint64_t mapped_bytes = 0;
    if (output < 0 || ioctl(output, BLKGETSIZE64, &mapped_bytes) || mapped_bytes != static_cast<uint64_t>(size)) return 14;
    phase("streaming verified host system image");
    std::array<unsigned char, 262144> buffer;
    uint64_t remaining = size;
    while (remaining) {
        ssize_t n = TEMP_FAILURE_RETRY(read(STDIN_FILENO, buffer.data(), std::min<uint64_t>(buffer.size(), remaining)));
        if (n <= 0 || !android::base::WriteFully(output, buffer.data(), n)) return 15;
        remaining -= n;
    }
    unsigned char extra;
    if (TEMP_FAILURE_RETRY(read(STDIN_FILENO, &extra, 1)) != 0 || fsync(output)) return 15;
    output.reset();
    if (!cleanup.close()) return 15;
    phase("verifying the new mapped system and unchanged userdata");
    const std::string installed = hash_image(service, slot, "system_gsi", size);
    const std::string after = hash_image(service, slot, "userdata_gsi", userdata_bytes);
    if (installed.empty() || installed != argv[3] || after != before) return 16;
    int error = -1;
    if (!service->enableGsi(true, slot, &error).isOk() || error != IGsiService::INSTALL_OK) {
        service->disableGsi(&ok);
        return 18;
    }
    std::printf("{\"dsu_slot\":\"%s\",\"system_bytes\":%lld,\"system_sha256\":\"%s\",\"userdata_sha256\":\"%s\","
                "\"userdata_unchanged\":true,\"single_boot_enabled\":true,\"rebooted\":false}\n",
                slot.c_str(), static_cast<long long>(size), installed.c_str(), after.c_str());
    return 0;
}
