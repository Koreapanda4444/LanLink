#include "lanlink/transport/wintun_adapter.hpp"

#include <winsock2.h>
#include <windows.h>
#include <netioapi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lanlink::transport {
namespace {

using AdapterHandle = void*;
using SessionHandle = void*;

struct WintunFunctions {
    AdapterHandle (WINAPI *create_adapter)(LPCWSTR, LPCWSTR, const GUID*) = nullptr;
    AdapterHandle (WINAPI *open_adapter)(LPCWSTR) = nullptr;
    void (WINAPI *close_adapter)(AdapterHandle) = nullptr;
    void (WINAPI *get_adapter_luid)(AdapterHandle, NET_LUID*) = nullptr;
    SessionHandle (WINAPI *start_session)(AdapterHandle, DWORD) = nullptr;
    void (WINAPI *end_session)(SessionHandle) = nullptr;
    HANDLE (WINAPI *get_read_wait_event)(SessionHandle) = nullptr;
    BYTE* (WINAPI *receive_packet)(SessionHandle, DWORD*) = nullptr;
    void (WINAPI *release_receive_packet)(SessionHandle, const BYTE*) = nullptr;
    BYTE* (WINAPI *allocate_send_packet)(SessionHandle, DWORD) = nullptr;
    void (WINAPI *send_packet)(SessionHandle, const BYTE*) = nullptr;
};

template <typename Function>
Function load_function(HMODULE module, const char* name) {
    const auto address = GetProcAddress(module, name);
    if (!address) {
        throw std::runtime_error(std::string{"wintun.dll is missing "} + name);
    }
    return reinterpret_cast<Function>(address);
}

std::wstring executable_directory() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        throw std::runtime_error("cannot locate lanlink-service.exe");
    }
    path.resize(length);
    const auto separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        throw std::runtime_error("cannot locate service executable directory");
    }
    return path.substr(0, separator + 1);
}

void check_windows(const DWORD code, const char* operation) {
    if (code != NO_ERROR) {
        throw std::runtime_error(std::string{operation} + " failed (Windows error " +
                                 std::to_string(code) + ")");
    }
}

std::uint32_t prefix_mask(const std::uint8_t prefix) {
    return 0xffffffffU << (32U - prefix);
}

void validate_address(const WintunNetworkAddress& address) {
    if (std::all_of(address.network_id.begin(), address.network_id.end(),
                    [](const std::byte byte) { return byte == std::byte{0}; }) ||
        address.prefix_length == 0 || address.prefix_length > 30 ||
        address.subnet != (address.address & prefix_mask(address.prefix_length)) ||
        address.address == address.subnet ||
        address.address == (address.subnet | ~prefix_mask(address.prefix_length))) {
        throw std::invalid_argument("invalid Wintun virtual IPv4 network address");
    }
}

MIB_UNICASTIPADDRESS_ROW address_row(const NET_LUID& luid,
                                     const WintunNetworkAddress& address) {
    MIB_UNICASTIPADDRESS_ROW row;
    InitializeUnicastIpAddressEntry(&row);
    row.InterfaceLuid = luid;
    row.Address.Ipv4.sin_family = AF_INET;
    row.Address.Ipv4.sin_addr.S_un.S_addr = htonl(address.address);
    row.OnLinkPrefixLength = address.prefix_length;
    return row;
}

MIB_IPFORWARD_ROW2 route_row(const NET_LUID& luid,
                             const WintunNetworkAddress& address) {
    MIB_IPFORWARD_ROW2 row;
    InitializeIpForwardEntry(&row);
    row.InterfaceLuid = luid;
    row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
    row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr = htonl(address.subnet);
    row.DestinationPrefix.PrefixLength = address.prefix_length;
    row.NextHop.Ipv4.sin_family = AF_INET;
    row.Metric = 1;
    row.Protocol = MIB_IPPROTO_NETMGMT;
    return row;
}

}

