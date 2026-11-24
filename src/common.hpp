#pragma once
#include <cstdint>
#include <cstddef>

inline uint32_t crc32_update(uint32_t crc, const unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}
// Basenames only: no paths, extensions, traversal, shell syntax or locale-dependent chars.
inline bool valid_zone_name(const char* s) {
    if (!s || !*s) return false;
    size_t count = 0;
    for (; *s; ++s) {
        const char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        if (++count > 64) return false;
    }
    return true;
}

// --- Общие типы движка (нужны и plugin.cpp, и zone_menu.cpp) ---
union DvarValue {
    bool enabled;
    int integer;
    float value;
    float vector[4];
    const char* string;
};
struct Dvar {
    const char* name;
    const char* description;
    uint16_t flags;
    int8_t type;
    bool modified;
    DvarValue current;
};
static_assert(offsetof(Dvar, current) == 12, "Unexpected dvar layout");

struct XZoneInfo { const char* name; int allocFlags; int freeFlags; };

// findDvar определён в plugin.cpp (naked-функция); здесь только объявление.
Dvar* findDvar(const char* name);

// Указатель на строку языка игры (0xCC147D4); определён в plugin.cpp.
extern const char** g_langPtr;

// Общий загрузчик/выгрузчик зон (реализация в plugin.cpp).
bool loadZoneByName(const char* nameWithoutExt, char* err, size_t errCap);
bool unloadLoadedZone(char* err, size_t errCap);

// Жизненный цикл меню (реализация в zone_menu.cpp).
void initZoneMenu(void* moduleHandle);
void shutdownZoneMenu();