#include "windows_service.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace lanlink::service {
namespace {

constexpr wchar_t service_name[] = L"LanLink";

class ServiceHandle {
public:
    explicit ServiceHandle(SC_HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~ServiceHandle() {
        if (handle_) {
            CloseServiceHandle(handle_);
        }
    }
    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;
    ServiceHandle(ServiceHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    ServiceHandle& operator=(ServiceHandle&&) = delete;
    [[nodiscard]] SC_HANDLE get() const noexcept { return handle_; }
private:
    SC_HANDLE handle_;
};

[[noreturn]] void fail(const char* action, const DWORD code) {
    throw std::runtime_error(std::string{action} + " failed (Windows error " +
                             std::to_string(code) + ")");
}

std::filesystem::path executable_path() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        fail("GetModuleFileNameW", GetLastError());
    }
    buffer.resize(length);
    return buffer;
}

std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result += character;
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            result += character;
            backslashes = 0;
        }
    }
    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}

ServiceHandle manager(const DWORD rights) {
    ServiceHandle handle(OpenSCManagerW(nullptr, nullptr, rights));
    if (!handle.get()) {
        fail("OpenSCManagerW", GetLastError());
    }
    return handle;
}

ServiceHandle opened_service(const SC_HANDLE scm, const DWORD rights) {
    ServiceHandle handle(OpenServiceW(scm, service_name, rights));
    if (!handle.get()) {
        fail("OpenServiceW", GetLastError());
    }
    return handle;
}

SERVICE_STATUS_PROCESS query_status(const SC_HANDLE handle) {
    SERVICE_STATUS_PROCESS status{};
    DWORD size = 0;
    if (!QueryServiceStatusEx(handle, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<LPBYTE>(&status), sizeof(status), &size)) {
        fail("QueryServiceStatusEx", GetLastError());
    }
    return status;
}

void wait_for(const SC_HANDLE handle, const DWORD desired) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{30};
    for (;;) {
        const auto status = query_status(handle);
        if (status.dwCurrentState == desired) {
            return;
        }
        if (desired == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) {
            fail("LanLink service start", status.dwWin32ExitCode == NO_ERROR
                ? ERROR_SERVICE_SPECIFIC_ERROR : status.dwWin32ExitCode);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("timed out waiting for LanLink service state");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
    }
}

void stop_service(const SC_HANDLE handle) {
    auto state = query_status(handle).dwCurrentState;
    if (state == SERVICE_STOPPED) {
        return;
    }
    if (state == SERVICE_START_PENDING) {
        wait_for(handle, SERVICE_RUNNING);
        state = SERVICE_RUNNING;
    }
    if (state == SERVICE_STOP_PENDING) {
        wait_for(handle, SERVICE_STOPPED);
        return;
    }
    SERVICE_STATUS status{};
    if (!ControlService(handle, SERVICE_CONTROL_STOP, &status) &&
        GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        fail("ControlService", GetLastError());
    }
    wait_for(handle, SERVICE_STOPPED);
}

struct ServiceContext {
    std::optional<std::filesystem::path> config_path;
    std::filesystem::path working_directory;
    Runtime runtime;
};

ServiceContext* active_context = nullptr;
SERVICE_STATUS_HANDLE status_handle = nullptr;
std::atomic_bool requested_stop = false;
std::atomic<DWORD> checkpoint = 0;

void report_status(const DWORD state, const DWORD error = NO_ERROR) noexcept {
    if (!status_handle) {
        return;
    }
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWin32ExitCode = error;
    status.dwServiceSpecificExitCode = error == ERROR_SERVICE_SPECIFIC_ERROR ? 1 : 0;
    status.dwCheckPoint = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING
        ? ++checkpoint : 0;
    status.dwWaitHint = status.dwCheckPoint != 0 ? 20000 : 0;
    static_cast<void>(SetServiceStatus(status_handle, &status));
}

