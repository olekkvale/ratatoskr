#include "a50_gen5.hpp"
#include "../hid_device.hpp"
#include <chrono>
#include <cstring>
#include <algorithm>

namespace {
inline int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

namespace ratatoskr {

A50Gen5Driver::~A50Gen5Driver() {
    stopListener();
}

bool A50Gen5Driver::open(HidDevice& device) {
    return device.open(vendorId(), productId(), usagePage());
}

void A50Gen5Driver::startListener(HidDevice& device) {
    if (listener_running_.load()) return;
    listener_running_.store(true);

    listener_thread_ = std::thread([this, &device]() {
        std::array<uint8_t, MSG_SIZE> buf{};
        while (listener_running_.load()) {
            if (listener_paused_.load() || !device.isOpen()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            int bytes = device.read(buf.data(), buf.size(), 200);
            if (bytes > 0 && !listener_paused_.load()) {
                handleSpontaneous(buf.data(), bytes);
            }
            // bytes < 0 means device disconnected — HidDevice::read closes handle
            // Loop continues, waiting for reconnect via device.isOpen()
        }
    });
}

void A50Gen5Driver::stopListener() {
    listener_running_.store(false);
    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }
}

bool A50Gen5Driver::sendCommandLocked(HidDevice& device,
                                       const uint8_t* cmd, size_t cmd_len,
                                       const uint8_t* data, size_t data_len) {
    // HIDAPI: byte[0] = Report ID (consumed by hidapi), byte[1..] = payload
    std::array<uint8_t, MSG_SIZE + 1> buf{};
    buf[0] = REPORT_ID;
    buf[1] = FRAME_MARKER;

    for (size_t i = 0; i < cmd_len && i + 2 < MSG_SIZE; ++i) {
        buf[i + 2] = cmd[i];
    }

    if (data) {
        for (size_t i = 0; i < data_len && i + 6 < MSG_SIZE; ++i) {
            buf[i + 6] = data[i];
        }
    }

    return device.write(buf.data(), MSG_SIZE + 1);
}

bool A50Gen5Driver::sendCommand(HidDevice& device,
                                 const uint8_t* cmd, size_t cmd_len,
                                 const uint8_t* data, size_t data_len) {
    std::lock_guard<std::mutex> lock(hid_mutex_);
    return sendCommandLocked(device, cmd, cmd_len, data, data_len);
}

void A50Gen5Driver::handleSpontaneous(const uint8_t* data, size_t len) {
    if (len < 7) return;

    uint8_t cmd1 = data[4];

    // Mic mute: byte[2]=0x07 AND cmd1=0x0c, byte[9] bit0: 1=on, 0=muted
    // (byte[2]=0x07 alone is not unique — sidetone spontan also has len=7)
    if (data[2] == 0x07 && cmd1 == 0x0c && len >= 10) {
        cache_.hw_mic_muted.store((data[9] & 0x01) == 0 ? 1 : 0);
        emitEvent(Event::MicMute);
        return;
    }

    switch (cmd1) {
        case 0x06: // Battery: byte[6]=percent, byte[8]=charging
            cache_.battery_percent.store(data[6]);
            cache_.battery_charging.store(len >= 9 && data[8] != 0);
            emitEvent(Event::Battery);
            break;
        case 0x08: // Volume: byte[6]=level (0-31)
            cache_.volume.store(data[6]);
            emitEvent(Event::Volume);
            break;
        case 0x0a: // MixAmp: byte[6]=level (0-12), same cmd1 as GET/SET
            cache_.mixamp.store(data[6]);
            emitEvent(Event::Mixamp);
            break;
        case 0x11: // EQ checksum: byte[6..9] = 4 bytes hash
            if (len >= 10) {
                uint32_t hash = (data[6] << 24) | (data[7] << 16) |
                                (data[8] << 8) | data[9];
                cache_.eq_checksum.store(hash);
                // The device echoes 0x11 after every EQ change, including ones
                // we just SET ourselves. Suppress *cache invalidation* if the
                // echo arrives within 500 ms of our own SET — the cache was
                // populated directly with the SET bytes and is still correct.
                // EqChanged is still emitted so other clients re-sync (their
                // getActiveEqualizerData hits the warm cache, no USB query).
                int64_t since_set = now_ms() - last_eq_set_ms_.load();
                if (since_set >= 500) {
                    std::lock_guard<std::mutex> lock(eq_cache_mutex_);
                    active_eq_[0].valid = false;
                    active_eq_[1].valid = false;
                }
                emitEvent(Event::EqChanged);
            }
            break;
        case 0x12: { // Power: raw byte[6]. 0x00=on (spontaneous), 0x05=off, 0x01-0x03 sub-states (Windows VM)
            // Stored raw to preserve PowerChanged signal semantics for existing clients.
            // GetPower normalizes to 1/0/-1 at read-time.
            // Fire signal only on actual state change — suppresses spurious initial event
            // at daemon startup that clients misinterpret as connect/disconnect flicker.
            int prev = cache_.power.load();
            cache_.power.store(data[6]);
            if (prev != -1 && prev != data[6]) {
                emitEvent(Event::Power);
            }
            break;
        }
        case 0x0e: // Bluetooth: byte[6]: 0x01=connected
            cache_.bt_connected.store(data[6]);
            emitEvent(Event::Bluetooth);
            break;
        default:
            break;
    }
}

bool A50Gen5Driver::sendAndReceive(HidDevice& device,
                                    const uint8_t* cmd, size_t cmd_len,
                                    uint8_t* response, size_t resp_len,
                                    const uint8_t* data, size_t data_len) {
    // Pause listener so it doesn't steal our response.
    // Must wait longer than the listener's read timeout (200ms) to ensure
    // any in-flight read has completed before we send our command.
    listener_paused_.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    if (!sendCommandLocked(device, cmd, cmd_len, data, data_len)) {
        listener_paused_.store(false);
        return false;
    }

    // cmd[2] = cmd1, cmd[3] = cmd2 in the command template
    // Response byte[4] = cmd1, byte[5] = cmd2
    uint8_t expected_cmd1 = (cmd_len > 2) ? cmd[2] : 0;
    uint8_t expected_cmd2 = (cmd_len > 3) ? cmd[3] : 0;

    // Try up to 10 reads — buffer spontaneous reports, discard stale responses
    bool found = false;
    for (int attempt = 0; attempt < 10; ++attempt) {
        int bytes_read = device.read(response, resp_len, 2000);
        if (bytes_read <= 0) {
            break;
        }

        // Check if response matches our command
        if (bytes_read >= 6 && response[4] == expected_cmd1 && response[5] == expected_cmd2) {
            found = true;
            break;
        }

        // Not our response — check if it's a spontaneous report and cache it
        if (bytes_read >= 6) {
            handleSpontaneous(response, bytes_read);
        }
    }

    listener_paused_.store(false);
    return found;
}

// ========================================================================
// GET
// ========================================================================

std::optional<BatteryInfo> A50Gen5Driver::getBattery(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};

