#pragma once
#include "netplay.h"
#include <filesystem>
#include <memory>
namespace uc {
// Client storage lives longer than SaveFs and is removed after its handles close.
class NetSaves {
public:
    NetSaves(bool host,const std::filesystem::path& local_root);
    ~NetSaves();
    const std::filesystem::path& root() const;
    void transfer(net::Socket&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
