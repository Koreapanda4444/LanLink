# Direct P2P 프로토콜

IPv4 UDP 후보는 인증된 Relay 제어 채널로만 전달한다. 같은 네트워크의 승인된 온라인 멤버, 현재 키 epoch, 양쪽 서명된 세션 공개키가 일치해야 한다. 후보는 최대 8개이며 generation은 세션마다 증가한다. 구버전은 기능 협상 결과에 따라 기존 Relay 경로를 사용한다.

UDP 탐색은 양쪽 장치 서명으로 네트워크·epoch·장치·세션 공개키·후보 generation·난수를 인증한다. 5초/최대 32회로 제한한다. STUN은 설정한 숫자 IPv4 엔드포인트에만 요청하며, 응답 출처·transaction·길이·XOR-MAPPED-ADDRESS를 검사한다. 기본 외부 STUN 서비스는 없다. IPv6는 후속 스키마 확장점이며 현재 후보는 IPv4만 지원한다.

UDP 탐색 소켓을 닫은 뒤 같은 로컬 포트를 QUIC에 넘긴다. 장치 ID 비교로 한쪽만 QUIC 클라이언트, 다른 쪽은 리스너가 된다. MsQuic에서 클라이언트와 리스너를 같은 UDP 포트에 함께 바인딩하는 방식은 사용하지 않는다. 근거: [MsQuic maintainer 설명](https://github.com/microsoft/msquic/discussions/1606).

Direct ALPN은 `lanlink-peer/1`이다. 임시 TLS 인증서는 메모리 PKCS#12로 생성한다. 별도의 장치 인증에서 신뢰된 Relay 멤버 목록의 Ed25519 키로 양쪽 새로운 난수와 위 세션 정보를 서명·검증한 뒤에만 데이터를 받는다. 애플리케이션 데이터는 기존 네트워크 키 기반 E2E 암호화와 재생 방어를 그대로 사용한다. 작은 암호문은 QUIC DATAGRAM, 큰 암호문은 제한된 reliable stream으로 보낸다. 전송 큐와 연결 수는 제한한다.

`lanlink-direct-quic` 테스트는 실제 UDP 탐색 후 같은 포트를 넘겨 Relay 프로세스 없이 두 QUIC 엔드포인트의 암호화된 IPv4·역방향·큰 패킷·재생 거부를 확인한다. `lanlink-punch`의 NAT 매트릭스는 모의 테스트이며 실제 서로 다른 공유기 실험을 대신하지 않는다.

## 서비스 설정 및 폴백

서비스는 승인된 피어마다 후보 교환 → 서명된 UDP 검사 → Direct QUIC를 시도한다. 기본 `p2p_enabled=true`이며 `false`로 기존 Relay 전용 동작을 선택할 수 있다. 자체 STUN을 사용하려면 `p2p_stun_host=숫자_IPv4`와 `p2p_stun_port=3478`을 설정한다. 외부 STUN에 자동 접속하지 않는다. 원본 가상 네트워크 주소는 후보에서 제외한다.

Direct 인증 전·후보 실패·전송 backpressure·연결 종료 시 유니캐스트는 기존 Relay QUIC 또는 TLS/TCP로 보낸다. broadcast/multicast는 기존 Relay fanout을 유지한다. 경로가 바뀌어도 동일한 패킷 암호·sequence/replay 창을 사용한다. 멤버 자격·epoch·양쪽 세션 공개키는 Direct의 매 송수신에서 현재 Relay 스냅샷과 대조한다.

`lanlink-network-client`는 실제 Relay와 두 서비스 경로 선택기를 띄워 후보 교환·Direct 무중계 전송·Relay 복귀·경로 간 중복 거부·UDP Relay 차단 시 TLS/TCP 전송·가상 IPv4 유지를 검증한다. NIC 주소 주입으로 환경을 고정한 네이티브 UDP/QUIC 테스트이며, 실물 공유기 시험으로 표시하지 않는다.
