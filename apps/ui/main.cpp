#include "lanlink/core/component.hpp"
#include "lanlink/protocol/network_messages.hpp"
#include "lanlink/protocol/game_profiles.hpp"
#include "network_model.hpp"
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

unsigned hex_digit(const char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    throw std::invalid_argument("IDs must contain hexadecimal digits");
}

template <std::size_t N>
std::array<std::byte, N> parse_id(const std::string_view text) {
    if (text.size() != N * 2) {
        throw std::invalid_argument("The ID has the wrong length");
    }
    std::array<std::byte, N> id{};
    for (std::size_t i = 0; i < N; ++i) {
        id[i] = static_cast<std::byte>((hex_digit(text[i * 2]) << 4) |
                                        hex_digit(text[i * 2 + 1]));
    }
    return id;
}

template <std::size_t N>
std::string hex_id(const std::array<std::byte, N>& id) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(id.size() * 2);
    for (const auto byte : id) {
        const auto value = std::to_integer<unsigned>(byte);
        text += digits[value >> 4];
        text += digits[value & 15];
    }
    return text;
}

std::string format_bytes(const std::uint64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double size = static_cast<double>(bytes);
    const char* suffix = "KiB";
    for (const auto* unit : {"KiB", "MiB", "GiB", "TiB"}) {
        size /= 1024.0;
        suffix = unit;
        if (size < 1024.0) break;
    }
    char text[64]{};
    std::snprintf(text, sizeof(text), "%.1f %s", size, suffix);
    return text;
}

void require(const HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string{operation} + " failed (HRESULT " +
                                 std::to_string(static_cast<unsigned long>(result)) + ")");
    }
}

