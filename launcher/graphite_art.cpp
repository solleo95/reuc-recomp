#include "graphite.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>

// The launcher's hand-drawn artwork (no platform code): the umbrella and the navigation icons. Shared by the Windows and the Steam Deck launcher.
namespace uc::launcher {
void draw_umbrella(ImDrawList& draw, ImVec2 center, float radius) {
    if (!std::isfinite(radius) || radius <= 0 ||
        !std::isfinite(center.x) || !std::isfinite(center.y)) return;
    constexpr float step = 0.7853981633974483f;
    for (int i = 0; i < 8; ++i) {
        const float a = float(i) * step;
        const float b = float(i + 1) * step;
        draw.AddTriangleFilled(center,
            ImVec2(center.x + std::cos(a) * radius, center.y + std::sin(a) * radius),
            ImVec2(center.x + std::cos(b) * radius, center.y + std::sin(b) * radius),
            i % 2 == 0 ? IM_COL32(238, 233, 220, 255) : IM_COL32(157, 21, 34, 255));
    }
}

void draw_navigation_icon(ImDrawList& draw,int icon,ImVec2 at,float size,unsigned color) {
    const float s=size/30.f,stroke=std::max(1.f,2*s);
    auto p=[&](float x,float y){return ImVec2{at.x+x*s,at.y+y*s};};
    auto line=[&](float x,float y,float a,float b){draw.AddLine(p(x,y),p(a,b),color,stroke);};
    if(icon==0) {
        // gamepad: wide body with two rounded grips, d-pad on the left, two buttons on the right
        draw.PathLineTo(p(9,7));draw.PathLineTo(p(21,7));
        draw.PathBezierCubicCurveTo(p(26,7),p(29,11),p(28.5,19));
        draw.PathBezierCubicCurveTo(p(28.2,22.5),p(25.5,24),p(23.5,21.5));
        draw.PathLineTo(p(21.5,18.5));
        draw.PathBezierCubicCurveTo(p(20.8,17.6),p(20,17.5),p(19,17.5));draw.PathLineTo(p(11,17.5));
        draw.PathBezierCubicCurveTo(p(10,17.5),p(9.2,17.6),p(8.5,18.5));
        draw.PathLineTo(p(6.5,21.5));
        draw.PathBezierCubicCurveTo(p(4.5,24),p(1.8,22.5),p(1.5,19));
        draw.PathBezierCubicCurveTo(p(1,11),p(4,7),p(9,7));draw.PathStroke(color,ImDrawFlags_Closed,stroke);
        line(6.5,12,11.5,12);line(9,9.5,9,14.5);
        draw.AddCircleFilled(p(20.5,10.5),1.4f*s,color);draw.AddCircleFilled(p(23,13.5),1.4f*s,color);
    } else if(icon==1) {
        draw.AddRect(p(2,3),p(28,22),color,1*s,0,stroke);line(15,22,15,27);line(8,28,22,28);
    } else if(icon==2) {
        draw.AddCircle(p(15,15),9*s,color,24,stroke);draw.AddCircle(p(15,15),3*s,color,16,stroke);
        for(int i=0;i<8;++i) {const float angle=i*0.7853981634f;line(15+9*std::cos(angle),15+9*std::sin(angle),15+13*std::cos(angle),15+13*std::sin(angle));}
    } else if(icon==4) {
        // controllers: a pad seen from the front with two thumbsticks and its shoulder buttons
        draw.AddRect(p(2,9),p(28,23),color,7*s,0,stroke);
        draw.AddCircle(p(10,16),3.2f*s,color,16,stroke);draw.AddCircle(p(20,16),3.2f*s,color,16,stroke);
        line(5,6,10,6);line(20,6,25,6);
    } else if(icon==5) {
        // mods: a box seen from above, with the lid's edges meeting in the middle
        ImVec2 box[]={p(15,3),p(26,9),p(26,21),p(15,27),p(4,21),p(4,9)};
        draw.AddPolyline(box,6,color,ImDrawFlags_Closed,stroke);
        line(4,9,15,15);line(26,9,15,15);line(15,15,15,27);
    } else {
        ImVec2 speaker[]={p(2,11),p(8,11),p(15,5),p(15,25),p(8,19),p(2,19)};
        draw.AddPolyline(speaker,6,color,ImDrawFlags_Closed,stroke);
        draw.PathArcTo(p(14,15),8*s,-.8f,.8f,12);draw.PathStroke(color,0,stroke);
        draw.PathArcTo(p(14,15),14*s,-.8f,.8f,16);draw.PathStroke(color,0,stroke);
    }
}
}
