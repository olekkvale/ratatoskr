#include "device_manager.hpp"
#include "drivers/a50_gen5.hpp"
#include <sdbus-c++/sdbus-c++.h>
#include <hidapi/hidapi.h>
#include <iostream>
#include <csignal>
#include <thread>
#include <atomic>

static std::atomic<bool> running{true};

static void signalHandler(int) {
    running = false;
}

using namespace ratatoskr;

static std::vector<std::unique_ptr<sdbus::IObject>> dbus_objects;

static void exportDevice(sdbus::IConnection& conn, ManagedDevice& dev) {
    std::string path = "/org/ratatoskr/devices/a50_gen5";

    auto& obj = dbus_objects.emplace_back(sdbus::createObject(conn, sdbus::ObjectPath{path}));

    const auto iface = sdbus::InterfaceName{"org.ratatoskr.Device"};

    // Properties
    obj->addVTable(
        sdbus::registerProperty("Name")
            .withGetter([&dev]() -> std::string { return dev.driver->name(); }),
        sdbus::registerProperty("Connected")
            .withGetter([&dev]() -> bool { return dev.connected; })
    ).forInterface(iface);

    // Methods
    obj->addVTable(
        sdbus::registerMethod("GetBattery")
            .implementedAs([&dev]() -> sdbus::Struct<int32_t, bool> {
                if (!dev.connected) return {-1, false};
                auto b = dev.driver->getBattery(dev.hid);
                if (!b) return {-1, false};
                return {b->percent, b->charging};
            }),
        sdbus::registerMethod("GetBatteryPercent")
            .implementedAs([&dev]() -> int32_t {
                if (!dev.connected) return -1;
                auto b = dev.driver->getBattery(dev.hid);
                return b ? b->percent : -1;
            }),
        sdbus::registerMethod("GetBatteryCharging")
            .implementedAs([&dev]() -> int32_t {
                if (!dev.connected) return 0;
                auto b = dev.driver->getBattery(dev.hid);
                return (b && b->charging) ? 1 : 0;
            }),
        sdbus::registerMethod("GetChatmix")
            .implementedAs([&dev]() -> int32_t {
                if (!dev.connected) return -1;
                auto c = dev.driver->getChatmix(dev.hid);
                if (!c) return -1;
                return c->level;
            }),
        sdbus::registerMethod("GetVolume")
            .implementedAs([&dev]() -> int32_t {
                if (!dev.connected) return -1;
                auto v = dev.driver->getVolume(dev.hid);
                if (!v) return -1;
                return v->level;
            }),
        sdbus::registerMethod("GetSidetone")
            .implementedAs([&dev]() -> int32_t {
                if (!dev.connected) return -1;
                auto s = dev.driver->getSidetone(dev.hid);
                if (!s) return -1;
                return s->level;
            }),
        sdbus::registerMethod("GetMicMute")
            .implementedAs([&dev]() -> bool {
                if (!dev.connected) return false;
                auto m = dev.driver->getMicMute(dev.hid);
                if (!m) return false;
                return m->muted;
            }),
        sdbus::registerMethod("GetSerialNumber")
            .implementedAs([&dev]() -> std::string {
                if (!dev.connected) return "";
                auto s = dev.driver->getSerialNumber(dev.hid);
                return s.value_or("");
            }),
        sdbus::registerMethod("GetDeviceName")
            .implementedAs([&dev]() -> std::string {
                if (!dev.connected) return "";
                auto n = dev.driver->getDeviceName(dev.hid);
                return n.value_or("");
            }),
        sdbus::registerMethod("GetBluetoothStatus")
            .implementedAs([&dev]() -> bool {
                if (!dev.connected) return false;
                auto b = dev.driver->getBluetoothStatus(dev.hid);
                if (!b) return false;
                return b->connected;
            }),
        sdbus::registerMethod("GetBluetoothName")
            .implementedAs([&dev]() -> std::string {
                if (!dev.connected) return "";
                auto n = dev.driver->getBluetoothName(dev.hid);
                return n.value_or("");
            }),
        sdbus::registerMethod("GetBluetoothAddress")
            .implementedAs([&dev]() -> std::string {
                if (!dev.connected) return "";
                auto a = dev.driver->getBluetoothAddress(dev.hid);
                return a.value_or("");
            }),
        sdbus::registerMethod("SetSidetone")
            .implementedAs([&dev](int32_t level) -> bool {
                return dev.connected && dev.driver->setSidetone(dev.hid, static_cast<uint8_t>(level));
            }),
        sdbus::registerMethod("SetVolume")
            .implementedAs([&dev](int32_t volume) -> bool {
                return dev.connected && dev.driver->setVolume(dev.hid, static_cast<uint8_t>(volume));
            }),
        sdbus::registerMethod("SetMixamp")
            .implementedAs([&dev](int32_t level) -> bool {
                return dev.connected && dev.driver->setMixamp(dev.hid, static_cast<uint8_t>(level));
            }),
        sdbus::registerMethod("SetEqualizerPreset")
            .implementedAs([&dev](int32_t preset) -> bool {
                return dev.connected && dev.driver->setEqualizerPreset(dev.hid, static_cast<uint8_t>(preset));
            }),
        sdbus::registerMethod("SetNoiseGate")
            .implementedAs([&dev](int32_t mode) -> bool {
                return dev.connected && dev.driver->setNoiseGate(dev.hid, static_cast<uint8_t>(mode));
            }),
        sdbus::registerMethod("SetNotificationSound")
            .implementedAs([&dev](int32_t level) -> bool {
                return dev.connected && dev.driver->setNotificationSound(dev.hid, static_cast<uint8_t>(level));
            }),
        sdbus::registerMethod("SetLedBrightness")
            .implementedAs([&dev](int32_t brightness) -> bool {
                return dev.connected && dev.driver->setLedBrightness(dev.hid, static_cast<uint8_t>(brightness));
            }),
        sdbus::registerMethod("SetInactiveTime")
            .implementedAs([&dev](int32_t minutes) -> bool {
                return dev.connected && dev.driver->setInactiveTime(dev.hid, static_cast<uint8_t>(minutes));
            })
    ).forInterface(iface);

    // A50-specific methods
    if (auto* a50 = dynamic_cast<A50Gen5Driver*>(dev.driver.get())) {
        obj->addVTable(
            sdbus::registerMethod("GetFirmwareVersion")
                .implementedAs([&dev, a50]() -> std::string {
                    if (!dev.connected) return "";
                    return a50->getFirmwareVersion(dev.hid);
                }),
            sdbus::registerMethod("GetUptime")
                .implementedAs([&dev, a50]() -> int32_t {
                    if (!dev.connected) return -1;
                    return a50->getUptime(dev.hid);
                }),
            sdbus::registerMethod("GetNoiseGate")
                .implementedAs([&dev, a50]() -> int32_t {
                    if (!dev.connected) return -1;
                    return a50->getNoiseGate(dev.hid);
                }),
            sdbus::registerMethod("GetSleepMode")
                .implementedAs([&dev, a50]() -> int32_t {
                    if (!dev.connected) return -1;
                    return a50->getSleepMode(dev.hid);
                }),
            sdbus::registerMethod("GetNotificationSound")
                .implementedAs([&dev, a50]() -> int32_t {
                    if (!dev.connected) return -1;
                    return a50->getNotificationSound(dev.hid);
                }),
            sdbus::registerMethod("GetLedBrightness")
                .implementedAs([&dev, a50]() -> int32_t {
                    if (!dev.connected) return -1;
                    return a50->getLedBrightness(dev.hid);
                }),
            sdbus::registerMethod("GetBaseMac")
                .implementedAs([&dev, a50]() -> std::string {
                    if (!dev.connected) return "";
                    return a50->getBaseMac(dev.hid);
                }),
            sdbus::registerMethod("GetPower")
                .implementedAs([&dev, a50]() -> int32_t {
                    if (!dev.connected) return -1;
                    return a50->getPower(dev.hid);
                }),
            sdbus::registerMethod("GetFirmwareShort")
                .implementedAs([&dev, a50]() -> std::string {
                    if (!dev.connected) return "";
                    return a50->getFirmwareShort(dev.hid);
                }),
            sdbus::registerMethod("GetDeviceInfo")
                .implementedAs([&dev, a50]() -> std::string {
                    if (!dev.connected) return "";
                    return a50->getDeviceInfo(dev.hid);
                }),
            sdbus::registerMethod("GetProtocolId")
                .implementedAs([&dev, a50]() -> std::string {
                    if (!dev.connected) return "";
                    return a50->getProtocolId(dev.hid);
                }),
            sdbus::registerMethod("GetSerialMeta")
                .implementedAs([&dev, a50]() -> std::string {
                    if (!dev.connected) return "";
                    return a50->getSerialMeta(dev.hid);
                }),
            sdbus::registerMethod("SetMicMute")
                .implementedAs([&dev, a50](bool mute) -> bool {
                    return dev.connected && a50->setMicMute(dev.hid, mute);
                }),
            sdbus::registerMethod("SetCustomEqualizer")
                .implementedAs([&dev, a50](int32_t type, std::vector<uint8_t> bands) -> bool {
                    if (!dev.connected || bands.size() != 50) return false;
                    return a50->setCustomEqualizer(dev.hid, static_cast<uint8_t>(type),
                                                    bands.data(), bands.size());
                }),
            sdbus::registerMethod("SetEqualizerActive")
                .implementedAs([&dev, a50](int32_t preset) -> bool {
                    return dev.connected && a50->setEqualizerActive(dev.hid, static_cast<uint8_t>(preset));
                }),
            sdbus::registerMethod("SaveEqualizerPreset")
                .implementedAs([&dev, a50]() -> bool {
                    return dev.connected && a50->saveEqualizerPreset(dev.hid);
                }),
            sdbus::registerMethod("FactoryReset")
                .implementedAs([&dev, a50](std::string serial) -> bool {
                    return dev.connected && a50->factoryReset(dev.hid, serial);
                }),
            sdbus::registerMethod("StartBluetoothPairing")
                .implementedAs([&dev, a50]() -> bool {
                    return dev.connected && a50->startBluetoothPairing(dev.hid);
                }),
            sdbus::registerMethod("GetRouting")
                .implementedAs([&dev, a50]() -> sdbus::Struct<int32_t, bool, int32_t, bool, int32_t, bool, int32_t, bool, int32_t, bool> {
                    if (!dev.connected) return {16, false, 16, true, 16, false, 16, true, 16, false};
                    auto cfg = a50->getRouting(dev.hid);
                    return {cfg.stream_vol, cfg.stream_mute,
                            cfg.mic_vol, cfg.mic_mute,
                            cfg.game_vol, cfg.game_mute,
                            cfg.bt_vol, cfg.bt_mute,
                            cfg.voice_vol, cfg.voice_mute};
                }),
            sdbus::registerMethod("SetRouting")
                .implementedAs([&dev, a50](int32_t streamVol, bool streamMute,
                                           int32_t micVol, bool micMute,
                                           int32_t gameVol, bool gameMute,
                                           int32_t btVol, bool btMute,
                                           int32_t voiceVol, bool voiceMute) -> bool {
                    if (!dev.connected) return false;
                    A50Gen5Driver::RoutingConfig cfg;
                    cfg.stream_vol = streamVol; cfg.stream_mute = streamMute;
                    cfg.mic_vol = micVol;       cfg.mic_mute = micMute;
                    cfg.game_vol = gameVol;     cfg.game_mute = gameMute;
                    cfg.bt_vol = btVol;         cfg.bt_mute = btMute;
                    cfg.voice_vol = voiceVol;   cfg.voice_mute = voiceMute;
                    return a50->setRouting(dev.hid, cfg);
                })
        ).forInterface(iface);
    }

    // Signals
    obj->addVTable(
        sdbus::registerSignal("BatteryChanged")
            .withParameters<int32_t, bool>(),
        sdbus::registerSignal("VolumeChanged")
            .withParameters<int32_t>(),
        sdbus::registerSignal("MicMuteChanged")
            .withParameters<bool>(),
        sdbus::registerSignal("MixampChanged")
            .withParameters<int32_t>(),
        sdbus::registerSignal("PowerChanged")
            .withParameters<int32_t>(),
        sdbus::registerSignal("BluetoothChanged")
            .withParameters<bool>()
    ).forInterface(iface);

    // Wire event callback to D-Bus signals
    if (auto* a50 = dynamic_cast<A50Gen5Driver*>(dev.driver.get())) {
        auto* obj_ptr = obj.get();
        a50->setEventCallback([obj_ptr, a50, iface](A50Gen5Driver::Event event) {
            const auto& c = a50->cache();
            switch (event) {
                case A50Gen5Driver::Event::Battery:
                    obj_ptr->emitSignal("BatteryChanged")
                        .onInterface(iface)
                        .withArguments(static_cast<int32_t>(c.battery_percent.load()),
                                       c.battery_charging.load());
                    break;
                case A50Gen5Driver::Event::Volume:
                    obj_ptr->emitSignal("VolumeChanged")
                        .onInterface(iface)
                        .withArguments(static_cast<int32_t>(c.volume.load()));
                    break;
                case A50Gen5Driver::Event::MicMute:
                    obj_ptr->emitSignal("MicMuteChanged")
                        .onInterface(iface)
                        .withArguments(c.hw_mic_muted.load() != 0);
                    break;
                case A50Gen5Driver::Event::Mixamp:
                    obj_ptr->emitSignal("MixampChanged")
                        .onInterface(iface)
                        .withArguments(static_cast<int32_t>(c.mixamp.load()));
                    break;
                case A50Gen5Driver::Event::Power:
                    obj_ptr->emitSignal("PowerChanged")
                        .onInterface(iface)
                        .withArguments(static_cast<int32_t>(c.power.load()));
                    break;
                case A50Gen5Driver::Event::Bluetooth:
                    obj_ptr->emitSignal("BluetoothChanged")
                        .onInterface(iface)
                        .withArguments(c.bt_connected.load() != 0);
                    break;
                default:
                    break;
            }
        });
    }

    // D-Bus object exported
}

