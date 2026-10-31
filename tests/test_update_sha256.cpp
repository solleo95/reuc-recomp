#include "check.h"
#include "sha256.h"
#include <string>
using namespace uc::update;
int main() {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");   // two-block message
    std::string million(1000000, 'a');
    CHECK(sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    Sha256 pieces;   // incremental == one-shot, whatever the piece sizes
    for (size_t at = 0, step = 1; at < million.size(); at += step, step = step % 97 + 1)
        pieces.update({reinterpret_cast<const unsigned char*>(million.data()) + at, std::min(step, million.size() - at)});
    CHECK(pieces.finish_hex() == sha256_hex(million));
    std::puts("update sha256: vectors passed");
}
