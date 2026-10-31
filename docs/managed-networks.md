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