DWORD WINAPI control_handler(const DWORD control, DWORD, LPVOID, LPVOID) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        requested_stop.store(true);
        report_status(SERVICE_STOP_PENDING);
        return NO_ERROR;
    }
    if (control == SERVICE_CONTROL_INTERROGATE) {
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI service_main(DWORD, LPWSTR*) {
    status_handle = RegisterServiceCtrlHandlerExW(service_name, control_handler, nullptr);
    if (!status_handle) {
        return;
    }
    checkpoint.store(0);
    requested_stop.store(false);
    report_status(SERVICE_START_PENDING);
    try {
        if (!active_context ||
            !SetCurrentDirectoryW(active_context->working_directory.c_str())) {
            fail("SetCurrentDirectoryW", GetLastError());
        }
        active_context->runtime(active_context->config_path,
                                [] { report_status(SERVICE_RUNNING); });
        report_status(SERVICE_STOPPED);
    } catch (const std::exception& error) {
        OutputDebugStringA(error.what());
        report_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    } catch (...) {
        report_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    }
    status_handle = nullptr;
}

}

void install(std::optional<std::filesystem::path> config_path) {
    if (config_path) {
        config_path = std::filesystem::absolute(*config_path);
    }
    const auto command = quote(executable_path().wstring()) + L" --service " +
                         quote(config_path ? config_path->wstring() : L"-") + L" " +
                         quote(std::filesystem::current_path().wstring());
    const auto scm = manager(SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    ServiceHandle created(CreateServiceW(scm.get(), service_name, L"LanLink",
        SERVICE_CHANGE_CONFIG, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
        nullptr, nullptr));
    if (!created.get()) {
        if (GetLastError() != ERROR_SERVICE_EXISTS) {
            fail("CreateServiceW", GetLastError());
        }
        const auto existing = opened_service(scm.get(), SERVICE_CHANGE_CONFIG);
        if (!ChangeServiceConfigW(existing.get(), SERVICE_WIN32_OWN_PROCESS,
                                  SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                  command.c_str(), nullptr, nullptr, nullptr,
                                  nullptr, nullptr, L"LanLink")) {
            fail("ChangeServiceConfigW", GetLastError());
        }
    }
}

void uninstall() {
    const auto scm = manager(SC_MANAGER_CONNECT);
    const auto handle = opened_service(scm.get(), SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    stop_service(handle.get());
    if (!DeleteService(handle.get())) {
        fail("DeleteService", GetLastError());
    }
}

void start() {
    const auto scm = manager(SC_MANAGER_CONNECT);
    const auto handle = opened_service(scm.get(), SERVICE_START | SERVICE_QUERY_STATUS);
    if (!StartServiceW(handle.get(), 0, nullptr) &&
        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        fail("StartServiceW", GetLastError());
    }
    wait_for(handle.get(), SERVICE_RUNNING);
}

void stop() {
    const auto scm = manager(SC_MANAGER_CONNECT);
    const auto handle = opened_service(scm.get(), SERVICE_STOP | SERVICE_QUERY_STATUS);
    stop_service(handle.get());
}

void dispatch(std::optional<std::filesystem::path> config_path,
              const std::filesystem::path& working_directory,
              Runtime runtime) {
    ServiceContext context{std::move(config_path), working_directory, std::move(runtime)};
    if (!context.runtime || context.working_directory.empty()) {
        throw std::invalid_argument("invalid LanLink service context");
    }
    active_context = &context;
    wchar_t name[] = L"LanLink";
    SERVICE_TABLE_ENTRYW entries[]{{name, service_main}, {nullptr, nullptr}};
    const auto success = StartServiceCtrlDispatcherW(entries);
    active_context = nullptr;
    if (!success) {
        fail("StartServiceCtrlDispatcherW", GetLastError());
    }
}

bool stop_requested() noexcept {
    return requested_stop.load();
}

}
