#include "desktop_view.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#include <png.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <filesystem>
#include <iostream>
#include <thread>
extern char** environ;
namespace {
std::filesystem::path executable_path() {
#ifdef __APPLE__
    std::uint32_t size=0;_NSGetExecutablePath(nullptr,&size);std::string buffer(size,'\0');
    if(_NSGetExecutablePath(buffer.data(),&size)!=0)throw std::runtime_error("cannot locate application");
    return std::filesystem::canonical(buffer.c_str());
#else
    return std::filesystem::canonical("/proc/self/exe");
#endif
}
void start_service() {
#ifdef __APPLE__
    const char* program="/usr/bin/osascript";
    const char* raw[]={program,"-e","do shell script \"/bin/launchctl kickstart system/com.lanlink.client\" with administrator privileges",nullptr};
#else
    const char* program="/usr/bin/pkexec";
    const char* raw[]={program,"/usr/bin/systemctl","start","lanlink-client.service",nullptr};
#endif
    pid_t process=0;const auto result=posix_spawn(&process,program,nullptr,nullptr,const_cast<char**>(raw),environ);
    if(result)throw std::runtime_error("cannot request service startup; use the installed system service");
    std::thread([process]{int status=0;while(waitpid(process,&status,0)<0 && errno==EINTR){}}).detach();
}
std::filesystem::path logo_path() {
    const auto directory=executable_path().parent_path();
#ifdef __APPLE__
    const auto installed=directory/"../Resources/lanlink.png";
#else
    const auto installed=directory/"../share/lanlink/lanlink.png";
#endif
    if(std::filesystem::is_regular_file(installed))return installed;
    return LANLINK_LOGO_PATH;
}
struct Desktop {
    SDL_Window* window=nullptr;SDL_GLContext context=nullptr;
    GLuint logo=0;bool imgui=false,platform=false,renderer=false;
    ~Desktop(){
        if(renderer)ImGui_ImplOpenGL3_Shutdown();
        if(platform)ImGui_ImplSDL3_Shutdown();
        if(imgui)ImGui::DestroyContext();
        if(logo)glDeleteTextures(1,&logo);
        if(context)SDL_GL_DestroyContext(context);
        if(window)SDL_DestroyWindow(window);
        SDL_Quit();
    }
    void load_logo() {
        png_image image{};image.version=PNG_IMAGE_VERSION;
        if(!png_image_begin_read_from_file(&image,logo_path().c_str()))throw std::runtime_error("cannot load LanLink logo");
        image.format=PNG_FORMAT_RGBA;
        if(image.width!=512 || image.height!=512){png_image_free(&image);throw std::runtime_error("invalid LanLink logo size");}
        std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(image));
        if(!png_image_finish_read(&image,nullptr,pixels.data(),0,nullptr)){png_image_free(&image);throw std::runtime_error("cannot decode LanLink logo");}
        glGenTextures(1,&logo);glBindTexture(GL_TEXTURE_2D,logo);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,static_cast<GLsizei>(image.width),static_cast<GLsizei>(image.height),0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
        auto* icon=SDL_CreateSurfaceFrom(static_cast<int>(image.width),static_cast<int>(image.height),SDL_PIXELFORMAT_RGBA32,pixels.data(),static_cast<int>(image.width*4));
        if(icon){SDL_SetWindowIcon(window,icon);SDL_DestroySurface(icon);}png_image_free(&image);
    }
    void capture(const std::filesystem::path& path,int width,int height) {
        if(width<=0 || height<=0 || width>8192 || height>8192)throw std::runtime_error("invalid capture size");
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width)*height*4),flipped(pixels.size());
        glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
        for(int y=0;y<height;++y)std::copy_n(pixels.data()+static_cast<std::size_t>(height-y-1)*width*4,static_cast<std::size_t>(width)*4,flipped.data()+static_cast<std::size_t>(y)*width*4);
        png_image image{};image.version=PNG_IMAGE_VERSION;image.width=width;image.height=height;image.format=PNG_FORMAT_RGBA;
        if(!png_image_write_to_file(&image,path.c_str(),0,flipped.data(),0,nullptr))throw std::runtime_error("cannot save desktop capture");
    }
};
}
int main(int argc,char* argv[]) {
    try {
        std::optional<lanlink::core::UiLanguage> language;bool management=false;
        std::filesystem::path screenshot;std::string invitation;
        if(argc==2 && std::string_view(argv[1]).starts_with("lanlink://")){invitation=argv[1];argc=1;}
        else if(argc>=2 && std::string_view(argv[1])=="--invite"){if(argc!=3)throw std::invalid_argument("--invite requires exactly one invitation URI");invitation=argv[2];argc=1;}
        for(int i=1;i<argc;++i) {
            const std::string_view option=argv[i];
            if(option=="--management")management=true;
            else if(option=="--screenshot" && i+1<argc)screenshot=argv[++i];
            else if(option=="--language" && i+1<argc){const std::string_view value=argv[++i];if(value!="en" && value!="ko")throw std::invalid_argument("language must be en or ko");language=value=="ko"?lanlink::core::UiLanguage::korean:lanlink::core::UiLanguage::english;}
            else throw std::invalid_argument("usage: lanlink-ui [--language en|ko] [--management] [--screenshot FILE.png]");
        }
        Desktop desktop;
        if(!SDL_Init(SDL_INIT_VIDEO))throw std::runtime_error(SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
        desktop.window=SDL_CreateWindow("LanLink",lanlink::ui::design::default_width,lanlink::ui::design::default_height,SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if(!desktop.window)throw std::runtime_error(SDL_GetError());
        SDL_SetWindowMinimumSize(desktop.window,640,440);
        desktop.context=SDL_GL_CreateContext(desktop.window);if(!desktop.context)throw std::runtime_error(SDL_GetError());
        SDL_GL_MakeCurrent(desktop.window,desktop.context);SDL_GL_SetSwapInterval(1);
        IMGUI_CHECKVERSION();ImGui::CreateContext();desktop.imgui=true;lanlink::ui::configure_desktop_style();
        for(const auto* name:{"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc","/System/Library/Fonts/AppleSDGothicNeo.ttc"})if(std::filesystem::is_regular_file(name)) {
            lanlink::ui::design::load_fonts(name,name);break;
        }
        if(!ImGui_ImplSDL3_InitForOpenGL(desktop.window,desktop.context))throw std::runtime_error("SDL desktop initialization failed");desktop.platform=true;
        if(!ImGui_ImplOpenGL3_Init("#version 150"))throw std::runtime_error("OpenGL desktop initialization failed");desktop.renderer=true;
        desktop.load_logo();
        lanlink::ui::DesktopView view(language,management,"Start service",start_service);view.set_logo(static_cast<ImTextureID>(desktop.logo));
        if(!invitation.empty())view.open_invitation(invitation);
        const auto opened=std::chrono::steady_clock::now();bool running=true;
        while(running) {
            SDL_Event event;while(SDL_PollEvent(&event)) {ImGui_ImplSDL3_ProcessEvent(&event);if(event.type==SDL_EVENT_DROP_FILE && event.drop.data && std::string_view(event.drop.data).starts_with("lanlink://"))view.attempt([&]{view.open_invitation(event.drop.data);});if(event.type==SDL_EVENT_QUIT || (event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID==SDL_GetWindowID(desktop.window)))running=false;}
            ImGui_ImplOpenGL3_NewFrame();ImGui_ImplSDL3_NewFrame();ImGui::NewFrame();view.render();ImGui::Render();
            int width=0,height=0;SDL_GetWindowSizeInPixels(desktop.window,&width,&height);glViewport(0,0,width,height);glClearColor(0.08f,0.1f,0.14f,1);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if(!screenshot.empty() && std::chrono::steady_clock::now()-opened>std::chrono::seconds{4}){desktop.capture(screenshot,width,height);break;}
            SDL_GL_SwapWindow(desktop.window);
        }
        return 0;
    }catch(const std::exception& error){std::cerr<<"lanlink-ui: "<<error.what()<<'\n';return 1;}
}
