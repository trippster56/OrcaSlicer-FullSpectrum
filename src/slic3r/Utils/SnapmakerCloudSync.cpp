#include "SnapmakerCloudSync.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/MoonRaker.hpp"
#include "slic3r/Utils/PrintHost.hpp"

namespace Slic3r { namespace GUI {

namespace {

// Snapmaker logs colors as either:
//   "filament_color":      [4278716941, ...]   ARGB packed int (0xAARRGGBB)
//   "filament_color_rgba": ["080A0DFF", ...]   8-hex-digit RGBA string
// We prefer the rgba string and drop the alpha to produce "#RRGGBB" — the
// shape OrcaSlicer stores in filament_colour.
std::string normalize_rgba_to_hex_color(const std::string& rgba)
{
    // Expect "RRGGBBAA" (8 hex chars). Tolerate "#RRGGBB" or "RRGGBB" too.
    std::string s = rgba;
    if (!s.empty() && s.front() == '#') s.erase(0, 1);
    if (s.size() >= 6) {
        return "#" + s.substr(0, 6);
    }
    return {};
}

// Read a JSON array field and return [] if missing. Coerces all entries to
// string for fields where Snapmaker sometimes ships ints (e.g. filament_sku).
std::vector<std::string> read_string_array(const nlohmann::json& obj, const char* key)
{
    std::vector<std::string> out;
    if (!obj.contains(key) || !obj[key].is_array()) return out;
    out.reserve(obj[key].size());
    for (const auto& v : obj[key]) {
        if (v.is_string())            out.push_back(v.get<std::string>());
        else if (v.is_number_integer()) out.push_back(std::to_string(v.get<long long>()));
        else if (v.is_number_float())   out.push_back(std::to_string(v.get<double>()));
        else if (v.is_boolean())        out.push_back(v.get<bool>() ? "1" : "0");
        else                            out.push_back(std::string{});
    }
    return out;
}

std::vector<bool> read_bool_array(const nlohmann::json& obj, const char* key)
{
    std::vector<bool> out;
    if (!obj.contains(key) || !obj[key].is_array()) return out;
    out.reserve(obj[key].size());
    for (const auto& v : obj[key]) {
        out.push_back(v.is_boolean() ? v.get<bool>() : false);
    }
    return out;
}

template <typename T>
T at_or(const std::vector<T>& v, size_t i, T fallback)
{
    return i < v.size() ? v[i] : fallback;
}

} // anonymous

bool SnapmakerCloudSync::is_snapmaker_cloud_printer(const std::string& printer_preset_name)
{
    // Today the only Snapmaker cloud-MQTT model is the U1. We look at the
    // preset name rather than the model record because that's what the rest
    // of the sync code base uses; widen this when more models ship.
    return printer_preset_name.find("Snapmaker U1") != std::string::npos;
}

bool SnapmakerCloudSync::find_paired_device(const std::string& printer_preset_name, DeviceInfo& out)
{
    auto* app_config = wxGetApp().app_config;
    if (!app_config) return false;

    const auto devices = app_config->get_devices();
    for (const auto& d : devices) {
        // Strictest match: the device was paired against this exact preset.
        if (!d.preset_name.empty() && d.preset_name == printer_preset_name) {
            out = d;
            return true;
        }
    }
    // Fall back: any U1 in the device list. Many users will have only one.
    for (const auto& d : devices) {
        if (boost::algorithm::icontains(d.model_name, "u1") ||
            boost::algorithm::icontains(d.dev_name,  "u1")) {
            out = d;
            return true;
        }
    }
    return false;
}

std::map<int, DynamicPrintConfig>
SnapmakerCloudSync::build_filament_ams_list_from_print_task_config(const nlohmann::json& ptc)
{
    std::map<int, DynamicPrintConfig> result;
    if (!ptc.is_object()) return result;

    const auto vendors  = read_string_array(ptc, "filament_vendor");
    const auto types    = read_string_array(ptc, "filament_type");
    const auto subtypes = read_string_array(ptc, "filament_sub_type");
    const auto skus     = read_string_array(ptc, "filament_sku");
    const auto colors   = read_string_array(ptc, "filament_color_rgba");
    const auto exists   = read_bool_array  (ptc, "filament_exist");
    const auto used     = read_bool_array  (ptc, "extruders_used");

    // Slot count = max of any array (Snapmaker U1 = 4).
    size_t n = std::max({vendors.size(), types.size(), colors.size(), exists.size()});

    for (size_t i = 0; i < n; ++i) {
        const bool slot_exists = at_or(exists, i, true);
        const std::string color_hex = normalize_rgba_to_hex_color(at_or(colors, i, std::string{}));
        const std::string type      = at_or(types,   i, std::string{});
        const std::string sku       = at_or(skus,    i, std::string{});

        // Skip totally empty slots so the matcher doesn't burn an unknown.
        if (!slot_exists && type.empty() && color_hex.empty()) continue;

        DynamicPrintConfig cfg;
        // filament_id is what PresetBundle::sync_ams_list() matches on first.
        // Snapmaker's SKU is their stable cross-version filament identifier;
        // 0 means "generic / unknown" so leave filament_id empty in that case
        // (sync_ams_list will fall through to "Generic <type>" matching).
        cfg.set_key_value("filament_id",
            new ConfigOptionStrings{ (sku == "0" || sku.empty()) ? std::string{} : sku });
        cfg.set_key_value("tag_uid",       new ConfigOptionStrings{ std::string{} });
        cfg.set_key_value("filament_type", new ConfigOptionStrings{ type });
        cfg.set_key_value("tray_name",     new ConfigOptionStrings{ std::to_string(i + 1) });
        cfg.set_key_value("filament_colour", new ConfigOptionStrings{
            color_hex.empty() ? std::string("#FFFFFF") : color_hex });
        cfg.set_key_value("filament_exist", new ConfigOptionBools{ slot_exists });

        // Single-color filaments only for now. The Snapmaker payload includes
        // filament_color_multi for blended/dual-color spools, but the U1 ships
        // one color per slot today; revisit when multi appears in the wild.
        auto* multi = new ConfigOptionStrings{};
        if (!color_hex.empty()) multi->values.push_back(color_hex);
        cfg.set_key_value("filament_multi_colors", multi);

        result.emplace(static_cast<int>(i), std::move(cfg));
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " produced " << result.size() << " AMS slot(s)";
    return result;
}

void SnapmakerCloudSync::fetch_filament_ams_list(const DeviceInfo& /*unused*/,
                                                std::function<void(const SyncResult&)> on_done)
{
    if (!on_done) return;

    // FullSpectrum and the official Snapmaker Orca don't persist AWS IoT
    // credentials to AppConfig — the Flutter UI fetches them fresh from
    // Snapmaker cloud at startup and binds them to a live Moonraker_Mqtt
    // engine that the Device tab uses for everything. Borrow that engine
    // instead of building our own.
    std::shared_ptr<PrintHost> live_host;
    wxGetApp().get_connect_host(live_host);
    std::shared_ptr<Moonraker_Mqtt> host = std::dynamic_pointer_cast<Moonraker_Mqtt>(live_host);

    if (!host) {
        SyncResult result;
        result.error_message = "No active printer connection. "
                               "Open the Device tab and connect to your Snapmaker printer first.";
        wxGetApp().CallAfter([on_done, result]() { on_done(result); });
        return;
    }

    std::thread([host, on_done]() {
        auto deliver = [&](SyncResult r) {
            wxGetApp().CallAfter([on_done, r = std::move(r)]() { on_done(r); });
        };

        SyncResult result;

        // The Moonraker query is async. Bridge it to this thread with a
        // promise so we can produce a synchronous SyncResult per-call.
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        auto future  = promise->get_future();

        host->async_get_machine_info(
            { { "print_task_config", {} } },
            [promise](const nlohmann::json& response) {
                promise->set_value(response);
            });

        // 15s upper bound — generous; in practice responses come back in <1s.
        auto status = future.wait_for(std::chrono::seconds(15));
        if (status != std::future_status::ready) {
            result.error_message = "Timed out waiting for printer.objects.query response";
            deliver(std::move(result));
            return;
        }

        const nlohmann::json response = future.get();
        if (response.is_null() || response.contains("error")) {
            result.error_message = response.contains("error") && response["error"].is_string()
                ? response["error"].get<std::string>()
                : "Empty response from printer";
            deliver(std::move(result));
            return;
        }

        // Snapmaker's MQTT bridge unwraps the Moonraker JSON-RPC envelope
        // and re-wraps the result under "data" instead of "result":
        //   {method:"", data:{eventtime, status:{print_task_config:{...}}}}
        // Standard Moonraker over HTTP would put it under "result"; we accept
        // either, plus a couple of flatter shapes for defensiveness.
        nlohmann::json ptc;
        auto try_path = [&](const nlohmann::json& root, std::initializer_list<const char*> keys) {
            const nlohmann::json* cur = &root;
            for (const char* k : keys) {
                if (!cur->is_object() || !cur->contains(k)) return false;
                cur = &(*cur)[k];
            }
            ptc = *cur;
            return true;
        };
        try_path(response, {"data",   "status", "print_task_config"}) ||
        try_path(response, {"result", "status", "print_task_config"}) ||
        try_path(response, {"status", "print_task_config"})            ||
        try_path(response, {"print_task_config"});

        if (!ptc.is_object()) {
            result.error_message = "Response did not contain print_task_config object. "
                                   "Firmware may have renamed the Klipper module.";
            deliver(std::move(result));
            return;
        }

        result.filament_ams_list = build_filament_ams_list_from_print_task_config(ptc);
        result.ok = !result.filament_ams_list.empty();
        if (!result.ok) result.error_message = "No filaments parsed from response";

        deliver(std::move(result));
    }).detach();
}

}} // namespace Slic3r::GUI
