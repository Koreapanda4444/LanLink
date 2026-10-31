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