class DesktopWindow {
public:
    explicit DesktopWindow(const HINSTANCE instance, std::filesystem::path screenshot = {},
        std::optional<lanlink::core::UiLanguage> language = std::nullopt,bool skip_setup=false,bool force_setup=false)
        : instance_(instance), screenshot_(std::move(screenshot)) {
        preferences_=lanlink::ui::load_preferences();
        if(language)preferences_.language=*language;
        if(skip_setup)preferences_.setup_completed=true;
        if(force_setup)preferences_.setup_completed=false;
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
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 780,
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
        create_logo();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        auto& style = ImGui::GetStyle();
        style.WindowPadding = ImVec2(22, 20);
        style.FramePadding = ImVec2(12, 8);
        style.ItemSpacing = ImVec2(10, 10);
        style.ChildRounding = 8;
        style.FrameRounding = 5;
        style.PopupRounding = 8;
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.07f, 0.10f, 1);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.095f, 0.13f, 1);
        style.Colors[ImGuiCol_Button] = ImVec4(0.12f, 0.29f, 0.48f, 1);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.39f, 0.64f, 1);
        style.Colors[ImGuiCol_Header] = ImVec4(0.12f, 0.24f, 0.36f, 1);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.15f, 0.30f, 0.45f, 1);
        wchar_t windows_directory[MAX_PATH]{};
        if (GetWindowsDirectoryW(windows_directory, MAX_PATH)) {
            for (const auto* face : {L"malgun.ttf", L"segoeui.ttf", L"arial.ttf"}) {
                const auto path = std::filesystem::path(windows_directory) / L"Fonts" / face;
                if (std::filesystem::is_regular_file(path)) {
                    if (auto* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), 18.0f, nullptr, ImGui::GetIO().Fonts->GetGlyphRangesKorean())) {
                        ImGui::GetIO().FontDefault = font;
                        break;
                    }
                }
            }
        }
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
        if (message == WM_GETMINMAXINFO) {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = POINT{960, 680};
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

    void create_target() {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
        require(swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf())),
                "GetBuffer");
        require(device_->CreateRenderTargetView(back_buffer.Get(), nullptr,
                target_.GetAddressOf()), "CreateRenderTargetView");
    }

    template <typename Action>
    void attempt(Action&& action) {
        try {
            std::forward<Action>(action)();
            local_error_.clear();
        } catch (const std::exception& error) {
            local_error_ = error.what();
        }
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
    }

    const char* tr(const char* text) const {
        return lanlink::core::localized_text(preferences_.language,text).data();
    }
    void finish_setup() {
        preferences_.setup_completed=true;
        attempt([&]{lanlink::ui::save_preferences(preferences_);});
    }
    void render() {
        namespace protocol = lanlink::protocol;
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_diagnostics_) {
            attempt([&] {
                networks_.submit(protocol::LocalCommand::diagnostics);
                networks_.submit(protocol::LocalCommand::path_diagnostics);
                networks_.submit(protocol::LocalCommand::game_diagnostics);
                networks_.submit(protocol::LocalCommand::activity);
                if (selected_) networks_.submit(protocol::LocalCommand::peers,
                    protocol::encode_network_selection_request({*selected_}));
                networks_.submit(protocol::LocalCommand::list, protocol::encode_network_list_request());
            });
            next_diagnostics_ = now + std::chrono::seconds{2};
        }
        const auto state = networks_.snapshot();
        if(state.operation_generation!=operation_generation_ && state.last_operation) {
            operation_generation_=state.operation_generation;
            const auto& operation=*state.last_operation;
            if(operation.code==protocol::NetworkResultCode::success || operation.code==protocol::NetworkResultCode::pending_approval) {
                if(operation.operation==protocol::NetworkOperation::create)close_create_=true;
                if(operation.operation==protocol::NetworkOperation::join)close_join_=true;
                if(operation.code==protocol::NetworkResultCode::success)selection_hint_=operation.network_id;
            }
        }
        if(selection_hint_ && std::any_of(state.networks.begin(),state.networks.end(),[&](const auto& n){return n.network_id==*selection_hint_;})) {
            selected_=selection_hint_;selection_hint_.reset();
        }
        if(state.access_generation!=access_generation_ && state.access) {
            access_generation_=state.access_generation;
            if(state.access->action<=protocol::AccessAction::device_revoke) {
                account_=state.access;
                if(!state.access->secret.empty())enrollment_code_=state.access->secret;
            }
            if(state.access->action==protocol::AccessAction::invite_issue && state.access->code==protocol::NetworkResultCode::success) {
                invitation_link_=state.access->secret;invitation_code_=state.access->text;invitation_network_=state.access->network_id;
            }
            if(state.access->action==protocol::AccessAction::invite_revoke && state.access->code==protocol::NetworkResultCode::success){invitation_link_.clear();invitation_code_.clear();}
        }
        if (selected_ && std::none_of(state.networks.begin(), state.networks.end(),
                [&](const auto& network) { return network.network_id == *selected_; })) selected_.reset();
        if (!selected_ && !state.networks.empty()) {
            selected_ = state.networks.front().network_id;
            networks_.submit(protocol::LocalCommand::peers, protocol::encode_network_selection_request({*selected_}));
        }
        if (state.diagnostics &&
            state.diagnostics_generation != last_diagnostics_generation_) {
            const auto& current = *state.diagnostics;
            if (last_diagnostics_generation_ != 0 && last_connected_ &&
                current.connected && last_tcp_ == current.tcp_fallback &&
                last_reconnects_ == current.reconnects &&
                current.sent_bytes >= last_sent_bytes_ &&
                current.received_bytes >= last_received_bytes_) {
                const auto elapsed = std::chrono::duration<double>(
                    now - last_diagnostics_time_).count();
                if (elapsed > 0) {
                    send_rate_ = (current.sent_bytes - last_sent_bytes_) / elapsed;
                    receive_rate_ = (current.received_bytes - last_received_bytes_) / elapsed;
                }
            } else {
                send_rate_ = 0;
                receive_rate_ = 0;
            }
            last_diagnostics_generation_ = state.diagnostics_generation;
            last_diagnostics_time_ = now;
            last_sent_bytes_ = current.sent_bytes;
            last_received_bytes_ = current.received_bytes;
            last_tcp_ = current.tcp_fallback;
            last_connected_ = current.connected;
            last_reconnects_ = current.reconnects;
        }
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
        ImGui::Begin("LanLink", nullptr, flags);
        const bool connected = state.service_online && state.diagnostics && state.diagnostics->authenticated;
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(logo_.Get())), ImVec2(44, 44));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted("LanLink");
        ImGui::TextDisabled(tr("Private network  /  %s"), lanlink::core::project_version().data());
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetWindowWidth() - 240);
        ImGui::TextColored(connected ? ImVec4(0.35f, 0.87f, 0.65f, 1) : ImVec4(0.95f, 0.70f, 0.35f, 1),
            "%s", connected ? tr("Connected") : state.service_online ? tr("Connecting to relay") : tr("Service unavailable"));
        ImGui::Separator();
        if (ImGui::BeginChild("navigation", ImVec2(270, -40), ImGuiChildFlags_Borders)) {
            int language=preferences_.language==lanlink::core::UiLanguage::korean ? 1 : 0;
            const char* languages[]{"English","한국어"};
            ImGui::TextUnformatted(tr("Language"));
            ImGui::SetNextItemWidth(-1);
            if(ImGui::Combo("##language",&language,languages,2)) {
                preferences_.language=language==1 ? lanlink::core::UiLanguage::korean : lanlink::core::UiLanguage::english;
                attempt([&]{lanlink::ui::save_preferences(preferences_);});
            }
            if(ImGui::SmallButton(tr("Setup guide"))){preferences_.setup_completed=false;setup_step_=0;}
            ImGui::SeparatorText(tr("Your networks"));
            ImGui::BeginDisabled(!connected || state.busy);
            if (ImGui::Button(tr("Create network"), ImVec2(-1, 0))) create_dialog_ = true;
            if (ImGui::Button(tr("Join network"), ImVec2(-1, 0))) join_dialog_ = true;
            ImGui::EndDisabled();
            ImGui::Spacing();
            if (state.networks.empty()) {
                ImGui::TextWrapped(connected ? tr("Create a network or join one using its network ID.")
                    : tr("Networks appear after the service connects to your relay."));
            }
            for (const auto& network : state.networks) {
                const auto id = hex_id(network.network_id);
                ImGui::PushID(id.c_str());
                if (ImGui::Selectable(network.name.c_str(), selected_ == network.network_id)) {
                    selected_ = network.network_id;
                    attempt([&] { networks_.submit(protocol::LocalCommand::peers,
                        protocol::encode_network_selection_request({network.network_id})); });
                }
                ImGui::TextDisabled(tr("%s  /  %u devices"), network.role == protocol::NetworkRole::owner ? tr("Owner") : tr("Member"), network.member_count);
                ImGui::Spacing();
                ImGui::PopID();
            }
            ImGui::SeparatorText(tr("This device"));
            const auto device = hex_id(state.activity.device_id);
            const bool has_identity = std::any_of(state.activity.device_id.begin(), state.activity.device_id.end(),
                [](std::byte value) { return value != std::byte{0}; });
            ImGui::TextDisabled("%s", has_identity ? (device.substr(0, 20) + "...").c_str() : tr("Waiting for authentication"));
            ImGui::BeginDisabled(!has_identity);
            if (ImGui::Button(tr("Copy device ID"))) ImGui::SetClipboardText(device.c_str());
            ImGui::EndDisabled();
            if (!state.service_online && ImGui::Button(tr("Start Windows service"))) {
                attempt([&] {
                    wchar_t executable[32768]{};
                    if (!GetModuleFileNameW(nullptr, executable, 32768)) throw std::runtime_error(tr("Cannot locate LanLink."));
                    const auto path = std::filesystem::path(executable).parent_path() / L"lanlink-service.exe";
                    if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"runas", path.c_str(), L"--start", nullptr, SW_HIDE)) <= 32)
                        throw std::runtime_error(tr("Service start was cancelled or could not be requested."));
                    next_diagnostics_ = {};
                });
            }
            if(ImGui::CollapsingHeader(tr("Account (optional)"))) {
                ImGui::TextWrapped(tr("Group your devices. A relay token works without an account."));
                ImGui::BeginDisabled(!connected || state.busy);
                if(ImGui::Button(tr("Refresh account")))attempt([&]{protocol::AccessRequest r;r.action=protocol::AccessAction::account_status;networks_.submit(protocol::LocalCommand::user_access,protocol::encode_access_request(r));});
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##accountname",tr("Account or device name"),account_name_.data(),account_name_.size());
                if(ImGui::Button(tr("Create optional account")))attempt([&]{protocol::AccessRequest r;r.action=protocol::AccessAction::account_create;r.text=account_name_.data();networks_.submit(protocol::LocalCommand::user_access,protocol::encode_access_request(r));});
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##enrollment",tr("Enrollment code"),enrollment_input_.data(),enrollment_input_.size(),ImGuiInputTextFlags_Password);
                if(ImGui::Button(tr("Enroll this device")))attempt([&]{protocol::AccessRequest r;r.action=protocol::AccessAction::device_enroll;r.text=account_name_.data();r.secret=enrollment_input_.data();networks_.submit(protocol::LocalCommand::user_access,protocol::encode_access_request(r));std::fill(enrollment_input_.begin(),enrollment_input_.end(),0);});
                if(account_ && account_->code==protocol::NetworkResultCode::success) {
                    ImGui::TextWrapped("%s",account_->text.c_str());
                    if(ImGui::Button(tr("Generate enrollment code")))attempt([&]{protocol::AccessRequest r;r.action=protocol::AccessAction::enrollment_issue;networks_.submit(protocol::LocalCommand::user_access,protocol::encode_access_request(r));});
                    if(!enrollment_code_.empty() && ImGui::Button(tr("Copy enrollment code")))ImGui::SetClipboardText(enrollment_code_.c_str());
                    if(!enrollment_code_.empty())ImGui::TextWrapped(tr("Enrollment expires after five minutes and one use."));
                    for(const auto& enrolled:account_->devices) {
                        const auto enrolled_id=hex_id(enrolled.device_id);ImGui::PushID(enrolled_id.c_str());
                        ImGui::TextWrapped("%s%s",enrolled.label.c_str(),enrolled.revoked ? tr(" (revoked)") : "");
                        if(!enrolled.revoked && enrolled.device_id!=state.activity.device_id && ImGui::SmallButton(tr("Revoke device"))) {
                            protocol::AccessRequest r;r.action=protocol::AccessAction::device_revoke;r.device_id=enrolled.device_id;
                            confirmation_=protocol::LocalCommand::user_access;confirmation_payload_=protocol::encode_access_request(r);
                            confirmation_text_=tr("Revoke this enrolled device and remove its network access?");confirm_dialog_=true;
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndDisabled();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("workspace", ImVec2(0, -40), ImGuiChildFlags_Borders)) {
            if(!preferences_.setup_completed) {
                ImGui::SeparatorText(tr("Welcome to LanLink"));
                if(setup_step_==0) {
                    ImGui::TextUnformatted(tr("Choose your language"));
                    ImGui::TextWrapped(tr("Choose English or Korean in the left panel. Accounts are optional; your self-hosted relay keeps working with a token."));
                } else if(setup_step_==1) {
                    ImGui::TextUnformatted(tr("Check your connection"));
                    ImGui::TextWrapped(tr("The installer configures the relay address, port and token file. Start the Windows service on the left and wait for authentication."));
                    ImGui::TextUnformatted(connected ? tr("Connected") : tr("Waiting for authentication"));
                    if(ImGui::Button(tr("Retry connection check")))next_diagnostics_={};
                } else {
                    ImGui::TextUnformatted(tr("Create or join your first network"));
                    ImGui::TextWrapped(tr("Create a private network and share a one-time invitation, or paste an invitation from its owner. You can reopen this guide at any time."));
                }
                if(setup_step_>0 && ImGui::Button(tr("Back")))--setup_step_;
                if(setup_step_>0)ImGui::SameLine();
                ImGui::BeginDisabled(setup_step_==1 && !connected);
                if(setup_step_<2) {if(ImGui::Button(tr("Next")))++setup_step_;}
                else if(ImGui::Button(tr("Finish")))finish_setup();
                ImGui::EndDisabled();ImGui::SameLine();
                if(ImGui::SmallButton(tr("Skip for now")))finish_setup();
                ImGui::Separator();
            }
            const auto selected = std::find_if(state.networks.begin(), state.networks.end(),
                [&](const auto& network) { return selected_ && network.network_id == *selected_; });
            if (selected == state.networks.end()) {
                ImGui::SeparatorText(tr("Connect your devices"));
                ImGui::TextWrapped(tr("LanLink connects Windows devices through a relay server. Create a network, share its ID, and approve the devices that should join."));
                ImGui::Spacing();
                ImGui::TextWrapped(tr("Your relay can run on a Linux server at any hosting provider or on your own hardware."));
            } else {
                const auto id = hex_id(selected->network_id);
                const bool owner = selected->role == protocol::NetworkRole::owner;
                ImGui::SeparatorText(selected->name.c_str());
                ImGui::TextDisabled("%s", owner ? tr("You manage this network") : tr("Managed by the network owner"));
                if (ImGui::Button(tr("Copy network ID"))) ImGui::SetClipboardText(id.c_str());
                ImGui::SameLine();
                ImGui::BeginDisabled(!connected || state.busy);
                if(owner && ImGui::Button(tr("Create invitation"))) attempt([&]{
                    protocol::AccessRequest r;r.action=protocol::AccessAction::invite_issue;r.network_id=selected->network_id;
                    r.lifetime_seconds=900;r.max_uses=1;
                    networks_.submit(protocol::LocalCommand::user_access,protocol::encode_access_request(r));
                });
                if(owner)ImGui::SameLine();
                if (ImGui::Button(tr("Leave network"))) {
                    confirmation_ = protocol::LocalCommand::leave;
                    confirmation_payload_ = protocol::encode_network_selection_request({selected->network_id});
                    confirmation_text_ = tr("Leave this network? You will need approval to join it again.");
                    confirm_dialog_ = true;
                }
                ImGui::EndDisabled();
                if(owner && invitation_network_==selected->network_id && !invitation_link_.empty()) {
                    ImGui::TextWrapped(tr("Invitation: one use, expires in 15 minutes. Share it privately."));
                    if(ImGui::Button(tr("Copy invitation link")))ImGui::SetClipboardText(invitation_link_.c_str());
                    ImGui::SameLine();if(ImGui::Button(tr("Copy invitation code")))ImGui::SetClipboardText(invitation_code_.c_str());
                }
                ImGui::BeginDisabled(!connected || state.busy);
                if(owner && ImGui::SmallButton(tr("Revoke invitation links"))) {
                    protocol::AccessRequest r;r.action=protocol::AccessAction::invite_revoke;r.network_id=selected->network_id;
                    confirmation_=protocol::LocalCommand::user_access;confirmation_payload_=protocol::encode_access_request(r);
                    confirmation_text_=tr("Revoke all unused invitation links for this network?");confirm_dialog_=true;
                }
                ImGui::EndDisabled();
                if (state.peers && state.peers->network_id == selected->network_id &&
                    now-state.peers_updated_at<std::chrono::seconds(6)) {
                    const auto& peers = *state.peers;
                    const auto own = ipv4(peers.own_address);
                    const auto* current_paths=state.paths && now-state.paths_updated_at<std::chrono::seconds(6) ? &*state.paths : nullptr;
                    const auto overview=protocol::network_overview(peers,current_paths,connected);
                    ImGui::Text(tr("Members: %u online / %u total"),overview.online,overview.total);
                    ImGui::Text(tr("Paths: %u Direct / %u Relay / %u connecting"),overview.direct,overview.relayed,overview.connecting);
                    const auto age=std::chrono::duration_cast<std::chrono::seconds>(now-state.peers_updated_at).count();
                    ImGui::TextDisabled(tr("Updated %lld seconds ago"),static_cast<long long>(age));
                    if(ImGui::Button(tr("Copy network overview"))) {
                        std::string report=tr("LanLink network: ")+selected->name+tr("\nNetwork ID: ")+id+tr("\nVirtual IP: ")+own+
                            tr("\nOnline: ")+std::to_string(overview.online)+"/"+std::to_string(overview.total)+
                            tr("\nDirect: ")+std::to_string(overview.direct)+tr(" Relay: ")+std::to_string(overview.relayed)+
                            tr("\nKey epoch: ")+std::to_string(peers.key_epoch);
                        ImGui::SetClipboardText(report.c_str());
                    }
                    ImGui::Text(tr("Your virtual IP: %s"), own.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr("Copy IP"))) ImGui::SetClipboardText(own.c_str());
                    ImGui::TextDisabled(tr("Use a device's virtual IP to connect to its game or application."));
                    ImGui::SeparatorText(tr("Devices"));
                    if (ImGui::BeginTable("devices", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn(tr("Virtual IP"), ImGuiTableColumnFlags_WidthFixed, 135);
                        ImGui::TableSetupColumn(tr("Device"));
                        ImGui::TableSetupColumn(tr("Status"), ImGuiTableColumnFlags_WidthFixed, 80);
                        ImGui::TableSetupColumn(tr("Path"), ImGuiTableColumnFlags_WidthFixed, 110);
                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80);
                        ImGui::TableHeadersRow();
                        const auto row = [&](const protocol::NetworkDeviceId& device_id, std::uint32_t address, bool online, bool own_device) {
                            const auto device_text = hex_id(device_id);
                            const auto ip = ipv4(address);
                            ImGui::PushID(device_text.c_str());
                            ImGui::TableNextRow(); ImGui::TableNextColumn();
                            if (ImGui::Selectable(ip.c_str())) ImGui::SetClipboardText(ip.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("%s%s", device_text.substr(0, 18).c_str(), own_device ? tr(" (you)") : "...");
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", device_text.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextColored(online ? ImVec4(0.35f, 0.87f, 0.65f, 1) : ImVec4(0.6f, 0.64f, 0.70f, 1), "%s", online ? tr("Online") : tr("Offline"));
                            ImGui::TableNextColumn();
                            const protocol::PeerPathStatus* path=nullptr;
                            if(current_paths)for(const auto& peer:current_paths->peers)
                                if(peer.network_id==selected->network_id && peer.device_id==device_id){path=&peer;break;}
                            const auto kind=path && path->path.lifecycle==protocol::PathLifecycle::authenticated ? path->path.kind : protocol::PathKind::none;
                            const char* label=own_device?tr("Local"):!online?tr("Offline"):kind==protocol::PathKind::direct_quic?tr("Direct"):kind==protocol::PathKind::relay_tls?tr("Relay (TCP)"):kind==protocol::PathKind::relay_quic?tr("Relay"):tr("Connecting");
                            ImGui::TextColored(kind==protocol::PathKind::direct_quic?ImVec4(.35f,.87f,.65f,1):ImVec4(.65f,.75f,.9f,1),"%s",label);
                            if(path && ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();ImGui::TextUnformatted(tr(protocol::path_reason_name(path->path.reason).data()));
                                if(path->path.rtt_us)ImGui::Text("RTT: %.1f ms",*path->path.rtt_us/1000.0);
                                ImGui::Text(tr("Packets: %llu sent / %llu received"),static_cast<unsigned long long>(path->path.sent_packets),static_cast<unsigned long long>(path->path.received_packets));
                                if(path->path.lost_packets)ImGui::Text(tr("Lost transport packets: %llu"),static_cast<unsigned long long>(*path->path.lost_packets));
                                ImGui::EndTooltip();
                            }
                            ImGui::TableNextColumn();
                            ImGui::BeginDisabled(!owner || own_device || !connected || state.busy);
                            if (ImGui::SmallButton(tr("Remove"))) {
                                confirmation_ = protocol::LocalCommand::kick;
                                confirmation_payload_ = protocol::encode_network_member_request({selected->network_id, device_id});
                                confirmation_text_ = tr("Remove this device from the network? Its network access will be revoked.");
                                confirm_dialog_ = true;
                            }
                            ImGui::EndDisabled(); ImGui::PopID();
                        };
                        row(state.activity.device_id, peers.own_address, connected, true);
                        for (const auto& peer : peers.peers) row(peer.device_id, peer.ipv4_address, connected && peer.signed_key.has_value(), false);
                        ImGui::EndTable();
                    }
                    if (owner) {
                        ImGui::SeparatorText(tr("Join requests"));
                        bool any_requests = false;
                        for (const auto& request : state.activity.requests) {
                            if (request.network_id != selected->network_id) continue;
                            any_requests = true;
                            const auto device_text = hex_id(request.device_id);
                            ImGui::PushID(device_text.c_str());
                            ImGui::Text("%s...", device_text.substr(0, 24).c_str());
                            ImGui::SameLine();
                            ImGui::BeginDisabled(!connected || state.busy);
                            if (ImGui::SmallButton(tr("Approve"))) attempt([&] {
                                networks_.submit(protocol::LocalCommand::approve, protocol::encode_network_member_request({selected->network_id, request.device_id}));
                            });
                            ImGui::EndDisabled(); ImGui::PopID();
                        }
                        if (!any_requests) ImGui::TextDisabled(tr("No new requests in this service session."));
                        if (ImGui::CollapsingHeader(tr("Manage a device by ID"))) {
                            ImGui::SetNextItemWidth(-1);
                            ImGui::InputTextWithHint("##device", tr("Device ID"), device_id_.data(), device_id_.size());
                            ImGui::BeginDisabled(!connected || state.busy || device_id_[0] == 0);
                            for (const auto& action : {std::pair{tr("Invite"), protocol::LocalCommand::invite}, std::pair{tr("Approve"), protocol::LocalCommand::approve}}) {
                                if (ImGui::Button(action.first)) attempt([&] {
                                    networks_.submit(action.second, protocol::encode_network_member_request({selected->network_id,
                                        parse_id<protocol::network_device_id_size>(device_id_.data())}));
                                });
                                ImGui::SameLine();
                            }
                            ImGui::NewLine(); ImGui::EndDisabled();
                        }
                    }
                } else ImGui::TextWrapped(tr("Waiting for an active network address. The owner may still need to approve this device."));
            }
            ImGui::Spacing();
            if (ImGui::CollapsingHeader(tr("Game network diagnostics"), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (state.games) {
                    const auto& metrics = *state.games;
                    ImGui::Text(tr("Virtual adapter MTU: %u  /  Rejected packets: %llu"),
                        static_cast<unsigned>(metrics.mtu), static_cast<unsigned long long>(metrics.rejected_packets));
                    if (metrics.adapter_failed)
                        ImGui::TextColored(ImVec4(1, .5f, .4f, 1), tr("The virtual adapter failed. Restart the LanLink service."));
                    const auto current = std::find_if(metrics.networks.begin(), metrics.networks.end(),
                        [&](const auto& item) { return selected_ && item.network_id == *selected_; });
                    if (current != metrics.networks.end()) {
                        ImGui::Text(tr("Virtual subnet: %s/%u  /  Online peers: %u"),
                            ipv4(current->subnet).c_str(), static_cast<unsigned>(current->prefix_length), current->online_peers);
                        ImGui::Text(tr("Discovery: %llu sent / %llu received"),
                            static_cast<unsigned long long>(current->discovery_sent),
                            static_cast<unsigned long long>(current->discovery_received));
                        ImGui::TextDisabled(tr("Suppressed: %llu rate limited / %llu duplicates / %llu reflections"),
                            static_cast<unsigned long long>(current->rate_limited),
                            static_cast<unsigned long long>(current->duplicates),
                            static_cast<unsigned long long>(current->reflections));
                        if (!current->online_peers)
                            ImGui::TextWrapped(tr("No other authenticated device is online. Check that the host joined this network and was approved."));
                        else if (!current->discovery_received)
                            ImGui::TextWrapped(tr("No inbound discovery has been observed in this session. Refresh the game's LAN list and check the host's game server and firewall rules."));
                        if (ImGui::Button(tr("Copy game diagnostics"))) {
                            const auto report = std::string(tr("LanLink game diagnostics\nVirtual IP: ")) + ipv4(current->address) +
                                tr("\nSubnet: ") + ipv4(current->subnet) + "/" + std::to_string(current->prefix_length) +
                                "\nMTU: " + std::to_string(metrics.mtu) +
                                tr("\nOnline peers: ") + std::to_string(current->online_peers) +
                                tr("\nDiscovery sent/received: ") + std::to_string(current->discovery_sent) + "/" +
                                std::to_string(current->discovery_received) +
                                tr("\nRate limited: ") + std::to_string(current->rate_limited) +
                                tr("\nFirewall/game server status: not measured");
                            ImGui::SetClipboardText(report.c_str());
                        }
                    } else ImGui::TextWrapped(tr("Select an approved network with an active virtual address."));
                    if (metrics.truncated) ImGui::TextDisabled(tr("Showing a limited number of network diagnostics."));
                } else ImGui::TextWrapped(tr("Game telemetry is unavailable. Check the background service and use matching UI/service versions."));
                ImGui::TextWrapped(tr("If LAN search fails, connect to the host's virtual IPv4 and game port. Both devices must use the same game version and virtual network."));
                ImGui::TextWrapped(tr("Allow the game's required inbound ports on the LanLink adapter. Firewall and game-server rules are not measured automatically."));
                const auto profiles = protocol::game_profiles();
                const char* selection = game_profile_ >= 0 && static_cast<std::size_t>(game_profile_) < profiles.size()
                    ? profiles[static_cast<std::size_t>(game_profile_)].name.data() : tr("General guidance");
                if (ImGui::BeginCombo(tr("Game profile"), selection)) {
                    if (ImGui::Selectable(tr("General guidance"), game_profile_ == -1)) game_profile_ = -1;
                    for (std::size_t index = 0; index < profiles.size(); ++index)
                        if (ImGui::Selectable(profiles[index].name.data(), game_profile_ == static_cast<int>(index)))
                            game_profile_ = static_cast<int>(index);
                    ImGui::EndCombo();
                }
                if (game_profile_ >= 0 && static_cast<std::size_t>(game_profile_) < profiles.size()) {
                    const auto& profile = profiles[static_cast<std::size_t>(game_profile_)];
                    ImGui::Text(tr("Version %s / UDP %u-%u"), profile.version.data(),
                        static_cast<unsigned>(profile.first_port), static_cast<unsigned>(profile.last_port));
                    ImGui::TextWrapped("%s", tr(profile.guidance.data()));
                }
            }
            if (ImGui::CollapsingHeader(tr("Connection details"), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (state.diagnostics) {
                    const auto& d = *state.diagnostics;
                    ImGui::Text(tr("Relay transport: %s   /   Reconnects: %u"), d.tcp_fallback ? tr("TLS over TCP") : tr("QUIC over UDP"), d.reconnects);
                    if (d.rtt_us) ImGui::Text(tr("Relay latency: %.1f ms"), *d.rtt_us / 1000.0);
                    ImGui::Text(tr("Upload %.1f KiB/s   /   Download %.1f KiB/s"), send_rate_ / 1024.0, receive_rate_ / 1024.0);
                    ImGui::TextDisabled(tr("Sent %s   /   Received %s"), format_bytes(d.sent_bytes).c_str(), format_bytes(d.received_bytes).c_str());
                    if(state.paths) {
                        unsigned direct=0,relayed=0;for(const auto& peer:state.paths->peers) {
                            direct+=peer.path.kind==protocol::PathKind::direct_quic;
                            relayed+=peer.path.kind==protocol::PathKind::relay_quic || peer.path.kind==protocol::PathKind::relay_tls;
                        }
                        ImGui::Text(tr("Peer paths: %u Direct / %u Relay"),direct,relayed);
                        if(state.paths->truncated)ImGui::TextDisabled(tr("Showing a limited number of peer paths."));
                    }
                    if (!d.last_error.empty()) ImGui::TextWrapped(tr("Last connection error: %s"), d.last_error.c_str());
                } else ImGui::TextWrapped(tr("The Windows service is unavailable. Start the service or run the LanLink installer to configure this device."));
            }
        }
        ImGui::EndChild();
        if (state.busy) ImGui::TextDisabled(tr("Updating..."));
        else if (!local_error_.empty()) ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "%s", tr(local_error_.c_str()));
        else if (!state.error.empty()) ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "%s", tr(state.error.c_str()));
        else if (!state.message.empty()) ImGui::TextUnformatted(tr(state.message.c_str()));
        else ImGui::TextDisabled(tr("The background service keeps your networks connected when this window is closed."));
        if (create_dialog_) { close_create_=false;ImGui::OpenPopup(tr("Create network")); create_dialog_ = false; }
        if (join_dialog_) { close_join_=false;ImGui::OpenPopup(tr("Join network")); join_dialog_ = false; }
        if (confirm_dialog_) { ImGui::OpenPopup(tr("Confirm network change")); confirm_dialog_ = false; }
        if (ImGui::BeginPopupModal(tr("Create network"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(tr("Name your private network."));
            ImGui::SetNextItemWidth(380);
            ImGui::InputTextWithHint("##name", tr("Network name"), name_.data(), name_.size());
            ImGui::SetNextItemWidth(380);
            ImGui::InputTextWithHint("##createpassword",tr("Password (optional)"),create_password_.data(),create_password_.size(),ImGuiInputTextFlags_Password);
            if(create_password_[0])ImGui::Checkbox(tr("Require owner approval"),&require_approval_);
            ImGui::BeginDisabled(!connected || state.busy || name_[0] == 0);
            if (ImGui::Button(tr("Create"), ImVec2(180, 0))) {
                attempt([&] { const auto request=protocol::create_network_command(name_.data(),create_password_.data(),require_approval_);
                    networks_.submit(request.command,request.payload); });
                if(local_error_.empty())std::fill(create_password_.begin(),create_password_.end(),0);
            }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"), ImVec2(180, 0))) {std::fill(create_password_.begin(),create_password_.end(),0);ImGui::CloseCurrentPopup();}
            if(!state.error.empty())ImGui::TextWrapped("%s",tr(state.error.c_str()));
            if(!local_error_.empty())ImGui::TextWrapped("%s",tr(local_error_.c_str()));
            if(close_create_){ImGui::CloseCurrentPopup();close_create_=false;}
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal(tr("Join network"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(tr("Paste a network ID, invitation link or invitation code."));
            ImGui::SetNextItemWidth(420);
            ImGui::InputTextWithHint("##network", tr("Network ID or invitation"), network_id_.data(), network_id_.size());
            ImGui::SetNextItemWidth(420);
            ImGui::InputTextWithHint("##joinpassword",tr("Network password (if required)"),join_password_.data(),join_password_.size(),ImGuiInputTextFlags_Password);
            ImGui::BeginDisabled(!connected || state.busy || network_id_[0] == 0);
            if (ImGui::Button(tr("Send join request"), ImVec2(200, 0))) {
                attempt([&] {const auto request=protocol::join_network_command(network_id_.data(),join_password_.data());
                    networks_.submit(request.command,request.payload);});
                if(local_error_.empty())std::fill(join_password_.begin(),join_password_.end(),0);
            }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"), ImVec2(200, 0))) {std::fill(join_password_.begin(),join_password_.end(),0);ImGui::CloseCurrentPopup();}
            if(close_join_){ImGui::CloseCurrentPopup();close_join_=false;}
            if(!state.error.empty())ImGui::TextWrapped("%s",tr(state.error.c_str()));
            if (!local_error_.empty()) ImGui::TextWrapped("%s", tr(local_error_.c_str()));
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal(tr("Confirm network change"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(tr(confirmation_text_.c_str()));
            ImGui::BeginDisabled(!connected || state.busy);
            if (ImGui::Button(tr("Confirm"), ImVec2(170, 0))) {
                attempt([&] { networks_.submit(confirmation_, confirmation_payload_); });
                if (local_error_.empty()) ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"), ImVec2(170, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::End();
    }

    static std::string ipv4(std::uint32_t address) {
        return std::to_string(address >> 24) + "." + std::to_string((address >> 16) & 255) + "." +
            std::to_string((address >> 8) & 255) + "." + std::to_string(address & 255);
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
    bool registered_ = false;
    bool resized_ = false;
    bool win32_ready_ = false;
    bool dx11_ready_ = false;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> logo_;
    lanlink::ui::NetworkModel networks_;
    lanlink::ui::Preferences preferences_;
    unsigned setup_step_=0;
    std::optional<lanlink::protocol::NetworkId> selected_;
    std::array<char, 129> name_{};
    std::array<char, 256> network_id_{};
    std::array<char,129> create_password_{},join_password_{};
    bool require_approval_=true,close_create_=false,close_join_=false;
    std::optional<lanlink::protocol::NetworkId> selection_hint_;
    std::uint64_t operation_generation_=0,access_generation_=0;
    lanlink::protocol::NetworkId invitation_network_{};
    std::string invitation_link_,invitation_code_;
    std::optional<lanlink::protocol::AccessResult> account_;
    std::array<char,129> account_name_{};
    std::array<char,41> enrollment_input_{};
    std::string enrollment_code_;
    std::array<char, 65> device_id_{};
    std::string local_error_;
    bool create_dialog_ = false;
    bool join_dialog_ = false;
    bool confirm_dialog_ = false;
    lanlink::protocol::LocalCommand confirmation_ = lanlink::protocol::LocalCommand::leave;
    std::vector<std::byte> confirmation_payload_;
    std::string confirmation_text_;
    std::filesystem::path screenshot_;
    std::chrono::steady_clock::time_point next_diagnostics_{};
    std::chrono::steady_clock::time_point last_diagnostics_time_{};
    std::uint64_t last_diagnostics_generation_ = 0;
    int game_profile_ = -1;
    std::uint64_t last_sent_bytes_ = 0;
    std::uint64_t last_received_bytes_ = 0;
    bool last_tcp_ = false;
    bool last_connected_ = false;
    std::uint32_t last_reconnects_ = 0;
    double send_rate_ = 0;
    double receive_rate_ = 0;
};

}

int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR, int) {
    std::filesystem::path screenshot;
    try {
        require(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "COM initialization");
        int count = 0;
        auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        std::optional<lanlink::core::UiLanguage> language;
        bool skip_setup=false,force_setup=false;
        for(int i=1;i<count;++i) {
            const std::wstring_view argument=arguments[i];
            if(argument==L"--screenshot" && i+1<count)screenshot=arguments[++i];
            else if(argument==L"--skip-setup")skip_setup=true;
            else if(argument==L"--setup")force_setup=true;
            else if(argument==L"--language" && i+1<count) {
                const std::wstring_view selected=arguments[++i];
                if(selected!=L"en" && selected!=L"ko")throw std::invalid_argument("language must be en or ko");
                language=selected==L"ko" ? lanlink::core::UiLanguage::korean : lanlink::core::UiLanguage::english;
            } else throw std::invalid_argument("usage: lanlink-ui [--language en|ko] [--setup] [--skip-setup] [--screenshot FILE.png]");
        }
        LocalFree(arguments);
        DesktopWindow window(instance, screenshot,language,skip_setup,force_setup);
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
