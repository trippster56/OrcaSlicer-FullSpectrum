#ifndef slic3r_SnapmakerCloudSync_hpp_
#define slic3r_SnapmakerCloudSync_hpp_

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class PresetBundle;

namespace GUI {

// Pulls the four loaded filaments off a Snapmaker U1 over its existing
// AWS-IoT cloud connection (no LAN Mode required) and pushes them onto the
// slicer's filament_ams_list so the existing PresetBundle::sync_ams_list()
// machinery can map them onto loaded filament presets.
//
// All public methods are safe to call from the UI thread. Cloud I/O runs on
// an internal worker; the completion callback is dispatched back to the UI
// thread via wxGetApp().CallAfter().
class SnapmakerCloudSync
{
public:
    // True if the printer preset is a Snapmaker model that uses the cloud
    // MQTT transport (U1 today; future models may add others).
    static bool is_snapmaker_cloud_printer(const std::string& printer_preset_name);

    // Look up a previously-paired Snapmaker device from AppConfig. Pairing is
    // done through the existing WebDeviceDialog flow, which persists the AWS
    // IoT certs into AppConfig as a DeviceInfo. Returns false if no matching
    // device has been added yet.
    //
    // `printer_preset_name` is the slicer-side preset (e.g. "Snapmaker U1
    // 0.4 nozzle"). We match the DeviceInfo whose preset_name matches.
    static bool find_paired_device(const std::string& printer_preset_name, DeviceInfo& out);

    struct SyncResult
    {
        bool        ok = false;
        std::string error_message;
        // On success, populated with the parsed slot data (vendor/type/color
        // per index 0..3). Empty on failure.
        std::map<int, DynamicPrintConfig> filament_ams_list;
    };

    // Connect to the device's cloud MQTT broker using the persisted certs,
    // query Moonraker's print_task_config object, parse the response into a
    // filament_ams_list, and invoke `on_done` on the UI thread.
    //
    // This does NOT mutate PresetBundle directly — the caller decides whether
    // to assign the result into preset_bundle->filament_ams_list (matching
    // the existing Bambu pattern in Sidebar::load_ams_list).
    static void fetch_filament_ams_list(const DeviceInfo& device,
                                        std::function<void(const SyncResult&)> on_done);

    // Convert a parsed print_task_config payload (the inner object — i.e.
    // result.status.print_task_config) into a filament_ams_list. Exposed for
    // unit-testing and for callers that already have the JSON from elsewhere.
    static std::map<int, DynamicPrintConfig> build_filament_ams_list_from_print_task_config(
        const nlohmann::json& print_task_config);
};

}} // namespace Slic3r::GUI

#endif
