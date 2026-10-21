#include "compile_screen.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

namespace uc {
struct CompileScreen::State {
    HWND window=nullptr;
    HFONT title=nullptr, detail=nullptr;
    size_t done=0, total=0;
    bool shown=false;
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
};
namespace {
constexpr int kWidth=520, kHeight=150;
constexpr COLORREF kBackground=RGB(22,24,28), kText=RGB(235,235,235), kMuted=RGB(150,155,165), kTrack=RGB(48,52,60), kFill=RGB(80,170,255);

void paint(HWND window,const CompileScreen::State& state) {
    PAINTSTRUCT paint;
    HDC target=BeginPaint(window,&paint);
    RECT client; GetClientRect(window,&client);
    HDC dc=CreateCompatibleDC(target);   // double buffered: the bar redraws many times per second
    HBITMAP bitmap=CreateCompatibleBitmap(target,client.right,client.bottom);
    HGDIOBJ old=SelectObject(dc,bitmap);
    HBRUSH background=CreateSolidBrush(kBackground);
    FillRect(dc,&client,background); DeleteObject(background);
    SetBkMode(dc,TRANSPARENT);
    SelectObject(dc,state.title); SetTextColor(dc,kText);
    const bool starting=state.total==0;   // before the first progress report: the renderer is still starting
    RECT title{24,18,client.right-24,52}; DrawTextW(dc,starting ? L"Запуск игры" : L"Компиляция шейдеров",-1,&title,DT_LEFT|DT_SINGLELINE);
    SelectObject(dc,state.detail); SetTextColor(dc,kMuted);
    RECT detail{24,54,client.right-24,80}; DrawTextW(dc,starting ? L"Загрузка DLSS и графического движка." : L"Нужно, чтобы игра шла без фризов.",-1,&detail,DT_LEFT|DT_SINGLELINE);
    if(starting) { BitBlt(target,0,0,client.right,client.bottom,dc,0,0,SRCCOPY); SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); EndPaint(window,&paint); return; }
    const RECT track{24,96,client.right-24,112};
    HBRUSH track_brush=CreateSolidBrush(kTrack); FillRect(dc,&track,track_brush); DeleteObject(track_brush);
    RECT fill=track; fill.right=track.left+LONG((track.right-track.left)*(state.total ? double(std::min(state.done,state.total))/double(state.total) : 0.0));
    HBRUSH fill_brush=CreateSolidBrush(kFill); FillRect(dc,&fill,fill_brush); DeleteObject(fill_brush);
    const std::wstring count=std::to_wstring(state.done)+L" / "+std::to_wstring(state.total);
    RECT counter{24,116,client.right-24,140}; DrawTextW(dc,count.c_str(),-1,&counter,DT_RIGHT|DT_SINGLELINE);
    BitBlt(target,0,0,client.right,client.bottom,dc,0,0,SRCCOPY);
    SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc);
    EndPaint(window,&paint);
}
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_NCCREATE) SetWindowLongPtrW(window,GWLP_USERDATA,LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* state=reinterpret_cast<CompileScreen::State*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_PAINT && state) { paint(window,*state); return 0; }
    if(message==WM_ERASEBKGND) return 1;
    return DefWindowProcW(window,message,w,l);
}
}

CompileScreen::CompileScreen() {
    state_=new State;
    try {
        WNDCLASSW cls{}; cls.lpfnWndProc=procedure; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"UcCompileScreen"; cls.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
        if(!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("compile screen: window class registration failed");
        state_->title=CreateFontW(-26,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        state_->detail=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        const int x=(GetSystemMetrics(SM_CXSCREEN)-kWidth)/2, y=(GetSystemMetrics(SM_CYSCREEN)-kHeight)/2;
        state_->window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"Resident Evil: The Umbrella Chronicles",WS_POPUP,x,y,kWidth,kHeight,nullptr,nullptr,cls.hInstance,state_);
        if(!state_->window) throw std::runtime_error("compile screen: window creation failed");
    } catch(...) { this->~CompileScreen(); throw; }
}
CompileScreen::~CompileScreen() {
    if(!state_) return;
    if(state_->window) DestroyWindow(state_->window);
    if(state_->title) DeleteObject(state_->title);
    if(state_->detail) DeleteObject(state_->detail);
    delete state_; state_=nullptr;
}
void CompileScreen::show() {
    ShowWindow(state_->window,SW_SHOWNOACTIVATE); state_->shown=true; UpdateWindow(state_->window);
}
void CompileScreen::update(size_t done,size_t total) {
    state_->done=done; state_->total=total;
    if(!state_->shown && std::chrono::steady_clock::now()-state_->started>std::chrono::milliseconds(400)) { ShowWindow(state_->window,SW_SHOWNOACTIVATE); state_->shown=true; }
    if(state_->shown) InvalidateRect(state_->window,nullptr,FALSE);
    MSG message;
    while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
}
