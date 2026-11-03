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


## Authenticated health and load

Configure `relay_id`, `relay_authority`, `relay_health_identity_file` and the existing connection limits on each node. `lanlink-relay --health-key relay.conf` creates or reads that private identity and prints only its public key for the signed regional catalog. Keep the identity file private and persistent; a directory response cannot supply its own replacement pin.

Clients probe through certificate/hostname-verified TLS with a fresh 32-byte nonce. The server signs its ID, authority, protocol, nonce, timestamp, uptime, capacity, active connection count and aggregate encrypted forwarding/drop counters. Clients require the configured pin, exact authority/capacity, matching challenge and at most 30 seconds clock skew. Nonces prevent response replay; samples are short-lived and never authorize network membership. No client token, device ID, network name or packet content is sent by the probe. Signals authenticate the server; this read-only aggregate endpoint is available before client authentication and does not enroll a device. Admission limits, a global signing rate limit and handshake deadlines also apply to probes.

## 실측 Relay 선택

LLD2에서 같은 authority의 호환 Relay만 후보로 유지한다. 접속 전에 호스트 인증 TLS와 디렉터리에 고정된 서명 키로 상태를 검증하며 최대 4개 probe를 동시에 실행한다. 점수는 실측 응답 RTT + 서명된 authority 상행 RTT + 연결 부하(최대 15ms)다. 건강하고 여유 용량이 있는 5초 이내 관측만 접속 후보가 된다. 단순 지역명이나 목록 순서는 우선순위가 아니다.

정상 QUIC 연결은 30초마다 다시 측정한다. 현재 Relay를 최소 30초 유지하고 25% 초과 개선일 때 전환한다. 실패는 유지 시간보다 우선하며 1~30초의 실패 노드 대기 시간을 적용한다. 안정된 TLS/TCP 폴백 연결은 끊길 때 다시 선택하고, 기존 UDP 복구 probe는 유지한다. 인증 실패를 우회해 다른 authority로 접속하지 않는다.

디렉터리는 만료 60초 전부터 갱신한다. 낮아진 revision, 바뀐 authority, 서명/범위 오류를 거부하고 키 교체 시 이전 관측을 폐기한다. 만료된 목록으로 새 연결을 시작하지 않으며 기존 정상 연결은 유지한다. `directory_relay_id`는 한 Relay로 고정한다. `bootstrap_mode=manual`과 기존 LLD1은 자동 선택을 사용하지 않는다. 실제 선택 ID/지역은 서비스 로그와 `QuicRelayClient::selected_relay()`에서 확인할 수 있다. 시스템 DNS resolver의 지연은 OS 설정의 영향을 받는다.
