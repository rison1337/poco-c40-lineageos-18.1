// Read known diagnostic transactions from the hash-pinned stock QTI interface.
// Layout/transaction evidence: build-environment/compatibility/gnss-debug-hidl.
// Never reads position vectors, starts navigation, or injects assistance.
#include <android/hidl/manager/1.0/IServiceManager.h>
#include <hidl/HidlBinderSupport.h>
#include <hidl/ServiceManagement.h>
#include <hidl/Status.h>
#include <hwbinder/IBinder.h>
#include <hwbinder/Parcel.h>
#include <linux/android/binder.h>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <limits>

using android::sp;
using android::hardware::IBinder;
using android::hardware::Parcel;
using android::hardware::Status;
static constexpr const char* kService = "vendor.qti.gnss@4.0::ILocHidlGnss";
static constexpr const char* kGnss = "vendor.qti.gnss@1.0::ILocHidlGnss";
static constexpr const char* kDebug = "vendor.qti.gnss@1.0::ILocHidlDebugReportService";

template<typename T> static T field(const uint8_t* buffer, size_t offset) {
    T value;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

static bool statusOk(const Parcel& reply) {
    Status status;
    return android::hardware::readFromParcel(&status, reply) == android::OK && status.isOk();
}

static int failure(const char* stage) {
    std::printf("{\"error\":\"%s\",\"coordinates_recorded\":false}\n", stage);
    return 2;
}

int main() {
    auto manager = android::hardware::defaultServiceManager();
    if (manager == nullptr) return failure("service_manager_unavailable");
    auto serviceResult = manager->get(kService, "gnss_vendor");
    if (!serviceResult.isOk()) return failure("service_lookup_failed");
    sp<android::hidl::base::V1_0::IBase> service = serviceResult;
    if (service == nullptr) return failure("vendor_gnss_unavailable");
    sp<IBinder> gnss = android::hardware::toBinder(service);
    if (gnss == nullptr) return failure("vendor_binder_unavailable");
    Parcel request, reply;
    if (request.writeInterfaceToken(kGnss) != android::OK ||
        gnss->transact(20, request, &reply) != android::OK || !statusOk(reply))
        return failure("debug_extension_transaction_failed");
    sp<IBinder> debug;
    if (reply.readNullableStrongBinder(&debug) != android::OK || debug == nullptr)
        return failure("debug_extension_unavailable");
    // Stock init only attaches the existing SystemStatus singleton. Its
    // implementation contains no navigation/configuration calls. Do not deinit
    // the shared service, which might be used by another diagnostics client.
    Parcel initRequest, initReply;
    bool initialized = false;
    if (initRequest.writeInterfaceToken(kDebug) != android::OK ||
        debug->transact(1, initRequest, &initReply) != android::OK ||
        !statusOk(initReply) || initReply.readBool(&initialized) != android::OK || !initialized)
        return failure("diagnostic_cache_attachment_failed");
    Parcel reportRequest, reportReply;
    if (reportRequest.writeInterfaceToken(kDebug) != android::OK ||
        reportRequest.writeInt32(1) != android::OK ||
        debug->transact(3, reportRequest, &reportReply) != android::OK || !statusOk(reportReply))
        return failure("report_transaction_failed");
    size_t parent = 0;
    const void* rawHeader = nullptr;
    if (reportReply.readBuffer(216, &parent, &rawHeader) != android::OK || rawHeader == nullptr)
        return failure("report_header_unavailable");
    auto header = static_cast<const uint8_t*>(rawHeader);
    if (header[0] != 1) return failure("diagnostic_report_not_valid");
    constexpr size_t missing = std::numeric_limits<size_t>::max();
    struct Section { const char* name; size_t offset, stride, parcelPosition; };
    Section sections[] = {{"time_clock", 24, 64, missing}, {"xo_state", 40, 40, missing},
        {"rf", 56, 104, missing}, {"error_recovery", 72, 40, missing},
        {"position_failure", 200, 40, missing}};
    // Parcel buffer objects are serialized sequentially. Locate the selected
    // child descriptors without dereferencing any position/string payloads.
    if (reportReply.objectsCount() > 64 || parent >= reportReply.objectsCount())
        return failure("unexpected_parcel_object_count");
    // Null buffer descriptors are not included in objectsCount(), but still
    // occupy space in the serialized parcel. Consume them as descriptors too.
    for (size_t i = 0; reportReply.dataAvail() != 0; i++) {
        if (i >= 64 || reportReply.dataAvail() < sizeof(binder_buffer_object))
            return failure("unexpected_parcel_tail");
        const size_t position = reportReply.dataPosition();
        const auto* object = reportReply.readObject<binder_buffer_object>();
        if (object == nullptr || object->hdr.type != BINDER_TYPE_PTR)
            return failure("unexpected_parcel_object_type");
        if (object->flags == BINDER_BUFFER_FLAG_HAS_PARENT && object->parent == parent) {
            for (auto& section : sections) {
                if (object->parent_offset == section.offset) {
                    if (section.parcelPosition != missing) return failure("duplicate_report_section");
                    section.parcelPosition = position;
                }
            }
        }
    }
    std::ostringstream out;
    out << "{\"coordinates_recorded\":false,\"starts_or_stops_gps\":false,"
           "\"injection_performed\":false,\"latest_cached_reports\":true";
    for (const auto& section : sections) {
        uint32_t count = field<uint32_t>(header, section.offset + 8);
        if (count > 1) return failure("unexpected_report_count");
        if (section.parcelPosition == missing) {
            std::fprintf(stderr, "missing_section=%s count=%u\n", section.name, count);
            return failure("report_section_missing");
        }
        reportReply.setDataPosition(section.parcelPosition);
        const void* raw = nullptr;
        size_t child = 0;
        const auto result = reportReply.readNullableEmbeddedBuffer(count * section.stride, &child, parent,
                                                                   section.offset, &raw);
        if (result != android::OK) {
            std::fprintf(stderr, "section=%s count=%u status=%d\n", section.name, count, result);
            return failure("report_vector_unavailable");
        }
        if (count != 0 && raw == nullptr) return failure("report_vector_null");
        out << ",\"" << section.name << "\":[";
        if (count != 0) {
            const auto* data = static_cast<const uint8_t*>(raw);
            out << "{\"updated_utc_seconds\":" << field<int64_t>(data, 0)
                << ",\"updated_utc_nanoseconds\":" << field<int64_t>(data, 8)
                << ",\"reported_utc_seconds\":" << field<int64_t>(data, 16)
                << ",\"reported_utc_nanoseconds\":" << field<int64_t>(data, 24);
            if (std::strcmp(section.name, "rf") == 0) {
                const char* labels[] = {"pga_gain", "gps_baseband_i", "gps_baseband_q", "adc_i", "adc_q",
                    "jammer_gps", "jammer_glonass", "jammer_beidou", "jammer_galileo"};
                out << ",\"pga_gain\":" << field<int32_t>(data, 32);
                for (size_t i = 1; i < 9; i++)
                    out << ",\"" << labels[i] << "\":" << field<uint32_t>(data, 32 + 4*i);
                // The stock population code does not copy the four AGC doubles
                // at72..103. Their zero initialization is not an RF measurement.
            } else if (std::strcmp(section.name, "time_clock") == 0) {
                out << ",\"gps_week\":" << field<uint16_t>(data, 32)
                    << ",\"gps_tow_ms\":" << field<uint32_t>(data, 36)
                    << ",\"time_valid\":" << unsigned(data[40])
                    << ",\"time_source\":" << unsigned(data[41])
                    << ",\"time_uncertainty_us\":" << field<int32_t>(data, 44)
                    << ",\"clock_frequency_bias\":" << field<int32_t>(data, 48)
                    << ",\"clock_frequency_bias_uncertainty\":" << field<int32_t>(data, 52);
            } else if (std::strcmp(section.name, "xo_state") == 0) {
                out << ",\"xo_state\":" << unsigned(data[32]);
            } else if (std::strcmp(section.name, "error_recovery") == 0) {
                out << ",\"recovery_raw\":" << field<uint32_t>(data, 32);
            } else {
                out << ",\"fix_info_mask\":" << field<uint32_t>(data, 32)
                    << ",\"hepe_limit\":" << field<uint32_t>(data, 36);
            }
            out << "}";
        }
        out << "]";
    }
    out << "}";
    std::puts(out.str().c_str());
    return 0;
}
