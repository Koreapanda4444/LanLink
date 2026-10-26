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

## 경로 품질

Direct QUIC의 실제 RTT와 전송된 QUIC 패킷/보정된 손실 카운터 차분을 200ms마다 샘플링한다. 가상 IPv4 패킷 수를 QUIC 손실률의 분모로 쓰지 않는다. RTT·손실 이동평균, 연속 3회 샘플, 2초 hold time으로 짧은 변동에 따른 왕복 전환을 제한한다. 연결 종료·전송 실패는 즉시 Relay로 보낸다. Relay RTT는 서버까지의 값이므로 피어 왕복 RTT라고 표시하지 않으며, 지나치게 느린 Direct를 거르는 보수적인 기준에만 사용한다. 선택 사유와 전환은 로컬 진단/고정 문구 로그에 남긴다.

## 재탐색과 세션 수명

인터페이스 주소/상태 변경을 주기적으로 확인하고, 변경 또는 연결 종료 시 Relay로 전환한 채 새 후보 generation으로 다시 검사한다. 재시도는 1초부터 30초까지 backoff하며 무한한 빠른 반복을 하지 않는다. 후보 스키마 2의 `response_to_generation`과 장치 ID 기반 역할 결정으로 동시 재탐색·늦은 응답·세션 재생을 구분한다. 새 `candidate_generations` 기능을 협상하지 못한 서버/피어는 Relay를 유지한다.

재탐색은 가상 주소와 공통 암호 상태를 교체하지 않는다. 네이티브 통합 시험에서 임시 중단 후 Direct 복구, 재검사 중 Relay 전달, NIC 식별 정보 변경 후 Direct 재연결과 sequence 연속성을 확인한다. 실제 물리 NIC 전환/서로 다른 공유기 검증은 별도 환경 시험으로 남긴다.

Windows의 Direct 인증서는 Schannel용 임시 CNG 키 컨테이너에 가져온다. QUIC 자원을 닫은 뒤 해당 키를 제거한다. 기존 Relay의 인증서 저장소 설정은 그대로 사용한다. QUIC 통계 조회는 콜백 상태 mutex 밖에서 수행하고, 별도의 handle 수명 잠금으로 종료 중 조회를 보호한다.
