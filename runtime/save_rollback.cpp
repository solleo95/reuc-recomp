#include "save_rollback.h"
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace uc {
namespace {
std::string parent(const std::string& path) { const auto at=path.rfind('/'); return at ? path.substr(0,at) : "/"; }
bool beneath(const std::string& child,const std::string& path) { return path=="/" || child.starts_with(path+"/"); }
}
int SaveFs::Snapshot::key(const std::string& path,std::string& result) {
    if(path.empty() || path[0]!='/' || path.size()>63 || path.starts_with("//")) return -101;
    result=path;
    for(auto& ch:result) if(ch>='A' && ch<='Z') ch+=char('a'-'A');
    for(size_t at=1;at<result.size();) {
        const auto end=result.find('/',at);
        const auto part=result.substr(at,end==std::string::npos ? end : end-at);
        if(part.empty() || part=="." || part==".." || part.back()=='.' || part.back()==' ') return -101;
        const auto stem=part.substr(0,part.find('.'));
        if(stem=="con" || stem=="prn" || stem=="aux" || stem=="nul" ||
           (stem.size()==4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3]>='0' && stem[3]<='9')) return -101;
        for(unsigned char ch:part) if(ch<32 || ch>=127 || std::string_view("\\:*?\"<>|").find(char(ch))!=std::string_view::npos) return -101;
        if(at==1 && (part==".backups" || part==".deleted")) return -102;
        if(end==std::string::npos) break;
        at=end+1;
    }
    if(result.size()>1 && result.back()=='/') result.pop_back();
    return 0;
}
bool SaveFs::Snapshot::busy(const std::string& path) const {
    return std::any_of(handles.begin(),handles.end(),[&](const auto& item) { return item.second.path==path || beneath(item.second.path,path); });
}
int SaveFs::Snapshot::create(const std::string& path,bool directory) {
    std::string name; if(auto error=key(path,name)) return error;
    if(nodes.contains(name)) return -105;
    if(nodes.size()>=4096) return -108;
    const auto p=nodes.find(parent(name)); if(p==nodes.end()) return -106;
    if(p->second.bytes) return -102;
    nodes.emplace(name,Node{std::filesystem::path(path).filename().string(),directory ? nullptr : std::make_shared<const std::vector<uint8_t>>()});
    return 0;
}
int SaveFs::Snapshot::open(const std::string& path,uint32_t mode) {
    if(mode<1 || mode>3 || next_fd==INT_MAX) return -101;
    std::string name; if(auto error=key(path,name)) return error;
    const auto node=nodes.find(name); if(node==nodes.end()) return -106;
    if(!node->second.bytes) return -102;
    for(const auto& [fd,opened]:handles) if(opened.path==name && ((mode|opened.mode)&2)) return -102;
    const int fd=next_fd++; handles.emplace(fd,Open{name,mode,0}); return fd;
}
int SaveFs::Snapshot::close(int fd) { return handles.erase(fd) ? 0 : -101; }
int SaveFs::Snapshot::read(int fd,std::span<uint8_t> out) {
    const auto found=handles.find(fd); if(found==handles.end() || out.size()>INT_MAX) return -101;
    auto& file=found->second; if(!(file.mode&1)) return -102;
    const auto& bytes=*nodes.at(file.path).bytes;
    const auto count=std::min(out.size(),bytes.size()-std::min<size_t>(file.position,bytes.size()));
    if(count) std::copy_n(bytes.data()+file.position,count,out.data());
    file.position+=uint32_t(count); return int(count);
}
int SaveFs::Snapshot::write(int fd,std::span<const uint8_t> input) {
    const auto found=handles.find(fd); if(found==handles.end() || input.size()>INT_MAX) return -101;
    auto& file=found->second; if(!(file.mode&2)) return -102;
    if(uint64_t(file.position)+input.size()>64u*1024*1024) return -108;
    auto& node=nodes.at(file.path);
    uint64_t total=0;
    for(const auto& [path,entry]:nodes) if(entry.bytes) total+=entry.bytes->size();
    if(total-node.bytes->size()+std::max<uint64_t>(node.bytes->size(),uint64_t(file.position)+input.size())>64u*1024*1024) return -108;
    auto bytes=std::make_shared<std::vector<uint8_t>>(*node.bytes);
    bytes->resize(std::max(bytes->size(),file.position+input.size()));
    if(!input.empty()) std::copy(input.begin(),input.end(),bytes->data()+file.position);
    file.position+=uint32_t(input.size()); node.bytes=std::move(bytes); return int(input.size());
}
int SaveFs::Snapshot::seek(int fd,int32_t offset,int32_t whence) {
    const auto found=handles.find(fd); if(found==handles.end() || whence<0 || whence>2) return -101;
    auto& file=found->second;
    const int64_t base=whence==0 ? 0 : whence==1 ? file.position : nodes.at(file.path).bytes->size();
    if(base+offset<0 || base+offset>INT_MAX) return -101;
    file.position=uint32_t(base+offset); return int(file.position);
}
SaveFs::Stats SaveFs::Snapshot::stats(int fd) const {
    const auto found=handles.find(fd); if(found==handles.end()) return {-101};
    return {0,uint32_t(nodes.at(found->second.path).bytes->size()),found->second.position};
}
int SaveFs::Snapshot::attributes(const std::string& path,bool& directory) const {
    std::string name; if(auto error=key(path,name)) return error;
    const auto node=nodes.find(name); if(node==nodes.end()) return -106;
    directory=!node->second.bytes; return 0;
}
SaveFs::Directory SaveFs::Snapshot::read_dir(const std::string& path) const {
    std::string name; if(auto error=key(path,name)) return {error};
    bool directory=false; if(auto error=attributes(path,directory)) return {error};
    if(!directory) return {-101};
    Directory result{0};
    for(const auto& [key,node]:nodes) if(key!="/" && parent(key)==name) result.names.push_back(node.name);
    std::sort(result.names.begin(),result.names.end()); return result;
}
SaveFs::Usage SaveFs::Snapshot::usage(const std::string& path) const {
    std::string name; if(auto error=key(path,name)) return {error};
    bool directory=false; if(auto error=attributes(path,directory)) return {error};
    Usage result{0};
    for(const auto& [key,node]:nodes) if((directory && key!=name && beneath(key,name)) || (!directory && key==name)) {
        ++result.entries; if(node.bytes) result.blocks+=uint32_t((node.bytes->size()+16383)/16384);
    }
    return result;
}
int SaveFs::Snapshot::remove(const std::string& path) {
    std::string name; if(auto error=key(path,name)) return error;
    if(name=="/") return -102;
    const auto node=nodes.find(name); if(node==nodes.end()) return -106;
    if(!node->second.bytes && !read_dir(path).names.empty()) return -115;
    if(busy(name)) return -102;
    nodes.erase(node); return 0;
}
int SaveFs::Snapshot::rename(const std::string& from,const std::string& to) {
    std::string source,target;
    if(auto error=key(from,source)) return error; if(auto error=key(to,target)) return error;
    if(source=="/" || target=="/") return -102;
    const auto node=nodes.find(source); if(node==nodes.end()) return -106;
    if(source==target) return 0;
    if(beneath(target,source)) return -101;
    const auto p=nodes.find(parent(target)); if(p==nodes.end()) return -106; if(p->second.bytes) return -102;
    if(busy(source) || busy(target)) return -102;
    if(auto dest=nodes.find(target);dest!=nodes.end() && (!dest->second.bytes || !node->second.bytes)) return -102;
    std::map<std::string,Node> moved;
    for(auto it=nodes.begin();it!=nodes.end();) {
        if(it->first==source || beneath(it->first,source)) {
            auto value=it->second;
            if(it->first==source) value.name=std::filesystem::path(to).filename().string();
            moved.emplace(target+it->first.substr(source.size()),std::move(value)); it=nodes.erase(it);
        } else ++it;
    }
    for(auto& [path,value]:moved) nodes.insert_or_assign(path,std::move(value));
    return 0;
}
SaveFs::Snapshot& SaveFs::writable() {
    if(rollback_.use_count()!=1) rollback_=std::make_shared<Snapshot>(*rollback_);
    rollback_->revision=++snapshot_revision_; return *rollback_;
}
void SaveFs::begin_rollback() {
    if(rollback_ || !files_.empty()) throw std::logic_error("save rollback must start before opening guest files");
    if(initialize()) throw std::runtime_error("cannot initialize rollback saves");
    auto state=std::make_shared<Snapshot>(); state->owner=this; state->nodes.emplace("/",Snapshot::Node{"",nullptr});
    uint64_t total=0;
    const std::function<void(const std::string&)> load=[&](const std::string& path) {
        const auto listing=read_dir(path); if(listing.result) throw std::runtime_error("cannot enumerate rollback saves");
        for(const auto& name:listing.names) {
            const auto child=(path=="/" ? path : path+"/")+name;
            bool directory=false; if(attributes(child,directory)) throw std::runtime_error("cannot inspect rollback save");
            std::string key; if(Snapshot::key(child,key)) throw std::runtime_error("invalid rollback save path");
            if(state->nodes.size()>=4096) throw std::runtime_error("rollback save tree exceeds entry budget");
            if(directory) { state->nodes.emplace(key,Snapshot::Node{name,nullptr}); load(child); }
            else {
                const int fd=open(child,1); if(fd<0) throw std::runtime_error("cannot open rollback save");
                const auto info=stats(fd);
                if(info.result || total+info.length>64u*1024*1024) { close(fd); throw std::runtime_error("rollback saves exceed memory budget"); }
                auto bytes=std::make_shared<std::vector<uint8_t>>(info.length);
                const int count=read(fd,*bytes),closed=close(fd);
                if(count!=int(info.length) || closed) throw std::runtime_error("cannot read rollback save");
                total+=bytes->size(); state->nodes.emplace(key,Snapshot::Node{name,std::move(bytes)});
            }
        }
    };
    load("/"); state->next_fd=next_fd_; rollback_=state; committed_=std::move(state);
}
std::shared_ptr<const SaveFs::Snapshot> SaveFs::checkpoint() const {
    if(!rollback_) throw std::logic_error("save rollback is not enabled");
    return rollback_;
}
void SaveFs::restore(const Snapshot& saved) {
    if(!rollback_ || saved.owner!=this || saved.revision<committed_->revision)
        throw std::logic_error("invalid save rollback checkpoint");
    rollback_=std::make_shared<Snapshot>(saved);
}
}