    if (sendAndReceive(device,
                       CMD_GET_BATTERY.data(), CMD_GET_BATTERY.size(),
                       response.data(), response.size())) {
        int level = response[6];
        if (level >= 0 && level <= 100) {
            cache_.battery_percent.store(level);
            cache_.battery_charging.store(response[8] != 0);
        }
    }

    int percent = cache_.battery_percent.load();
    if (percent < 0) return std::nullopt;
    return BatteryInfo{.percent = percent, .charging = cache_.battery_charging.load()};
}

std::optional<ChatmixInfo> A50Gen5Driver::getChatmix(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};

    if (sendAndReceive(device,
                       CMD_GET_MIXAMP.data(), CMD_GET_MIXAMP.size(),
                       response.data(), response.size())) {
        cache_.mixamp.store(std::clamp(static_cast<int>(response[6]), 0, 12));
    }

    int level = cache_.mixamp.load();
    if (level < 0) return std::nullopt;
    return ChatmixInfo{.level = level};
}

std::optional<VolumeInfo> A50Gen5Driver::getVolume(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};

    if (sendAndReceive(device, CMD_GET_VOLUME.data(), CMD_GET_VOLUME.size(),
                       response.data(), response.size())) {
        cache_.volume.store(response[6]);
    }

    int level = cache_.volume.load();
    if (level < 0) return std::nullopt;
    return VolumeInfo{.level = level};
}

std::optional<SidetoneInfo> A50Gen5Driver::getSidetone(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_SIDETONE.data(), CMD_GET_SIDETONE.size(),
                        response.data(), response.size())) {
        return std::nullopt;
    }
    // 09 0b: byte[6]=kanal, byte[7]=min, byte[8]=maks, byte[9]=nivå (0-6)
    return SidetoneInfo{.level = response[9]};
}

