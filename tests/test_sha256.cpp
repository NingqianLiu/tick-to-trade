#include <gtest/gtest.h>

#include <string>

#include "common/sha256.hpp"

namespace {

std::string hash_of(const std::string& s) {
    crypto::Sha256 sha;
    sha.update(s.data(), s.size());
    return sha.hex();
}

TEST(Sha256, known_vectors) {
    EXPECT_EQ(hash_of(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hash_of("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hash_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(hash_of(std::string(1000000, 'a')), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, incremental_matches_one_shot) {
    const std::string data(5000, 'x');
    crypto::Sha256 chunked;
    for (std::size_t off = 0; off < data.size(); off += 7) {
        chunked.update(data.data() + off, std::min<std::size_t>(7, data.size() - off));
    }
    EXPECT_EQ(chunked.hex(), hash_of(data));
}

}  // namespace

