// Read-only IGnssDebug snapshot. Position coordinates are never printed.
#include <android/hardware/gnss/1.0/IGnss.h>
#include <android/hardware/gnss/1.0/IGnssDebug.h>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdio>

using android::hardware::gnss::V1_0::IGnss;
using android::hardware::gnss::V1_0::IGnssDebug;

static void number(double value) {
    if (std::isfinite(value)) std::printf("%.9g", value);
    else std::printf("null");
}

int main() {
    const auto service = IGnss::tryGetService();
    if (service == nullptr) {
        std::puts("{\"error\":\"GNSS service unavailable\"}");
        return 2;
    }
    const auto result = service->getExtensionGnssDebug();
    if (!result.isOk()) {
        std::puts("{\"error\":\"GNSS debug extension transaction failed\"}");
        return 3;
    }
    android::sp<IGnssDebug> debug = result;
    if (debug == nullptr) {
        std::puts("{\"debug_extension_available\":false}");
        return 4;
    }
    bool received = false;
    const auto status = debug->getDebugData([&](const IGnssDebug::DebugData& data) {
        received = true;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        std::printf("{\"debug_extension_available\":true,\"coordinates_recorded\":false,"
                    "\"system_epoch_ms\":%" PRId64 ",\"position_valid\":%s,"
                    "\"time_estimate_epoch_ms\":%" PRId64 ",\"time_uncertainty_ns\":",
                    static_cast<int64_t>(now), data.position.valid ? "true" : "false",
                    static_cast<int64_t>(data.time.timeEstimate));
        number(data.time.timeUncertaintyNs);
        std::printf(",\"frequency_uncertainty_ns_per_sec\":");
        number(data.time.frequencyUncertaintyNsPerSec);
        std::printf(",\"satellite_debug_entries\":%zu,\"satellites\":[", data.satelliteDataArray.size());
        bool first = true;
        for (const auto& satellite : data.satelliteDataArray) {
            if (!first) std::printf(",");
            first = false;
            std::printf("{\"svid\":%d,\"constellation\":%u,\"ephemeris_type\":%u,"
                        "\"ephemeris_source\":%u,\"ephemeris_health\":%u,\"ephemeris_age_seconds\":",
                        satellite.svid, static_cast<unsigned>(satellite.constellation),
                        static_cast<unsigned>(satellite.ephemerisType),
                        static_cast<unsigned>(satellite.ephemerisSource),
                        static_cast<unsigned>(satellite.ephemerisHealth));
            number(satellite.ephemerisAgeSeconds);
            std::printf(",\"server_prediction_available\":%s,\"server_prediction_age_seconds\":",
                        satellite.serverPredictionIsAvailable ? "true" : "false");
            number(satellite.serverPredictionAgeSeconds);
            std::printf("}");
        }
        std::puts("]}");
    });
    return status.isOk() && received ? 0 : 5;
}
