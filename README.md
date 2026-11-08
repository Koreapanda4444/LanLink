<img src="assets/branding/lanlink.png" width="80" alt="LanLink">

# LanLink

Windows·Linux·macOS 기기를 가상 IPv4 네트워크로 연결하는 프로그램이다. 게임이나 일반 애플리케이션에서 상대 기기의 LanLink IP로 접속한다. 예를 들어 게임 서버의 가상 IP가 `10.77.0.2`라면 `10.77.0.2:8211`처럼 사용한다.

Windows 서비스·Linux systemd·macOS launchd가 연결과 가상 어댑터를 유지하고, 데스크톱 UI에서 네트워크 생성·가입·기기 승인·가상 IP 복사를 처리한다. QUIC UDP를 사용하며 연결할 수 없으면 TLS TCP로 전환한다. 가상 네트워크 패킷은 클라이언트 사이에서 암호화하고 릴레이는 암호문을 전달한다.

## 설치

최신 `main`은 설치·서버 연결·개별 기기 인증 구조를 개편했다. Windows Setup은 서버 주소·포트·토큰 입력 없이 설치하며 동일 EXE에서 Modify·Repair·Uninstall을 제공한다. Linux/macOS도 서버 정보 없이 설치한다. 등록된 서버가 없어도 앱과 서비스가 정상 실행된다.

앱의 **서버 추가 → 운영자의 일회용 코드로 기기 등록 → 연결 → 네트워크 생성·가입** 순서로 사용한다. 서버별 identity·인증 정보·신뢰 지문을 분리하여 여러 프로필을 저장하고 전환할 수 있다. `lanlink://` 초대는 서버·지문·네트워크를 보여 주고 명시적 승인 후 연결한다. 공식 서버나 공용 Directory는 필요 없다. 운영자 master token은 클라이언트에 전달하지 않는다. [서버 등록·초대 안내](docs/server-onboarding.md)를 참고한다.

[최신 설치 패키지: v1.0.0-preview.4](https://github.com/Koreapanda4444/LanLink/releases/tag/v1.0.0-preview.4)에서 개편된 설치·서버 연결 기능을 받는다. Windows 사용자는 `LanLink-Setup-1.0.0-windows-x64.exe`를 선택한다. 기존 `v1.0.0`은 개편 전 패키지로 보존한다.

릴레이는 특정 클라우드에 종속되지 않는다. Oracle Cloud, 다른 VPS, 자체 서버에서 운영할 수 있다. 배포 파일은 Linux x64용이며 glibc 2.34 이상과 systemd를 기준으로 한다. 같은 패키지를 Oracle Linux 9, Ubuntu 22.04, Debian 12에서 검증한다. 서버 설치와 방화벽 설정은 [설치 안내](release/INSTALL.md)를 참고한다.

설치를 시작할 때 한국어 또는 영어를 선택할 수 있다. 설치 언어와 별도로 데스크톱 UI에서도 한국어·영어를 전환할 수 있다. 앱을 열면 연결 상태와 네트워크 목록을 바로 표시한다.

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
```

Windows에서는 DirectX 11/Win32 UI와 서비스를 함께 빌드한다. Linux/macOS 데스크톱은 `-DLANLINK_BUILD_DESKTOP=ON`으로 SDL3/OpenGL UI와 native client를 빌드한다. 설치와 지원 환경은 [Linux·macOS 클라이언트 안내](docs/clients-posix.md)를 참고한다. Linux 릴레이 패키지는 `VCPKG_ROOT`를 지정한 뒤 `bash deploy/linux/build-relay.sh`로 만든다. 직접 빌드하는 서버의 라이브러리·glibc 요구 사항은 해당 빌드 환경을 따른다.

## 빌드·배포 확인

빌드는 제품 실행 파일과 설치 패키지를 생성한다. 저장소의 자동 테스트·벤치마크·과거 검증 보고서는 유지관리 방침에 따라 제거했다. CI는 플랫폼별 컴파일, 패키징, 의존성·패키지 무결성 및 체크섬 생성을 수행한다.
