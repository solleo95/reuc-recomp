#include "motion_tracker.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace gx {
namespace {
using Matrix=std::array<std::array<float,4>,4>;
Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix r{};
    for(size_t i=0;i<4;++i) for(size_t j=0;j<4;++j) for(size_t k=0;k<4;++k) r[i][j]+=a[i][k]*b[k][j];
    return r;
}
Matrix model_view(const std::array<std::array<float,4>,3>& rows) { return {rows[0],rows[1],rows[2],std::array<float,4>{0,0,0,1}}; }
std::optional<Matrix> inverse(Matrix m) {   // Gauss-Jordan with partial pivoting
    Matrix r{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    for(size_t c=0;c<4;++c) {
        size_t pivot=c;
        for(size_t i=c+1;i<4;++i) if(std::abs(m[i][c])>std::abs(m[pivot][c])) pivot=i;
        if(std::abs(m[pivot][c])<1e-12f) return std::nullopt;
        std::swap(m[c],m[pivot]); std::swap(r[c],r[pivot]);
        const float scale=1/m[c][c];
        for(size_t j=0;j<4;++j) { m[c][j]*=scale; r[c][j]*=scale; }
        for(size_t i=0;i<4;++i) if(i!=c) {
            const float f=m[i][c];
            for(size_t j=0;j<4;++j) { m[i][j]-=f*m[c][j]; r[i][j]-=f*r[c][j]; }
        }
    }
    return r;
}
// Screen position (NDC) of the matrix origin, none when behind the camera.
std::optional<std::array<float,2>> origin_ndc(const Projection& p, const std::array<std::array<float,4>,3>& rows) {
    const std::array<float,4> view{rows[0][3],rows[1][3],rows[2][3],1};
    std::array<float,4> clip{};
    for(size_t r=0;r<4;++r) for(size_t c=0;c<4;++c) clip[r]+=p[r][c]*view[c];
    if(clip[3]<=1e-6f) return std::nullopt;
    return std::array<float,2>{clip[0]/clip[3],clip[1]/clip[3]};
}
}
MotionTracker::MotionTracker(std::pmr::memory_resource* upstream)
    : pool_(upstream), previous_(&pool_), current_(&pool_), occurrences_(&pool_), instances_(&pool_), spans_(&pool_) {}
