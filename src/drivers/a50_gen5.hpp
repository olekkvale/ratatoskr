#pragma once

#include "driver.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace ratatoskr {

/// Logitech Astro A50 Gen 5 driver
///
/// USB basestation (046d:0b1c), vendor-specific HID (Usage Page 0xFF32).
/// Frame format: [Report ID 0x02] [0x0c] [len] [0x00] [cmd1] [cmd2] [data...] (64+1 bytes via HIDAPI)
///
/// Spontaneous reports (cmd2=0x00) are buffered in cache_ during sendAndReceive.
/// GET methods return cached values when available.
class A50Gen5Driver : public Driver {
public:
    std::string name() const override { return "Logitech Astro A50 Gen 5"; }
    uint16_t vendorId() const override { return 0x046d; }
    uint16_t productId() const override { return 0x0b1c; }
    uint16_t usagePage() const override { return 0xff32; }

    ~A50Gen5Driver();

    bool open(HidDevice& device) override;

    /// Start background listener for spontaneous reports
    void startListener(HidDevice& device);

    /// Stop background listener
    void stopListener();

    // GET (overrides)
    std::optional<BatteryInfo> getBattery(HidDevice& device) override;
    std::optional<ChatmixInfo> getChatmix(HidDevice& device) override;
    std::optional<VolumeInfo> getVolume(HidDevice& device) override;
    std::optional<SidetoneInfo> getSidetone(HidDevice& device) override;
    std::optional<MicMuteInfo> getMicMute(HidDevice& device) override;
    std::optional<BluetoothInfo> getBluetoothStatus(HidDevice& device) override;
    std::optional<std::string> getSerialNumber(HidDevice& device) override;
    std::optional<std::string> getDeviceName(HidDevice& device) override;
    std::optional<std::string> getBluetoothName(HidDevice& device) override;
    std::optional<std::string> getBluetoothAddress(HidDevice& device) override;

    // GET (A50-specific)
    std::string getFirmwareVersion(HidDevice& device);
    int getUptime(HidDevice& device);
    int getNoiseGate(HidDevice& device);
    int getSleepMode(HidDevice& device);
    int getNotificationSound(HidDevice& device);
    int getLedBrightness(HidDevice& device);
    int getMicVolume(HidDevice& device);
    std::string getBaseMac(HidDevice& device);
    std::string getFirmwareShort(HidDevice& device);   // metadata 0x10
    std::string getDeviceInfo(HidDevice& device);      // metadata 0x03
    std::string getProtocolId(HidDevice& device);      // metadata 0x05
    std::string getSerialMeta(HidDevice& device);      // metadata 0x13

    // SET (overrides)
    bool setSidetone(HidDevice& device, uint8_t level) override;
    bool setLedBrightness(HidDevice& device, uint8_t brightness) override;
    bool setInactiveTime(HidDevice& device, uint8_t minutes) override;
    bool setNotificationSound(HidDevice& device, uint8_t level) override;
    bool setEqualizerPreset(HidDevice& device, uint8_t preset) override;
    bool setMicVolume(HidDevice& device, uint8_t volume) override;
    bool setVolume(HidDevice& device, uint8_t volume) override;
    bool setNoiseGate(HidDevice& device, uint8_t mode) override;
    bool setMixamp(HidDevice& device, uint8_t level) override;

    // SET (A50-specific)
    bool setMicMute(HidDevice& device, bool mute);
    bool setCustomEqualizer(HidDevice& device, uint8_t type,
                            const uint8_t* band_data, size_t band_len);
    bool setEqualizerActive(HidDevice& device, uint8_t preset);
    bool saveEqualizerPreset(HidDevice& device);
    bool factoryReset(HidDevice& device, const std::string& serial);
    bool startBluetoothPairing(HidDevice& device);

    /// Stream routing: 5 channels (stream master + mic out + game + bluetooth + voice)
    /// Each channel: volume 0-32, mute 0/1
    struct RoutingConfig {
        uint8_t stream_vol = 26;  bool stream_mute = false;
        uint8_t mic_vol = 16;    bool mic_mute = true;
        uint8_t game_vol = 16;   bool game_mute = false;
        uint8_t bt_vol = 16;     bool bt_mute = true;
        uint8_t voice_vol = 16;  bool voice_mute = false;
    };
    RoutingConfig getRouting(HidDevice& device);
    bool setRouting(HidDevice& device, const RoutingConfig& cfg);

