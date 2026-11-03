// Read-only verification/export through DSU's actual block mapping.
#include <android/gsi/IImageService.h>
#include <android/gsi/MappedImage.h>
#include <android-base/properties.h>
#include <android-base/file.h>
#include <android-base/unique_fd.h>
#include <libgsi/libgsid.h>
#include <openssl/sha.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2 || getuid() != 0 ||
        android::base::GetProperty("ro.product.vendor.device", "") != "frost") return 2;
    bool export_userdata = std::strcmp(argv[1], "--export-userdata") == 0;
    bool size_only = std::strcmp(argv[1], "--userdata-size") == 0;
    bool userdata = export_userdata || size_only;
    if (!userdata && (std::strlen(argv[1]) != 64 || std::strspn(argv[1], "0123456789abcdef") != 64)) return 2;
    auto service = android::gsi::GetGsiService();
    if (!service) return 3;
    bool running = true;
    if (export_userdata && (!service->isGsiRunning(&running).isOk() || running)) return 11;
    const char* image = userdata ? "userdata_gsi" : "system_gsi";
    android::sp<android::gsi::IImageService> images;
    std::string slot;
    if (!android::base::ReadFileToString("/metadata/gsi/dsu/active", &slot) || (slot!="dsu" && slot!="c40grow")) return 4;
    if (!service->openImageService("dsu/"+slot+"/", &images).isOk() || !images) return 4;
    bool was_mapped = false;
    if (!images->isImageMapped(image, &was_mapped).isOk()) return 5;
    android::gsi::MappedImage mapping;
    auto status = was_mapped ? images->getMappedImageDevice(image, &mapping.path)
                             : images->mapImageDevice(image, 10000, &mapping);
    if (!status.isOk() || mapping.path.empty()) return 6;
    int result = 0;
    android::base::unique_fd fd(open(mapping.path.c_str(), O_RDONLY | O_CLOEXEC));
    uint64_t expected_bytes = 0;
    if (fd < 0 || ioctl(fd, BLKGETSIZE64, &expected_bytes)) result = 7;
    else if ((userdata && expected_bytes != 2147483648ULL && expected_bytes != 8589934592ULL) ||
             (!userdata && (expected_bytes < 512*1024*1024ULL || expected_bytes > 3284570112ULL))) result = 13;
    else if (size_only) {
        std::printf("{\"userdata_bytes\":%llu,\"read_only\":true,\"contents_read\":false}\n",
                    static_cast<unsigned long long>(expected_bytes));
    } else {
        SHA256_CTX sha;
        SHA256_Init(&sha);
        std::array<unsigned char, 262144> buffer;
        unsigned long long bytes = 0;
        ssize_t count;
        while ((count = TEMP_FAILURE_RETRY(read(fd, buffer.data(), buffer.size()))) > 0) {
            SHA256_Update(&sha, buffer.data(), count);
            bytes += count;
            if (export_userdata && std::fwrite(buffer.data(), 1, count, stdout) != static_cast<size_t>(count)) {
                result = 12;
                break;
            }
        }
        if (count < 0) result = 8;
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256_Final(digest, &sha);
        char hex[65];
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) std::sprintf(hex+i*2, "%02x", digest[i]);
        bool matches = bytes == expected_bytes && (export_userdata || std::strcmp(hex, argv[1]) == 0);
        if (export_userdata && std::fflush(stdout)) result = 12;
        std::fprintf(export_userdata ? stderr : stdout,
                     "{\"block_device\":\"%s\",\"bytes\":%llu,\"sha256\":\"%s\",\"matches_source\":%s}\n",
                     mapping.path.c_str(), bytes, hex, matches ? "true" : "false");
        if (!matches && !result) result = 9;
    }
    fd.reset();
    if (!was_mapped && !images->unmapImageDevice(image).isOk() && !result) result = 10;
    return result;
}
