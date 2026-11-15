/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the VM manager's IPC protocol
 *          (qt_vmmanager_protocol.cpp): JSON messages framed like a
 *          QDataStream QByteArray.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "haiku_vmmanager_protocol.hpp"

extern "C" {
#include <86box/version.h>
}

/* ----------------------------------------------------------------- JSON */

VmmJson::VmmJson()
    : type(Null)
    , boolean(false)
    , number(0)
{
}

VmmJson
VmmJson::MakeString(const std::string &value)
{
    VmmJson json;
    json.type   = String;
    json.string = value;
    return json;
}

VmmJson
VmmJson::MakeNumber(double value)
{
    VmmJson json;
    json.type   = Number;
    json.number = value;
    return json;
}

VmmJson &
VmmJson::operator[](const std::string &key)
{
    if (type != Object) {
        type = Object;
        object.clear();
    }
    return object[key];
}

const VmmJson *
VmmJson::Find(const char *key) const
{
    if (type != Object)
        return nullptr;
    const auto it = object.find(key);
    return (it == object.end()) ? nullptr : &it->second;
}

std::string
VmmJson::Str(const char *key) const
{
    const VmmJson *value = Find(key);
    return ((value != nullptr) && (value->type == String)) ? value->string : std::string();
}

bool
VmmJson::operator==(const VmmJson &other) const
{
    if (type != other.type)
        return false;
    switch (type) {
        case Null:
            return true;
        case Bool:
            return boolean == other.boolean;
        case Number:
            return number == other.number;
        case String:
            return string == other.string;
        case Object:
            return object == other.object;
        case Array:
            return array == other.array;
    }
    return false;
}

static void
serialize_string(std::string &out, const std::string &value)
{
    out += '"';
    for (const unsigned char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char temp[8];
                    snprintf(temp, sizeof(temp), "\\u%04x", c);
                    out += temp;
                } else
                    out += (char) c;
                break;
        }
    }
    out += '"';
}

static void
serialize(std::string &out, const VmmJson &json)
{
    switch (json.type) {
        case VmmJson::Null:
            out += "null";
            break;
        case VmmJson::Bool:
            out += json.boolean ? "true" : "false";
            break;
        case VmmJson::Number:
            {
                char temp[40];
                if (std::isfinite(json.number))
                    snprintf(temp, sizeof(temp), "%.17g", json.number);
                else
                    strcpy(temp, "null");
                out += temp;
                break;
            }
        case VmmJson::String:
            serialize_string(out, json.string);
            break;
        case VmmJson::Object:
            {
                bool first = true;
                out += '{';
                for (const auto &member : json.object) {
                    if (!first)
                        out += ',';
                    first = false;
                    serialize_string(out, member.first);
                    out += ':';
                    serialize(out, member.second);
                }
                out += '}';
                break;
            }
        case VmmJson::Array:
            {
                bool first = true;
                out += '[';
                for (const auto &element : json.array) {
                    if (!first)
                        out += ',';
                    first = false;
                    serialize(out, element);
                }
                out += ']';
                break;
            }
    }
}

std::string
VmmJson::Serialize() const
{
    std::string out;
    serialize(out, *this);
    return out;
}

namespace {

class JsonParser {
public:
    explicit JsonParser(const std::string &text)
        : text(text)
        , pos(0)
    {
    }

    bool Document(VmmJson &out)
    {
        if (!Value(out, 0))
            return false;
        SkipSpace();
        return pos == text.size();
    }

private:
    void SkipSpace()
    {
        while ((pos < text.size()) && ((text[pos] == ' ') || (text[pos] == '\t') || (text[pos] == '\n') || (text[pos] == '\r')))
            pos++;
    }

    bool Literal(const char *word)
    {
        const size_t len = strlen(word);
        if (text.compare(pos, len, word) != 0)
            return false;
        pos += len;
        return true;
    }