    /// Cached state from spontaneous reports
    struct Cache {
        std::atomic<int> battery_percent{-1};
        std::atomic<bool> battery_charging{false};
        std::atomic<int> volume{-1};        // 0-31
        std::atomic<int> mixamp{-1};        // 0-12
        std::atomic<int> hw_mic_muted{-1};   // hardware flip-to-mute: 0=on, 1=muted (spontan rapport)
        std::atomic<int> sw_mic_muted{-1};   // software mute: 0=unmute, 1=mute (0c 3b)
        std::atomic<int> power{-1};         // 0x00=on, 0x05=off
        std::atomic<int> bt_connected{-1};  // 0=no, 1=yes
        std::atomic<uint32_t> eq_checksum{0}; // EQ state hash (cmd1=0x11)
    };

    const Cache& cache() const { return cache_; }

    /// Callbacks for state changes (called from listener thread)
    enum class Event { Battery, Volume, Mixamp, MicMute, Power, Bluetooth, EqChanged };
    using EventCallback = std::function<void(Event)>;
    void setEventCallback(EventCallback cb) { event_cb_ = std::move(cb); }

private:
    static constexpr size_t MSG_SIZE = 64;
    static constexpr uint8_t REPORT_ID = 0x02;
    static constexpr uint8_t FRAME_MARKER = 0x0c;

    // GET commands
    static constexpr std::array<uint8_t, 4> CMD_GET_BATTERY   {0x03, 0x00, 0x06, 0x0b};
    static constexpr std::array<uint8_t, 4> CMD_GET_VOLUME    {0x03, 0x00, 0x08, 0x0b};
    static constexpr std::array<uint8_t, 4> CMD_GET_SIDETONE  {0x03, 0x00, 0x09, 0x0b};  // 09 0b byte[9]=level (0-6)
    static constexpr std::array<uint8_t, 4> CMD_GET_SERIAL    {0x03, 0x00, 0x04, 0x2b};
    static constexpr std::array<uint8_t, 4> CMD_GET_DEVNAME   {0x03, 0x00, 0x05, 0x0b};
    static constexpr std::array<uint8_t, 4> CMD_GET_SW_MUTE   {0x03, 0x00, 0x0c, 0x3b};  // 0c 3b byte[9]: 0=unmute, 1=mute
    static constexpr std::array<uint8_t, 4> CMD_GET_MIXAMP    {0x03, 0x00, 0x0a, 0x0b};
    static constexpr std::array<uint8_t, 4> CMD_GET_BT_STATUS {0x03, 0x00, 0x0e, 0x0b};
    static constexpr std::array<uint8_t, 4> CMD_GET_BT_NAME   {0x03, 0x00, 0x0e, 0x2b};
    static constexpr std::array<uint8_t, 4> CMD_GET_BT_INFO   {0x03, 0x00, 0x0e, 0x1b};
    static constexpr std::array<uint8_t, 4> CMD_GET_FIRMWARE  {0x03, 0x00, 0x04, 0x1b};  // 04 1b: version + "main"
    static constexpr std::array<uint8_t, 4> CMD_GET_UPTIME    {0x03, 0x00, 0x04, 0x3b};  // 04 3b: byte[8..9] BE seconds
    static constexpr std::array<uint8_t, 4> CMD_GET_NOISE_GATE{0x03, 0x00, 0x14, 0x1b};  // 14 1b: byte[6] = 0x01/02/04
    static constexpr std::array<uint8_t, 4> CMD_GET_SLEEP     {0x03, 0x00, 0x07, 0x0b};  // 07 0b: byte[6] = 0/15/30/60
    static constexpr std::array<uint8_t, 4> CMD_GET_NOTIF     {0x03, 0x00, 0x10, 0x0b};  // 10 0b: byte[6] = 0/1/2
    static constexpr std::array<uint8_t, 4> CMD_GET_LED       {0x03, 0x00, 0x0f, 0x0b};  // 0f 0b: byte[6] = 0-100
    static constexpr std::array<uint8_t, 4> CMD_GET_MIC_VOL   {0x03, 0x00, 0x0c, 0x2b};  // 0c 2b: byte[9] = 0-32
    static constexpr std::array<uint8_t, 4> CMD_GET_BASE_MAC  {0x03, 0x00, 0x0b, 0x6b};  // 0b 6b: byte[6..11] = MAC

