#include "update_process.h"
#include "localization.h"
#include <shellapi.h>
#include <future>
#include <limits>
#include <dwmapi.h>
using namespace uc::update;
namespace {
struct WindowState {
    bool recover=false;
    std::wstring updating,restoring,wait,failsafe;   // the launcher's language
    UINT dpi=96;
    HFONT heading=nullptr,body=nullptr,caption=nullptr;
    ~WindowState() {for(auto font:{heading,body,caption}) if(font) DeleteObject(font);}
    int px(int value) const {return MulDiv(value,dpi,96);}
    void layout(UINT value) {
        dpi=value;
        auto make=[&](HFONT& font,int size,int weight) {
            auto next=CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            if(next) {if(font) DeleteObject(font);font=next;}
        };
        make(heading,24,FW_SEMIBOLD);make(body,16,FW_NORMAL);make(caption,13,FW_NORMAL);
    }
    void paint(HWND window,HDC dc) const {
        RECT area{};GetClientRect(window,&area);
        auto fill=[&](RECT bounds,COLORREF color) {auto brush=CreateSolidBrush(color);FillRect(dc,&bounds,brush);DeleteObject(brush);};
        fill(area,RGB(28,33,34));
        SetBkMode(dc,TRANSPARENT);
        auto text=[&](const wchar_t* value,HFONT font,int top,int height,COLORREF color) {
            auto previous=SelectObject(dc,font?font:GetStockObject(DEFAULT_GUI_FONT));SetTextColor(dc,color);
            RECT bounds{px(32),px(top),area.right-px(32),px(top+height)};
            DrawTextW(dc,value,-1,&bounds,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
            SelectObject(dc,previous);
        };
        text(L"UMBRELLA CHRONICLES",caption,24,20,RGB(146,152,153));
        text((recover?restoring:updating).c_str(),heading,52,36,RGB(238,233,220));
        text(wait.c_str(),body,102,48,RGB(182,187,187));
        RECT track{px(32),px(172),area.right-px(32),px(178)};
        fill(track,RGB(48,54,56));
        const int width=track.right-track.left,segment=width/4;
        const int offset=int((GetTickCount64()%2400)*(width+segment)/2400)-segment;
        RECT progress{track.left+std::max(0,offset),track.top,track.left+std::min(width,offset+segment),track.bottom};
        fill(progress,RGB(179,35,49));
        text(failsafe.c_str(),caption,198,28,RGB(146,152,153));
    }
};
LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto state=reinterpret_cast<WindowState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        state=static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));
    }
    if(message==WM_DPICHANGED) {
        const auto& area=*reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window,nullptr,area.left,area.top,area.right-area.left,area.bottom-area.top,SWP_NOZORDER|SWP_NOACTIVATE);
        if(state) state->layout(HIWORD(wparam));
        InvalidateRect(window,nullptr,FALSE);
        return 0;
    }
    if(message==WM_ERASEBKGND) return 1;
    if(message==WM_TIMER) {InvalidateRect(window,nullptr,FALSE);return 0;}
    if(message==WM_PAINT && state) {
        PAINTSTRUCT paint{};auto dc=BeginPaint(window,&paint);
        RECT area{};GetClientRect(window,&area);
        auto buffer=CreateCompatibleDC(dc);auto bitmap=CreateCompatibleBitmap(dc,area.right,area.bottom);
        if(buffer && bitmap) {
            auto previous=SelectObject(buffer,bitmap);state->paint(window,buffer);
            BitBlt(dc,0,0,area.right,area.bottom,buffer,0,0,SRCCOPY);SelectObject(buffer,previous);
        } else state->paint(window,dc);
        if(bitmap) DeleteObject(bitmap);if(buffer) DeleteDC(buffer);
        EndPaint(window,&paint);return 0;
    }
    if(message==WM_PRINTCLIENT && state) {state->paint(window,reinterpret_cast<HDC>(wparam));return 0;}
    if(message==WM_DESTROY) {KillTimer(window,1);return 0;}
    if(message==WM_CLOSE) return 0;
    return DefWindowProcW(window,message,wparam,lparam);
}
HANDLE inherited_handle(std::wstring_view value) {
    uintptr_t number=0;
    if(value.empty()) throw std::runtime_error("Missing inherited handle");
    for(auto c:value) {
        if(c<L'0' || c>L'9' || number>(std::numeric_limits<uintptr_t>::max()-(c-L'0'))/10)
            throw std::runtime_error("Invalid inherited handle");
        number=number*10+(c-L'0');
    }
    auto handle=reinterpret_cast<HANDLE>(number);DWORD flags=0;
    if(!number || !GetHandleInformation(handle,&flags) || !(flags&HANDLE_FLAG_INHERIT))
        throw std::runtime_error("Handle was not inherited");
    return handle;
}
std::wstring wide(const std::string& text) {
    int size=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    std::wstring result(size,L' ');
    MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size);return result;
}
// The launcher's language (launcher.language in the client's config.json, "system" or unreadable: Windows' language). The client is two levels
// above the operation folder (<client>/.uc-update/<id>).
std::string updater_language(const fs::path& operation) {
    try {
        const auto configured=uc::launcher::config_language(uc::load_config(operation.parent_path().parent_path()/L"config.json"));
        if(configured!="system") return configured;
    } catch(const std::exception&) {}
    return uc::launcher::system_language();
}
std::wstring tr(const std::string& language,std::string_view key) {
    const auto text=uc::launcher::text(language,key);
    return wide(std::string(text));
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    std::string language=uc::launcher::system_language();   // until the operation folder (and so the client's config) is known
    std::wstring title=tr(language,"updater_title");
    try {
        wchar_t module[32768]{};
        if(!GetModuleFileNameW(nullptr,module,32768)) throw std::runtime_error("Cannot find updater location");
        int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        if(!argv) throw std::runtime_error("Cannot read updater arguments");
        struct Arguments {wchar_t** value;~Arguments(){LocalFree(value);}} arguments{argv};
        fs::path operation;bool recover=false;Handle parent,ready;
        if(argc==1) {
            operation=fs::path(module).parent_path().parent_path();recover=true;
            if(fs::path(module).parent_path().filename()!=L"recovery")
                throw std::runtime_error("Start updates from uc_launcher.exe. Recovery copies are under .uc-update/<operation>/recovery/.");
            language=updater_language(operation);title=tr(language,"updater_title");
            if(MessageBoxW(nullptr,tr(language,"updater_recover_ask").c_str(),
                title.c_str(),MB_YESNO|MB_ICONQUESTION)!=IDYES) return 0;
        } else if((argc==5 || argc==3) && (std::wstring_view(argv[1])==L"--apply" || std::wstring_view(argv[1])==L"--recover")) {
            operation=fs::absolute(argv[2]).lexically_normal();recover=std::wstring_view(argv[1])==L"--recover";
            language=updater_language(operation);title=tr(language,"updater_title");
            if(argc==5) {parent=Handle(inherited_handle(argv[3]));ready=Handle(inherited_handle(argv[4]));}
            else if(!recover) throw std::runtime_error("Apply requires a launcher handshake");
        } else throw std::runtime_error("Invalid updater arguments");
        if(!fs::equivalent(fs::path(module).parent_path(),operation/"recovery"))
            throw std::runtime_error("Updater must run from this operation's recovery directory");
        WNDCLASSW cls{};cls.lpfnWndProc=window_proc;cls.hInstance=instance;cls.lpszClassName=L"UCUpdater";
        cls.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));
        if(!RegisterClassW(&cls)) throw std::runtime_error("Cannot register updater window");
        RECT area{};SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0);
        WindowState state;state.recover=recover;
        state.updating=tr(language,"updater_updating");state.restoring=tr(language,"updater_restoring");
        state.wait=tr(language,"updater_wait");state.failsafe=tr(language,"updater_failsafe");
        const DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU;
        const auto dpi=GetDpiForSystem();
        RECT bounds{0,0,MulDiv(560,dpi,96),MulDiv(248,dpi,96)};
        AdjustWindowRectExForDpi(&bounds,style,FALSE,WS_EX_DLGMODALFRAME,dpi);
        const auto width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
        HWND window=CreateWindowExW(WS_EX_DLGMODALFRAME,cls.lpszClassName,title.c_str(),WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
            area.left+(area.right-area.left-width)/2,area.top+(area.bottom-area.top-height)/2,width,height,nullptr,nullptr,instance,&state);
        if(!window) throw std::runtime_error("Cannot create updater window");
        struct WindowOwner {HWND handle;~WindowOwner(){DestroyWindow(handle);}} owner{window};
        EnableMenuItem(GetSystemMenu(window,FALSE),SC_CLOSE,MF_BYCOMMAND|MF_GRAYED);
        state.layout(GetDpiForWindow(window));
        BOOL dark=TRUE;DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
        SetTimer(window,1,40,nullptr);
        ShowWindow(window,SW_SHOWNORMAL);UpdateWindow(window);
        auto work=std::async(std::launch::async,[&] {
            try {return run_update_helper(operation,parent.valid()?parent.get():nullptr,ready.valid()?ready.get():nullptr,recover);}
            catch(const std::exception& e) {return Status{Error{ErrorCode::recovery_required,e.what()}};}
        });
        while(work.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) {
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&message);DispatchMessageW(&message);}
            MsgWaitForMultipleObjects(0,nullptr,FALSE,50,QS_ALLINPUT);
        }
        auto status=work.get();
        if(failed(status)) {
            auto message=wide(std::get<Error>(status).detail)+L"\n\n"+operation.wstring();
            MessageBoxW(window,message.c_str(),title.c_str(),MB_OK|MB_ICONERROR);
        }
        return failed(status)?2:0;
    } catch(const std::exception& e) {MessageBoxW(nullptr,wide(e.what()).c_str(),title.c_str(),MB_OK|MB_ICONERROR);return 2;}
}