std::optional<MicMuteInfo> A50Gen5Driver::getMicMute(HidDevice& device) {
    // HW flip-to-mute via 0c 2b byte[9] bit 0 (0=muted, 1=on) — queryable!
    //   Spontaneous reports (0c 00) still push transitions in real-time, but
    //   the query lets us know state on connection without waiting for a flip.
    // SW mute via 0c 3b byte[9]: 0=unmute, 1=mute (separate mechanism)
    std::array<uint8_t, MSG_SIZE> response{};

    if (sendAndReceive(device, CMD_GET_MIC_VOL.data(), CMD_GET_MIC_VOL.size(),
                       response.data(), response.size())) {
        cache_.hw_mic_muted.store((response[9] & 0x01) == 0 ? 1 : 0);
    }

    if (sendAndReceive(device, CMD_GET_SW_MUTE.data(), CMD_GET_SW_MUTE.size(),
                       response.data(), response.size())) {
        cache_.sw_mic_muted.store(response[9] != 0 ? 1 : 0);
    }

    // Prefer hardware flip-to-mute, fall back to SW mute
    int hw = cache_.hw_mic_muted.load();
    if (hw >= 0) return MicMuteInfo{.muted = (hw != 0)};

    int sw = cache_.sw_mic_muted.load();
    if (sw >= 0) return MicMuteInfo{.muted = (sw != 0)};

    return std::nullopt;
}

std::optional<BluetoothInfo> A50Gen5Driver::getBluetoothStatus(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_BT_STATUS.data(), CMD_GET_BT_STATUS.size(),
                        response.data(), response.size())) {
        return std::nullopt;
    }
    return BluetoothInfo{.connected = (response[6] == 1)};
}

std::optional<std::string> A50Gen5Driver::getSerialNumber(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_SERIAL.data(), CMD_GET_SERIAL.size(),
                        response.data(), response.size())) {
        return std::nullopt;
    }
    // byte[7..18] = ASCII serial number
    std::string serial(reinterpret_cast<char*>(&response[7]), 12);
    // Trim null bytes
    auto end = serial.find('\0');
    if (end != std::string::npos) serial.resize(end);
    return serial;
}

std::optional<std::string> A50Gen5Driver::getDeviceName(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_DEVNAME.data(), CMD_GET_DEVNAME.size(),
                        response.data(), response.size())) {
        return std::nullopt;
    }
    // byte[8..10] = ASCII device name ("A50")
    std::string name(reinterpret_cast<char*>(&response[8]), 3);
    auto end = name.find('\0');
    if (end != std::string::npos) name.resize(end);
    return name;
}

std::optional<std::string> A50Gen5Driver::getBluetoothName(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_BT_NAME.data(), CMD_GET_BT_NAME.size(),
                        response.data(), response.size())) {
        return std::nullopt;
    }
    // byte[7] = name length, byte[8..] = ASCII name
    int name_len = response[7];
    if (name_len <= 0 || name_len > 50) return std::string("");
    std::string name(reinterpret_cast<char*>(&response[8]), name_len);
    auto end = name.find('\0');
    if (end != std::string::npos) name.resize(end);
    return name;
}

std::optional<std::string> A50Gen5Driver::getBluetoothAddress(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_BT_INFO.data(), CMD_GET_BT_INFO.size(),
                        response.data(), response.size())) {
        return std::nullopt;
    }
    // byte[7..11] = BT MAC address
    char addr[18];
    snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
             response[7], response[8], response[9],
             response[10], response[11], response[12]);
    return std::string(addr);
}

// ========================================================================
// GET (A50-specific)
// ========================================================================

std::string A50Gen5Driver::getFirmwareVersion(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_FIRMWARE.data(), CMD_GET_FIRMWARE.size(),
                        response.data(), response.size())) {
        return "";
    }
    // 04 1b: byte[6]=major, byte[7]=minor, byte[9]=patch, byte[10]=namelen, byte[11..]="main"
    int major = response[6], minor = response[7], patch = response[9];
    int name_len = response[10];
    std::string comp(reinterpret_cast<char*>(&response[11]),
                     std::min(name_len, 20));
    auto end = comp.find('\0');
    if (end != std::string::npos) comp.resize(end);

    return std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(patch) + " " + comp;
}

int A50Gen5Driver::getUptime(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_UPTIME.data(), CMD_GET_UPTIME.size(),
                        response.data(), response.size())) {
        return -1;
    }
    // 04 3b: byte[8..9] big-endian seconds
    return (response[8] << 8) | response[9];
}