    // SET commands
    static constexpr std::array<uint8_t, 4> CMD_SET_SIDETONE      {0x06, 0x00, 0x09, 0x1b};
    static constexpr std::array<uint8_t, 4> CMD_SET_LED           {0x04, 0x00, 0x0f, 0x1b};
    static constexpr std::array<uint8_t, 4> CMD_SET_SLEEP         {0x05, 0x00, 0x07, 0x1b};
    static constexpr std::array<uint8_t, 4> CMD_SET_NOTIFICATIONS {0x04, 0x00, 0x10, 0x1b};
    static constexpr std::array<uint8_t, 4> CMD_SET_EQ            {0x38, 0x00, 0x0d, 0x2b};
    static constexpr std::array<uint8_t, 4> CMD_SET_MIC_VOL       {0x13, 0x00, 0x0c, 0x6b};
    static constexpr std::array<uint8_t, 4> CMD_SET_VOLUME        {0x05, 0x00, 0x08, 0x1b};
    static constexpr std::array<uint8_t, 4> CMD_SET_NOISE_GATE    {0x04, 0x00, 0x14, 0x2b};
    static constexpr std::array<uint8_t, 4> CMD_SET_MIXAMP        {0x06, 0x00, 0x0a, 0x1b};
    static constexpr std::array<uint8_t, 4> CMD_SET_MIC_MUTE      {0x13, 0x00, 0x0c, 0x6b};  // byte[8]=0x00 unmute, 0x01 mute
    static constexpr std::array<uint8_t, 4> CMD_SET_CUSTOM_EQ     {0x38, 0x00, 0x0d, 0x2b};  // 56 bytes: type + 10 bands
    static constexpr std::array<uint8_t, 4> CMD_SET_EQ_ACTIVE     {0x05, 0x00, 0x0d, 0x5b};  // byte[6]=preset-nr
    static constexpr std::array<uint8_t, 4> CMD_SET_EQ_SAVE       {0x05, 0x00, 0x0d, 0x1b};  // save active EQ
    static constexpr std::array<uint8_t, 4> CMD_SET_FACTORY_RESET {0x10, 0x00, 0x04, 0x4b};  // byte[6..17]=serial
    static constexpr std::array<uint8_t, 4> CMD_SET_BT_PAIR       {0x04, 0x00, 0x0b, 0x1b};  // trigger BT search
    static constexpr std::array<uint8_t, 4> CMD_GET_ROUTING       {0x03, 0x00, 0x0c, 0x6e};  // stream routing GET
    static constexpr std::array<uint8_t, 4> CMD_SET_ROUTING       {0x13, 0x00, 0x0c, 0x6e};  // stream routing SET (19 bytes)

    Cache cache_;
    EventCallback event_cb_;
    mutable std::mutex hid_mutex_;
    std::thread listener_thread_;
    std::atomic<bool> listener_running_{false};
    std::atomic<bool> listener_paused_{false};  // pause during sendAndReceive

    void emitEvent(Event e) { if (event_cb_) event_cb_(e); }

    /// Parse a spontaneous report and update cache
    void handleSpontaneous(const uint8_t* data, size_t len);

    bool sendCommand(HidDevice& device,
                     const uint8_t* cmd, size_t cmd_len,
                     const uint8_t* data = nullptr, size_t data_len = 0);

    bool sendCommandLocked(HidDevice& device,
                           const uint8_t* cmd, size_t cmd_len,
                           const uint8_t* data = nullptr, size_t data_len = 0);

    bool sendAndReceive(HidDevice& device,
                        const uint8_t* cmd, size_t cmd_len,
                        uint8_t* response, size_t resp_len,
                        const uint8_t* data = nullptr, size_t data_len = 0);

    /// Send metadata command (byte[1] != 0x0c) and receive response
    bool sendMetadata(HidDevice& device, uint8_t type,
                      uint8_t* response, size_t resp_len);
};

} // namespace ratatoskr
