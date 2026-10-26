#define wWinMain unused_updater_entry
#include "../updater/main.cpp"
#undef wWinMain
#include "check.h"
#include <fstream>

int main(int argc,char** argv) {
    WindowState state;
    WNDCLASSW cls{};cls.lpfnWndProc=window_proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"UCUpdaterTest";
    CHECK(RegisterClassW(&cls));
    auto window=CreateWindowW(cls.lpszClassName,L"Updater test",WS_POPUP,0,0,560,248,
        nullptr,nullptr,cls.hInstance,&state);CHECK(window);
    for(auto dpi:{96u,120u,144u,192u,96u}) {
        RECT bounds{0,0,MulDiv(560,dpi,96),MulDiv(248,dpi,96)};
        // Synthetic DPI messages exercise client rendering; Windows keeps real-monitor title-bar metrics.
        SendMessageW(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&bounds));
        CHECK(state.dpi==dpi);
        LOGFONTW font{};CHECK(GetObjectW(state.heading,sizeof(font),&font));CHECK(font.lfHeight==-MulDiv(24,dpi,96));
        CHECK(GetObjectW(state.body,sizeof(font),&font));CHECK(font.lfHeight==-MulDiv(16,dpi,96));
        RECT area{};CHECK(GetClientRect(window,&area));
        CHECK(area.right==MulDiv(560,dpi,96));CHECK(area.bottom==MulDiv(248,dpi,96));
        auto dc=GetDC(window),memory=CreateCompatibleDC(dc);
        auto bitmap=CreateCompatibleBitmap(dc,area.right,area.bottom);CHECK(bitmap && memory);
        auto old=SelectObject(memory,bitmap);
        for(bool russian:{false,true}) for(bool recover:{false,true}) {
            state.russian=russian;state.recover=recover;
            SendMessageW(window,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT);
            CHECK(GetPixel(memory,0,0)==RGB(28,33,34));
            auto old_font=SelectObject(memory,state.heading);
            RECT text_area{0,0,state.px(496),0};
            auto title=recover?(russian?L"Восстанавливаем клиент":L"Restoring your client"):
                (russian?L"Обновляем клиент":L"Updating your client");
            CHECK(DrawTextW(memory,title,-1,&text_area,DT_CALCRECT|DT_WORDBREAK)>0);
            CHECK(text_area.bottom<=state.px(36));SelectObject(memory,old_font);
            if(argc==2 && dpi==120 && russian && !recover) {
                SelectObject(memory,old);
                BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=area.right;
                info.bmiHeader.biHeight=-area.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
                std::vector<char> pixels(size_t(area.right)*area.bottom*4);
                CHECK(GetDIBits(memory,bitmap,0,area.bottom,pixels.data(),&info,DIB_RGB_COLORS)==area.bottom);
                BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info.bmiHeader);
                header.bfSize=header.bfOffBits+DWORD(pixels.size());
                std::ofstream output(argv[1],std::ios::binary);output.write(reinterpret_cast<char*>(&header),sizeof(header));
                output.write(reinterpret_cast<char*>(&info.bmiHeader),sizeof(info.bmiHeader));output.write(pixels.data(),pixels.size());CHECK(output.good());
                SelectObject(memory,bitmap);
            }
        }
        SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(window,dc);
    }
    SendMessageW(window,WM_CLOSE,0,0);CHECK(IsWindow(window));CHECK(DestroyWindow(window));
    std::puts("updater window: 100/125/150/200% DPI, RU/EN, restore, repaint and close guard passed");
}
