#include "hid_device.hpp"
#include <cstring>
#include <iostream>

namespace ratatoskr {

HidDevice::~HidDevice() {
    close();
}

HidDevice::HidDevice(HidDevice&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

HidDevice& HidDevice::operator=(HidDevice&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

bool HidDevice::open(uint16_t vendor_id, uint16_t product_id, uint16_t usage_page) {
    close();

    auto* devs = hid_enumerate(vendor_id, product_id);
    if (!devs) return false;

    const char* path = nullptr;
    for (auto* d = devs; d; d = d->next) {
        if (d->usage_page == usage_page) {
            path = d->path;
            break;
        }
    }

    if (path) {
        handle_ = hid_open_path(path);
    }

    hid_free_enumeration(devs);
    return handle_ != nullptr;
}

void HidDevice::close() {
    if (handle_) {
        hid_close(handle_);
        handle_ = nullptr;
    }
}

bool HidDevice::write(const uint8_t* data, size_t length) {
    if (!handle_) return false;
    int result = hid_write(handle_, data, length);
    if (result < 0) {
        close();
        return false;
    }
    return true;
}

int HidDevice::read(uint8_t* buffer, size_t length, int timeout_ms) {
    if (!handle_) return -1;
    int result = hid_read_timeout(handle_, buffer, length, timeout_ms);
    if (result < 0) {
        // Device likely disconnected — close handle so isOpen() returns false
        close();
    }
    return result;
}

std::string HidDevice::lastError() const {
    if (!handle_) return "Device not open";
    const wchar_t* err = hid_error(handle_);
    if (!err) return "";

    // Convert wchar_t to string
    std::string result;
    for (const wchar_t* p = err; *p; ++p) {
        result += static_cast<char>(*p);
    }
    return result;
}

} // namespace ratatoskr