int A50Gen5Driver::getNoiseGate(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_NOISE_GATE.data(), CMD_GET_NOISE_GATE.size(),
                        response.data(), response.size())) {
        return -1;
    }
    // 14 1b: byte[6] = 0x01=Home, 0x02=Night, 0x04=Tournament, 0x40=Disabled (intern)
    // Normalize 0x40 to 0x00 (the byte SetNoiseGate sends for Disabled) so
    // clients can compare against a single canonical value per state.
    if (response[6] == 0x40) return 0x00;
    return response[6];
}

int A50Gen5Driver::getSleepMode(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_SLEEP.data(), CMD_GET_SLEEP.size(),
                        response.data(), response.size())) {
        return -1;
    }
    // 07 0b: byte[6] = 0x00=Never, 0x0f=15min, 0x1e=30min, 0x3c=60min
    return response[6];
}

int A50Gen5Driver::getNotificationSound(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_NOTIF.data(), CMD_GET_NOTIF.size(),
                        response.data(), response.size())) {
        return -1;
    }
    // 10 0b: byte[6] = 0=None, 1=Minimal, 2=All
    return response[6];
}

int A50Gen5Driver::getLedBrightness(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_LED.data(), CMD_GET_LED.size(),
                        response.data(), response.size())) {
        return -1;
    }
    // 0f 0b: byte[6] = 0-100 (cached from boot)
    return response[6];
}

int A50Gen5Driver::getMicVolume(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_MIC_VOL.data(), CMD_GET_MIC_VOL.size(),
                        response.data(), response.size())) {
        return -1;
    }
    // 0c 2b: byte[9] = 0-32
    return response[9];
}

std::string A50Gen5Driver::getBaseMac(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_BASE_MAC.data(), CMD_GET_BASE_MAC.size(),
                        response.data(), response.size())) {
        return "";
    }
    // 0b 6b: byte[6..11] = MAC
    char addr[18];
    snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
             response[6], response[7], response[8],
             response[9], response[10], response[11]);
    return std::string(addr);
}

int A50Gen5Driver::getPower(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (sendAndReceive(device, CMD_GET_POWER.data(), CMD_GET_POWER.size(),
                       response.data(), response.size())) {
        // Valid response has length-byte (response[2]) >= 4. Length 3 means echo only.
        if (response[2] >= 4) {
            cache_.power.store(response[6]);  // raw byte
        }
    }
    int raw = cache_.power.load();
    if (raw < 0) return -1;
    return raw == 0x05 ? 0 : 1;
}

// ========================================================================
// Metadata protocol (byte[1] != 0x0c)
// ========================================================================

bool A50Gen5Driver::sendMetadata(HidDevice& device, uint8_t type,
                                  uint8_t* response, size_t resp_len) {
    listener_paused_.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    std::array<uint8_t, MSG_SIZE + 1> buf{};
    buf[0] = REPORT_ID;
    buf[1] = type;
    if (!device.write(buf.data(), MSG_SIZE + 1)) {
        listener_paused_.store(false);
        return false;
    }

    bool found = false;
    for (int attempt = 0; attempt < 10; ++attempt) {
        int bytes_read = device.read(response, resp_len, 2000);
        if (bytes_read <= 0) break;

        // Metadata response: byte[1] = 0x02 (not 0x0c)
        if (bytes_read >= 4 && response[1] != FRAME_MARKER) {
            found = true;
            break;
        }
        // Got a control-protocol packet — cache it
        if (bytes_read >= 6) {
            handleSpontaneous(response, bytes_read);
        }
    }

    listener_paused_.store(false);
    return found;
}

std::string A50Gen5Driver::getFirmwareShort(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendMetadata(device, 0x10, response.data(), response.size())) {
        return "";
    }
    // Response: 02 02 04 [major] [minor] 00 [patch]
    int major = response[3], minor = response[4], patch = response[6];
    return std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(patch);
}

std::string A50Gen5Driver::getDeviceInfo(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendMetadata(device, 0x03, response.data(), response.size())) {
        return "";
    }
    // Response: 02 02 26 [HW-ID 4B] [year LE 2B] [month] [day] [h] [m] [s] [fw 3B] ...
    uint32_t hw_id = (response[3] << 24) | (response[4] << 16) |
                     (response[5] << 8) | response[6];
    int year = (response[8] << 8) | response[7]; // little-endian
    int month = response[9], day = response[10];
    int hour = response[11], minute = response[12], second = response[13];
    int fw_major = response[14], fw_minor = response[15], fw_patch = response[16];

    char buf[128];
    snprintf(buf, sizeof(buf), "HW:0x%08x FW:%d.%d.%d Built:%04d-%02d-%02d %02d:%02d:%02d",
             hw_id, fw_major, fw_minor, fw_patch, year, month, day, hour, minute, second);
    return std::string(buf);
}

