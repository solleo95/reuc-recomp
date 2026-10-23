#pragma once
// Builds small U8 archives by hand (independent of U8Archive) for the archive tests.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace u8_fixture {

struct File {
    std::string path;   // '/' separated; folders are made as needed, in the order the files first mention them
    std::vector<uint8_t> data;
};

inline std::vector<uint8_t> bytes(const std::string& text) { return std::vector<uint8_t>(text.begin(), text.end()); }

inline void put32(std::vector<uint8_t>& v, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) v[at + size_t(i)] = uint8_t(value >> (24 - 8 * i));
}

struct Item {
    bool dir = false;
    std::string name;
    uint32_t parent = 0, end = 0, name_off = 0;
    const std::vector<uint8_t>* data = nullptr;
};

// reverse_data: members are stored in the opposite order of their nodes (the data area order is independent of the node order).
inline std::vector<uint8_t> make_u8(const std::vector<File>& files, bool reverse_data = false) {
    std::vector<Item> items{{true, "", 0, 0, 0, nullptr}};
    for (const File& file : files) {
        uint32_t folder = 0;
        size_t start = 0;
        while (true) {
            const size_t slash = file.path.find('/', start);
            const std::string part = file.path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (slash == std::string::npos) {
                items.push_back({false, part, folder, 0, 0, &file.data});
                break;
            }
            uint32_t found = 0;
            for (uint32_t i = folder + 1; i < items.size(); ++i)
                if (items[i].dir && items[i].parent == folder && items[i].name == part) found = i;
            if (!found) { items.push_back({true, part, folder, 0, 0, nullptr}); found = uint32_t(items.size() - 1); }
            folder = found;
            start = slash + 1;
        }
    }
    // The items were appended in file order, which is not depth first when files of one folder are not adjacent: sort into depth-first order (stable).
    std::vector<uint32_t> order;
    std::vector<std::vector<uint32_t>> kids(items.size());
    for (uint32_t i = 1; i < items.size(); ++i) kids[items[i].parent].push_back(i);
    std::vector<uint32_t> remap(items.size());
    const auto visit = [&](auto&& self, uint32_t i) -> void {
        remap[i] = uint32_t(order.size());
        order.push_back(i);
        for (uint32_t kid : kids[i]) self(self, kid);
        items[i].end = uint32_t(order.size());
    };
    visit(visit, 0);

    std::vector<uint8_t> pool{0};
    for (uint32_t i : order) {
        if (i == 0) continue;
        items[i].name_off = uint32_t(pool.size());
        pool.insert(pool.end(), items[i].name.begin(), items[i].name.end());
        pool.push_back(0);
    }
    const uint32_t count = uint32_t(order.size());
    const uint32_t tables = count * 12 + uint32_t(pool.size());
    const uint32_t data_off = (0x20 + tables + 0x1F) & ~0x1Fu;
    std::vector<uint32_t> files_in_order;
    for (uint32_t k = 0; k < count; ++k) if (!items[order[k]].dir) files_in_order.push_back(k);
    if (reverse_data) std::reverse(files_in_order.begin(), files_in_order.end());
    std::vector<uint32_t> offset(count, 0);
    uint32_t cursor = data_off;
    for (uint32_t k : files_in_order) {
        offset[k] = cursor;
        cursor += (uint32_t(items[order[k]].data->size()) + 0x1F) & ~0x1Fu;
    }
    std::vector<uint8_t> out(cursor, 0);
    out[0] = 0x55; out[1] = 0xAA; out[2] = 0x38; out[3] = 0x2D;
    put32(out, 4, 0x20); put32(out, 8, tables); put32(out, 12, data_off);
    for (uint32_t k = 0; k < count; ++k) {
        const Item& item = items[order[k]];
        const size_t at = 0x20 + size_t(k) * 12;
        out[at] = item.dir ? 1 : 0;
        out[at + 1] = uint8_t(item.name_off >> 16); out[at + 2] = uint8_t(item.name_off >> 8); out[at + 3] = uint8_t(item.name_off);
        if (item.dir) { put32(out, at + 4, k == 0 ? 0 : remap[item.parent]); put32(out, at + 8, item.end); }
        else { put32(out, at + 4, offset[k]); put32(out, at + 8, uint32_t(item.data->size())); std::copy(item.data->begin(), item.data->end(), out.begin() + offset[k]); }
    }
    std::copy(pool.begin(), pool.end(), out.begin() + 0x20 + size_t(count) * 12);
    return out;
}

}  // namespace u8_fixture
