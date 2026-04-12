#pragma once

#include <cstdint>
#include <string>
#include <optional>

namespace ratatoskr {

class HidDevice;

struct BatteryInfo {
    int percent = -1;
    bool charging = false;
};

struct ChatmixInfo {
    int level = 6;  // 0=Voice, 6=50/50, 12=Game
};

struct VolumeInfo {
    int level = 0;  // 0-31
};

struct SidetoneInfo {
    int level = 0;  // 0-6 (hardware steps)
};

struct MicMuteInfo {
    bool muted = false;
};

struct BluetoothInfo {
    bool connected = false;
    std::string name;
    std::string address;
};

/// Base class for device drivers
class Driver {
public:
    virtual ~Driver() = default;

    virtual std::string name() const = 0;
    virtual uint16_t vendorId() const = 0;
    virtual uint16_t productId() const = 0;
    virtual uint16_t usagePage() const = 0;

    virtual bool open(HidDevice& device) = 0;

    // GET
    virtual std::optional<BatteryInfo> getBattery(HidDevice& device) = 0;
    virtual std::optional<ChatmixInfo> getChatmix(HidDevice& device) { return std::nullopt; }
    virtual std::optional<VolumeInfo> getVolume(HidDevice& device) { return std::nullopt; }
    virtual std::optional<SidetoneInfo> getSidetone(HidDevice& device) { return std::nullopt; }
    virtual std::optional<MicMuteInfo> getMicMute(HidDevice& device) { return std::nullopt; }
    virtual std::optional<BluetoothInfo> getBluetoothStatus(HidDevice& device) { return std::nullopt; }
    virtual std::optional<std::string> getSerialNumber(HidDevice& device) { return std::nullopt; }
    virtual std::optional<std::string> getDeviceName(HidDevice& device) { return std::nullopt; }
    virtual std::optional<std::string> getBluetoothName(HidDevice& device) { return std::nullopt; }
    virtual std::optional<std::string> getBluetoothAddress(HidDevice& device) { return std::nullopt; }

    // SET — returns true on success
    virtual bool setSidetone(HidDevice& device, uint8_t level) { return false; }
    virtual bool setLedBrightness(HidDevice& device, uint8_t brightness) { return false; }
    virtual bool setInactiveTime(HidDevice& device, uint8_t minutes) { return false; }
    virtual bool setNotificationSound(HidDevice& device, uint8_t level) { return false; }
    virtual bool setEqualizerPreset(HidDevice& device, uint8_t preset) { return false; }
    virtual bool setMicVolume(HidDevice& device, uint8_t volume) { return false; }
    virtual bool setVolume(HidDevice& device, uint8_t volume) { return false; }
    virtual bool setNoiseGate(HidDevice& device, uint8_t mode) { return false; }
    virtual bool setMixamp(HidDevice& device, uint8_t level) { return false; }
};

} // namespace ratatoskr