std::string A50Gen5Driver::getProtocolId(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendMetadata(device, 0x05, response.data(), response.size())) {
        return "";
    }
    // Response: 02 02 04 [version] [id_char1] [id_char2]
    int version = response[3];
    char id[3] = {static_cast<char>(response[4]), static_cast<char>(response[5]), '\0'};
    return std::string(id) + " v" + std::to_string(version);
}

std::string A50Gen5Driver::getSerialMeta(HidDevice& device) {
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendMetadata(device, 0x13, response.data(), response.size())) {
        return "";
    }
    // Response: 02 02 0e 00 [len] [ASCII...]
    int sn_len = response[4];
    if (sn_len <= 0 || sn_len > 20) return "";
    std::string serial(reinterpret_cast<char*>(&response[5]), sn_len);
    auto end = serial.find('\0');
    if (end != std::string::npos) serial.resize(end);
    return serial;
}

// ========================================================================
// SET
// ========================================================================

bool A50Gen5Driver::setSidetone(HidDevice& device, uint8_t level) {
    // Device range: 0-6
    uint8_t clamped = std::min(level, uint8_t(6));
    std::array<uint8_t, 3> data{0x01, 0xff, clamped};
    return sendCommand(device, CMD_SET_SIDETONE.data(), CMD_SET_SIDETONE.size(),
                       data.data(), data.size());
}

bool A50Gen5Driver::setLedBrightness(HidDevice& device, uint8_t brightness) {
    // Device range: 0-100
    uint8_t clamped = std::min(brightness, uint8_t(100));
    std::array<uint8_t, 1> data{clamped};
    return sendCommand(device, CMD_SET_LED.data(), CMD_SET_LED.size(),
                       data.data(), data.size());
}

bool A50Gen5Driver::setInactiveTime(HidDevice& device, uint8_t minutes) {
    // Valid values: 0 (never), 15, 30, 60
    uint8_t validated;
    if (minutes == 0)       validated = 0x00;
    else if (minutes <= 22) validated = 0x0f; // 15 min
    else if (minutes <= 45) validated = 0x1e; // 30 min
    else                    validated = 0x3c; // 60 min

    std::array<uint8_t, 1> data{validated};
    return sendCommand(device, CMD_SET_SLEEP.data(), CMD_SET_SLEEP.size(),
                       data.data(), data.size());
}

bool A50Gen5Driver::setNotificationSound(HidDevice& device, uint8_t level) {
    // 0=none, 1=minimal, 2=all
    uint8_t clamped = std::min(level, uint8_t(2));
    std::array<uint8_t, 1> data{clamped};
    return sendCommand(device, CMD_SET_NOTIFICATIONS.data(), CMD_SET_NOTIFICATIONS.size(),
                       data.data(), data.size());
}

bool A50Gen5Driver::setEqualizerPreset(HidDevice& device, uint8_t preset) {
    if (preset > 2) return false;

    // EQ frame: byte[0]=0x01 (sound type), byte[6]=preset value
    static constexpr uint8_t presets[] = {0x78, 0x14, 0xc8}; // Standard, Gaming, Media
    std::array<uint8_t, 50> data{};
    data[0] = 0x01; // Sound EQ
    data[6] = presets[preset];

    return sendCommand(device, CMD_SET_EQ.data(), CMD_SET_EQ.size(),
                       data.data(), data.size());
}

bool A50Gen5Driver::setMicVolume(HidDevice& device, uint8_t volume) {
    // Device range: 0-32
    // CMD_SET_MIC_VOL and CMD_SET_MIC_MUTE share the same HID command (0c 6b).
    // Must preserve mute state when changing volume, otherwise mute is reset to 0.
    uint8_t clamped = std::min(volume, uint8_t(32));
    int sw_mute = cache_.sw_mic_muted.load();
    uint8_t mute_byte = (sw_mute > 0) ? 0x01 : 0x00;

    std::array<uint8_t, 11> data{};
    data[1] = 0x20; // Stream volume max
    data[2] = mute_byte;
    data[4] = clamped;

    return sendCommand(device, CMD_SET_MIC_VOL.data(), CMD_SET_MIC_VOL.size(),
                       data.data(), data.size());
}