class WintunAdapter::Impl {
public:
    Impl(std::wstring name, const std::uint32_t mtu, const std::uint32_t capacity)
        : name_(std::move(name)), mtu_(mtu), capacity_(capacity) {
        if (name_.empty() || name_.size() > 127 || mtu_ < 576 || mtu_ > 65535 ||
            capacity_ < 0x20000U || capacity_ > 0x4000000U ||
            (capacity_ & (capacity_ - 1U)) != 0) {
            throw std::invalid_argument("invalid Wintun adapter settings");
        }
    }

    ~Impl() {
        stop();
        while (!addresses_.empty()) {
            remove(addresses_.begin()->first);
        }
        if (original_mtu_ != 0) {
            MIB_IPINTERFACE_ROW row;
            InitializeIpInterfaceEntry(&row);
            row.InterfaceLuid = luid_;
            row.Family = AF_INET;
            if (GetIpInterfaceEntry(&row) == NO_ERROR) {
                row.NlMtu = original_mtu_;
                static_cast<void>(SetIpInterfaceEntry(&row));
            }
        }
        if (adapter_) {
            functions_.close_adapter(adapter_);
        }
        if (module_) {
            FreeLibrary(module_);
        }
    }

    void open() {
        const auto path = executable_directory() + L"wintun.dll";
        module_ = LoadLibraryW(path.c_str());
        if (!module_) {
            throw std::runtime_error("wintun.dll must be next to lanlink-service.exe");
        }
        functions_.create_adapter = load_function<decltype(functions_.create_adapter)>(
            module_, "WintunCreateAdapter");
        functions_.open_adapter = load_function<decltype(functions_.open_adapter)>(
            module_, "WintunOpenAdapter");
        functions_.close_adapter = load_function<decltype(functions_.close_adapter)>(
            module_, "WintunCloseAdapter");
        functions_.get_adapter_luid = load_function<decltype(functions_.get_adapter_luid)>(
            module_, "WintunGetAdapterLUID");
        functions_.start_session = load_function<decltype(functions_.start_session)>(
            module_, "WintunStartSession");
        functions_.end_session = load_function<decltype(functions_.end_session)>(
            module_, "WintunEndSession");
        functions_.get_read_wait_event =
            load_function<decltype(functions_.get_read_wait_event)>(
                module_, "WintunGetReadWaitEvent");
        functions_.receive_packet = load_function<decltype(functions_.receive_packet)>(
            module_, "WintunReceivePacket");
        functions_.release_receive_packet =
            load_function<decltype(functions_.release_receive_packet)>(
                module_, "WintunReleaseReceivePacket");
        functions_.allocate_send_packet =
            load_function<decltype(functions_.allocate_send_packet)>(
                module_, "WintunAllocateSendPacket");
        functions_.send_packet = load_function<decltype(functions_.send_packet)>(
            module_, "WintunSendPacket");

        adapter_ = functions_.open_adapter(name_.c_str());
        if (!adapter_) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NOT_FOUND) {
                check_windows(error, "WintunOpenAdapter");
            }
            adapter_ = functions_.create_adapter(name_.c_str(), L"LanLink", nullptr);
            if (!adapter_) {
                check_windows(GetLastError(), "WintunCreateAdapter");
            }
        }
        functions_.get_adapter_luid(adapter_, &luid_);

        MIB_IPINTERFACE_ROW row;
        InitializeIpInterfaceEntry(&row);
        row.InterfaceLuid = luid_;
        row.Family = AF_INET;
        check_windows(GetIpInterfaceEntry(&row), "GetIpInterfaceEntry");
        if (row.NlMtu != mtu_) {
            const auto previous = row.NlMtu;
            row.NlMtu = mtu_;
            check_windows(SetIpInterfaceEntry(&row), "SetIpInterfaceEntry");
            original_mtu_ = previous;
        }
    }

    void configure(const WintunNetworkAddress& address) {
        validate_address(address);
        const auto existing = addresses_.find(address.network_id);
        if (existing != addresses_.end() && existing->second.address == address) {
            return;
        }
        for (const auto& [network_id, installed] : addresses_) {
            if (network_id != address.network_id &&
                installed.address.subnet == address.subnet &&
                installed.address.prefix_length == address.prefix_length) {
                throw std::invalid_argument("Wintun networks have overlapping routes");
            }
        }
        if (existing != addresses_.end()) {
            remove(address.network_id);
        }

        auto unicast = address_row(luid_, address);
        bool owns_unicast = false;
        const auto address_result = CreateUnicastIpAddressEntry(&unicast);
        if (address_result == NO_ERROR) {
            owns_unicast = true;
        } else if (address_result != ERROR_OBJECT_ALREADY_EXISTS) {
            check_windows(address_result, "CreateUnicastIpAddressEntry");
        }

        auto route = route_row(luid_, address);
        const auto route_result = CreateIpForwardEntry2(&route);
        if (route_result != NO_ERROR && route_result != ERROR_OBJECT_ALREADY_EXISTS) {
            if (owns_unicast) {
                static_cast<void>(DeleteUnicastIpAddressEntry(&unicast));
            }
            check_windows(route_result, "CreateIpForwardEntry2");
        }
        try {
            addresses_.emplace(address.network_id, InstalledAddress{
                address, owns_unicast, route_result == NO_ERROR});
        } catch (...) {
            if (route_result == NO_ERROR) {
                static_cast<void>(DeleteIpForwardEntry2(&route));
            }
            if (owns_unicast) {
                static_cast<void>(DeleteUnicastIpAddressEntry(&unicast));
            }
            throw;
        }
        if (addresses_.size() == 1) {
            auto multicast = route_row(luid_, {{}, 0, 0xe0000000U, 4});
            const auto result = CreateIpForwardEntry2(&multicast);
            if (result != NO_ERROR && result != ERROR_OBJECT_ALREADY_EXISTS) {
                remove(address.network_id);
                check_windows(result, "CreateIpForwardEntry2(multicast)");
            }
            owns_multicast_route_ = result == NO_ERROR;
        }
    }

    void remove(const protocol::NetworkId& network_id) noexcept {
        const auto found = addresses_.find(network_id);
        if (found == addresses_.end()) {
            return;
        }
        const auto installed = found->second;
        addresses_.erase(found);
        if (installed.owns_route) {
            auto row = route_row(luid_, installed.address);
            static_cast<void>(DeleteIpForwardEntry2(&row));
        }
        if (installed.owns_unicast) {
            auto row = address_row(luid_, installed.address);
            static_cast<void>(DeleteUnicastIpAddressEntry(&row));
        }
        if (addresses_.empty() && owns_multicast_route_) {
            auto route = route_row(luid_, {{}, 0, 0xe0000000U, 4});
            static_cast<void>(DeleteIpForwardEntry2(&route));
            owns_multicast_route_ = false;
        }
    }

    void start(PacketHandler handler) {
        if (!handler || session_) {
            throw std::invalid_argument("Wintun packet pump is already running or has no handler");
        }
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event_) {
            check_windows(GetLastError(), "CreateEventW");
        }
        session_ = functions_.start_session(adapter_, capacity_);
        if (!session_) {
            const auto error = GetLastError();
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
            check_windows(error, "WintunStartSession");
        }
        handler_ = std::move(handler);
        {
            std::lock_guard lock(error_mutex_);
            pump_error_ = nullptr;
        }
        try {
            pump_ = std::thread([this] {
                try {
                    run_pump();
                } catch (...) {
                    std::lock_guard lock(error_mutex_);
                    pump_error_ = std::current_exception();
                }
            });
        } catch (...) {
            functions_.end_session(session_);
            session_ = nullptr;
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
            handler_ = {};
            throw;
        }
    }

    void inject(const std::span<const std::byte> packet) {
        if (packet.empty() || packet.size() > 65535) {
            throw std::invalid_argument("invalid Wintun packet size");
        }
        std::lock_guard lock(send_mutex_);
        if (!session_) {
            throw std::runtime_error("Wintun session is stopped");
        }
        auto* buffer = functions_.allocate_send_packet(session_,
                         static_cast<DWORD>(packet.size()));
        if (!buffer) {
            check_windows(GetLastError(), "WintunAllocateSendPacket");
        }
        std::memcpy(buffer, packet.data(), packet.size());
        functions_.send_packet(session_, buffer);
    }

    void stop() noexcept {
        if (stop_event_) {
            SetEvent(stop_event_);
        }
        if (pump_.joinable()) {
            pump_.join();
        }
        {
            std::lock_guard lock(send_mutex_);
            if (session_) {
                functions_.end_session(session_);
                session_ = nullptr;
            }
        }
        if (stop_event_) {
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
        }
        handler_ = {};
    }

    [[nodiscard]] std::exception_ptr pump_error() const noexcept {
        std::lock_guard lock(error_mutex_);
        return pump_error_;
    }

