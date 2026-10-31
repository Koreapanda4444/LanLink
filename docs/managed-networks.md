# Managed networks (v0.7 development)

Existing networks retain their original mesh behavior. An owner can explicitly enable a
managed policy. Managed mesh is default deny: matching allow rules permit a device pair;
any matching deny wins. The same predicate guards relay delivery, candidate forwarding,
direct QUIC authorization and packet decryption.

Policies are bounded, stored transactionally in SQLite schema 7, and edited using a
compare-and-swap revision. Every policy update advances the packet key epoch and peer
revision, invalidating prior direct sessions. An owner must be online to distribute the
new network key. Older clients retain legacy networks, but receive revocation instead of
managed peer states and cannot receive managed network keys or relay traffic.

The authenticated `network_management` RPC is also available through local service IPC.
No arbitrary shell commands or operating-system settings are accepted by the policy API.

Hub-and-spoke adds an independent ceiling to the ACL: a pair must include a
designated hub. Even a wildcard allow cannot connect two spokes. Broadcast fanout
uses the same check. An unavailable hub does not authorize a spoke-to-spoke fallback.
The hub is an endpoint, not an implicit packet router between spokes.

Gateway registration requires two actions: the member requests gateway status, and the
owner approves that pending device. A member cannot approve itself or name another
device in its request. Approval alone does not enable routes or OS forwarding. Removing
approval invalidates the policy epoch; routes may only refer to approved gateways.

Approved gateways can advertise canonical RFC1918 IPv4 subnets (/8 through /30,
within the appropriate private range). Default, public, loopback, multicast, overlapping
remote routes and the complete 10.64.0.0/10 overlay pool are rejected. Clients also
reject overlap with existing OS routes. Each route requires the gateway ACL and, when
specified, the member's group.

Routed packets use authenticated encrypted IPv4-in-IPv4 between member and gateway.
The inner source/destination is checked against the peer lease and approved route in
both directions. Inner fragmentation accounts for the 20-byte tunnel header. The relay
continues to see ciphertext only. LAN return routes and firewall access must be set up
by the LAN administrator; LanLink does not silently configure NAT.

Local configuration defaults `accept_routed_subnets=false` and `gateway_forwarding=false`.
Remote approval cannot enable either option on a host. Windows client routes are owned
by LanLink and removed on policy withdrawal, disconnect and clean shutdown. DNS and
the Internet default route are never changed. The optional Linux TAP backend does not
install routed subnets; full Linux/macOS L3 adapters belong to the next milestone.
