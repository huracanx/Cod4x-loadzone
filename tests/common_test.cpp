#include "../src/common.hpp"
#include <cassert>
#include <cstring>
int main() {
    const char* s = "123456789";
    assert((crc32_update(0xffffffffu, reinterpret_cast<const unsigned char*>(s), strlen(s)) ^ 0xffffffffu) == 0xcbf43926u);
    assert(valid_zone_name("my-zone_01"));
    assert(!valid_zone_name("")); assert(!valid_zone_name("../bad"));
    assert(!valid_zone_name("test.ff")); assert(!valid_zone_name("a/b"));
    assert(!valid_zone_name("a\\b")); assert(!valid_zone_name("a;b"));
    assert(!valid_zone_name("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
}