int main() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "ratatoskrd v0.1.0 starting...\n";

    if (hid_init() != 0) {
        std::cerr << "Failed to initialize HIDAPI\n";
        return 1;
    }

    auto conn = sdbus::createSystemBusConnection(sdbus::ServiceName{"org.ratatoskr"});

    DeviceManager manager;
    manager.scan();

    for (auto& dev : manager.devices()) {
        if (dev.connected) {
            exportDevice(*conn, dev);
            if (auto* a50 = dynamic_cast<A50Gen5Driver*>(dev.driver.get())) {
                a50->startListener(dev.hid);
            }
        } else {
            // Pre-export so D-Bus object exists before device connects
            exportDevice(*conn, dev);
        }
    }

    // Handle reconnect after USB re-enumeration
    manager.setDeviceCallback([](const ManagedDevice&, bool) {
        // Listener thread auto-resumes via device.isOpen() check
    });

    std::cout << "ratatoskrd running.\n";

    // Run D-Bus event loop in its own thread
    std::thread dbus_thread([&conn]() {
        conn->enterEventLoop();
    });

    // Main thread handles udev and periodic tasks
    while (running) {
        manager.processUdevEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    conn->leaveEventLoop();
    dbus_thread.join();

    std::cout << "ratatoskrd shutting down.\n";
    hid_exit();
    return 0;
}
