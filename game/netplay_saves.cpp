#include "netplay_saves.h"
#include "savefs.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <stdexcept>
namespace uc {
namespace fs=std::filesystem;
namespace {
constexpr size_t max_entries=256,max_bytes=16u<<20;
void require(bool ok) {if(!ok)throw std::runtime_error("Invalid or inaccessible network save snapshot");}
struct Entry {std::string path;bool directory;std::vector<uint8_t> data;};
void collect(SaveFs& saves,const std::string& parent,std::vector<Entry>& entries,size_t& total) {
    const auto listing=saves.read_dir(parent);require(listing.result==0);
    for(const auto& name:listing.names) {
        const auto path=(parent=="/" ? parent : parent+"/")+name;
        bool directory=false;require(saves.attributes(path,directory)==0 && entries.size()<max_entries);
        entries.push_back({path.substr(1),directory,{}});
        if(directory)collect(saves,path,entries,total);
        else {
            const int fd=saves.open(path,1);require(fd>=0);
            const auto info=saves.stats(fd);require(info.result==0 && info.length<=max_bytes-total);
            total+=info.length;auto& data=entries.back().data;data.resize(info.length);
            require(saves.read(fd,data)==int(data.size()));require(saves.close(fd)==0);
        }
    }
}
}
struct NetSaves::Impl {
    bool host;
    fs::path path;
    fs::path parent;
    std::vector<Entry> entries;
    Impl(bool is_host,const fs::path& local):host(is_host),path(fs::absolute(local).lexically_normal()) {
        if(host)return;
        parent=path.parent_path();
        // An exclusively created sibling never aliases the client's personal saves.
        SaveFs check(parent);bool directory=false;require(check.attributes("/",directory)==0 && directory);
        static std::atomic<unsigned> sequence{0};
        for(unsigned attempt=0;attempt<100;++attempt) {
            const auto candidate=parent/(".netplay-session-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+"-"+std::to_string(sequence.fetch_add(1)));
            if(CreateDirectoryW(candidate.c_str(),nullptr)) {path=candidate;return;}
            if(GetLastError()!=ERROR_ALREADY_EXISTS)throw std::runtime_error("Cannot create temporary network saves");
        }
        throw std::runtime_error("Cannot allocate temporary network saves");
    }
    ~Impl() {
        if(host)return;
        // Refuse substituted roots/ancestors; remove only this session's exclusively created directory.
        SaveFs check(path);bool directory=false;
        if(path.parent_path()!=parent || check.attributes("/",directory)!=0 || !directory) {
            std::fprintf(stderr,"[netplay] temporary saves cleanup refused: %s\n",path.string().c_str());return;
        }
        std::error_code error;fs::remove_all(path,error);
        if(error)std::fprintf(stderr,"[netplay] temporary saves cleanup failed: %s\n",error.message().c_str());
    }
    void transfer(net::Socket& socket) {
        SaveFs saves(path);
        if(host) {
            require(saves.initialize()==0);size_t total=0;entries.clear();collect(saves,"/",entries,total);
            for(const auto& entry:entries) {
                require(!entry.path.empty() && entry.path.size()<63);
                std::vector<uint8_t> header{uint8_t(entry.directory ? 1 : 2),uint8_t(entry.path.size())};
                header.insert(header.end(),entry.path.begin(),entry.path.end());
                for(int shift=24;shift>=0;shift-=8)header.push_back(uint8_t(entry.data.size()>>shift));
                socket.send(header);
                for(size_t offset=0;offset<entry.data.size();offset+=net::max_packet)
                    socket.send(std::span(entry.data).subspan(offset,std::min(net::max_packet,entry.data.size()-offset)));
            }
            socket.send(std::array<uint8_t,1>{0});entries.clear();return;
        }
        size_t total=0,count=0;
        for(;;) {
            const auto header=socket.receive();
            if(header==std::vector<uint8_t>{0})break;
            require(header.size()>=6 && (header[0]==1 || header[0]==2) && ++count<=max_entries);
            const size_t length=header[1];require(length>0 && length<63 && header.size()==length+6);
            const std::string name(header.begin()+2,header.begin()+2+length);
            require(name.front()!='/' && name.back()!='/');
            uint32_t size=0;for(size_t i=length+2;i<header.size();++i)size=(size<<8)|header[i];
            require(size<=max_bytes-total);total+=size;
            const auto guest="/"+name;
            if(header[0]==1) {require(size==0 && saves.create_dir(guest)==0);continue;}
            require(saves.create_file(guest)==0);
            const int fd=saves.open(guest,2);require(fd>=0);
            size_t remaining=size;
            while(remaining) {
                const auto data=socket.receive();require(!data.empty() && data.size()<=remaining);
                require(saves.write(fd,data)==int(data.size()));remaining-=data.size();
            }
            require(saves.close(fd)==0);
        }
        std::fprintf(stderr,"[netplay] imported saves: %zu entries, %zu bytes\n",count,total);
    }
};
NetSaves::NetSaves(bool host,const fs::path& local):impl_(std::make_unique<Impl>(host,local)) {}
NetSaves::~NetSaves()=default;
const fs::path& NetSaves::root() const {return impl_->path;}
void NetSaves::transfer(net::Socket& socket) {impl_->transfer(socket);}
}
