/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the memory viewer's disassembler, a port
 *          of the Qt UI's (qt_disasm.cpp) with std::string in place of
 *          QString. Zydis, with what 86Box's processors do differently
 *          from the modern instruction set it decodes worked around here,
 *          outside the library.
 *
 *          - The 8086 and 80186 (808x.c) and the NEC V20/V30 (vx0.c) alias
 *            opcodes later processors gave other meanings: 60-6F are the
 *            70-7F jumps on the 8086, 0F is POP CS (and on the NEC a prefix
 *            for its own instructions), C0/C1/C8/C9 are returns, F1 is
 *            LOCK, the unused ModR/M reg values of 8C/8E/8F/C6/C7/FE/FF
 *            repeat the used ones, shifts /6 are SETMO, and on the NEC
 *            64/65 are REPNC/REPC and D6 is XLAT.
 *          - The NEC's 8080 emulation mode runs 8080 code.
 *          - The 8086-class prefetch queue: the processor runs the bytes it
 *            has already fetched, not what memory holds now.
 *          - The 286 and later run what their opcode tables hold: an
 *            opcode a processor does not have raises #UD, and some mean
 *            what they did on that processor (LOADALL, UMOV, the Cyrix
 *            and MediaGX system management instructions, the Cyrix EMMI
 *            in 0F 50-5E, Virtual PC's 0F 3F), not what they mean now
 *            (SYSCALL, SSE, VMX, three-byte opcode maps). VEX, EVEX and XOP are LES, LDS, BOUND and POP
 *            with a register operand, which raise #UD. The spare ModR/M
 *            forms of the groups (0F 00, 0F 01, 0F AE, 0F C7 and so on),
 *            which the library takes as later instructions (MONITOR,
 *            RDTSCP, LFENCE, RDRAND), raise #UD as 86Box's handlers do.
 *            A 3DNow! suffix is looked up in the processor's own table:
 *            the K6-2 and WinChip 2 lack the K6-2+ and K6-III+'s additions.
 *          - The x87 opcodes are checked against the coprocessor's own
 *            tables (the 8087's on the 8086 class), and the undocumented
 *            register forms that repeat others are said to.
 *          - The library's decoder modes for extensions that reuse old
 *            encodings (MPX, CET, LZCNT/TZCNT, WBNOINVD, CLDEMOTE,
 *            instruction prefetch hints) are off: these processors have
 *            none of them.
 *
 *          Each byte is read once, and only bytes of the instruction being
 *          decoded are read.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "haiku_disasm.hpp"

#ifdef USE_DISASM
#    include <Zydis/Zydis.h>
#endif

namespace {

/* printf into a std::string. */
std::string strfmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

std::string
strfmt(const char *fmt, ...)
{
    char    buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

std::vector<std::string>
splitWords(const std::string &text)
{
    std::vector<std::string> words;
    size_t                   start = 0;
    for (;;) {
        const size_t sp = text.find(' ', start);
        words.push_back(text.substr(start, (sp == std::string::npos) ? std::string::npos : (sp - start)));
        if (sp == std::string::npos)
            break;
        start = sp + 1;
    }
    return words;
}

std::string
joinWords(const std::vector<std::string> &words, size_t from = 0, const char *sep = " ")
{
    std::string out;
    for (size_t i = from; i < words.size(); i++) {
        if (i > from)
            out += sep;
        out += words[i];
    }
    return out;
}

/* What \b in a regular expression takes as part of a word. */
bool
isWordChar(char c)
{
    return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (c == '_');
}

/* Where word stands on its own in text (\bword\b), or npos. */
size_t
findWord(const std::string &text, const char *word)
{
    const size_t len = std::char_traits<char>::length(word);
    for (size_t pos = text.find(word); pos != std::string::npos; pos = text.find(word, pos + 1)) {
        if (((pos == 0) || !isWordChar(text[pos - 1])) && (((pos + len) >= text.size()) || !isWordChar(text[pos + len])))
            return pos;
    }
    return std::string::npos;
}

/* Every \b(word|dword) ptr in text made "tbyte ptr". */
void
makeTbytePtr(std::string &text)
{
    size_t pos = 0;
    while ((pos = text.find("word ptr", pos)) != std::string::npos) {
        size_t start = pos;
        if ((start > 0) && (text[start - 1] == 'd'))
            start--;
        if ((start == 0) || !isWordChar(text[start - 1])) {
            text.replace(start, pos + 8 - start, "tbyte ptr");
            pos = start + 9;
        } else
            pos += 8;
    }
}

/* Reads an instruction's bytes on demand, each once. */
class Fetcher {
public:
    Fetcher(uint32_t csBase, uint32_t ip, bool wrap16, const uint8_t *queue, int queueLen)
        : csBase_(csBase)
        , ip_(ip)
        , wrap16_(wrap16)
        , queue_(queue)
        , queueLen_(queueLen)
    {
    }

    /* Byte i of the instruction; false if it cannot be read. */
    bool get(int i, uint8_t *val)
    {
        if (i >= 16)
            return false;
        while (count_ <= i) {
            uint8_t b;
            uint8_t cls = DEBUGGER_MEM_RAM;

            if (count_ < queueLen_)
                b = queue_[count_];
            else
                debugger_mem_read(DEBUGGER_SPACE_LINEAR, addr(count_), 1, &b, &cls);
            if ((cls != DEBUGGER_MEM_RAM) && (cls != DEBUGGER_MEM_ROM) && (cls != DEBUGGER_MEM_VIDEO)) {
                failCls_ = cls;
                return false;
            }
            bytes_[count_++] = b;
        }
        *val = bytes_[i];
        return true;
    }

    uint32_t addr(int i) const { return csBase_ + (wrap16_ ? ((ip_ + i) & 0xffff) : (ip_ + i)); }
    const uint8_t *bytes() const { return bytes_; }
    int            count() const { return count_; }
    int            fromQueue() const { return queueLen_; }
    uint8_t        failCls() const { return failCls_; }

private:
    uint32_t       csBase_;
    uint32_t       ip_;
    bool           wrap16_;
    const uint8_t *queue_;
    int            queueLen_;
    uint8_t        bytes_[16] {};
    int            count_   = 0;
    uint8_t        failCls_ = DEBUGGER_MEM_RAM;
};

/* What the library is given in place of the bytes: head (the prefixes and
   opcode as rewritten), then the original bytes from resume on, with the
   ModR/M byte's reg field optionally forced. */
struct View {
    std::vector<uint8_t> head;
    int                  resume   = 0;
    int                  modrmAt  = -1; /* original index of the ModR/M byte */
    int                  forceReg = -1;
};

struct Result {
    bool    ok  = false;
    int     len = 0; /* in original bytes */
    std::string text;
};

const char *const reg8Names[8] = { "al", "cl", "dl", "bl", "ah", "ch", "dh", "bh" };

/* Words the library puts before the mnemonic. */
bool
isPrefixWord(const std::string &w)
{
    static const char *const words[] = { "rep", "repe", "repz", "repne", "repnz", "lock", "xacquire", "xrelease", "bnd", "bht", "bhnt", "notrack" };
    for (const char *p : words) {
        if (w == p)
            return true;
    }
    return false;
}

/* text with its mnemonic replaced. */
std::string
renameMnemonic(const std::string &text, const std::string &name)
{
    std::vector<std::string> words = splitWords(text);
    for (size_t i = 0; i < words.size(); i++) {
        if (!isPrefixWord(words[i])) {
            words[i] = name;
            break;
        }
    }
    return joinWords(words);
}

/* The operands of text: what follows the mnemonic. */
std::string
operandsOf(const std::string &text)
{
    const std::vector<std::string> words = splitWords(text);
    size_t                         i     = 0;
    while ((i < words.size()) && isPrefixWord(words[i]))
        i++;
    return joinWords(words, i + 1);
}

/* Bytes of displacement and SIB after a ModR/M byte, as the processor reads
   them; the SIB byte, if any, must be fetchable at modrmAt + 1. */
bool
modrmExtra(Fetcher &f, int modrmAt, bool addr32, int *extra)
{
    uint8_t m;
    if (!f.get(modrmAt, &m))
        return false;
    const int mod = m >> 6;
    const int rm  = m & 7;

    if (mod == 3) {
        *extra = 0;
        return true;
    }
    if (!addr32) {
        *extra = (mod == 1) ? 1 : ((mod == 2) || ((mod == 0) && (rm == 6))) ? 2 : 0;
        return true;
    }

    int n = 0;
    if (rm == 4) {
        uint8_t sib;
        if (!f.get(modrmAt + 1, &sib))
            return false;
        n = 1;
        if ((mod == 0) && ((sib & 7) == 5))
            n += 4;
    } else if ((mod == 0) && (rm == 5))
        n += 4;
    if (mod == 1)
        n += 1;
    else if (mod == 2)
        n += 4;
    *extra = n;
    return true;
}

/* Whether opcode op, of map 0 (one byte) or 1 (after 0F), takes a ModR/M
   byte wherever it is defined, and the bytes of immediate after it (reg:
   the ModR/M byte's reg field, for F6 and F7; o32: 32-bit operands). An
   encoding the processor refuses is then shown with all its bytes, so that
   the instructions after it are decoded from where they start. */
bool
modrmLayout(int map, uint8_t op, int reg, bool o32, int *imm)
{
    *imm = 0;
    if (!map) {
        switch (op) {
            case 0x00 ... 0x03:
            case 0x08 ... 0x0b:
            case 0x10 ... 0x13:
            case 0x18 ... 0x1b:
            case 0x20 ... 0x23:
            case 0x28 ... 0x2b:
            case 0x30 ... 0x33:
            case 0x38 ... 0x3b:
            case 0x62:
            case 0x63:
            case 0x84 ... 0x8f:
            case 0xc4:
            case 0xc5:
            case 0xd0 ... 0xd3:
            case 0xd8 ... 0xdf:
            case 0xfe:
            case 0xff:
                return true;
            case 0x69:
            case 0x81:
            case 0xc7:
                *imm = o32 ? 4 : 2;
                return true;
            case 0x6b:
            case 0x80:
            case 0x82:
            case 0x83:
            case 0xc0:
            case 0xc1:
            case 0xc6:
                *imm = 1;
                return true;
            case 0xf6:
                *imm = (reg < 2) ? 1 : 0;
                return true;
            case 0xf7:
                *imm = (reg < 2) ? (o32 ? 4 : 2) : 0;
                return true;
            default:
                return false;
        }
    }

    switch (op) {
        case 0x04 ... 0x0b:
        case 0x0e:
        case 0x30 ... 0x3f:
        case 0x77:
        case 0x80 ... 0x8f:
        case 0xa0 ... 0xa2:
        case 0xa8 ... 0xaa:
        case 0xc8 ... 0xcf:
        case 0xff: /* UD0, which only took one later */
            return false;
        case 0x0f: /* 3DNow!: the suffix opcode */
        case 0x70 ... 0x73:
        case 0xa4:
        case 0xac:
        case 0xba:
        case 0xc2:
        case 0xc4 ... 0xc6:
            *imm = 1;
            return true;
        default:
            return true;
    }
}

#ifdef USE_DISASM
/* Lower case, with single spaces, and RETF as it is usually written
   (the library has "ret far"). */
std::string
tidy(const char *text)
{
    std::string lower(text);
    for (char &c : lower) {
        if ((c >= 'A') && (c <= 'Z'))
            c = (char) (c - 'A' + 'a');
    }

    std::vector<std::string> out;
    for (const std::string &w : splitWords(lower)) {
        if (!w.empty())
            out.push_back(w);
    }
    for (size_t i = 0; (i + 1) < out.size(); i++) {
        if (!isPrefixWord(out[i])) {
            if ((out[i] == "ret") && (out[i + 1] == "far")) {
                out[i] = "retf";
                out.erase(out.begin() + (ptrdiff_t) (i + 1));
            }
            break;
        }
    }
    return joinWords(out);
}

/* Decodes the view with the library, feeding it one byte at a time so that
   no byte past the instruction's end is read. */
Result
libDecode(Fetcher &f, const View &v, const debugger_decode_info_t &info, bool code32, uint32_t ip)
{
    /* Extensions that give old encodings new meanings, which none of these
       processors have: BND and NOTRACK prefixes, LZCNT/TZCNT for REP
       BSR/BSF, WBNOINVD for REP WBINVD, and the NOP space hints. */
    static const ZydisDecoderMode modesOff[] = {
        ZYDIS_DECODER_MODE_MPX, ZYDIS_DECODER_MODE_CET, ZYDIS_DECODER_MODE_LZCNT, ZYDIS_DECODER_MODE_TZCNT,
        ZYDIS_DECODER_MODE_WBNOINVD, ZYDIS_DECODER_MODE_CLDEMOTE, ZYDIS_DECODER_MODE_IPREFETCH
    };
    Result                  r;
    ZydisDecoder            dec;
    ZydisFormatter          fmt;
    ZydisDecodedInstruction ix;
    ZydisDecodedOperand     ops[ZYDIS_MAX_OPERAND_COUNT];
    uint8_t                 buf[ZYDIS_MAX_INSTRUCTION_LENGTH];
    char                    text[256];
    const int               headLen = (int) v.head.size();

    if (!ZYAN_SUCCESS(ZydisDecoderInit(&dec, code32 ? ZYDIS_MACHINE_MODE_LEGACY_32 : ZYDIS_MACHINE_MODE_LEGACY_16,
                                       info.stack32 ? ZYDIS_STACK_WIDTH_32 : ZYDIS_STACK_WIDTH_16)))
        return r;
    for (const ZydisDecoderMode m : modesOff)
        ZydisDecoderEnableMode(&dec, m, ZYAN_FALSE);
    if (!ZYAN_SUCCESS(ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL)))
        return r;
    /* Memory operand sizes always, which the Cyrix and UMOV rewrites use,
       and 16-bit addresses as written ([bx+si], not [bx+si*1]). */
    ZydisFormatterSetProperty(&fmt, ZYDIS_FORMATTER_PROP_FORCE_SIZE, ZYAN_TRUE);
    ZydisFormatterSetProperty(&fmt, ZYDIS_FORMATTER_PROP_FORCE_SCALE_ONE, ZYAN_FALSE);
    ZydisFormatterSetProperty(&fmt, ZYDIS_FORMATTER_PROP_ADDR_PADDING_ABSOLUTE, ZYDIS_PADDING_DISABLED);

    for (int i = 0; (i < headLen) && (i < ZYDIS_MAX_INSTRUCTION_LENGTH); i++)
        buf[i] = v.head[i];

    for (int n = (headLen ? headLen : 1); n <= ZYDIS_MAX_INSTRUCTION_LENGTH; n++) {
        if (n > headLen) {
            const int orig = v.resume + (n - 1 - headLen);
            uint8_t   b;
            if (!f.get(orig, &b))
                return r;
            if ((orig == v.modrmAt) && (v.forceReg >= 0))
                b = (uint8_t) ((b & ~0x38) | (v.forceReg << 3));
            buf[n - 1] = b;
        }

        const ZyanStatus st = ZydisDecoderDecodeFull(&dec, buf, (ZyanUSize) n, &ix, ops);
        if (st == ZYDIS_STATUS_NO_MORE_DATA)
            continue;
        if (!ZYAN_SUCCESS(st))
            return r;

        const uint64_t rip = code32 ? ip : (ip & 0xffff);
        if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&fmt, &ix, ops, ix.operand_count_visible, text, sizeof(text), rip, ZYAN_NULL)))
            snprintf(text, sizeof(text), "?");
        r.ok   = true;
        r.len  = v.resume + (ix.length - headLen);
        r.text = tidy(text);
        return r;
    }

    return r;
}
#else
Result
libDecode(Fetcher &, const View &, const debugger_decode_info_t &, bool, uint32_t)
{
    return Result();
}
#endif