bool A50Gen5Driver::setVolume(HidDevice& device, uint8_t volume) {
    // Device range: 0-21
    uint8_t clamped = std::min(volume, uint8_t(21));
    std::array<uint8_t, 2> data{0xff, clamped};
    return sendCommand(device, CMD_SET_VOLUME.data(), CMD_SET_VOLUME.size(),
                       data.data(), data.size());
}

bool A50Gen5Driver::setNoiseGate(HidDevice& device, uint8_t mode) {
    // 0x00=Disabled, 0x01=Home, 0x02=Night, 0x04=Tournament
    uint8_t validated;
    switch (mode) {
        case 0: validated = 0x01; break; // Home
        case 1: validated = 0x02; break; // Night
        case 2: validated = 0x04; break; // Tournament
        case 3: validated = 0x00; break; // Disabled
        default: return false;
    }

    std::array<uint8_t, 1> data{validated};
    return sendCommand(device, CMD_SET_NOISE_GATE.data(), CMD_SET_NOISE_GATE.size(),
                       data.data(), data.size());
}

bool A50Gen5Driver::setMixamp(HidDevice& device, uint8_t level) {
    // Device range: 0-12 (0=Voice, 6=50/50, 12=Game)
    uint8_t clamped = std::min(level, uint8_t(12));
    std::array<uint8_t, 3> data{0x01, 0xff, clamped};
    return sendCommand(device, CMD_SET_MIXAMP.data(), CMD_SET_MIXAMP.size(),
                       data.data(), data.size());
}

// ========================================================================
// SET (A50-specific)
// ========================================================================

bool A50Gen5Driver::setMicMute(HidDevice& device, bool mute) {
    // CMD_SET_MIC_MUTE and CMD_SET_MIC_VOL share the same HID command (0c 6b).
    // Must preserve mic volume when changing mute, otherwise volume is reset to 0.
    std::array<uint8_t, MSG_SIZE> response{};
    int mic_vol = 0;
    if (sendAndReceive(device, CMD_GET_MIC_VOL.data(), CMD_GET_MIC_VOL.size(),
                       response.data(), response.size())) {
        mic_vol = response[9];
    }

    std::array<uint8_t, 11> data{};
    data[1] = 0x20; // stream volume max
    data[2] = mute ? 0x01 : 0x00;
    data[4] = static_cast<uint8_t>(mic_vol);

    cache_.sw_mic_muted.store(mute ? 1 : 0);
    return sendCommand(device, CMD_SET_MIC_MUTE.data(), CMD_SET_MIC_MUTE.size(),
                       data.data(), data.size());
}

bool A50Gen5Driver::setCustomEqualizer(HidDevice& device, uint8_t type,
                                        const uint8_t* band_data, size_t band_len) {
    // 0d 2c: byte[6]=type, byte[7]=0x03, byte[8]=0x00 (spacer)
    // byte[9..58] = 10 bands × 5 bytes [freq_hi, freq_lo, Q, 0x00, gain]
    if (band_len != 50 || type > 1) return false;

    std::array<uint8_t, 53> data{};
    data[0] = type;   // 0x00=mic, 0x01=headphone
    data[1] = 0x03;
    data[2] = 0x00;   // spacer (matches G HUB frame format)
    std::memcpy(&data[3], band_data, 50);

    bool ok = sendCommand(device, CMD_SET_CUSTOM_EQ.data(), CMD_SET_CUSTOM_EQ.size(),
                          data.data(), data.size());
    if (ok) {
        // Populate cache directly — avoids round-trip device query for state we just set.
        std::lock_guard<std::mutex> lock(eq_cache_mutex_);
        std::memcpy(active_eq_[type].data.data(), band_data, 50);
        active_eq_[type].valid = true;
        // Stamp self-SET so the 0x11 echo doesn't immediately invalidate this entry.
        last_eq_set_ms_.store(now_ms());
    }
    return ok;
}

bool A50Gen5Driver::setEqualizerActive(HidDevice& device, uint8_t preset) {
    // 0d 5b: byte[6]=preset-nr
    std::array<uint8_t, 1> data{preset};
    bool ok = sendCommand(device, CMD_SET_EQ_ACTIVE.data(), CMD_SET_EQ_ACTIVE.size(),
                          data.data(), data.size());
    if (ok) {
        // Active EQ now reflects a different preset — invalidate both types so
        // the next GET pulls fresh data. Stamp self-SET so the 0x11 echo
        // doesn't fire EqChanged for our own action.
        std::lock_guard<std::mutex> lock(eq_cache_mutex_);
        active_eq_[0].valid = false;
        active_eq_[1].valid = false;
        last_eq_set_ms_.store(now_ms());
    }
    return ok;
}

