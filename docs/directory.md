# Directory bootstrap

`bootstrap_mode=manual` remains the default and never performs a directory request. `relay_host` and `relay_port` remain the manual override and fallback. Set `bootstrap_mode=official` or `self_hosted`, `directory_url=https://directory.example.com/v1/relays`, `directory_public_key=<64 hex Ed25519 digits>` and optionally `directory_relay_id=region-id` to enable discovery. Both modes verify HTTPS and the independently pinned catalog key; neither requires a user account. No official directory is deployed or hardcoded in this build.

The API is GET `/v1/relays`, HTTP 200, `application/vnd.lanlink.directory`, at most 16384 bytes. The canonical signed body is magic `LLD1`, big-endian uint64 issued/expires timestamps in milliseconds, uint8 entry count, then uint16-length-prefixed ASCII ID and hostname plus uint16 port for each entry. A 64-byte Ed25519 signature follows. Up to 32 unique IDs and a validity window of at most 24 hours are accepted. Future, expired, unsigned, modified and oversized catalogs are rejected. No redirects or catalog-provided trust keys are accepted. Selection failure retains the explicitly configured manual relay.

Generate a one-hour catalog with `lanlink-directory-sign data/directory.identity relays.bin kr relay.example.com 4433`. Pin the printed public key in client configuration through a trusted channel. Keep the signing identity offline. Run `python deploy/directory/serve.py --catalog relays.bin --cert cert.pem --key key.pem` behind a managed HTTPS endpoint, refreshing the signed file before expiry. The serving process cannot sign catalogs and supports only read requests. Use a valid DNS/IP certificate trusted by the client's OS (Windows) or OpenSSL (Linux). Local verification may use a fixture CA through SSL_CERT_FILE; production verification is never disabled.

Legacy LLD1 catalogs retain their existing selection and do not enable automatic failover. Directory discovery never distributes or replaces authentication tokens.

## Regional catalogs

LLD2 adds a nonzero catalog revision, an explicit `official` or `self_hosted` scope and per-relay region, capacity, wire protocol version, shared authority ID and pinned Ed25519 health key. All fields are covered by the directory signature. Scope must match the configured bootstrap mode. Clients exclude unsupported protocol versions and never form a failover pool across authority IDs. A configured `directory_relay_id` pins one endpoint. Operators must only group relays that use the same live authority; independent SQLite databases are separate networks even if their names match.

```sh
lanlink-directory-sign --regional self_hosted 1 data/directory.identity relays.bin kr relay.example.com 4433 kr-seoul 128 home HEALTH_PUBLIC_KEY
```

The additional canonical header after issued/expires is uint64 revision and uint8 scope (1 official, 2 self-hosted). Each LLD2 endpoint appends uint16-length-prefixed region, uint32 capacity, uint16 protocol, length-prefixed authority and 32 health-key bytes. Endpoint addresses and IDs must be unique; IDs/authorities are at most 64 ASCII characters, regions 32, hosts 253, capacity 1..1,000,000. Existing size, lifetime and signature checks remain. The unsigned HTTPS serving process cannot add a node, replace a trust key or change load metadata.
