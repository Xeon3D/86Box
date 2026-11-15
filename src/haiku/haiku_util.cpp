/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the machine UUID helpers (qt_util.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>

#include "haiku_util.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/network.h>
#include <86box/random.h>
}

/* Qt's UUID_MIN_LENGTH. */
static const size_t UUID_MIN_LENGTH = 36;

/* SHA-1 (FIPS 180-4), enough for the one name-based UUID. */
static void
sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };

    const size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t     *msg   = static_cast<uint8_t *>(calloc(total, 1));
    memcpy(msg, data, len);
    msg[len]              = 0x80;
    const uint64_t bits   = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; i++)
        msg[total - 1 - i] = static_cast<uint8_t>(bits >> (i * 8));

    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };

    for (size_t chunk = 0; chunk < total; chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (msg[chunk + i * 4] << 24) | (msg[chunk + i * 4 + 1] << 16) | (msg[chunk + i * 4 + 2] << 8) | msg[chunk + i * 4 + 3];
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f;
            uint32_t k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    free(msg);

    for (int i = 0; i < 5; i++) {
        out[i * 4]     = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
}

std::string
haiku_uuid_for_config(const char *config_path)
{
    /* The folder of the configuration file, canonical, ending in '/'. */
    std::string dir = config_path ? config_path : "";
    const size_t slash = dir.rfind('/');
    dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);
    if (dir.empty())
        dir = "/";

    char resolved[PATH_MAX];
    if (realpath(dir.c_str(), resolved) != nullptr)
        dir = resolved;
    if (dir.empty() || (dir.back() != '/'))
        dir += '/';

    /* QUuid::createUuidV5(QUuid{}, name): SHA-1 of the nil namespace and
       the name, version 5, RFC 4122 variant. */
    std::string input(16, '\0');
    input += dir;
    uint8_t hash[20];
    sha1(reinterpret_cast<const uint8_t *>(input.data()), input.size(), hash);
    hash[6] = (hash[6] & 0x0f) | 0x50;
    hash[8] = (hash[8] & 0x3f) | 0x80;

    char text[40];
    snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7], hash[8], hash[9], hash[10],
             hash[11], hash[12], hash[13], hash[14], hash[15]);
    return text;
}

static bool
has_configured_nics(void)
{
    for (int i = 0; i < NET_CARD_MAX; i++) {
        if (net_cards_conf[i].device_num != 0)
            return true;
    }
    return false;
}

void
haiku_store_current_uuid(void)
{
    strncpy(uuid, haiku_uuid_for_config(cfg_path).c_str(), sizeof(uuid) - 1);
}

bool
haiku_compare_uuid(void)
{
    /* A uuid that is missing or too short is replaced. */
    if (strlen(uuid) < UUID_MIN_LENGTH) {
        haiku_store_current_uuid();
        return true;
    }

    const std::string current = haiku_uuid_for_config(cfg_path);

    /* Without network cards a move or copy doesn't matter: just update. */
    if (!has_configured_nics() && (current != uuid)) {
        haiku_store_current_uuid();
        return true;
    }

    return current == uuid;
}

void
haiku_generate_new_mac_addresses(void)
{
    for (int i = 0; i < NET_CARD_MAX; i++) {
        if (net_cards_conf[i].device_num == 0)
            continue;

        const device_t  *dev = network_card_getdevice(net_cards_conf[i].device_num);
        device_context_t ctx;
        device_set_context(&ctx, dev, i + 1);

        char mac[16];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x", random_generate(), random_generate(), random_generate());
        config_set_string(ctx.name, "mac", mac);
    }
}