/* The prefix bytes before index at, as the library is to see them. */
std::vector<uint8_t>
prefixHead(Fetcher &f, int at, bool nec)
{
    std::vector<uint8_t> head;
    for (int i = 0; i < at; i++) {
        uint8_t b = 0;
        f.get(i, &b); /* already fetched, so never fails */
        if (b == 0xf1)
            b = 0xf0; /* the LOCK alias */
        else if (nec && ((b == 0x64) || (b == 0x65) || (b == 0x0f)))
            continue; /* REPNC/REPC and the NEC's 0F are not the library's */
        head.push_back(b);
    }
    return head;
}

/* NEC REPC/REPNC, which the library does not know, written out. */
std::string
necRepPrefix(Fetcher &f, int at)
{
    std::string s;
    for (int i = 0; i < at; i++) {
        uint8_t b = 0;
        f.get(i, &b); /* already fetched, so never fails */
        if (b == 0x64)
            s += "repnc ";
        else if (b == 0x65)
            s += "repc ";
    }
    return s;
}

std::string
dbText(Fetcher &f, int len)
{
    std::vector<std::string> parts;
    for (int i = 0; i < len; i++) {
        uint8_t b;
        if (f.get(i, &b))
            parts.push_back(strfmt("%02Xh", b));
    }
    return "db " + joinWords(parts, 0, ", ");
}

