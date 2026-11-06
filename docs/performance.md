# 패킷 인증 헤더 성능 측정

암호화 시 1회, 복호화 시 2회 수행하던 전체 패킷 직렬화를 고정 크기 46바이트 헤더 생성으로 대체했다. payload·tag 복사와 해당 임시 vector 할당을 제거하며, 필드 검증·AAD 바이트·AES-GCM·replay window와 wire protocol 5는 유지한다. 실제 전송에 필요한 전체 wire 직렬화는 유지한다.

`LANLINK_BUILD_BENCHMARKS=ON`으로 빌드한 `lanlink_packet_benchmark`는 전체 wire 생성 후 헤더만 남기는 기존 호출 방식과 새 헤더 전용 호출을 같은 프로세스에서 비교한다. 크기별 10,000회, 5개 표본의 중앙값을 기록하며 표본 순서를 교차한다. 절대 속도를 CI 통과 기준으로 사용하지 않는다. wire 헤더의 byte 동일성과 고정 필드를 확인하며 Linux CI의 `LanLink-linux-x64` artifact에 CSV를 보관한다.

로컬 Linux x86_64, AMD EPYC 9V74 가상 CPU, GCC 13.3.0, `-O2`, 외부 네트워크 없음:

| payload | 전체 복사 기준 (ns/회) | 헤더 전용 (ns/회) | encrypt+decrypt 1쌍에서 제거한 payload/tag 복사 |
|---:|---:|---:|---:|
| 24 B | 36.31 | 8.67 | 120 B |
| 1,400 B | 77.59 | 8.67 | 4,248 B |
| 65,535 B | 1,603.36 | 14.74 | 196,653 B |

이는 직렬화 호출만의 비교다. 종단 간 처리량·인터넷 RTT·loss·CPU/RAM 개선률로 해석하지 않는다. 실제 Direct QUIC·Relay QUIC·TLS 왕복과 재접속 회귀 및 세 OS의 native 암호화 상호운용 시험은 별도로 수행한다. 공인 네트워크에서의 비교 성능 수치는 아직 없다.
