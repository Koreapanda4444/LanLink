#include "lanlink/core/component.hpp"
#include "lanlink/protocol/network_messages.hpp"
#include "lanlink/protocol/game_profiles.hpp"
#include "desktop_view.hpp"
#include "management_panel.hpp"
#include "preferences.hpp"
#include "lanlink/protocol/network_workflow.hpp"
#include "lanlink/protocol/network_overview.hpp"
#include "lanlink_resources.h"

#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <windows.h>
#include <wrl/client.h>
#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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
    explicit DesktopWindow(const HINSTANCE instance, std::filesystem::path screenshot = {},
        std::optional<lanlink::core::UiLanguage> language = std::nullopt,bool force_management=false)
        : instance_(instance), screenshot_(std::move(screenshot)),
          view_(language,force_management,"Start Windows service",[this] {
              wchar_t executable[32768]{};
              if(!GetModuleFileNameW(nullptr,executable,32768))throw std::runtime_error("Cannot locate LanLink.");
              const auto path=std::filesystem::path(executable).parent_path()/L"lanlink-service.exe";
              if(reinterpret_cast<INT_PTR>(ShellExecuteW(window_,L"runas",path.c_str(),L"--start",nullptr,SW_HIDE))<=32)
                  throw std::runtime_error("Service start was cancelled or could not be requested.");
          }) {

    }

    ~DesktopWindow() {
        if (dx11_ready_) ImGui_ImplDX11_Shutdown();
        if (win32_ready_) ImGui_ImplWin32_Shutdown();
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
        target_.Reset();
        logo_.Reset();
        context_.Reset();
        device_.Reset();
        swap_chain_.Reset();
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        if (registered_) UnregisterClassW(window_class, instance_);
        if (desktop_mutex_) CloseHandle(desktop_mutex_);
    }

    DesktopWindow(const DesktopWindow&) = delete;
    DesktopWindow& operator=(const DesktopWindow&) = delete;

    void open_invitation(std::string uri){view_.open_invitation(std::move(uri));}
    void run() {
        desktop_mutex_ = CreateMutexW(nullptr, FALSE, L"LanLink.Desktop");
        if (!desktop_mutex_) throw std::runtime_error("Cannot create desktop lifetime marker.");
        WNDCLASSEXW klass{};
        klass.cbSize = sizeof(klass);
        klass.lpfnWndProc = window_proc;
        klass.hInstance = instance_;
        klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        klass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_LANLINK));
        klass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_LANLINK),
            IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        if (!klass.hIcon || !klass.hIconSm) throw std::runtime_error("Cannot load the LanLink application icon");
        klass.lpszClassName = window_class;
        if (!RegisterClassExW(&klass)) {
            throw std::runtime_error("RegisterClassExW failed");
        }
        registered_ = true;
        window_ = CreateWindowExW(0, window_class, L"LanLink",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, lanlink::ui::design::default_width, lanlink::ui::design::default_height,
            nullptr, nullptr, instance_, this);
        if (!window_) throw std::runtime_error("CreateWindowExW failed");
        pending_scale_=ImGui_ImplWin32_GetDpiScaleForHwnd(window_);
        SetWindowPos(window_,nullptr,0,0,
            static_cast<int>(lanlink::ui::design::default_width*pending_scale_),
            static_cast<int>(lanlink::ui::design::default_height*pending_scale_),SWP_NOMOVE|SWP_NOZORDER);
        if(auto library=LoadLibraryExW(L"dwmapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            using SetAttribute=HRESULT(WINAPI*)(HWND,DWORD,LPCVOID,DWORD);
            if(auto attribute=reinterpret_cast<SetAttribute>(GetProcAddress(library,"DwmSetWindowAttribute"))) {
                const BOOL dark=TRUE;
                if(FAILED(attribute(window_,20,&dark,sizeof(dark))))attribute(window_,19,&dark,sizeof(dark));
            }
            FreeLibrary(library);
        }

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
        create_logo();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        apply_scale();
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

        const auto opened = std::chrono::steady_clock::now();
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

            if(std::abs(pending_scale_-scale_)>.001f)apply_scale();
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            view_.render();
            ImGui::Render();

            constexpr float background[]{0.08f, 0.10f, 0.14f, 1.0f};
            auto* target = target_.Get();
            context_->OMSetRenderTargets(1, &target, nullptr);
            context_->ClearRenderTargetView(target, background);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (!screenshot_.empty() && std::chrono::steady_clock::now() - opened > std::chrono::seconds{4}) {
                capture();
                return;
            }
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
        if(message==WM_COPYDATA && self) {
            const auto* data=reinterpret_cast<const COPYDATASTRUCT*>(lparam);
            if(!data || data->dwData!=0x4c4c || !data->lpData || data->cbData<2 || data->cbData>2049)return FALSE;
            const auto* uri=static_cast<const char*>(data->lpData);
            if(uri[data->cbData-1]!=0 || std::char_traits<char>::length(uri)!=data->cbData-1)return FALSE;
            try{self->open_invitation(std::string(uri,data->cbData-1));ShowWindow(window,SW_RESTORE);SetForegroundWindow(window);return TRUE;}catch(...){return FALSE;}
        }
        if (message == WM_GETMINMAXINFO) {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            const auto dpi=self?self->pending_scale_:1.0f;
            limits->ptMinTrackSize = POINT{static_cast<LONG>(460*dpi),static_cast<LONG>(560*dpi)};
            return 0;
        }
        if(message==WM_DPICHANGED && self) {
            self->pending_scale_=static_cast<float>(HIWORD(wparam))/96.0f;
            const auto* bounds=reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,
                bounds->bottom-bounds->top,SWP_NOZORDER|SWP_NOACTIVATE);
            return 0;
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

    void apply_scale() {
        scale_=pending_scale_;
        if(dx11_ready_)ImGui_ImplDX11_InvalidateDeviceObjects();
        auto& io=ImGui::GetIO();
        io.FontDefault=nullptr;
        io.Fonts->Clear();
        lanlink::ui::design::emphasis_font=nullptr;
        lanlink::ui::design::title_font=nullptr;
        lanlink::ui::configure_desktop_style(scale_);
        wchar_t windows_directory[MAX_PATH]{};
        if(GetWindowsDirectoryW(windows_directory,MAX_PATH)) {
            for(const auto* face:{L"malgun.ttf",L"segoeui.ttf",L"arial.ttf"}) {
                const auto directory=std::filesystem::path(windows_directory)/L"Fonts";
                const auto path=directory/face;
                if(std::filesystem::is_regular_file(path)) {
                    const auto bold=directory/(std::wstring_view(face)==L"malgun.ttf"?L"malgunbd.ttf":L"seguisb.ttf");
                    lanlink::ui::design::load_fonts(path,bold);
                    break;
                }
            }
        }
        if(io.Fonts->Fonts.empty())io.Fonts->AddFontDefault();
    }

    void create_target() {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
        require(swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf())),
                "GetBuffer");
        require(device_->CreateRenderTargetView(back_buffer.Get(), nullptr,
                target_.GetAddressOf()), "CreateRenderTargetView");
    }

    void create_logo() {
        const auto resource = FindResourceW(instance_, MAKEINTRESOURCEW(IDB_LANLINK_LOGO), MAKEINTRESOURCEW(10));
        if (!resource) throw std::runtime_error("Cannot find the LanLink logo");
        const auto loaded = LoadResource(instance_, resource);
        auto* data = static_cast<BYTE*>(LockResource(loaded));
        const auto size = SizeofResource(instance_, resource);
        if (!data || size == 0) throw std::runtime_error("Cannot load the LanLink logo");
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf())), "Logo imaging factory");
        Microsoft::WRL::ComPtr<IWICStream> stream;
        require(factory->CreateStream(stream.GetAddressOf()), "Logo stream");
        require(stream->InitializeFromMemory(data, size), "Logo resource stream");
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        require(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
            decoder.GetAddressOf()), "Logo PNG decoder");
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        require(decoder->GetFrame(0, frame.GetAddressOf()), "Logo frame");
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        require(factory->CreateFormatConverter(converter.GetAddressOf()), "Logo converter");
        require(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "Logo pixel format");
        UINT width = 0, height = 0;
        require(converter->GetSize(&width, &height), "Logo dimensions");
        if (width != 512 || height != 512) throw std::runtime_error("Unexpected logo dimensions");
        std::vector<BYTE> pixels(width * height * 4);
        require(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()),
            "Logo pixels");
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial{pixels.data(), width * 4, 0};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        require(device_->CreateTexture2D(&description, &initial, texture.GetAddressOf()), "Logo texture");
        require(device_->CreateShaderResourceView(texture.Get(), nullptr, logo_.GetAddressOf()), "Logo view");
        view_.set_logo(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(logo_.Get())));
    }

    void capture() {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> image;
        require(swap_chain_->GetBuffer(0, IID_PPV_ARGS(image.GetAddressOf())), "Capture back buffer");
        D3D11_TEXTURE2D_DESC description{};
        image->GetDesc(&description);
        description.BindFlags = 0; description.MiscFlags = 0;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        require(device_->CreateTexture2D(&description, nullptr, staging.GetAddressOf()), "Capture staging texture");
        context_->CopyResource(staging.Get(), image.Get());
        D3D11_MAPPED_SUBRESOURCE pixels{};
        require(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &pixels), "Capture pixels");
        std::vector<BYTE> bgra(static_cast<std::size_t>(description.Width) * description.Height * 4);
        for (UINT y = 0; y < description.Height; ++y) {
            const auto* row = static_cast<const BYTE*>(pixels.pData) + y * pixels.RowPitch;
            for (UINT x = 0; x < description.Width; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * description.Width + x) * 4;
                bgra[offset] = row[x * 4 + 2]; bgra[offset + 1] = row[x * 4 + 1];
                bgra[offset + 2] = row[x * 4]; bgra[offset + 3] = 255;
            }
        }
        context_->Unmap(staging.Get(), 0);
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf())), "Capture image encoder");
        Microsoft::WRL::ComPtr<IWICStream> stream;
        require(factory->CreateStream(stream.GetAddressOf()), "Capture output stream");
        require(stream->InitializeFromFilename(screenshot_.c_str(), GENERIC_WRITE), "Capture output file");
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        require(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()), "Capture PNG encoder");
        require(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Capture PNG initialization");
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        require(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr), "Capture frame");
        require(frame->Initialize(nullptr), "Capture frame initialization");
        require(frame->SetSize(description.Width, description.Height), "Capture dimensions");
        auto format = GUID_WICPixelFormat32bppBGRA;
        require(frame->SetPixelFormat(&format), "Capture pixel format");
        require(frame->WritePixels(description.Height, description.Width * 4, static_cast<UINT>(bgra.size()), bgra.data()), "Capture PNG data");
        require(frame->Commit(), "Capture frame commit"); require(encoder->Commit(), "Capture PNG commit");
    }

    HANDLE desktop_mutex_ = nullptr;
    HINSTANCE instance_;
    HWND window_ = nullptr;
    float scale_=0, pending_scale_=1;
    bool registered_ = false;
    bool resized_ = false;
    bool win32_ready_ = false;
    bool dx11_ready_ = false;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> logo_;
    std::filesystem::path screenshot_;
    lanlink::ui::DesktopView view_;
};

}