/* An operand of the V20/V30 ModR/M form at index at, as the library prints
   it, by decoding opcode subst (FE: byte, FF: word) with reg 0 (INC). */
bool
modrmOperand(Fetcher &f, int at, uint8_t subst, const debugger_decode_info_t &info, bool code32, uint32_t ip, std::string *out, int *len)
{
    View v;
    v.head     = prefixHead(f, at - 1, true);
    v.head.push_back(subst);
    v.resume   = at;
    v.modrmAt  = at;
    v.forceReg = 0;
    Result r   = libDecode(f, v, info, code32, ip);
    if (!r.ok)
        return false;
    *out = operandsOf(r.text);
    *len = r.len;
    return true;
}

/* ---- the 8080 ---- */

const char *const i8080Regs[8]  = { "b", "c", "d", "e", "h", "l", "m", "a" };
const char *const i8080Pairs[4] = { "b", "d", "h", "sp" };
const char *const i8080Cond[8]  = { "nz", "z", "nc", "c", "po", "pe", "p", "m" };
const char *const i8080Alu[8]   = { "add", "adc", "sub", "sbb", "ana", "xra", "ora", "cmp" };
const char *const i8080AluI[8]  = { "adi", "aci", "sui", "sbi", "ani", "xri", "ori", "cpi" };

/* As i8080.c runs it, with the NEC's ED escapes. */
void
decode8080(Fetcher &f, DisasmInsn &d)
{
    uint8_t op;
    uint8_t b1 = 0;
    uint8_t b2 = 0;
    int     len = 1;

    if (!f.get(0, &op))
        return;

    auto imm8 = [&](int at) { return f.get(at, &b1); };
    auto imm16 = [&](int at) { return f.get(at, &b1) && f.get(at + 1, &b2); };
    auto w = [&]() { return strfmt("%04Xh", b1 | (b2 << 8)); };

    switch (op >> 6) {
        case 0:
            switch (op & 7) {
                case 0:
                    d.text = "nop";
                    if (op)
                        d.note = "Undocumented NOP";
                    break;
                case 1:
                    if (op & 8)
                        d.text = strfmt("dad %s", i8080Pairs[(op >> 4) & 3]);
                    else {
                        if (!imm16(1))
                            return;
                        len    = 3;
                        d.text = strfmt("lxi %s, %s", i8080Pairs[(op >> 4) & 3], w().c_str());
                    }
                    break;
                case 2: {
                    static const char *const names[8] = { "stax b", "ldax b", "stax d", "ldax d", "shld", "lhld", "sta", "lda" };
                    const int                n        = (op >> 3) & 7;
                    if (n >= 4) {
                        if (!imm16(1))
                            return;
                        len    = 3;
                        d.text = strfmt("%s %s", names[n], w().c_str());
                    } else
                        d.text = std::string(names[n]);
                    break;
                }
                case 3:
                    d.text = strfmt("%s %s", (op & 8) ? "dcx" : "inx", i8080Pairs[(op >> 4) & 3]);
                    break;
                case 4:
                    d.text = strfmt("inr %s", i8080Regs[(op >> 3) & 7]);
                    break;
                case 5:
                    d.text = strfmt("dcr %s", i8080Regs[(op >> 3) & 7]);
                    break;
                case 6:
                    if (!imm8(1))
                        return;
                    len    = 2;
                    d.text = strfmt("mvi %s, %02Xh", i8080Regs[(op >> 3) & 7], b1);
                    break;
                default: {
                    static const char *const names[8] = { "rlc", "rrc", "ral", "rar", "daa", "cma", "stc", "cmc" };
                    d.text                            = std::string(names[(op >> 3) & 7]);
                    break;
                }
            }
            break;
        case 1:
            d.text = (op == 0x76) ? std::string("hlt") : strfmt("mov %s, %s", i8080Regs[(op >> 3) & 7], i8080Regs[op & 7]);
            break;
        case 2:
            d.text = strfmt("%s %s", i8080Alu[(op >> 3) & 7], i8080Regs[op & 7]);
            break;
        default:
            switch (op & 7) {
                case 0:
                    d.text = strfmt("r%s", i8080Cond[(op >> 3) & 7]);
                    break;
                case 1:
                    if (!(op & 8))
                        d.text = strfmt("pop %s", ((op >> 4) & 3) == 3 ? "psw" : i8080Pairs[(op >> 4) & 3]);
                    else if (op == 0xe9)
                        d.text = "pchl";
                    else if (op == 0xf9)
                        d.text = "sphl";
                    else {
                        d.text = "ret";
                        if (op == 0xd9)
                            d.note = "Undocumented RET";
                    }
                    break;
                case 2:
                    if (!imm16(1))
                        return;
                    len    = 3;
                    d.text = strfmt("j%s %s", i8080Cond[(op >> 3) & 7], w().c_str());
                    break;
                case 3:
                    switch (op) {
                        case 0xc3:
                        case 0xcb:
                            if (!imm16(1))
                                return;
                            len    = 3;
                            d.text = strfmt("jmp %s", w().c_str());
                            if (op == 0xcb)
                                d.note = "Undocumented JMP";
                            break;
                        case 0xd3:
                        case 0xdb:
                            if (!imm8(1))
                                return;
                            len    = 2;
                            d.text = strfmt("%s %02Xh", (op == 0xd3) ? "out" : "in", b1);
                            break;
                        case 0xe3:
                            d.text = "xthl";
                            break;
                        case 0xeb:
                            d.text = "xchg";
                            break;
                        case 0xf3:
                            d.text = "di";
                            break;
                        default:
                            d.text = "ei";
                            break;
                    }
                    break;
                case 4:
                    if (!imm16(1))
                        return;
                    len    = 3;
                    d.text = strfmt("c%s %s", i8080Cond[(op >> 3) & 7], w().c_str());
                    break;
                case 5:
                    if (!(op & 8)) {
                        d.text = strfmt("push %s", ((op >> 4) & 3) == 3 ? "psw" : i8080Pairs[(op >> 4) & 3]);
                        break;
                    }
                    if (op == 0xed) {
                        /* The NEC's way back: CALLN and RETEM. */
                        if (!imm8(1))
                            return;
                        if (b1 == 0xed) {
                            if (!f.get(2, &b2))
                                return;
                            len    = 3;
                            d.text = strfmt("calln %02Xh", b2);
                            d.note = "NEC: native-mode interrupt from 8080 mode";
                            break;
                        }
                        if (b1 == 0xfd) {
                            len    = 2;
                            d.text = "retem";
                            d.note = "NEC: return from 8080 mode";
                            break;
                        }
                        if (!f.get(2, &b2))
                            return;
                        len    = 3;
                        d.text = strfmt("call %s", w().c_str());
                        d.note = "Undocumented CALL";
                        break;
                    }
                    if (!imm16(1))
                        return;
                    len    = 3;
                    d.text = strfmt("call %s", w().c_str());
                    if (op != 0xcd)
                        d.note = "Undocumented CALL";
                    break;
                case 6:
                    if (!imm8(1))
                        return;
                    len    = 2;
                    d.text = strfmt("%s %02Xh", i8080AluI[(op >> 3) & 7], b1);
                    break;
                default:
                    d.text = strfmt("rst %d", (op >> 3) & 7);
                    break;
            }
            break;
    }

    d.len = len;
}

