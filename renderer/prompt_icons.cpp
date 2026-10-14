#include "prompt_icons.h"
#include "imgui.h"
#include <algorithm>
namespace uc {
void draw_mouse_icon(ImDrawList& list,ImVec2 position,ImVec2 size,PhysicalInput input,bool both_directions,PromptStyle style) {
    const bool sculpted=style==PromptStyle::sculpted;
    const ImU32 body=sculpted ? IM_COL32(45,49,55,255) : IM_COL32(88,88,88,255);
    const ImU32 active=sculpted ? IM_COL32(135,240,245,255) : IM_COL32_WHITE;
    const ImU32 seam=IM_COL32(20,22,25,255);
    const float stroke=size.x*(sculpted ? .036f : .06f);
    auto point=[&](float x,float y) {return ImVec2{position.x+size.x*x,position.y+size.y*y};};
    auto close_path=[&] {
        auto& path=list._Path;
        if(path.Size>1 && path[0].x==path.back().x && path[0].y==path.back().y)path.pop_back();
    };
    auto fill=[&](ImU32 color) {
        close_path();auto& path=list._Path;
        float area=0;for(int i=0;i<path.Size;++i) {const auto a=path[i],b=path[(i+1)%path.Size];area+=a.x*b.y-b.x*a.y;}
        if(area<0)std::reverse(path.begin(),path.end());
        list.PathFillConcave(color);
    };
    auto body_path=[&] {
        list.PathLineTo(point(.5f,.03f));
        list.PathBezierCubicCurveTo(point(.17f,.03f),point(.03f,.10f),point(.03f,.33f));
        if(sculpted) {
            list.PathBezierCubicCurveTo(point(.03f,.48f),point(.10f,.52f),point(.04f,.73f));
            list.PathBezierCubicCurveTo(point(-.01f,.90f),point(.18f,.97f),point(.5f,.97f));
            list.PathBezierCubicCurveTo(point(.82f,.97f),point(1.01f,.90f),point(.96f,.73f));
            list.PathBezierCubicCurveTo(point(.90f,.52f),point(.97f,.48f),point(.97f,.33f));
        } else {
            list.PathBezierCubicCurveTo(point(.03f,.73f),point(.04f,.97f),point(.5f,.97f));
            list.PathBezierCubicCurveTo(point(.96f,.97f),point(.97f,.73f),point(.97f,.33f));
        }
        list.PathBezierCubicCurveTo(point(.97f,.10f),point(.83f,.03f),point(.5f,.03f));
    };
    body_path();fill(body);
    if(sculpted) {
        for(bool right:{false,true}) {
            auto p=[&](float x,float y){return point(right ? 1-x : x,y);};
            const ImVec2 facet[]={p(.08f,.45f),p(.20f,.53f),p(.24f,.90f),p(.09f,.80f)};
            list.AddConvexPolyFilled(facet,4,IM_COL32(58,62,68,255));
        }
    }
    // One panel path mirrored for both buttons: fill reaches the identical inner boundary.
    for(bool right:{false,true}) {
        auto p=[&](float x,float y){return point(right ? 1-x : x,y);};
        auto curve=[&](float x1,float y1,float x2,float y2,float x3,float y3) {list.PathBezierCubicCurveTo(p(x1,y1),p(x2,y2),p(x3,y3));};
        auto panel=[&] {
            list.PathLineTo(p(.46f,.065f));
            curve(.26f,.065f,.11f,.11f,.085f,.24f);
            list.PathLineTo(p(.078f,.33f));
            curve(.072f,.41f,.17f,.435f,.40f,.435f);
            curve(.44f,.435f,.46f,.42f,.46f,.395f);
            list.PathLineTo(p(.46f,.345f));
            curve(.415f,.34f,.405f,.30f,.405f,.26f);
            curve(.405f,.215f,.41f,.18f,.46f,.17f);
            list.PathLineTo(p(.46f,.065f));
        };
        const bool selected=input.kind==PhysicalKind::mouse && input.code==(right ? 3 : 1);
        panel();fill(selected ? active : body);
        panel();close_path();list.PathStroke(sculpted ? IM_COL32(85,90,96,255) : seam,ImDrawFlags_Closed,stroke*.65f);
    }
    const bool wheel=input.kind==PhysicalKind::wheel || (input.kind==PhysicalKind::mouse && input.code==2);
    list.AddRectFilled(point(.435f,.175f),point(.565f,.33f),seam,size.x*.06f);
    list.AddRectFilled(point(.455f,.19f),point(.545f,.315f),wheel ? active : IM_COL32(75,78,82,255),size.x*.035f);
    list.AddRect(point(.435f,.175f),point(.565f,.33f),IM_COL32_WHITE,size.x*.06f,stroke*.45f,0);
    if(sculpted)for(float y:{.22f,.25f,.28f})list.AddLine(point(.463f,y),point(.537f,y),IM_COL32(45,49,55,255),stroke*.5f);
    if(input.kind==PhysicalKind::mouse && (input.code==4 || input.code==5)) {
        const float y=input.code==4 ? .53f : .69f;
        list.AddRectFilled(point(.10f,y),point(.18f,y+.11f),active,size.x*.02f);
    }
    body_path();close_path();list.PathStroke(IM_COL32_WHITE,ImDrawFlags_Closed,stroke);
    if(input.kind==PhysicalKind::wheel)for(int up:{1,0}) {
        if(!both_directions && up!=(input.code==1))continue;
        const float tip=up ? -.08f : 1.08f,base=up ? .005f : .995f;
        list.AddTriangleFilled(point(.5f,tip),point(.36f,base),point(.64f,base),active);
    }
}
void draw_keycap(ImDrawList& list,ImVec2 position,ImVec2 size,PromptStyle style) {
    const bool sculpted=style==PromptStyle::sculpted;
    const float unit=std::min(size.x,size.y),rounding=unit*.17f;
    const float border=unit*(sculpted ? .035f : .075f),gap=unit*.035f;
    auto inset=[&](float value){return ImVec2{position.x+value,position.y+value};};
    auto end=[&](float value){return ImVec2{position.x+size.x-value,position.y+size.y-value};};
    list.AddRectFilled(position,end(0),IM_COL32_WHITE,rounding);
    list.AddRectFilled(inset(border),end(border),IM_COL32(20,22,25,255),std::max(0.f,rounding-border));
    list.AddRectFilled(inset(border+gap),end(border+gap),sculpted ? IM_COL32(42,46,52,255) : IM_COL32(88,88,88,255),std::max(0.f,rounding-border-gap));
    if(sculpted) {
        const float bevel=unit*.09f,pad=border+gap;
        const ImVec2 top[]={{position.x+pad,position.y+pad},{position.x+size.x-pad,position.y+pad},{position.x+size.x-pad-bevel,position.y+pad+bevel},{position.x+pad+bevel,position.y+pad+bevel}};
        list.AddConvexPolyFilled(top,4,IM_COL32(70,75,82,255));
    }
}
}
