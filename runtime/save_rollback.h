#pragma once
#include "savefs.h"

namespace uc {
struct SaveFs::Snapshot {
    struct Node { std::string name; std::shared_ptr<const std::vector<uint8_t>> bytes; }; // Null bytes means directory.
    struct Open { std::string path; uint32_t mode,position; };
    const SaveFs* owner;
    uint64_t revision=0;
    std::map<std::string,Node> nodes;
    std::map<int,Open> handles;
    int next_fd=100;
    static int key(const std::string&,std::string&);
    int create(const std::string&,bool directory);
    int open(const std::string&,uint32_t mode);
    int close(int fd);
    int read(int fd,std::span<uint8_t>);
    int write(int fd,std::span<const uint8_t>);
    int seek(int fd,int32_t offset,int32_t whence);
    int rename(const std::string&,const std::string&);
    int remove(const std::string&);
    int attributes(const std::string&,bool&) const;
    Stats stats(int fd) const;
    Directory read_dir(const std::string&) const;
    Usage usage(const std::string&) const;
    bool busy(const std::string&) const;
};
}