/* ---- the NEC V20/V30's own instructions, after its 0F prefix ---- */

void
decodeNec0F(Fetcher &f, int at, const debugger_decode_info_t &info, uint32_t ip, DisasmInsn &d)
{
    uint8_t op;
    if (!f.get(at, &op))
        return;

    /* Which take a ModR/M byte, as the V20/V30 has them. */
    const bool documented = ((op >= 0x10) && (op <= 0x1f)) || (op == 0x28) || (op == 0x2a) || (op == 0x31) || (op == 0x33) ||
                            (op == 0x39) || (op == 0x3b);
    const bool modrm      = debugger_nec_0f_modrm(op);
    const bool imm        = ((op >= 0x18) && (op <= 0x1f)) || (op == 0x39) || (op == 0x3b) || (op == 0xff);
    std::string    name;
    std::string    operands;
    int        len = at + 1;

    switch (op) {
        case 0x10 ... 0x1f: {
            static const char *const names[4] = { "test1", "clr1", "set1", "not1" };
            name                              = std::string(names[(op >> 1) & 3]);
            break;
        }
        case 0x20:
            name = "add4s";
            break;
        case 0x22:
            name = "sub4s";
            break;
        case 0x26:
            name = "cmp4s";
            break;
        case 0x28:
            name = "rol4";
            break;
        case 0x2a:
            name = "ror4";
            break;
        case 0x31:
        case 0x39:
            name = "ins";
            break;
        case 0x33:
        case 0x3b:
            name = "ext";
            break;
        case 0xff:
            name = "brkem";
            break;
        default:
            break;
    }

    if (modrm) {
        int     mlen;
        uint8_t m;
        /* Bit operations take a byte or a word; the rest byte registers. */
        const bool word = ((op >= 0x10) && (op <= 0x1f)) && (op & 1);
        if (!f.get(at + 1, &m))
            return;
        if (((op >= 0x10) && (op <= 0x1f)) || (op == 0x28) || (op == 0x2a)) {
            if (!modrmOperand(f, at + 1, word ? 0xff : 0xfe, info, false, ip, &operands, &mlen))
                return;
            len = mlen;
        } else {
            int extra;
            if (!modrmExtra(f, at + 1, false, &extra))
                return;
            len      = at + 2 + extra;
            operands = ((m >> 6) == 3) ? strfmt("%s, %s", reg8Names[m & 7], reg8Names[(m >> 3) & 7]) : std::string();
        }
        if ((op >= 0x10) && (op <= 0x17))
            operands += ", cl";
    }
    if (imm) {
        uint8_t i8;
        if (!f.get(len, &i8))
            return;
        len++;
        if (op == 0xff)
            operands = strfmt("%02Xh", i8);
        else
            operands += (operands.empty() ? std::string() : std::string(", ")) + strfmt("%d", i8 & ((op & 1) ? 15 : 7));
    }

    d.len  = len;
    d.text = necRepPrefix(f, at) + (name.empty() ? std::string("(undefined)") : name) + (operands.empty() ? std::string() : (' ' + operands));
    if (name.empty())
        d.note = "NEC: undefined 0F opcode, runs as a no-op";
    else if (op == 0xff)
        d.note = "NEC: enter 8080 emulation mode through the vector";
    if (documented != modrm)
        d.note += (d.note.empty() ? std::string() : std::string("; ")) +
                  (modrm ? "86Box's V20/V30 core reads a ModR/M byte here" : "86Box's V20/V30 core reads no ModR/M byte here");
}

std::string
fpuNote(const debugger_decode_info_t &info)
{
    if (info.fpu != DEBUGGER_FPU_NONE)
        return std::string();
    if (info.fpu_trap)
        return "No FPU: raises #NM (CR0.EM/TS set)";
    return "No FPU: reads the operand and does nothing else";
}

/* The x87 opcodes whose meaning depends on which coprocessor it is. */
void
fpuOverride(Fetcher &f, int at, const debugger_decode_info_t &info, DisasmInsn &d)
{
    uint8_t op;
    uint8_t m;

    if ((info.fpu == DEBUGGER_FPU_NONE) || !f.get(at, &op) || (op != 0xdb) || !f.get(at + 1, &m))
        return;

    if ((m == 0xe0) || (m == 0xe1) || (m == 0xe4)) {
        if ((info.fpu == DEBUGGER_FPU_8087) && (m != 0xe4))
            d.text = (m == 0xe0) ? "feni" : "fdisi";
        else if ((info.fpu == DEBUGGER_FPU_287) && (m == 0xe4))
            d.text = "fsetpm";
        else {
            d.text = "fnop";
            d.note = "Ignored by this coprocessor";
        }
    }
}

/* The register forms of the x87 opcodes that repeat others, which the
   coprocessor's tables have (FSTP1, FXCH4, FCOM2 and the like), and the
   Cyrix FTSTP, which the library lacks; for an opcode the coprocessor has. */
void
fpuAlias(Fetcher &f, int at, DisasmInsn &d)
{
    uint8_t op;
    uint8_t m;

    if (!f.get(at, &op) || !f.get(at + 1, &m) || (m < 0xc0) || !d.note.empty())
        return;

    const int rg = (m >> 3) & 7;
    std::string   of;
    switch (op) {
        case 0xd9:
            if (rg == 3) { /* the library has FSTPNCE */
                d.text = renameMnemonic(d.text, "fstp");
                of     = "FSTP (DD D8+i)";
            } else if (m == 0xe6) {
                d.text = "ftstp";
                d.note = "Cyrix: FTST, then pop";
            }
            break;
        case 0xdc:
            if (rg == 2)
                of = "FCOM (D8 D0+i)";
            else if (rg == 3)
                of = "FCOMP (D8 D8+i)";
            break;
        case 0xdd:
            if (rg == 1)
                of = "FXCH (D9 C8+i)";
            break;
        case 0xde:
            if (rg == 2)
                of = "FCOMP (D8 D8+i)";
            break;
        case 0xdf:
            if (rg == 0)
                d.note = "FFREE, then pop (undocumented by Intel)";
            else if (rg == 1)
                of = "FXCH (D9 C8+i)";
            else if ((rg == 2) || (rg == 3))
                of = "FSTP (DD D8+i)";
            break;
        default:
            break;
    }
    if (!of.empty())
        d.note = "Undocumented alias of " + of;
}

} // namespace

