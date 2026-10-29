#include "lanlink/network/linux_tap.hpp"
#include "lanlink/network/ethernet.hpp"
#include "lanlink/protocol/ipv4_fanout.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace lanlink::network {
namespace {
void checked(int result, const char* operation) {
    if (result < 0) throw std::system_error(errno, std::generic_category(), operation);
}
}
struct LinuxTapAdapter::Impl {
    std::string requested;
    std::string actual;
    int tap = -1;
    int control = -1;
    std::optional<VirtualNetworkAddress> address;
    mutable std::mutex mutex;
    std::exception_ptr error;
    std::jthread worker;

    explicit Impl(std::string name) : requested(std::move(name)) {
        if (requested.empty() || requested.size() >= IFNAMSIZ ||
            !std::all_of(requested.begin(), requested.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '%';
            }) || (requested.find('%') != std::string::npos && requested != "lltap%d"))
            throw std::invalid_argument("invalid TAP interface name");
    }
    ifreq request() const {
        ifreq value{};
        std::copy(actual.begin(), actual.end(), value.ifr_name);
        return value;
    }
    void down() noexcept {
        if (control < 0 || actual.empty()) return;
        auto value = request();
        if (ioctl(control, SIOCGIFFLAGS, &value) == 0) {
            value.ifr_flags = static_cast<short>(value.ifr_flags & ~IFF_UP);
            static_cast<void>(ioctl(control, SIOCSIFFLAGS, &value));
        }
        address.reset();
    }
    void close() noexcept {
        down();
        if (tap >= 0) ::close(tap);
        if (control >= 0) ::close(control);
        tap = control = -1;
        actual.clear();
    }
    void reject_overlap(const VirtualNetworkAddress& network) {
        ifaddrs* list = nullptr;
        checked(getifaddrs(&list), "enumerate TAP route conflicts");
        const auto release = std::unique_ptr<ifaddrs, decltype(&freeifaddrs)>(list, freeifaddrs);
        const auto mask = 0xffffffffU << (32U - network.prefix_length);
        for (auto* it = list; it; it = it->ifa_next) {
            if (!it->ifa_addr || !it->ifa_netmask || it->ifa_addr->sa_family != AF_INET ||
                actual == it->ifa_name) continue;
            const auto other = ntohl(reinterpret_cast<sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr);
            const auto other_mask = ntohl(reinterpret_cast<sockaddr_in*>(it->ifa_netmask)->sin_addr.s_addr);
            if ((other & mask & other_mask) == (network.subnet & mask & other_mask))
                throw std::runtime_error("TAP subnet overlaps an existing interface");
        }
    }
};

LinuxTapAdapter::LinuxTapAdapter(std::string name) : impl_(std::make_unique<Impl>(std::move(name))) {}
LinuxTapAdapter::~LinuxTapAdapter() { stop(); }

