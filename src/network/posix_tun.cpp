#include "lanlink/network/posix_tun.hpp"
#include "../platform/tun_device.hpp"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/uio.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>
namespace lanlink::network {
class PosixTunAdapter::Impl {
public:
    std::uint16_t mtu;
    std::string requested;
    std::unique_ptr<platform::TunDevice> device;
    mutable std::mutex mutex;
    std::exception_ptr error;
    std::jthread worker;
    Impl(std::uint16_t size, std::string name) : mtu(size), requested(std::move(name)) {
        if (mtu < 576 || mtu > 65515) throw std::invalid_argument("invalid tunnel MTU");
    }
};
PosixTunAdapter::PosixTunAdapter(std::uint16_t mtu, std::string name)
    : impl_(std::make_unique<Impl>(mtu, std::move(name))) {}
PosixTunAdapter::~PosixTunAdapter() { stop(); }
void PosixTunAdapter::start(PacketHandler handler) {
    if (!handler) throw std::invalid_argument("missing TUN packet handler");
    std::lock_guard lock(impl_->mutex);
    if (impl_->device) throw std::logic_error("TUN is already running");
    impl_->error = {};
    impl_->device = platform::open_tun(impl_->mtu, impl_->requested);
    try {
        impl_->worker = std::jthread([this, handler = std::move(handler)](std::stop_token stop) {
            try {
                std::array<std::byte, 65540> bytes{};
                const auto fd = impl_->device->descriptor();
                const auto prefix = impl_->device->prefix_size();
                while (!stop.stop_requested()) {
                    pollfd descriptor{fd, POLLIN, 0};
                    const auto ready = poll(&descriptor, 1, 100);
                    if (ready < 0 && errno == EINTR) continue;
                    if (ready < 0) throw std::system_error(errno, std::generic_category(), "poll TUN");
                    if (!ready) continue;
                    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("TUN is unavailable");
                    const auto count = read(fd, bytes.data(), bytes.size());
                    if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                    if (count < 0) throw std::system_error(errno, std::generic_category(), "read TUN");
                    if (static_cast<std::size_t>(count) <= prefix) continue;
                    if (prefix) {
                        std::uint32_t family = 0; std::memcpy(&family, bytes.data(), 4);
                        if (ntohl(family) != AF_INET) continue;
                    }
                    const auto packet = std::span(bytes).subspan(prefix, static_cast<std::size_t>(count) - prefix);
                    if ((std::to_integer<unsigned>(packet[0]) >> 4U) == 4) handler(packet);
                }
            } catch (...) { std::lock_guard guard(impl_->mutex); impl_->error = std::current_exception(); }
        });
    } catch (...) { impl_->device.reset(); throw; }
}
void PosixTunAdapter::configure(const VirtualNetworkAddress& address) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->device) throw std::logic_error("TUN is stopped");
    impl_->device->configure(address);
}
void PosixTunAdapter::remove(const protocol::NetworkId& id) noexcept {
    std::lock_guard lock(impl_->mutex);
    if (impl_->device) impl_->device->remove(id);
}
void PosixTunAdapter::configure_routing(const RoutingConfiguration& value) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->device) {
        if (!value.routes.empty() || !value.forwards.empty()) throw std::logic_error("TUN is stopped");
        return;
    }
    impl_->device->routing(value);
}
void PosixTunAdapter::inject(std::span<const std::byte> packet) {
    if (packet.empty() || packet.size() > 65535 || (std::to_integer<unsigned>(packet[0]) >> 4U) != 4)
        throw std::invalid_argument("TUN expects IPv4");
    std::lock_guard lock(impl_->mutex);
    if (!impl_->device) return;
    const auto family = htonl(AF_INET);
    iovec segments[]{{const_cast<std::uint32_t*>(&family), impl_->device->prefix_size()},
                     {const_cast<std::byte*>(packet.data()), packet.size()}};
    const auto count = writev(impl_->device->descriptor(), segments, 2);
    if (count < 0 && (errno == EAGAIN || errno == ENOBUFS)) return;
    if (count < 0) throw std::system_error(errno, std::generic_category(), "write TUN");
    if (static_cast<std::size_t>(count) != packet.size() + impl_->device->prefix_size())
        throw std::runtime_error("incomplete TUN packet write");
}
void PosixTunAdapter::stop() noexcept {
    impl_->worker.request_stop();
    if (impl_->worker.joinable()) impl_->worker.join();
    std::lock_guard lock(impl_->mutex); impl_->device.reset();
}
std::exception_ptr PosixTunAdapter::pump_error() const noexcept { std::lock_guard lock(impl_->mutex); return impl_->error; }
std::uint16_t PosixTunAdapter::packet_mtu() const noexcept { return impl_->mtu; }
std::string PosixTunAdapter::name() const { std::lock_guard lock(impl_->mutex); return impl_->device ? impl_->device->name() : std::string{}; }
}