void MotionTracker::begin_frame() {
    current_.clear(); occurrences_.clear(); journal_.clear();
    for(auto& instance:instances_) instance.claimed=false;   // a frame begun again without end_frame starts its claims over
    for(auto& [identity,span]:spans_) span.cursor=0;
}
// shortcut: a linear scan per draw, quadratic in the instances of one identity (about 20 seats per mesh in the train car); a spatial index if a group reaches thousands.
uint32_t MotionTracker::claim(Span& span, const std::array<float,3>& origin) {
    std::array<float,3> predicted{};   // where the camera's last motion puts this view-space point in the previous view
    for(size_t r=0;r<3;++r) predicted[r]=view_to_prev_[r][0]*origin[0]+view_to_prev_[r][1]*origin[1]+view_to_prev_[r][2]*origin[2]+view_to_prev_[r][3];
    uint32_t best=span.count; float best_distance=0;
    for(uint32_t i=span.cursor;i<span.count;++i) {
        const auto& instance=instances_[span.first+i];
        if(instance.claimed) continue;
        float distance=0;
        for(size_t r=0;r<3;++r) distance+=(instance.origin[r]-predicted[r])*(instance.origin[r]-predicted[r]);
        if(best==span.count || distance<best_distance) { best=i; best_distance=distance; }   // ties keep the earliest: identical matrices pair in order
        if(distance==0) break;
    }
    instances_[span.first+best].claimed=true;
    while(span.cursor<span.count && instances_[span.first+span.cursor].claimed) ++span.cursor;
    return instances_[span.first+best].key.occurrence;
}
DrawKey MotionTracker::key(const DrawPacket& draw) {
    DrawKey k{draw.callsite,draw.mesh.get(),uint32_t(draw.vertex_view().size()),0};
    const auto span=spans_.find(k);
    if(span!=spans_.end() && span->second.cursor<span->second.count) {
        const auto row=std::min<uint32_t>(draw.transforms->current,61);
        const auto& rows=draw.transforms->position_rows;
        k.occurrence=claim(span->second,{rows[row][3],rows[row+1][3],rows[row+2][3]});
    } else k.occurrence=(span!=spans_.end() ? span->second.next : 0)+occurrences_[k]++;
    journal_.push_back(k);
    return k;
}
void MotionTracker::rollback(size_t mark) {
    while(journal_.size()>mark) {
        const auto& k=journal_.back();
        current_.erase(k);
        auto identity=k; identity.occurrence=0;
        const auto span=spans_.find(identity);
        if(span!=spans_.end() && k.occurrence<span->second.next) {   // a claimed previous instance: free it
            auto& s=span->second;
            const auto first=instances_.begin()+s.first;
            const auto found=std::lower_bound(first,first+s.count,k,[](const Instance& instance,const DrawKey& key) { return instance.key<key; });
            found->claimed=false;
            s.cursor=std::min(s.cursor,uint32_t(found-first));
        } else --occurrences_[identity];
        journal_.pop_back();
    }
}
const PreviousDraw* MotionTracker::previous(const DrawKey& key) const {
    const auto found=previous_.find(key);
    return found==previous_.end() ? nullptr : &found->second.draw;
}
void MotionTracker::record(const DrawKey& key, std::shared_ptr<const Transforms> transforms, bool z_write) {
    Entry entry;
    const auto row=std::min<uint32_t>(transforms->current,61);
    entry.rows={transforms->position_rows[row],transforms->position_rows[row+1],transforms->position_rows[row+2]};
    entry.draw.transforms=std::move(transforms);
    entry.z_write=z_write; entry.mesh=key.mesh!=nullptr;
    current_[key]=std::move(entry);
}
void MotionTracker::end_frame() {
    moves_.clear();
    bool fov_jump=false;
    const Entry* largest=nullptr; const Entry* largest_previous=nullptr; uint32_t largest_vertices=0;
    DrawKey largest_key;
    for(const auto& [key,now]:current_) {
        const auto before=previous_.find(key);
        if(before==previous_.end()) continue;
        const auto& p0=before->second.draw.transforms->projection; const auto& p1=now.draw.transforms->projection;
        for(size_t i:{0,1}) if(p0[i][i]!=0 && std::abs(p1[i][i]-p0[i][i])>kProjectionCutRelative*std::abs(p0[i][i])) fov_jump=true;
        if(!now.mesh) continue;
        const auto a=origin_ndc(p0,before->second.rows), b=origin_ndc(p1,now.rows);
        if(a && b) moves_.push_back(std::hypot((*b)[0]-(*a)[0],(*b)[1]-(*a)[1]));
        if(now.z_write && key.vertices!=0 && (key.vertices>largest_vertices || (key.vertices==largest_vertices && key<largest_key))) {
            largest_vertices=key.vertices; largest_key=key; largest=&now; largest_previous=&before->second;
        }
    }
    float median=0;
    if(!moves_.empty()) { std::nth_element(moves_.begin(),moves_.begin()+moves_.size()/2,moves_.end()); median=moves_[moves_.size()/2]; }
    cut_=fov_jump || median>kCameraCutNdc;
    clip_to_prev_={{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    view_to_prev_={{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
    if(largest && !cut_) {
        // the same level geometry's previous view from its current view: MV_prev * inverse(MV)
        if(const auto inv=inverse(model_view(largest->rows))) {
            const auto motion=multiply(model_view(largest_previous->rows),*inv);
            view_to_prev_={motion[0],motion[1],motion[2]};
        }
        // the level geometry's previous clip from its current clip: P_prev * MV_prev * inverse(P * MV)
        if(const auto inv=inverse(multiply(largest->draw.transforms->projection,model_view(largest->rows))))
            clip_to_prev_=multiply(multiply(largest_previous->draw.transforms->projection,model_view(largest_previous->rows)),*inv);
    }
    previous_.swap(current_);
    current_.clear(); journal_.clear();
    instances_.clear(); spans_.clear();
    for(const auto& [key,entry]:previous_) instances_.push_back({key,{entry.rows[0][3],entry.rows[1][3],entry.rows[2][3]},false});
    std::sort(instances_.begin(),instances_.end(),[](const Instance& a,const Instance& b) { return a.key<b.key; });
    for(uint32_t i=0;i<instances_.size();++i) {
        auto identity=instances_[i].key; identity.occurrence=0;
        auto& span=spans_.try_emplace(identity,Span{i,0,0,0}).first->second;
        ++span.count; span.next=instances_[i].key.occurrence+1;
    }
}
}