    static void AppendUtf8(std::string &out, uint32_t cp)
    {
        if (cp < 0x80)
            out += (char) cp;
        else if (cp < 0x800) {
            out += (char) (0xc0 | (cp >> 6));
            out += (char) (0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += (char) (0xe0 | (cp >> 12));
            out += (char) (0x80 | ((cp >> 6) & 0x3f));
            out += (char) (0x80 | (cp & 0x3f));
        } else {
            out += (char) (0xf0 | (cp >> 18));
            out += (char) (0x80 | ((cp >> 12) & 0x3f));
            out += (char) (0x80 | ((cp >> 6) & 0x3f));
            out += (char) (0x80 | (cp & 0x3f));
        }
    }

    bool Hex4(uint32_t &value)
    {
        if ((pos + 4) > text.size())
            return false;
        value = 0;
        for (int i = 0; i < 4; i++) {
            const char c = text[pos++];
            value <<= 4;
            if ((c >= '0') && (c <= '9'))
                value |= c - '0';
            else if ((c >= 'a') && (c <= 'f'))
                value |= c - 'a' + 10;
            else if ((c >= 'A') && (c <= 'F'))
                value |= c - 'A' + 10;
            else
                return false;
        }
        return true;
    }

    bool StringValue(std::string &out)
    {
        if ((pos >= text.size()) || (text[pos] != '"'))
            return false;
        pos++;
        while (pos < text.size()) {
            const char c = text[pos++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos >= text.size())
                return false;
            const char e = text[pos++];
            switch (e) {
                case '"':
                case '\\':
                case '/':
                    out += e;
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u':
                    {
                        uint32_t cp;
                        if (!Hex4(cp))
                            return false;
                        if ((cp >= 0xd800) && (cp < 0xdc00) && (text.compare(pos, 2, "\\u") == 0)) {
                            uint32_t low;
                            pos += 2;
                            if (!Hex4(low))
                                return false;
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                default:
                    return false;
            }
        }
        return false;
    }

    bool Value(VmmJson &out, int depth)
    {
        if (depth > 32)
            return false;
        SkipSpace();
        if (pos >= text.size())
            return false;

        const char c = text[pos];
        if (c == '{') {
            pos++;
            out.type = VmmJson::Object;
            SkipSpace();
            if ((pos < text.size()) && (text[pos] == '}')) {
                pos++;
                return true;
            }
            for (;;) {
                std::string key;
                SkipSpace();
                if (!StringValue(key))
                    return false;
                SkipSpace();
                if ((pos >= text.size()) || (text[pos] != ':'))
                    return false;
                pos++;
                if (!Value(out.object[key], depth + 1))
                    return false;
                SkipSpace();
                if (pos >= text.size())
                    return false;
                if (text[pos] == ',') {
                    pos++;
                    continue;
                }
                if (text[pos] == '}') {
                    pos++;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            pos++;
            out.type = VmmJson::Array;
            SkipSpace();
            if ((pos < text.size()) && (text[pos] == ']')) {
                pos++;
                return true;
            }
            for (;;) {
                out.array.emplace_back();
                if (!Value(out.array.back(), depth + 1))
                    return false;
                SkipSpace();
                if (pos >= text.size())
                    return false;
                if (text[pos] == ',') {
                    pos++;
                    continue;
                }
                if (text[pos] == ']') {
                    pos++;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out.type = VmmJson::String;
            return StringValue(out.string);
        }
        if (Literal("true")) {
            out.type    = VmmJson::Bool;
            out.boolean = true;
            return true;
        }
        if (Literal("false")) {
            out.type    = VmmJson::Bool;
            out.boolean = false;
            return true;
        }
        if (Literal("null")) {
            out.type = VmmJson::Null;
            return true;
        }

        const char *start = text.c_str() + pos;
        char       *end   = nullptr;
        const double value = strtod(start, &end);
        if (end == start)
            return false;
        pos += end - start;
        out.type   = VmmJson::Number;
        out.number = value;
        return true;
    }

    const std::string &text;
    size_t             pos;
};

}

bool
VmmJson::Parse(const std::string &text, VmmJson &out)
{
    out = VmmJson();
    JsonParser parser(text);
    return parser.Document(out);
}

/* ------------------------------------------------------------- protocol */

VmmJson
vmm_protocol_message(bool from_manager, const char *message)
{
    VmmJson json;
    json["type"]    = VmmJson::MakeString(from_manager ? "VMManager" : "Client");
    json["version"] = VmmJson::MakeString(EMU_VERSION);
    json["message"] = VmmJson::MakeString(message);
    return json;
}

VmmJson
vmm_manager_identity(void)
{
    VmmJson identity;
    identity["name"]    = VmmJson::MakeString(EMU_NAME " VM Manager");
    identity["version"] = VmmJson::MakeString(EMU_VERSION);
    return identity;
}

bool
vmm_protocol_valid(const VmmJson &json)
{
    return (json.Find("type") != nullptr) && (json.Find("message") != nullptr);
}

VmmJson
vmm_protocol_params(const VmmJson &json)
{
    const VmmJson *params = json.Find("params");
    if ((params == nullptr) || (params->type != VmmJson::Object)) {
        VmmJson empty;
        empty.type = VmmJson::Object;
        return empty;
    }
    return *params;
}

static bool
write_all(int fd, const char *data, size_t length)
{
    while (length > 0) {
#ifdef MSG_NOSIGNAL
        const ssize_t written = send(fd, data, length, MSG_NOSIGNAL);
#else
        const ssize_t written = send(fd, data, length, 0);
#endif
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += written;
        length -= written;
    }
    return true;
}

bool
vmm_send(int fd, const VmmJson &json)
{
    if (fd < 0)
        return false;

    const std::string payload = json.Serialize();
    const uint32_t    length  = (uint32_t) payload.size();
    std::string       frame;
    frame.reserve(payload.size() + 4);
    frame += (char) (length >> 24);
    frame += (char) (length >> 16);
    frame += (char) (length >> 8);
    frame += (char) length;
    frame += payload;
    return write_all(fd, frame.data(), frame.size());
}

void
VmmFrameReader::Append(const char *data, size_t length)
{
    buffer.append(data, length);
}

bool
VmmFrameReader::Next(std::string &payload)
{
    for (;;) {
        if (buffer.size() < 4)
            return false;
        const uint32_t length = ((uint32_t) (uint8_t) buffer[0] << 24) | ((uint32_t) (uint8_t) buffer[1] << 16)
            | ((uint32_t) (uint8_t) buffer[2] << 8) | (uint32_t) (uint8_t) buffer[3];
        /* QDataStream writes 0xffffffff for a null QByteArray. */
        if (length == 0xffffffff) {
            buffer.erase(0, 4);
            continue;
        }
        if (buffer.size() < (4 + (size_t) length))
            return false;
        payload.assign(buffer, 4, length);
        buffer.erase(0, 4 + (size_t) length);
        return true;
    }
}

bool
vmm_wait_readable(int fd, int wake_fd)
{
    for (;;) {
        struct pollfd fds[2];
        fds[0].fd      = fd;
        fds[0].events  = POLLIN;
        fds[0].revents = 0;
        fds[1].fd      = wake_fd;
        fds[1].events  = POLLIN;
        fds[1].revents = 0;

        const int result = poll(fds, (wake_fd >= 0) ? 2 : 1, -1);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if ((wake_fd >= 0) && (fds[1].revents != 0))
            return false;
        if (fds[0].revents & POLLNVAL)
            return false;
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR))
            return true;
    }
}
