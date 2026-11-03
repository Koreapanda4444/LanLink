# v0.9.0 검증 범위 (50~55)

v0.9.0은 지역별 탐색·상태 확인·선택·복구·제한과 자체 서버 배포 도구까지의 마일스톤이다. 56~60은 미착수이며 공개 Release는 발행하지 않는다. 이전 OS·설치·상호운용 검증과 제한은 [v0.8 기록](verification-v0.8.md)을 함께 참고한다.

| 번호 | 구현 | 자동 검증 항목 |
|---|---|---|
| 50 | authority·지역·용량·프로토콜·노드 상태 키를 포함하는 서명된 LLD2 목록 | scope·revision·만료·서명·크기·endpoint 검증과 기존 LLD1 호환 |
| 51 | 검증된 TLS와 노드 키·nonce로 확인하는 상태/부하 조회 | 다른 키·응답 재사용·잘못된 ID/authority/용량 거부, 조회만으로 멤버 등록하지 않음 |
| 52 | 실측 RTT·부하에 따른 선택, 유지 시간·개선 임계값·실패 cooldown·수동 고정 | 선택 상태 기계와 실제 QUIC/TLS 연결·fallback 회귀 |
| 53 | 하나의 영구 중앙 authority를 공유하는 지역 QUIC/TLS 앞단 | 실제 중앙·지역 연결, 지역 장애·중앙 재시작 뒤 재인증과 기존 네트워크·IP·키 복원 |
| 54 | 중앙 전체/네트워크/장치의 바이트·전달·요청 제한과 aggregate 계수 | 병렬 전역 한도·fanout 비용·재접속 후 제어 제한·고정 메모리 상한·LLH1 호환 및 선택 LLH2 집계 |
| 55 | inventory 설정·서명 catalog 생성, 노드 키 대조 설치, DB snapshot과 차단된 복원 | 동일 inventory 설정 재현·변조 거부·세 패키지 프로세스·장애 복구·live WAL 백업/복원·기존 상태 보존 |

54번까지의 기준은 [소스 ed3c6b7](https://github.com/Koreapanda4444/LanLink/commit/ed3c6b767f778b3dd44c536a247a98006c7aedda)의 [전체 CI 실행](https://github.com/Koreapanda4444/LanLink/actions/runs/41631093428)이다. 검증 job 8개가 통과했고 공개 발행 job은 건너뛰었다.

## 55번 로컬 실행 증거

`tests/relay_package_tests.py`로 실제 v0.9.0 Linux Relay archive를 검사했다. 패키지 안의 Relay·Directory signer·Python 도구를 그대로 실행하며 대체 구현이나 가짜 DB를 사용하지 않는다.

- 세 노드의 private health identity를 각각 만들고 실제 공개 키를 inventory에 기록했다. 생성된 노드 설정·catalog·manifest hash와 재생성한 설정의 동일성을 확인했다.
- 기존 출력 덮어쓰기, 잘못된 inventory, catalog 변조, 실제 identity와 다른 공개 키, 중앙/지역 역할 변경과 지역에 대한 중앙 토큰 전달을 거부했다.
- 중앙 한 개와 지역 두 개를 실제 패키지 바이너리로 실행했다. 서명된 readiness, 중앙 중단 후 지역의 unready 전환, 중앙·지역 재시작 후 ready 복원을 확인했다.
- 실행 중인 중앙의 DB에 열린 WAL writer를 유지한 채 snapshot을 만들었다. 백업 뒤 원본을 변경했으며 복원 DB에는 백업 당시 값이 남았다. 지역 백업에는 중앙 DB·토큰이 없었다.
- 백업 폴더 0700·파일 0600, checksum과 SQLite integrity 검사를 확인했다. `--fenced` 누락, 기존 대상 디렉터리와 손상된 백업을 거부했고 거부 시 대상 상태가 보존됐다.
- 복원한 중앙 위에 패키지를 설치하고, 패키지의 MsQuic RPATH로 실행·정상 종료했다. 기존 설치·업그레이드·실패 롤백·재시작·제거 검사를 유지했다.

로컬 CTest는 45개 중 43개가 통과했다. 나머지 `lanlink-posix-ipc`, `lanlink-ui-network-model`은 작업 환경의 Unix 소켓 생성 금지(`Operation not permitted`)로 실행되지 않았다. 테스트를 생략하거나 완화하지 않는다. Release validator의 별도 Python 시험 16개도 통과했다.

## CI와 운영 범위

최종 CI 판정은 이 문서를 포함한 main 커밋의 [build 실행](https://github.com/Koreapanda4444/LanLink/actions/workflows/build.yml?query=branch%3Amain)에서 확인한다. Windows·Linux·macOS native 회귀와 설치·커널 장치·암호화 상호운용 시험을 유지한다. Relay는 Oracle Linux 9에서 빌드하고 동일 archive를 Ubuntu 22.04·Debian 12에서도 실행한다. 새 세 노드 설치·백업·복원 시험도 이 세 배포 환경에서 수행한다. 다섯 배포 패키지의 버전·출처 SHA·필수 파일·실행 파일 형식·라이선스·체크섬 검사는 `verify-release`가 담당한다.

로컬 archive는 기능 검사용이다. Oracle Linux 9/glibc 2.34 배포 호환성은 해당 CI가 생성한 archive로 검사한다. 실제 공인 다중 리전 인프라·물리 NAT·서로 다른 물리 OS 사이 실시간 통신·Developer ID/Apple 공증은 검증 범위에 포함하지 않는다.

중앙은 단일 영구 authority다. active-active DB 합의나 중앙의 무중단 failover는 제공하지 않는다. 지역 상행은 검증된 TLS/TCP이며 중앙에 의존한다. 백업은 credential을 포함하는 개인 자료이고, 복원 전에 모든 writer를 차단해야 한다. 설치·catalog 갱신·순차 업그레이드·롤백은 [자체 서버 운영 절차](self-hosting.md)를 따른다.
