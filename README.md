<img src="assets/branding/lanlink.png" width="80" alt="LanLink">

# LanLink

Windows·Linux·macOS 기기를 가상 IPv4 네트워크로 연결하는 프로그램이다. 게임이나 일반 애플리케이션에서 상대 기기의 LanLink IP로 접속한다. 예를 들어 게임 서버의 가상 IP가 `10.77.0.2`라면 `10.77.0.2:8211`처럼 사용한다.

Windows 서비스·Linux systemd·macOS launchd가 연결과 가상 어댑터를 유지하고, 데스크톱 UI에서 네트워크 생성·가입·기기 승인·가상 IP 복사를 처리한다. QUIC UDP를 사용하며 연결할 수 없으면 TLS TCP로 전환한다. 가상 네트워크 패킷은 클라이언트 사이에서 암호화하고 릴레이는 암호문을 전달한다.

## 설치

현재 `main`은 [설치 구조 개편 Phase 1](docs/installation-redesign-phase1.md)을 적용한다. Windows Setup은 서버 주소·포트·토큰 입력 없이 앱, 서비스, Wintun과 런타임을 설치한다. 등록된 서버가 없어도 서비스와 GUI가 실행되며, 같은 Setup에서 수정·복구·제거를 선택할 수 있다. Linux/macOS도 서버 정보 없이 설치하고 시작한다.

[공개 v1.0.0 프리릴리스](https://github.com/Koreapanda4444/LanLink/releases/tag/v1.0.0)는 개편 전 패키지다. 이 변경으로 기존 Release 첨부 파일을 교체하지 않는다. 새 동작을 확인하려면 최신 `main`의 CI 패키지를 사용한다. **앱 내 서버 등록·프로필 전환은 다음 Phase 2 범위이며 아직 구현하지 않았다.**

릴레이는 특정 클라우드에 종속되지 않는다. Oracle Cloud, 다른 VPS, 자체 서버에서 운영할 수 있다. 배포 파일은 Linux x64용이며 glibc 2.34 이상과 systemd를 기준으로 한다. 같은 패키지를 Oracle Linux 9, Ubuntu 22.04, Debian 12에서 검증한다. 서버 설치와 방화벽 설정은 [설치 안내](release/INSTALL.md)를 참고한다.

설치를 시작할 때 한국어 또는 영어를 선택할 수 있다. 설치 언어와 별도로 데스크톱 UI에서도 한국어·영어를 전환할 수 있으며 첫 실행 안내를 제공한다.

## 구성

- `lanlink-relay`: 서버의 패킷 중계와 네트워크 관리
- `lanlink-service`: Windows Wintun·Linux TUN·macOS utun과 각 OS의 상주 서비스
- `lanlink-ui`: 네트워크·기기·접속 상태를 관리하는 데스크톱 화면
- `lanlink-ui-cli`: 명령줄 관리 도구

## 소스 빌드

CMake 3.24 이상, C++20 컴파일러, 저장소 `vcpkg.json`의 baseline과 일치하는 vcpkg가 필요하다.

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

Windows에서는 DirectX 11/Win32 UI와 서비스를 함께 빌드한다. Linux/macOS 데스크톱은 `-DLANLINK_BUILD_DESKTOP=ON`으로 SDL3/OpenGL UI와 native client를 빌드한다. 설치와 지원 환경은 [Linux·macOS 클라이언트 안내](docs/clients-posix.md)를 참고한다. Linux 릴레이 패키지는 `VCPKG_ROOT`를 지정한 뒤 `bash deploy/linux/build-relay.sh`로 만든다. 직접 빌드하는 서버의 라이브러리·glibc 요구 사항은 해당 빌드 환경을 따른다.

## v1.0 개발

v1.0.0 프리릴리스 이후 설치·서버 연결·인증 구조를 단계적으로 개편하고 있다. 기존 Commit 01~60의 네트워크 기능과 새 Phase 1~4 작업은 구분한다. Windows·Linux·macOS native client와 한국어·영어 UI, Direct QUIC/Relay, LAN 검색·게임 진단, 선택 계정·서버 목록·비밀번호·초대, Mesh·Hub·ACL·Windows Gateway와 자체 서버 운영 도구를 제공한다. 실제 공유기·혼합 OS 실시간 통신·물리 host/guest 게임 검증은 남아 있으며 공개 안정판은 v0.2.2다. [로드맵](docs/v1-development.md), [v1.0 검증 범위](docs/verification-v1.0.md), [호환성 표](docs/compatibility-matrix.md), [보안 검토](docs/security-review.md), [성능 측정](docs/performance.md), [자체 서버 운영](docs/self-hosting.md), [Linux·macOS](docs/clients-posix.md), [고급 네트워크 관리](docs/managed-networks.md)를 참고한다.