Disassembler::Disassembler()
{
    debugger_cpu_info_t cpu;

    debugger_decode_info(&info_);
    debugger_cpu_info(&cpu);
    csBase_ = cpu.cs_base;
}

uint32_t
Disassembler::next(uint32_t ip, int len) const
{
    ip += (len > 0) ? len : 1;
    return info_.wrap16 ? (ip & 0xffff) : ip;
}

DisasmInsn
Disassembler::decode(uint32_t ip, bool current)
{
    DisasmInsn d;
    uint8_t    queue[16];
    const int  queueLen = current ? debugger_prefetch_queue(queue, sizeof(queue)) : 0;
    Fetcher    f(csBase_, ip, info_.wrap16 || (info_.isa == DEBUGGER_ISA_I8080), queue, queueLen);
    const bool x86_86  = (info_.isa == DEBUGGER_ISA_8086) || (info_.isa == DEBUGGER_ISA_80186) || (info_.isa == DEBUGGER_ISA_V30);
    const bool nec     = info_.isa == DEBUGGER_ISA_V30;

    d.lin = f.addr(0);
    d.ip  = ip;

    auto finish = [&]() {
        if (d.len <= 0) {
            if (f.count() == 0) {
                d.text = (f.failCls() == DEBUGGER_MEM_UNMAPPED) ? "(unmapped)" : "(inaccessible)";
                d.len  = 0;
            } else {
                d.len  = f.count();
                d.text = "(runs into memory that cannot be read)";
            }
        }
        for (int i = 0; (i < d.len) && (i < f.count()) && (i < 16); i++)
            d.bytes[i] = f.bytes()[i];
        if ((queueLen > 0) && (d.len > 0)) {
            /* Show where the queue and memory part ways. */
            for (int i = 0; (i < queueLen) && (i < d.len); i++) {
                uint8_t mem;
                uint8_t cls;
                debugger_mem_read(DEBUGGER_SPACE_LINEAR, f.addr(i), 1, &mem, &cls);
                if (((cls == DEBUGGER_MEM_RAM) || (cls == DEBUGGER_MEM_ROM)) && (mem != f.bytes()[i])) {
                    d.note += (d.note.empty() ? std::string() : std::string("; ")) +
                              std::string("Runs the prefetch queue's bytes; memory has since changed");
                    break;
                }
            }
        }
        return d;
    };
    auto invalid = [&](int len, const std::string &why) {
        /* A byte that could not be read is not a bad opcode. */
        if (f.failCls() != DEBUGGER_MEM_RAM) {
            d.len = 0;
            return;
        }
        d.len     = len;
        d.text    = dbText(f, len);
        d.note    = why;
        d.invalid = true;
    };

    if (info_.isa == DEBUGGER_ISA_I8080) {
        decode8080(f, d);
        return finish();
    }

    /* Prefixes, as this processor has them. */
    int     at  = 0;
    int     map = 0;
    int     opsize = info_.code32 ? 0x300 : 0;
    uint8_t op;
    for (;;) {
        if (!f.get(at, &op))
            return finish();

        bool prefix = (op == 0x26) || (op == 0x2e) || (op == 0x36) || (op == 0x3e) || (op == 0xf0) || (op == 0xf2) || (op == 0xf3);
        if (x86_86)
            prefix |= (op == 0xf1) || (nec && ((op == 0x64) || (op == 0x65) || (op == 0x0f)));
        else if (info_.isa == DEBUGGER_ISA_286)
            prefix |= (op == 0xf1) && (debugger_opcode_kind(0, op, 0) == DEBUGGER_OP_LOCK);
        else
            prefix |= (op == 0x64) || (op == 0x65) || (op == 0x66) || (op == 0x67);

        /* The 286 and later raise #UD at an opcode they do not have. */
        if (!x86_86 && !prefix && (debugger_opcode_kind(0, op, opsize) == DEBUGGER_OP_ILLEGAL)) {
            invalid(at + 1, "Not an instruction on this processor: #UD");
            return finish();
        }
        if (!prefix)
            break;

        if (op == 0x66)
            opsize ^= 0x100;
        else if (op == 0x67)
            opsize ^= 0x200;
        else if (op == 0x0f)
            map = 1;
        at++;
        /* Only so many fit in an instruction the library will take; the
           processor takes each prefix on its own anyway. */
        if (at >= 14) {
            d.len  = 1;
            d.text = dbText(f, 1);
            d.note = "A prefix, of a long run of them";
            return finish();
        }
    }

    const bool addr32 = !!(opsize & 0x200);
    const bool code32 = info_.code32;
    View       v;
    v.head   = prefixHead(f, at, nec);
    v.resume = at + 1;

    auto lib = [&](const View &view) {
        Result r = libDecode(f, view, info_, code32, ip);
        if (r.ok) {
            d.len  = r.len;
            d.text = (nec ? necRepPrefix(f, at) : std::string()) + r.text;
            return true;
        }

        /* The library takes LOCK only where the 386 and later do; these
           processors may take it anywhere. Decode without it, then ask
           the processor. */
        View      unlocked = view;
        const int pre      = (int) unlocked.head.size(); /* the prefixes; F1 is F0 by now */
        bool      locked   = false;
        for (int i = 0; i < pre; i++) {
            if (unlocked.head[i] == 0xf0) {
                unlocked.head.erase(unlocked.head.begin() + i);
                locked = true;
                break;
            }
        }
        if (!locked || (f.failCls() != DEBUGGER_MEM_RAM))
            return false;
        r = libDecode(f, unlocked, info_, code32, ip);
        if (!r.ok)
            return false;

        uint8_t b[3] = { 0, 0, 0 };
        for (int i = 0; (i < 3) && ((at + i) < r.len); i++)
            f.get(at + i, &b[i]);
        d.len  = r.len;
        d.text = (nec ? necRepPrefix(f, at) : std::string()) + std::string("lock ") + r.text;
        if (!debugger_lock_legal(b[0], b[1], b[2])) {
            d.invalid = true;
            d.note    = "LOCK on an instruction that cannot be locked: #UD";
        } else if (!x86_86)
            d.note = "The 286 locks any instruction";
        else
            d.note = "The 8086 class locks any instruction";
        return true;
    };
    auto withOp = [&](uint8_t subst, int resume) {
        View w   = v;
        w.head.push_back(subst);
        w.resume = resume;
        return w;
    };
    auto modrmReg = [&](int idx, int *reg, int *mod) {
        uint8_t m;
        if (!f.get(idx, &m))
            return false;
        *reg = (m >> 3) & 7;
        if (mod)
            *mod = m >> 6;
        return true;
    };
    /* An encoding the processor refuses, with its ModR/M byte at modrmAt
       and imm bytes of immediate: all its bytes, where they can be read. */
    auto invalidModrm = [&](int modrmAt, int imm, const std::string &why) {
        int     extra;
        uint8_t b;
        if (!modrmExtra(f, modrmAt, addr32, &extra) || !f.get(modrmAt + extra + imm, &b))
            return;
        invalid(modrmAt + 1 + extra + imm, why);
    };
    /* Opcode op of map, at index opAt, refused: as long as it would be.
       (F6 and F7, whose immediate depends on the reg field, are never.) */
    auto invalidOp = [&](int opMap, uint8_t opc, int opAt, const std::string &why) {
        int imm;
        if (modrmLayout(opMap, opc, 0, !!(opsize & 0x100), &imm))
            invalidModrm(opAt + 1, imm, why);
        else
            invalid(opAt + 1, why);
    };

    /* ---- the 8086, 80186 and NEC V20/V30 ---- */
    if (x86_86) {
        int reg;

        if (map) {
            decodeNec0F(f, at, info_, ip, d);
            return finish();
        }

        if ((op == 0x0f) && !nec) {
            d.len  = at + 1;
            d.text = "pop cs";
            d.note = "8086/80186: 0F is POP CS";
            return finish();
        }
        if ((op >= 0x60) && (op <= 0x6f)) {
            if (info_.isa == DEBUGGER_ISA_8086) {
                lib(withOp(op | 0x10, at + 1));
                d.note = strfmt("8086: %02X is an alias of %02X", op, op | 0x10);
                return finish();
            }
            if ((info_.isa == DEBUGGER_ISA_80186) && (op >= 0x63) && (op <= 0x67)) {
                uint8_t dummy;
                if (!f.get(at + 1, &dummy))
                    return finish();
                invalid(at + 2, "80186: undefined; 86Box skips it and the next byte");
                return finish();
            }
            if (nec && ((op == 0x63) || (op == 0x66) || (op == 0x67))) {
                int extra;
                if (!modrmExtra(f, at + 1, false, &extra))
                    return finish();
                d.len  = at + 2 + extra;
                d.text = (op == 0x63) ? "(undefined)" : "fpo2";
                d.note = (op == 0x63) ? "NEC: undefined, reads its ModR/M operand" : "NEC: coprocessor escape";
                return finish();
            }
        }
        if (info_.isa == DEBUGGER_ISA_8086) {
            if ((op == 0xc0) || (op == 0xc1) || (op == 0xc8) || (op == 0xc9)) {
                lib(withOp(op | 0x02, at + 1));
                d.note = strfmt("8086: %02X is an alias of %02X", op, op | 0x02);
                return finish();
            }
        }
        if (nec && (op == 0xd6)) {
            lib(withOp(0xd7, at + 1));
            d.note = "NEC: D6 is an alias of XLAT";
            return finish();
        }

        switch (op) {
            case 0xd0 ... 0xd3:
            case 0xc0 ... 0xc1:
                /* 8086: C0/C1 were handled above. */
                if (!modrmReg(at + 1, &reg, nullptr))
                    return finish();
                if (reg == 6) {
                    View w     = withOp(op, at + 1);
                    w.modrmAt  = at + 1;
                    w.forceReg = 4;
                    if (lib(w)) {
                        d.text = renameMnemonic(d.text, "setmo");
                        d.note = "Undocumented: sets the operand to all ones";
                    }
                    return finish();
                }
                break;
            case 0x62:
            case 0xc4:
            case 0xc5: {
                /* BOUND, LES and LDS (the 8086 took 62 above). The library
                   would take them as EVEX or VEX with a register operand. */
                int mod;
                if (!modrmReg(at + 1, &reg, &mod))
                    return finish();
                if (mod == 3) {
                    static const char *const regs[8] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di" };
                    d.len  = at + 2;
                    d.text = strfmt("%s %s, ?", (op == 0x62) ? "bound" : ((op == 0xc4) ? "les" : "lds"), regs[reg]);
                    d.note = "Register operand: undefined";
                    return finish();
                }
                break;
            }
            case 0x8c:
            case 0x8e:
                if (!modrmReg(at + 1, &reg, nullptr))
                    return finish();
                if ((reg >= 4) || ((op == 0x8e) && (reg == 1))) {
                    static const char *const segs[4] = { "es", "cs", "ss", "ds" };
                    View                     w       = withOp(op, at + 1);
                    w.modrmAt                        = at + 1;
                    w.forceReg                       = ((reg & 3) == 1) ? 3 : (reg & 3); /* the library takes no MOV CS */
                    if (lib(w)) {
                        if ((reg & 3) == 1) {
                            const size_t ds = findWord(d.text, "ds");
                            if (ds != std::string::npos)
                                d.text.replace(ds, 2, segs[1]);
                        }
                        if (reg >= 4)
                            d.note = strfmt("Segment register %d is an alias of %s", reg, segs[reg & 3]);
                        else
                            d.note = "8086-class: loads CS";
                    }
                    return finish();
                }
                break;
            case 0x8f:
            case 0xc6:
            case 0xc7:
                if (!modrmReg(at + 1, &reg, nullptr))
                    return finish();
                if (reg) {
                    View w     = withOp(op, at + 1);
                    w.modrmAt  = at + 1;
                    w.forceReg = 0;
                    if (lib(w))
                        d.note = strfmt("ModR/M reg %d is an alias of reg 0", reg);
                    return finish();
                }
                break;
            case 0xfe:
            case 0xff:
                if (!modrmReg(at + 1, &reg, nullptr))
                    return finish();
                if ((op == 0xfe) && (reg >= 2)) {
                    static const char *const names[8] = { "inc", "dec", "call", "callf", "jmp", "jmpf", "push", "push" };
                    View                     w        = withOp(0xfe, at + 1);
                    w.modrmAt                         = at + 1;
                    w.forceReg                        = 0;
                    if (lib(w)) {
                        d.text = renameMnemonic(d.text, std::string(names[reg]));
                        d.note = strfmt("FE /%d: FF /%d on a byte", reg, reg);
                    }
                    return finish();
                }
                if ((op == 0xff) && (reg == 7)) {
                    View w     = withOp(0xff, at + 1);
                    w.modrmAt  = at + 1;
                    w.forceReg = 6;
                    if (lib(w))
                        d.note = "FF /7 is an alias of PUSH (FF /6)";
                    return finish();
                }
                break;
            case 0xd8 ... 0xdf: {
                if (!lib(withOp(op, at + 1))) {
                    int extra;
                    if (!modrmExtra(f, at + 1, false, &extra))
                        return finish();
                    d.len  = at + 2 + extra;
                    d.text = strfmt("esc %d", op & 7);
                }
                fpuOverride(f, at, info_, d);
                if (info_.fpu != DEBUGGER_FPU_NONE) {
                    uint8_t m;
                    if (f.get(at + 1, &m) && (debugger_fpu_kind(op, m, 0) == DEBUGGER_OP_ILLEGAL))
                        d.note = "Not on this coprocessor: reads the operand and does nothing";
                    else
                        fpuAlias(f, at, d);
                }
                if (d.note.empty())
                    d.note = fpuNote(info_);
                return finish();
            }
            default:
                break;
        }

        if (!lib(withOp(op, at + 1)))
            invalid(at + 1, "Not decodable");
        return finish();
    }

    /* ---- the 286 and later, from their opcode tables ---- */
    if (op == 0x0f) {
        uint8_t op2;
        if (!f.get(at + 1, &op2))
            return finish();

        const int kind = debugger_opcode_kind(1, op2, opsize);
        /* MOV to and from the test registers, which the library lacks: the
           ModR/M byte always names a register, as for CRn and DRn. */
        if (((op2 == 0x24) || (op2 == 0x26)) && (kind != DEBUGGER_OP_ILLEGAL)) {
            static const char *const regs[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
            uint8_t                  m;
            if (!f.get(at + 2, &m))
                return finish();
            d.len  = at + 3;
            d.text = (op2 == 0x24) ? strfmt("mov %s, tr%d", regs[m & 7], (m >> 3) & 7)
                                   : strfmt("mov tr%d, %s", (m >> 3) & 7, regs[m & 7]);
            return finish();
        }
        /* The ModR/M forms of these that the processor refuses, as 86Box's
           handlers do: the library takes some of them as later
           instructions (MONITOR, SERIALIZE, RDTSCP, LFENCE, RDRAND). */
        /* 3DNow!: the suffix after the operand picks the instruction. */
        if ((kind == DEBUGGER_OP_NORMAL) && (op2 == 0x0f)) {
            int     extra;
            uint8_t suffix;
            if (!modrmExtra(f, at + 2, addr32, &extra) || !f.get(at + 3 + extra, &suffix))
                return finish();
            if (debugger_3dnow_kind(suffix) == DEBUGGER_OP_ILLEGAL) {
                invalid(at + 4 + extra, strfmt("3DNow! opcode %02Xh is not on this processor: #UD", suffix));
                return finish();
            }
        }
        if (kind == DEBUGGER_OP_NORMAL) {
            int           reg;
            int           mod;
            std::string       refused;
            const std::string badReg = "ModR/M reg %1: #UD";
            const std::string regOp  = "Register operand: #UD";
            switch (op2) {
                case 0x00:
                case 0x01:
                case 0x0d:
                case 0x71 ... 0x73:
                case 0xae:
                case 0xba:
                case 0xc7:
                    if (!modrmReg(at + 2, &reg, &mod))
                        return finish();
                    break;
                default:
                    reg = mod = -1;
                    break;
            }
            switch (op2) {
                case 0x00: /* SLDT, STR, LLDT, LTR, VERR, VERW */
                    if (reg >= 6)
                        refused = badReg;
                    break;
                case 0x01: /* SGDT, SIDT, LGDT, LIDT and INVLPG take memory; no /5 */
                    if (reg == 5)
                        refused = badReg;
                    else if ((reg == 7) && !info_.invlpg)
                        refused = "No INVLPG on this processor: #UD";
                    else if ((mod == 3) && ((reg <= 3) || (reg == 7)))
                        refused = regOp;
                    break;
                case 0x0d: /* PREFETCH (the P6 hint NOPs are their own kind) */
                    if (mod == 3)
                        refused = regOp;
                    break;
                case 0x71 ... 0x73: /* PSRLx, PSRAx, PSLLx, on a register */
                    if ((reg != 2) && (reg != 6) && ((reg != 4) || (op2 == 0x73)))
                        refused = badReg;
                    else if (mod != 3)
                        refused = "Memory operand: #UD";
                    break;
                case 0xae: /* FXSAVE, FXRSTOR */
                    if (reg > 1)
                        refused = badReg;
                    else if (mod == 3)
                        refused = regOp;
                    break;
                case 0xba: /* BT, BTS, BTR, BTC */
                    if (reg < 4)
                        refused = badReg;
                    break;
                case 0xc7: /* CMPXCHG8B */
                    if ((mod == 3) && info_.vpc) {
                        d.len  = at + 3;
                        d.text = dbText(f, d.len);
                        d.note = "Virtual PC: CMPXCHG8B on a register is a no-op (the BIOS's host call)";
                        return finish();
                    }
                    if (reg != 1)
                        refused = badReg;
                    else if (mod == 3)
                        refused = regOp;
                    break;
                default:
                    break;
            }
            if (!refused.empty()) {
                const size_t arg = refused.find("%1");
                if (arg != std::string::npos)
                    refused.replace(arg, 2, std::to_string(reg));
                invalidOp(1, op2, at + 1, refused);
                return finish();
            }
            if (op2 == 0x0d) {
                /* The library has /2 as PREFETCHWT1; all but /1 are PREFETCH. */
                if (lib(withOp(0x0f, at + 1))) {
                    d.text = renameMnemonic(d.text, (reg == 1) ? "prefetchw" : "prefetch");
                    if (reg > 1)
                        d.note = strfmt("0F 0D /%d is an alias of PREFETCH (0F 0D /0)", reg);
                }
                return finish();
            }
        }
        switch (kind) {
            case DEBUGGER_OP_ILLEGAL:
                invalidOp(1, op2, at + 1, "Not an instruction on this processor: #UD");
                return finish();
            case DEBUGGER_OP_LOADALL:
            case DEBUGGER_OP_LOADALL386:
            case DEBUGGER_OP_SMINT:
            case DEBUGGER_OP_BB0_RESET:
            case DEBUGGER_OP_BB1_RESET:
            case DEBUGGER_OP_CPU_WRITE:
            case DEBUGGER_OP_CPU_READ: {
                static const char *const names[] = { "loadall", "loadall", "smint", "bb0_reset", "bb1_reset", "cpu_write", "cpu_read" };
                const int idx = (kind == DEBUGGER_OP_LOADALL) ? 0 : (kind == DEBUGGER_OP_LOADALL386) ? 1 : (kind == DEBUGGER_OP_SMINT) ? 2 :
                                (kind - DEBUGGER_OP_BB0_RESET + 3);
                d.len  = at + 2;
                d.text = std::string(names[idx]);
                if (kind == DEBUGGER_OP_LOADALL)
                    d.note = "286 LOADALL (later SYSCALL)";
                else if (kind == DEBUGGER_OP_LOADALL386)
                    d.note = "386 LOADALL (later SYSRET)";
                else
                    d.note = "Cyrix system management";
                return finish();
            }
            case DEBUGGER_OP_VPCEXT: {
                uint8_t a;
                uint8_t b;
                if (!f.get(at + 2, &a) || !f.get(at + 3, &b))
                    return finish();
                d.len  = at + 4;
                d.text = strfmt("vpcext %02Xh, %02Xh", a, b);
                d.note = info_.vpc ? "Virtual PC extension" : "Virtual PC extension: #UD on other machines";
                return finish();
            }
            case DEBUGGER_OP_UMOV:
                /* 0F 10-13 are MOV 88-8B on the bus as user memory. */
                if (lib(withOp(0x88 + (op2 - 0x10), at + 2))) {
                    d.text = renameMnemonic(d.text, "umov");
                    d.note = "UMOV (later SSE moves)";
                }
                return finish();
            case DEBUGGER_OP_RDSHR:
            case DEBUGGER_OP_WRSHR:
            case DEBUGGER_OP_SVDC:
            case DEBUGGER_OP_RSDC:
            case DEBUGGER_OP_SVLDT:
            case DEBUGGER_OP_RSLDT:
            case DEBUGGER_OP_SVTS:
            case DEBUGGER_OP_RSTS: {
                static const char *const names[] = { "rdshr", "wrshr", "svdc", "rsdc", "svldt", "rsldt", "svts", "rsts" };
                const int                idx     = (kind <= DEBUGGER_OP_WRSHR) ? (kind - DEBUGGER_OP_RDSHR) : (kind - DEBUGGER_OP_SVDC + 2);
                View                     w;
                if ((kind == DEBUGGER_OP_SVDC) || (kind == DEBUGGER_OP_RSDC)) {
                    /* m80 and a segment register, as MOV Ew, Sw has them. */
                    w = withOp(0x8c, at + 2);
                } else {
                    /* One operand, as PUSH Ev has it. */
                    w          = withOp(0xff, at + 2);
                    w.modrmAt  = at + 2;
                    w.forceReg = 6;
                }
                if (lib(w)) {
                    std::string ops = operandsOf(d.text);
                    if (kind >= DEBUGGER_OP_SVDC)
                        makeTbytePtr(ops);
                    if (kind == DEBUGGER_OP_RSDC) {
                        const size_t comma = ops.rfind(", ");
                        if ((comma != std::string::npos) && (comma > 0))
                            ops = ops.substr(comma + 2) + ", " + ops.substr(0, comma);
                    }
                    d.text = std::string(names[idx]) + ' ' + ops;
                    d.note = "Cyrix system management (later VMX/MMX/GETSEC)";
                }
                return finish();
            }
            case DEBUGGER_OP_EMMI: {
                /* mm, mm/m64 as the MMX instructions have them (PADDB, which
                   the library decodes, stands in), without the prefixes
                   that would make that SSE. */
                static const char *const names[16] = { "paveb", "paddsiw", "pmagw", nullptr, "pdistib", "psubsiw", nullptr, nullptr,
                                                       "pmvzb", "pmulhrwc", "pmvnzb", "pmvlzb", "pmvgezb", "pmulhriw", "pmachriw", nullptr };
                const char              *name      = names[op2 & 15];
                int                      reg;
                int                      mod;
                if (!modrmReg(at + 2, &reg, &mod))
                    return finish();
                if (!name) {
                    invalidOp(1, op2, at + 1, "Not an instruction on this processor: #UD");
                    return finish();
                }
                /* Memory only: PDISTIB, PMACHRIW and the conditional moves. */
                if ((mod == 3) && ((op2 == 0x54) || (op2 == 0x58) || ((op2 >= 0x5a) && (op2 != 0x5d)))) {
                    invalidOp(1, op2, at + 1, "Register operand: #UD");
                    return finish();
                }
                View w = v;
                for (size_t i = 0; i < w.head.size();) {
                    if ((w.head[i] == 0x66) || (w.head[i] == 0xf2) || (w.head[i] == 0xf3))
                        w.head.erase(w.head.begin() + (ptrdiff_t) i);
                    else
                        i++;
                }
                w.head.push_back(0x0f);
                w.head.push_back(0xfc);
                w.resume = at + 2;
                if (!lib(w))
                    return finish();
                d.text = renameMnemonic(d.text, std::string(name));
                /* Some write, or test, the other register of the pair. */
                switch (op2) {
                    case 0x51:
                    case 0x54:
                    case 0x55:
                    case 0x5d:
                    case 0x5e:
                        d.note = strfmt("Cyrix EMMI: the result goes to mm%d", reg ^ 1);
                        break;
                    case 0x58:
                    case 0x5a:
                    case 0x5b:
                    case 0x5c: {
                        static const char *const conds[4] = { "are zero", "are not zero", "are negative", "are not negative" };
                        const int                c        = (op2 == 0x58) ? 0 : (op2 - 0x59);
                        d.note                            = strfmt("Cyrix EMMI: moves the bytes where those of mm%d %s", reg ^ 1, conds[c]);
                        break;
                    }
                    default:
                        d.note = "Cyrix EMMI";
                        break;
                }
                if (!info_.emmi) {
                    d.invalid = true;
                    d.note    = "Cyrix EMMI, which is off (CCR7 bit 0): #UD";
                }
                return finish();
            }
            case DEBUGGER_OP_HINT_NOP:
                if (lib(withOp(0x0f, at + 1)) && !(d.text.compare(0, 3, "nop") == 0))
                    d.text = renameMnemonic(d.text, "nop");
                d.note = "Hint NOP on this processor";
                return finish();
            default: {
                /* 86Box has no SSE: 66 only picks the operand size half of
                   the tables, and REP before 0F runs the 0F opcode as if it
                   were not there (opREPE/opREPNE), so these are the MMX
                   instructions, where the library would decode SSE ones. */
                View       w        = withOp(0x0f, at + 1);
                const bool sseSpace = ((op2 >= 0x50) && (op2 <= 0x7f)) || ((op2 >= 0xc2) && (op2 <= 0xc6)) || (op2 >= 0xd0);
                bool       dropped  = false;
                if (sseSpace) {
                    for (size_t i = 0; i + 1 < w.head.size();) {
                        if ((w.head[i] == 0x66) || (w.head[i] == 0xf2) || (w.head[i] == 0xf3)) {
                            w.head.erase(w.head.begin() + (ptrdiff_t) i);
                            dropped = true;
                        } else
                            i++;
                    }
                }
                if (!lib(w))
                    invalidOp(1, op2, at + 1, "In the opcode tables, but not decodable");
                else if (dropped)
                    d.note = "No SSE: the 66/F2/F3 prefix does not make this an SSE instruction";
                return finish();
            }
        }
    }

    int reg;
    int mod;
    switch (op) {
        case 0x62:
        case 0xc4:
        case 0xc5:
            /* The library would take these as EVEX or VEX with a register
               operand; here they are BOUND, LES and LDS, which raise #UD. */
            if (!modrmReg(at + 1, &reg, &mod))
                return finish();
            if (mod == 3) {
                invalid(at + 2, "Register operand: #UD");
                return finish();
            }
            break;
        case 0x8f:
        case 0xc6:
        case 0xc7:
            /* XOP, XABORT and XBEGIN to the library. */
            if (!modrmReg(at + 1, &reg, nullptr))
                return finish();
            if (reg) {
                invalidOp(0, op, at, strfmt("ModR/M reg %d: #UD", reg));
                return finish();
            }
            break;
        case 0xfe:
        case 0xff:
            /* INC and DEC only on a byte; no FF /7. */
            if (!modrmReg(at + 1, &reg, nullptr))
                return finish();
            if ((op == 0xfe) ? (reg >= 2) : (reg == 7)) {
                invalidOp(0, op, at, strfmt("ModR/M reg %d: #UD", reg));
                return finish();
            }
            break;
        case 0x8c:
        case 0x8e:
            if (!modrmReg(at + 1, &reg, nullptr))
                return finish();
            if ((reg >= ((info_.isa == DEBUGGER_ISA_386) ? 6 : 4)) || ((op == 0x8e) && (reg == 1))) {
                invalidOp(0, op, at, "No such segment register here: #UD");
                return finish();
            }
            break;
        case 0xd8 ... 0xdf: {
            uint8_t m;
            if (!f.get(at + 1, &m))
                return finish();
            if (!lib(withOp(op, at + 1))) {
                int extra;
                if (!modrmExtra(f, at + 1, addr32, &extra))
                    return finish();
                d.len  = at + 2 + extra;
                d.text = strfmt("esc %d", op & 7);
            }
            fpuOverride(f, at, info_, d);
            /* The coprocessor's own tables say which it has: the library
               knows the later ones (FCMOVcc, FCOMI, FUCOMI and the like). */
            if ((info_.fpu != DEBUGGER_FPU_NONE) && (debugger_fpu_kind(op, m, addr32) == DEBUGGER_OP_ILLEGAL)) {
                if (info_.fpu_ud) {
                    invalid(d.len, "Not on this coprocessor: #UD");
                    return finish();
                }
                d.note = "Not on this coprocessor: reads the operand and does nothing";
            } else if (info_.fpu != DEBUGGER_FPU_NONE)
                fpuAlias(f, at, d);
            if (d.note.empty())
                d.note = fpuNote(info_);
            return finish();
        }
        default:
            break;
    }

    if (!lib(withOp(op, at + 1)))
        invalidOp(0, op, at, "In the opcode tables, but not decodable");
    return finish();
}