void LinuxTapAdapter::start(PacketHandler handler) {
    if (!handler) throw std::invalid_argument("TAP packet handler is missing");
    std::lock_guard lock(impl_->mutex);
    if (impl_->tap >= 0) throw std::logic_error("TAP is already running");
    try {
        impl_->error = {};
        impl_->tap = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
        checked(impl_->tap, "open Linux TAP");
        ifreq request{};
        request.ifr_flags = static_cast<short>(IFF_TAP | IFF_NO_PI | IFF_TUN_EXCL);
        std::copy(impl_->requested.begin(), impl_->requested.end(), request.ifr_name);
        checked(ioctl(impl_->tap, TUNSETIFF, &request), "create isolated Linux TAP");
        impl_->actual = request.ifr_name;
        impl_->control = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        checked(impl_->control, "open TAP control socket");
        impl_->worker = std::jthread([this, handler = std::move(handler)](std::stop_token stop) {
            try {
                std::array<std::byte, 65536> frame{};
                while (!stop.stop_requested()) {
                    pollfd descriptor{impl_->tap, POLLIN, 0};
                    const auto ready = poll(&descriptor, 1, 100);
                    if (ready < 0 && errno == EINTR) continue;
                    checked(ready, "poll Linux TAP");
                    if (!ready) continue;
                    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
                        throw std::runtime_error("Linux TAP became unavailable");
                    std::vector<std::byte> packet;
                    {
                        std::lock_guard guard(impl_->mutex);
                        const auto size = read(impl_->tap, frame.data(), frame.size());
                        if (size < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                        checked(static_cast<int>(size), "read Linux TAP");
                        if (!impl_->address) continue;
                        try {
                            packet = encapsulate_ethernet(std::span(frame).first(static_cast<std::size_t>(size)), *impl_->address);
                        } catch (const std::invalid_argument&) { continue; }
                    }
                    handler(packet);
                }
            } catch (...) {
                std::lock_guard guard(impl_->mutex);
                impl_->error = std::current_exception();
            }
        });
    } catch (...) {
        impl_->close();
        throw;
    }
}

void LinuxTapAdapter::configure(const VirtualNetworkAddress& address) {
    if (!protocol::is_ipv4_host(address.address, address.subnet, address.prefix_length))
        throw std::invalid_argument("invalid TAP network");
    std::lock_guard lock(impl_->mutex);
    if (impl_->tap < 0) throw std::logic_error("TAP is stopped");
    if (impl_->address == address) return;
    if (impl_->address) throw std::runtime_error("TAP accepts one stable virtual network; restart after an address change");
    impl_->reject_overlap(address);
    auto value = impl_->request();
    const auto set_address = [&](unsigned long operation, std::uint32_t ipv4) {
        auto* socket_address = reinterpret_cast<sockaddr_in*>(&value.ifr_addr);
        socket_address->sin_family = AF_INET;
        socket_address->sin_addr.s_addr = htonl(ipv4);
        checked(ioctl(impl_->control, operation, &value), "configure TAP IPv4");
    };
    try {
        value.ifr_hwaddr.sa_family = ARPHRD_ETHER;
        const auto mac = virtual_mac(address.address);
        std::memcpy(value.ifr_hwaddr.sa_data, mac.data(), mac.size());
        checked(ioctl(impl_->control, SIOCSIFHWADDR, &value), "configure TAP MAC");
        value = impl_->request(); value.ifr_mtu = ethernet_mtu;
        checked(ioctl(impl_->control, SIOCSIFMTU, &value), "configure TAP MTU");
        value = impl_->request(); set_address(SIOCSIFADDR, address.address);
        value = impl_->request(); set_address(SIOCSIFNETMASK, 0xffffffffU << (32U - address.prefix_length));
        value = impl_->request();
        checked(ioctl(impl_->control, SIOCGIFFLAGS, &value), "read TAP interface flags");
        value.ifr_flags = static_cast<short>(value.ifr_flags | IFF_UP);
        checked(ioctl(impl_->control, SIOCSIFFLAGS, &value), "start TAP interface");
        impl_->address = address;
    } catch (...) { impl_->down(); throw; }
}

void LinuxTapAdapter::remove(const protocol::NetworkId& network_id) noexcept {
    std::lock_guard lock(impl_->mutex);
    if (impl_->address && impl_->address->network_id == network_id) impl_->down();
}
void LinuxTapAdapter::inject(std::span<const std::byte> packet) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->address || impl_->tap < 0) return;
    const auto frame = decapsulate_ethernet(packet, *impl_->address);
    const auto size = write(impl_->tap, frame.data(), frame.size());
    if (size < 0 && errno == EAGAIN) return;
    checked(static_cast<int>(size), "write Linux TAP");
    if (static_cast<std::size_t>(size) != frame.size()) throw std::runtime_error("partial TAP frame write");
}
void LinuxTapAdapter::stop() noexcept {
    impl_->worker.request_stop();
    if (impl_->worker.joinable()) impl_->worker.join();
    std::lock_guard lock(impl_->mutex);
    impl_->close();
}
std::exception_ptr LinuxTapAdapter::pump_error() const noexcept {
    std::lock_guard lock(impl_->mutex); return impl_->error;
}
std::string LinuxTapAdapter::name() const {
    std::lock_guard lock(impl_->mutex); return impl_->actual;
}

}
