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

Windows gateway forwarding uses a dynamic Windows Filtering Platform session at the
IPv4 forwarding layer. The LanLink interface is blocked in both directions except for
explicit (member lease, approved private subnet, LAN interface) pairs. Filter replacement
is transactional. The original forwarding flag is restored before filters are removed.
LanLink does not enable forwarding on physical LAN interfaces: an administrator must
already have configured those and the return route. This avoids enabling general LAN
routing as a side effect, including after a service crash. Client routes have a 15-second
lifetime, renewed every five seconds while authorized.

Only TCP, UDP and ordinary ICMP echo/error traffic can traverse the routed tunnel.
IP options, nested tunnels, exhausted TTL, ICMP redirects/router advertisements, routed
broadcasts and multicast are denied. Each IPv4 fragment retains checked source and
destination scope. DNS servers, DNS search suffixes, NAT, Internet default routes and
physical interface forwarding flags are never modified.

The local `routing_options` IPC reads or changes the two opt-ins for the current service
session. A service restart uses the config defaults. Remote policy cannot invoke it.

Windows API references: [forwarding layer conditions](https://learn.microsoft.com/en-us/windows/win32/fwp/filtering-conditions-available-at-each-filtering-layer),
[interface settings](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-setipinterfaceentry),
[route creation](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-createipforwardentry2).

ACL enforcement is independent of the desktop. Managed senders refuse denied unicast
destinations; modified senders are still denied by relay routing, candidate authorization,
direct session checks and the receiver's active peer cipher. Every edit rotates the epoch.
Membership removal also prunes device-specific rules, hub/gateway assignments, routed
subnets and roles in the membership transaction. Rejoining does not restore former
privileges. An empty hub set after removal is fail-closed until an authorized edit.

Groups are named sets (one group per assignment, otherwise `members`). Rules can
match a device, a group, or all members. Deny wins across all matching rules. The
owner alone grants/removes roles. Administrators manage ACL/topology/routes and
ordinary membership, but cannot change roles, disable managed mode, remove another
administrator or the owner, delete the network, or publish owner key envelopes.
Ordinary members can inspect policy and request gateway status. Restricted members
cannot manage, use gateway routes, become gateways, or send/receive discovery fanout;
explicitly allowed unicast remains available.

Privileged role assignments are revoked on membership removal. Restricted assignments
are retained as rejoin restrictions until the owner clears them, so leaving and rejoining
cannot escalate a restricted member. Pending approvals are returned only to authorized
managers, including after a manager/service reconnect.

Administrative audit records contain only a fixed event kind/result, relay timestamp,
actor/target IDs, network ID and policy revision. Passwords, enrollment codes, tokens,
invitation URLs, network/account labels and packet data are never copied into audit.
Policy changes and their audit evidence commit atomically. Admission/account events
are appended after their existing operations; audit write failures do not undo a
successful admission and are exposed as a manager troubleshooting counter. Failed
access/policy events are limited to one per second per actor, with bounded tracking.

SQLite schema 8 keeps at most 256 events per network, 10,000 overall and 30 days.
Authorized managers can retrieve the newest 64 events; ordinary/restricted members
cannot read them. Age filtering also applies when reading an inactive network. This is
a bounded operational history, not a tamper-proof compliance ledger. Relay operators
retain administrative control of the database.

In the Windows desktop, choose **Network management**. Draft changes in Topology,
Access rules, Groups and roles, or Gateways and routes, then use **Preview changes**
and review allowed/blocked device-pair counts before applying. The desktop keeps a
stale draft until it is explicitly reloaded; the relay rejects its old revision. Members
see the same policy in read-only form. Local routing uses a separate confirmation.
The Audit and approvals tab shows bounded history and persisted join requests to managers.

`lanlink-ui-cli manage NETWORK_ID` prints policy counts and available audit metadata.
`lanlink-ui-cli routing status|off|routes|gateway|both` reads/changes the current service
session's local route opt-ins. `lanlink-ui --management` opens the management dialog.

Managed authority also has a six-second monotonic lease, renewed by authenticated
peer-state requests. New/changed managed states remain inactive until the exact policy
is confirmed. Renewal uses request send time, so delayed responses and unsolicited
duplicates cannot extend old authority. Missing confirmations remove managed snapshots,
direct authorization and adapter routing even while the Relay transport still reports
authenticated. Renewal is paced to stay below control-request limits; very large or
slow sets of managed networks may expire safely if confirmations cannot keep up.
TCP/UDP fragments are supported; fragmented ICMP and IP options are excluded.