int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR, int) {
    std::filesystem::path screenshot;
    ImGui_ImplWin32_EnableDpiAwareness();
    try {
        require(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "COM initialization");
        int count = 0;
        auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        std::optional<lanlink::core::UiLanguage> language;
        bool management=false;std::string invitation;
        const auto invitation_arg=[&](const wchar_t* raw){const std::wstring_view text(raw);if(text.size()>2048)throw std::invalid_argument("Invitation too long");std::string uri;for(auto ch:text){if(ch>127 || ch<33)throw std::invalid_argument("Invalid invitation URI");uri+=static_cast<char>(ch);}return uri;};
        if(count==2 && std::wstring_view(arguments[1]).starts_with(L"lanlink://")){invitation=invitation_arg(arguments[1]);count=1;}
        else if(count>=2 && std::wstring_view(arguments[1])==L"--invite"){if(count!=3)throw std::invalid_argument("--invite requires exactly one invitation URI");invitation=invitation_arg(arguments[2]);count=1;}
        for(int i=1;i<count;++i) {
            const std::wstring_view argument=arguments[i];
            if(argument==L"--screenshot" && i+1<count)screenshot=arguments[++i];
            else if(argument==L"--management")management=true;
            else if(argument==L"--language" && i+1<count) {
                const std::wstring_view selected=arguments[++i];
                if(selected!=L"en" && selected!=L"ko")throw std::invalid_argument("language must be en or ko");
                language=selected==L"ko" ? lanlink::core::UiLanguage::korean : lanlink::core::UiLanguage::english;
            } else throw std::invalid_argument("usage: lanlink-ui [--language en|ko] [--management] [--screenshot FILE.png]");
        }
        LocalFree(arguments);
        DesktopWindow window(instance, screenshot,language,management);
        if(!invitation.empty()) {
            window.open_invitation(invitation);
            if(auto existing=FindWindowW(window_class,nullptr)) {
                COPYDATASTRUCT data{0x4c4c,static_cast<DWORD>(invitation.size()+1),invitation.data()};DWORD_PTR delivered=0;
                if(!SendMessageTimeoutW(existing,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&data),SMTO_ABORTIFHUNG,3000,&delivered) || !delivered)throw std::runtime_error("Cannot deliver invitation to the running application");
                return 0;
            }
        }
        window.run();
        return 0;
    } catch (const std::exception& error) {
        OutputDebugStringA(error.what());
        if (!screenshot.empty()) {
            auto log = screenshot;
            log += L".log";
            std::ofstream(log) << error.what() << '\n';
        } else MessageBoxA(nullptr, error.what(), "LanLink", MB_OK | MB_ICONERROR);
        return 1;
    }
}
