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

#include <algorithm>
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


/* Draw `top` over `bottom` (both 16x16 B_RGBA32 with straight alpha). */
static void
blend_over(BBitmap *bottom, const BBitmap *top)
{
    const int32 rows = std::min(bottom->Bounds().IntegerHeight(), top->Bounds().IntegerHeight()) + 1;
    const int32 cols = std::min(bottom->Bounds().IntegerWidth(), top->Bounds().IntegerWidth()) + 1;

    for (int32 y = 0; y < rows; y++) {
        uint8       *d = static_cast<uint8 *>(bottom->Bits()) + (y * bottom->BytesPerRow());
        const uint8 *s = static_cast<const uint8 *>(top->Bits()) + (y * top->BytesPerRow());
        for (int32 x = 0; x < cols; x++, d += 4, s += 4) {
            const int sa = s[3];
            if (sa == 0)
                continue;
            const int da = d[3];
            const int oa = sa + (da * (255 - sa) / 255);
            for (int c = 0; c < 3; c++)
                d[c] = static_cast<uint8>((s[c] * sa + (d[c] * da * (255 - sa) / 255)) / (oa ? oa : 1));
            d[3] = static_cast<uint8>(oa);
        }
    }
}

BBitmap *
haiku_icon_with(const char *base, const char *overlay, const char *overlay2)
{
    if (overlay == nullptr)
        return haiku_icon(base);

    static std::map<std::string, BBitmap *> cache;
    static std::mutex                       cache_lock;

    const std::string key = std::string(base) + "+" + overlay + "+" + (overlay2 ? overlay2 : "");

    BBitmap *source = haiku_icon(base);
    BBitmap *top    = haiku_icon(overlay);
    BBitmap *top2   = overlay2 ? haiku_icon(overlay2) : nullptr;

    std::lock_guard<std::mutex> guard(cache_lock);
    auto                        found = cache.find(key);
    if (found != cache.end())
        return found->second;

    BBitmap *bitmap = nullptr;
    if (source != nullptr) {
        bitmap = new BBitmap(source);
        if (top != nullptr)
            blend_over(bitmap, top);
        if (top2 != nullptr)
            blend_over(bitmap, top2);
    }
    cache[key] = bitmap;
    return bitmap;
}

BBitmap *
haiku_icon_disabled(BBitmap *icon)
{
    if (icon == nullptr)
        return nullptr;

    static std::map<BBitmap *, BBitmap *> cache;
    static std::mutex                     cache_lock;
    std::lock_guard<std::mutex>           guard(cache_lock);

    auto found = cache.find(icon);
    if (found != cache.end())
        return found->second;

    /* Grey and at 40% opacity, close to Qt's disabled icon mode. */
    BBitmap    *faded = new BBitmap(icon);
    const int32 rows  = faded->Bounds().IntegerHeight() + 1;
    const int32 cols  = faded->Bounds().IntegerWidth() + 1;
    for (int32 y = 0; y < rows; y++) {
        uint8 *p = static_cast<uint8 *>(faded->Bits()) + (y * faded->BytesPerRow());
        for (int32 x = 0; x < cols; x++, p += 4) {
            const uint8 grey = static_cast<uint8>((p[0] * 11 + p[1] * 59 + p[2] * 30) / 100);
            p[0] = p[1] = p[2] = grey;
            p[3]               = static_cast<uint8>(p[3] * 2 / 5);
        }
    }
    cache[icon] = faded;
    return faded;
}
