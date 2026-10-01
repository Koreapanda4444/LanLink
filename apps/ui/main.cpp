#include "lanlink/core/component.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <windows.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

constexpr wchar_t window_class[] = L"LanLinkDesktopWindow";

void require(const HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string{operation} + " failed (HRESULT " +
                                 std::to_string(static_cast<unsigned long>(result)) + ")");
    }
}

class DesktopWindow {
public:
    explicit DesktopWindow(const HINSTANCE instance) : instance_(instance) {}

    ~DesktopWindow() {
        if (dx11_ready_) ImGui_ImplDX11_Shutdown();
        if (win32_ready_) ImGui_ImplWin32_Shutdown();
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
        target_.Reset();
        context_.Reset();
        device_.Reset();
        swap_chain_.Reset();
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        if (registered_) UnregisterClassW(window_class, instance_);
    }

    DesktopWindow(const DesktopWindow&) = delete;
    DesktopWindow& operator=(const DesktopWindow&) = delete;

    void run() {
        WNDCLASSEXW klass{};
        klass.cbSize = sizeof(klass);
        klass.lpfnWndProc = window_proc;
        klass.hInstance = instance_;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.lpszClassName = window_class;
        if (!RegisterClassExW(&klass)) {
            throw std::runtime_error("RegisterClassExW failed");
        }
        registered_ = true;
        window_ = CreateWindowExW(0, window_class, L"LanLink",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
            nullptr, nullptr, instance_, this);
        if (!window_) throw std::runtime_error("CreateWindowExW failed");

        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferCount = 2;
        description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.OutputWindow = window_;
        description.SampleDesc.Count = 1;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        auto create_device = [&](const D3D_DRIVER_TYPE driver) {
            return D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, 0,
                nullptr, 0, D3D11_SDK_VERSION, &description,
                swap_chain_.GetAddressOf(), device_.GetAddressOf(), nullptr,
                context_.GetAddressOf());
        };
        auto result = create_device(D3D_DRIVER_TYPE_HARDWARE);
        if (FAILED(result)) {
            swap_chain_.Reset();
            device_.Reset();
            context_.Reset();
            result = create_device(D3D_DRIVER_TYPE_WARP);
        }
        require(result, "D3D11CreateDeviceAndSwapChain");
        create_target();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        if (!ImGui_ImplWin32_Init(window_)) {
            throw std::runtime_error("ImGui Win32 initialization failed");
        }
        win32_ready_ = true;
        if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) {
            throw std::runtime_error("ImGui DirectX 11 initialization failed");
        }
        dx11_ready_ = true;
        ShowWindow(window_, SW_SHOWDEFAULT);
        UpdateWindow(window_);

        MSG message{};
        for (;;) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) return;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (IsIconic(window_)) {
                WaitMessage();
                continue;
            }
            if (resized_) {
                resized_ = false;
                context_->OMSetRenderTargets(0, nullptr, nullptr);
                target_.Reset();
                require(swap_chain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0),
                        "ResizeBuffers");
                create_target();
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            render();
            ImGui::Render();

            constexpr float background[]{0.08f, 0.10f, 0.14f, 1.0f};
            auto* target = target_.Get();
            context_->OMSetRenderTargets(1, &target, nullptr);
            context_->ClearRenderTargetView(target, background);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            const auto presented = swap_chain_->Present(1, 0);
            if (presented != DXGI_STATUS_OCCLUDED) require(presented, "Present");
        }
    }

private:
    static LRESULT CALLBACK window_proc(const HWND window, const UINT message,
                                         const WPARAM wparam, const LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<DesktopWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (ImGui::GetCurrentContext() &&
            ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) {
            return 1;
        }
        if (message == WM_SIZE && self && wparam != SIZE_MINIMIZED) {
            self->resized_ = true;
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void create_target() {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
        require(swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf())),
                "GetBuffer");
        require(device_->CreateRenderTargetView(back_buffer.Get(), nullptr,
                target_.GetAddressOf()), "CreateRenderTargetView");
    }

    static void render() {
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
        ImGui::Begin("LanLink", nullptr, flags);
        ImGui::Text("LanLink %s", lanlink::core::project_version().data());
        ImGui::Separator();
        ImGui::TextUnformatted("Desktop client");
        ImGui::End();
    }

    HINSTANCE instance_;
    HWND window_ = nullptr;
    bool registered_ = false;
    bool resized_ = false;
    bool win32_ready_ = false;
    bool dx11_ready_ = false;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
};

}

int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        DesktopWindow window(instance);
        window.run();
        return 0;
    } catch (const std::exception& error) {
        OutputDebugStringA(error.what());
        MessageBoxA(nullptr, error.what(), "LanLink", MB_OK | MB_ICONERROR);
        return 1;
    }
}