bool A50Gen5Driver::saveEqualizerPreset(HidDevice& device) {
    // 0d 1b: save active EQ to headset
    return sendCommand(device, CMD_SET_EQ_SAVE.data(), CMD_SET_EQ_SAVE.size());
}

/// Convert 50 bytes from GET response wire format to SET-compatible canonical format.
/// Wire (GET) per slot:  [freq_lo(N), Q(N), pad=0, gain(N), freq_hi(N+1)]   (cyclic-shifted freq_hi)
/// Canonical per band:   [freq_hi(N), freq_lo(N), Q(N), 0x00, gain(N)]      (matches setCustomEqualizer)
/// freq_hi_0 comes from the byte BEFORE the slot data (response[7] for 0d 1a, implicit 0 for 0d 5a).
static std::array<uint8_t, 50> normalizeEqResponse(uint8_t freq_hi_0, const uint8_t* slots) {
    std::array<uint8_t, 50> out{};
    uint8_t freq_hi = freq_hi_0;
    for (int b = 0; b < 10; b++) {
        const int src = b * 5;
        const int dst = b * 5;
        out[dst + 0] = freq_hi;          // freq_hi(N) — carried over from previous slot (or freq_hi_0)
        out[dst + 1] = slots[src + 0];   // freq_lo(N)
        out[dst + 2] = slots[src + 1];   // Q(N)
        out[dst + 3] = 0x00;             // spacer
        out[dst + 4] = slots[src + 3];   // gain(N)
        freq_hi = slots[src + 4];        // store freq_hi for next band
    }
    return out;
}

std::array<uint8_t, 50> A50Gen5Driver::getActiveEqualizerData(HidDevice& device, uint8_t type) {
    if (type > 1) return {};
    {
        std::lock_guard<std::mutex> lock(eq_cache_mutex_);
        if (active_eq_[type].valid) return active_eq_[type].data;
    }
    // 0d 1a (type, 0x03): byte[7]=0x03 is constant marker for "active EQ"
    // (NOT a preset index). Response: [02 0c 36 00 0d 1a type 0x00 <50 bytes>].
    // Bands start at byte[8]; per-band wire format differs from SET (see normalizeEqResponse).
    std::array<uint8_t, 2> args{type, 0x03};
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_EQ_ACTIVE.data(), CMD_GET_EQ_ACTIVE.size(),
                        response.data(), response.size(), args.data(), args.size())) {
        return {};
    }
    // Note: response[6] is NOT a type echo for 0d 1a — it's an extra byte (always 0x00 in observed
    // captures). The actual type information is implicit (we requested it). Skip type validation.
    auto out = normalizeEqResponse(/*freq_hi_0=*/response[7], /*slots=*/&response[8]);
    {
        std::lock_guard<std::mutex> lock(eq_cache_mutex_);
        active_eq_[type].data = out;
        active_eq_[type].valid = true;
    }
    return out;
}

std::array<uint8_t, 50> A50Gen5Driver::getEqualizerPresetData(HidDevice& device, uint8_t type, uint8_t index) {
    if (type > 1 || index > 2) return {};
    {
        std::lock_guard<std::mutex> lock(eq_cache_mutex_);
        if (builtin_eq_[type][index].valid) return builtin_eq_[type][index].data;
    }
    // 0d 5a (type, index): response [02 0c 35 00 0d 5a type <50 bytes>].
    // Bands start at byte[7] (no 0x00 padding byte that 0d 1a has).
    std::array<uint8_t, 2> args{type, index};
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_EQ_PRESET.data(), CMD_GET_EQ_PRESET.size(),
                        response.data(), response.size(), args.data(), args.size())) {
        return {};
    }
    // 0d 5a response: byte[6] is implicit freq_hi(0) (always 0 for built-in presets where
    // band 0 freq < 256 Hz). Bands at byte[7..56].
    auto out = normalizeEqResponse(/*freq_hi_0=*/response[6], /*slots=*/&response[7]);
    {
        std::lock_guard<std::mutex> lock(eq_cache_mutex_);
        builtin_eq_[type][index].data = out;
        builtin_eq_[type][index].valid = true;
    }
    return out;
}

