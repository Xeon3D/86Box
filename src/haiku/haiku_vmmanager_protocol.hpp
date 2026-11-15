/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the VM manager's IPC protocol
 *          (qt_vmmanager_protocol.cpp), shared by the manager and the
 *          machines it starts.
 *
 *          The messages are the Qt ones: compact JSON objects with "type"
 *          ("VMManager" or "Client"), "version", "message" and optional
 *          "params", each sent the way QDataStream sends a QByteArray (a
 *          big-endian 32-bit length, then the bytes), over a Unix domain
 *          socket whose path the manager passes in VMM_86BOX_SOCKET.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_VMMANAGER_PROTOCOL_HPP
#define HAIKU_VMMANAGER_PROTOCOL_HPP

#include <cstddef>
#include <map>
#include <string>
#include <vector>

/* The environment variable that gives a started machine the socket path. */
#define VMM_SOCKET_ENV "VMM_86BOX_SOCKET"

/* Just enough JSON for the protocol. */
class VmmJson {
public:
    enum Type {
        Null,
        Bool,
        Number,
        String,
        Object,
        Array,
    };

    VmmJson();
    static VmmJson MakeString(const std::string &value);
    static VmmJson MakeNumber(double value);

    /* Makes this an object (if it is not) and returns the member. */
    VmmJson &operator[](const std::string &key);

    const VmmJson *Find(const char *key) const;
    /* The member as a string, or "" when missing or not a string. */
    std::string Str(const char *key) const;
    bool        operator==(const VmmJson &other) const;

    std::string Serialize() const;
    static bool Parse(const std::string &text, VmmJson &out);

    Type                           type;
    bool                           boolean;
    double                         number;
    std::string                    string;
    std::map<std::string, VmmJson> object;
    std::vector<VmmJson>           array;
};

/* A new message (VMManagerProtocol::protocolManagerMessage() and
   protocolClientMessage()). */
VmmJson vmm_protocol_message(bool from_manager, const char *message);

/* VMManagerProtocol::managerIdentity(). */
VmmJson vmm_manager_identity(void);

/* True if the object has "type" and "message". */
bool vmm_protocol_valid(const VmmJson &json);

/* The "params" object, or an empty one. */
VmmJson vmm_protocol_params(const VmmJson &json);

/* Sends one framed message; false if the connection failed. */
bool vmm_send(int fd, const VmmJson &json);

/* Splits the received bytes into messages. */
class VmmFrameReader {
public:
    void Append(const char *data, size_t length);
    /* The next complete message, if there is one. */
    bool Next(std::string &payload);

private:
    std::string buffer;
};

/* Waits until `fd` can be read (true) or `wake_fd` becomes readable or an
   error happens (false). */
bool vmm_wait_readable(int fd, int wake_fd);

#endif