private:
    struct InstalledAddress {
        WintunNetworkAddress address;
        bool owns_unicast = false;
        bool owns_route = false;
    };

    void run_pump() {
        const HANDLE waits[]{stop_event_, functions_.get_read_wait_event(session_)};
        if (!waits[1]) {
            check_windows(GetLastError(), "WintunGetReadWaitEvent");
        }
        for (;;) {
            if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) {
                return;
            }
            DWORD size = 0;
            const auto* packet = functions_.receive_packet(session_, &size);
            if (packet) {
                struct Release {
                    WintunFunctions& functions;
                    SessionHandle session;
                    const BYTE* packet;
                    ~Release() { functions.release_receive_packet(session, packet); }
                } release{functions_, session_, packet};
                const auto bytes = std::span<const std::byte>{
                    reinterpret_cast<const std::byte*>(packet), size};
                if (size > 0 && size <= 65535) {
                    handler_(bytes);
                }
                continue;
            }
            const auto error = GetLastError();
            if (error == ERROR_HANDLE_EOF) {
                return;
            }
            if (error != ERROR_NO_MORE_ITEMS) {
                check_windows(error, "WintunReceivePacket");
            }
            const auto result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0) {
                return;
            }
            if (result != WAIT_OBJECT_0 + 1) {
                check_windows(GetLastError(), "WaitForMultipleObjects");
            }
        }
    }

    std::wstring name_;
    std::uint32_t mtu_;
    std::uint32_t capacity_;
    HMODULE module_ = nullptr;
    WintunFunctions functions_;
    AdapterHandle adapter_ = nullptr;
    NET_LUID luid_{};
    std::uint32_t original_mtu_ = 0;
    bool owns_multicast_route_ = false;
    std::map<protocol::NetworkId, InstalledAddress> addresses_;
    SessionHandle session_ = nullptr;
    HANDLE stop_event_ = nullptr;
    PacketHandler handler_;
    std::thread pump_;
    std::mutex send_mutex_;
    mutable std::mutex error_mutex_;
    std::exception_ptr pump_error_;
};

WintunAdapter::WintunAdapter(std::wstring name, const std::uint32_t mtu,
                             const std::uint32_t ring_capacity)
    : impl_(std::make_unique<Impl>(std::move(name), mtu, ring_capacity)) {
    impl_->open();
}

WintunAdapter::~WintunAdapter() = default;

void WintunAdapter::configure(const WintunNetworkAddress& address) {
    impl_->configure(address);
}

void WintunAdapter::remove(const protocol::NetworkId& network_id) noexcept {
    impl_->remove(network_id);
}

void WintunAdapter::start(PacketHandler handler) {
    impl_->start(std::move(handler));
}

void WintunAdapter::inject(const std::span<const std::byte> packet) {
    impl_->inject(packet);
}

void WintunAdapter::stop() noexcept {
    impl_->stop();
}

std::exception_ptr WintunAdapter::pump_error() const noexcept {
    return impl_->pump_error();
}

}
