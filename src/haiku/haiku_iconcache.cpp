/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: decoding the icons compiled into the
 *          binary (see haiku_icons.hpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <Bitmap.h>

#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "haiku_icons.hpp"

static uint32_t
read_le(const uint8_t *p, int bytes)
{
    uint32_t v = 0;
    for (int i = bytes - 1; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

/* Take the 16x16 32-bit image out of an .ico file. The 86Box icons store it
   as an uncompressed bottom-up BGRA bitmap, which is the memory layout of
   B_RGBA32, so the rows only need flipping. (Haiku's ICO translator loses
   the alpha channel of these icons, leaving a pink background.) */
static BBitmap *
decode_ico(const uint8_t *data, size_t size)
{
    if ((size < 6) || (read_le(data + 2, 2) != 1))
        return nullptr;

    const int count = read_le(data + 4, 2);
    for (int i = 0; i < count; i++) {
        const uint8_t *entry = data + 6 + (i * 16);
        if (entry + 16 > data + size)
            break;

        const int      w      = entry[0] ? entry[0] : 256;
        const int      h      = entry[1] ? entry[1] : 256;
        const uint32_t offset = read_le(entry + 12, 4);
        if ((w != 16) || (h != 16) || (offset + 40 > size))
            continue;

        const uint8_t *dib = data + offset;
        if ((read_le(dib, 4) < 40) || (read_le(dib + 14, 2) != 32) || (read_le(dib + 16, 4) != 0))
            continue; /* not an uncompressed 32-bit bitmap (e.g. PNG) */

        const uint8_t *pixels = dib + read_le(dib, 4);
        if (pixels + (w * h * 4) > data + size)
            continue;

        BBitmap *bitmap = new BBitmap(BRect(0, 0, w - 1, h - 1), B_RGBA32);
        uint8   *bits   = static_cast<uint8 *>(bitmap->Bits());
        for (int y = 0; y < h; y++)
            memcpy(bits + (y * bitmap->BytesPerRow()), pixels + ((h - 1 - y) * w * 4), w * 4);
        return bitmap;
    }

    return nullptr;
}

/* Decode an icon compiled into the binary; nullptr if it is missing. */
BBitmap *
haiku_icon(const char *name)
{
    static std::map<std::string, BBitmap *> cache;
    static std::mutex                       cache_lock;
    std::lock_guard<std::mutex>             guard(cache_lock);

    auto found = cache.find(name);
    if (found != cache.end())
        return found->second;

    BBitmap *bitmap = nullptr;
    for (const haiku_icon_t *icon = haiku_icons; icon->name != nullptr; icon++) {
        if (strcmp(icon->name, name) == 0) {
            bitmap = decode_ico(icon->data, icon->size);
            break;
        }
    }

    cache[name] = bitmap;
    return bitmap;
}