int A50Gen5Driver::getEqualizerPresetCount(HidDevice& device, uint8_t type) {
    if (type > 1) return -1;
    {
        std::lock_guard<std::mutex> lock(eq_cache_mutex_);
        if (builtin_count_[type] >= 0) return builtin_count_[type];
    }
    // 0d 3a (type): response byte[6] = preset count (3 in firmware 1.3.24).
    std::array<uint8_t, 1> args{type};
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_EQ_COUNT.data(), CMD_GET_EQ_COUNT.size(),
                        response.data(), response.size(), args.data(), args.size())) {
        return -1;
    }
    int count = response[6];
    {
        std::lock_guard<std::mutex> lock(eq_cache_mutex_);
        builtin_count_[type] = count;
    }
    return count;
}

bool A50Gen5Driver::factoryReset(HidDevice& device, const std::string& serial) {
    // 04 4b: byte[6]=0x0c (SN length), byte[7..18] = serial number (ASCII, 12 chars)
    if (serial.size() != 12) return false;

    std::array<uint8_t, 13> data{};
    data[0] = 0x0c; // serial number length
    std::memcpy(&data[1], serial.data(), 12);
    return sendCommand(device, CMD_SET_FACTORY_RESET.data(), CMD_SET_FACTORY_RESET.size(),
                       data.data(), data.size());
}

bool A50Gen5Driver::startBluetoothPairing(HidDevice& device) {
    // 0b 1b: trigger BT pair/search, response byte[6]=0x03
    std::array<uint8_t, 1> data{0x00};
    return sendCommand(device, CMD_SET_BT_PAIR.data(), CMD_SET_BT_PAIR.size(),
                       data.data(), data.size());
}

// ========================================================================
// Stream Routing (0c 6e)
// ========================================================================

A50Gen5Driver::RoutingConfig A50Gen5Driver::getRouting(HidDevice& device) {
    RoutingConfig cfg;
    std::array<uint8_t, MSG_SIZE> response{};
    if (!sendAndReceive(device, CMD_GET_ROUTING.data(), CMD_GET_ROUTING.size(),
                        response.data(), response.size())) {
        return cfg;
    }
    // Response matches SET frame: byte[6]=0x00, byte[7]=stream_vol, byte[8]=stream_mute,
    // byte[9]=4 (count), then 4 × [id, vol, mute]
    cfg.stream_vol = response[7];
    cfg.stream_mute = (response[8] != 0);
    int count = response[9];
    for (int i = 0; i < count && i < 4; i++) {
        int off = 10 + i * 3;
        uint8_t id = response[off];
        uint8_t vol = response[off + 1];
        bool mute = (response[off + 2] != 0);
        switch (id) {
            case 0: cfg.mic_vol = vol;   cfg.mic_mute = mute;   break;
            case 1: cfg.game_vol = vol;  cfg.game_mute = mute;  break;
            case 2: cfg.bt_vol = vol;    cfg.bt_mute = mute;    break;
            case 3: cfg.voice_vol = vol; cfg.voice_mute = mute; break;
        }
    }
    return cfg;
}

bool A50Gen5Driver::setRouting(HidDevice& device, const RoutingConfig& cfg) {
    // Frame: byte[6]=0x00, byte[7]=stream_vol, byte[8]=stream_mute,
    // byte[9]=4, then 4 × [channel_id, vol, mute]
    std::array<uint8_t, 16> data{};
    data[0] = 0x00;  // channel selector
    data[1] = std::min(cfg.stream_vol, uint8_t(32));
    data[2] = cfg.stream_mute ? 1 : 0;
    data[3] = 4;     // sub-channel count
    // Ch 0: Mic out
    data[4] = 0;  data[5] = std::min(cfg.mic_vol, uint8_t(32));   data[6] = cfg.mic_mute ? 1 : 0;
    // Ch 1: Game
    data[7] = 1;  data[8] = std::min(cfg.game_vol, uint8_t(32));  data[9] = cfg.game_mute ? 1 : 0;
    // Ch 2: Bluetooth
    data[10] = 2; data[11] = std::min(cfg.bt_vol, uint8_t(32));   data[12] = cfg.bt_mute ? 1 : 0;
    // Ch 3: Voice
    data[13] = 3; data[14] = std::min(cfg.voice_vol, uint8_t(32)); data[15] = cfg.voice_mute ? 1 : 0;

    return sendCommand(device, CMD_SET_ROUTING.data(), CMD_SET_ROUTING.size(),
                       data.data(), data.size());
}

} // namespace ratatoskr
