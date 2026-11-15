/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the New Image dialog, like the Qt
 *          NewFloppyDialog (qt_newfloppydialog.cpp). The image is written
 *          by a worker thread while the dialog shows its progress, as Qt
 *          does with a QProgressDialog.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Button.h>
#include <Directory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <OS.h>
#include <Path.h>
#include <StatusBar.h>
#include <TextControl.h>
#include <Window.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fnmatch.h>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "haiku_newfloppydialog.hpp"
#include "haiku_settings_common.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/random.h>
#include <86box/scsi_device.h>
#include <86box/disksizes.h>
#include <86box/rdisk.h>
#include <86box/mo.h>
#include <86box/scsi_tape.h>
#include <86box/version.h>
}

enum : uint32 {
    MSG_NF_IGNORE   = 'nfig', /* a pop-up that needs no reaction */
    MSG_NF_OK       = 'nfok',
    MSG_NF_CANCEL   = 'nfcn',
    MSG_NF_BROWSE   = 'nfbr',
    MSG_NF_FILE     = 'nffp', /* from the save panel */
    MSG_NF_NAME     = 'nfnm', /* the file name was edited */
    MSG_NF_PROGRESS = 'nfpr', /* from the worker: "value", "max" */
    MSG_NF_DONE     = 'nfdn', /* from the worker: "ok" */
};

static const char *rpm_modes[] = {
    "Perfect RPM",
    "1% below perfect RPM",
    "1.5% below perfect RPM",
    "2% below perfect RPM",
};

/* The first KNOWN_FLOPPY_TYPES entries of disk_sizes[]. */
static const char *floppy_types[] = {
    "160 KB",
    "180 KB",
    "320 KB",
    "360 KB",
    "640 KB",
    "720 KB",
    "1.2 MB",
    "1.25 MB",
    "1.44 MB",
    "DMF (cluster 1024)",
    "DMF (cluster 2048)",
    "2.88 MB",
};

typedef std::function<void(uint32_t value, uint32_t max)> progress_fn;

static std::string
to_lower(std::string s)
{
    for (char &c : s)
        c = (char) tolower((unsigned char) c);
    return s;
}

/* Like QFileInfo::suffix(): what follows the last dot of the file name. */
static std::string
file_suffix(const std::string &path)
{
    const size_t slash = path.rfind('/');
    const size_t dot   = path.rfind('.');
    if ((dot == std::string::npos) || ((slash != std::string::npos) && (dot < slash)))
        return std::string();
    return to_lower(path.substr(dot + 1));
}

/* ------------------------------------------------------------- filter */

void
ImageRefFilter::SetPatterns(const std::vector<std::string> &patterns)
{
    globs.clear();
    for (const std::string &p : patterns) {
        if (p == "*") {
            globs.clear();
            return;
        }
        globs.push_back("*." + to_lower(p));
    }
}

bool
ImageRefFilter::Filter(const entry_ref *ref, BNode *, struct stat_beos *, const char *)
{
    if (globs.empty())
        return true;

    BEntry entry(ref, true);
    if (entry.IsDirectory())
        return true;

    const std::string name = to_lower(ref->name);
    for (const std::string &glob : globs) {
        if (fnmatch(glob.c_str(), name.c_str(), 0) == 0)
            return true;
    }
    return false;
}

/* ------------------------------------------------------ image writers */

static inline void
put16(uint8_t *p, uint16_t v)
{
    memcpy(p, &v, sizeof(v));
}

static inline void
put32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}

static inline void
put64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, sizeof(v));
}

/* A file opened for writing that remembers whether every write worked. */
class ImageFile {
public:
    explicit ImageFile(const std::string &fn)
        : fp(plat_fopen(fn.c_str(), "wb"))
        , ok(fp != nullptr)
    {
    }

    ~ImageFile()
    {
        if (fp != nullptr)
            fclose(fp);
    }

    bool IsOpen() const { return fp != nullptr; }

    void Write(const void *data, size_t len)
    {
        if (ok && (len > 0) && (fwrite(data, 1, len, fp) != len))
            ok = false;
    }

    bool Close()
    {
        if (fp != nullptr) {
            if (fclose(fp) != 0)
                ok = false;
            fp = nullptr;
        }
        return ok;
    }

private:
    FILE *fp;
    bool  ok;
};

static bool
create_86f(const std::string &filename, const disk_size_t &disk_size, uint8_t rpm_mode)
{
    const uint32_t magic          = 0x46423638;
    const uint16_t version        = 0x020C;
    uint16_t       dflags         = 0;
    uint16_t       tflags         = 0;
    const uint32_t index_hole_pos = 0;
    uint32_t       tarray[512];
    uint32_t       array_size;
    uint32_t       shift = 0;

    dflags = 0;                             /* Has surface data? - Assume no for now. */
    dflags |= (disk_size.hole << 1);        /* Hole */
    dflags |= ((disk_size.sides - 1) << 3); /* Sides. */
    dflags |= (0 << 4);                     /* Write protect? - Assume no for now. */
    dflags |= (rpm_mode << 5);              /* RPM mode. */
    dflags |= (0 << 7);                     /* Has extra bit cells? - Assume no for now. */

    tflags = disk_size.data_rate;        /* Data rate. */
    tflags |= (disk_size.encoding << 3); /* Encoding. */
    tflags |= (disk_size.rpm << 5);      /* RPM. */

    if (disk_size.hole == 2) {
        static const uint32_t sizes[] = { 50000, 50500, 50750, 51000 };
        array_size                    = sizes[(rpm_mode <= 3) ? rpm_mode : 0];
    } else {
        static const uint32_t sizes[] = { 25000, 25250, 25374, 25750 };
        array_size                    = sizes[(rpm_mode <= 3) ? rpm_mode : 0];
    }

    std::vector<uint8_t> empty(array_size, 0);
    memset(tarray, 0, sizeof(tarray));

    ImageFile file(filename);
    if (!file.IsOpen())
        return false;

    file.Write(&magic, 4);
    file.Write(&version, 2);
    file.Write(&dflags, 2);

    const uint32_t track_size = array_size + 6;
    const uint32_t track_base = 8 + ((disk_size.sides == 2) ? 2048 : 1024);

    if (disk_size.tracks <= 43)
        shift = 1;

    const int tracks = (disk_size.tracks * disk_size.sides) << shift;
    for (int i = 0; (i < tracks) && (i < 512); i++)
        tarray[i] = track_base + (i * track_size);

    file.Write(tarray, (disk_size.sides == 2) ? 2048 : 1024);

    for (int i = 0; i < tracks; i++) {
        file.Write(&tflags, 2);
        file.Write(&index_hole_pos, 4);
        file.Write(empty.data(), array_size);
    }

    return file.Close();
}

/* The boot loader of a new floppy: it tries to boot the first hard disk,
   printing an error message if unable to do so. */
static const uint8_t floppy_boot_code[] = {
    0xFA, 0x31, 0xC0, 0x8E, 0xD8, 0x8E, 0xC0, 0x8E, 0xD0, 0xBC, 0x00, 0x7C,
    0xFB, 0xBE, 0x00, 0x7C, 0xBF, 0x00, 0x06, 0xB9, 0x00, 0x01, 0xF3, 0xA5,
    0xEA, 0x5B, 0x06, 0x00, 0x00, 0xB2, 0x80, 0x31, 0xC0, 0xCD, 0x13, 0x72,
    0x14, 0xB8, 0x01, 0x02, 0xB9, 0x01, 0x00, 0xB6, 0x00, 0xBB, 0x00, 0x7C,
    0xCD, 0x13, 0x72, 0x05, 0xEA, 0x00, 0x7C, 0x00, 0x00, 0xBE, 0x89, 0x7C,
    0xAC, 0x84, 0xC0, 0x74, 0x06, 0xB4, 0x0E, 0xCD, 0x10, 0xEB, 0xF5, 0xFA,
    0xF4, 0xEB, 0xFC,
    'F', 'a', 'i', 'l', 'e', 'd', ' ', 't', 'o', ' ', 'f', 'i', 'n', 'd', ' ',
    'a', ' ', 'v', 'a', 'l', 'i', 'd', ' ', 'b', 'o', 'o', 't', ' ',
    'd', 'e', 'v', 'i', 'c', 'e', '.', '\r', '\n',
};

static bool
create_sector_image(const std::string &filename, const disk_size_t &disk_size, bool fdi)
{
    const uint16_t base = 0x1000;

    ImageFile file(filename);
    if (!file.IsOpen())
        return false;

    const uint32_t sector_bytes  = (128 << disk_size.sector_len);
    uint32_t       total_sectors = disk_size.sides * disk_size.tracks * disk_size.sectors;
    if (total_sectors > ZIP_SECTORS)
        total_sectors = ZIP_250_SECTORS;
    const uint64_t total_size     = (uint64_t) total_sectors * sector_bytes;
    const uint32_t root_dir_bytes = (disk_size.root_dir_entries << 5);
    const uint32_t fat_size       = (disk_size.spfat * sector_bytes);
    const uint32_t fat1_offs      = sector_bytes;
    const uint32_t fat2_offs      = fat1_offs + fat_size;
    const uint32_t zero_bytes     = fat2_offs + fat_size + root_dir_bytes;

    if (fdi) {
        std::vector<uint8_t> header(base, 0);
        put32(&header[0x08], (uint32_t) base);
        put32(&header[0x0C], (uint32_t) total_size);
        put16(&header[0x10], (uint16_t) sector_bytes);
        header[0x14] = (uint8_t) disk_size.sectors;
        header[0x18] = (uint8_t) disk_size.sides;
        header[0x1C] = (uint8_t) disk_size.tracks;
        file.Write(header.data(), base);
    }

    std::vector<uint8_t> image(total_size, 0);
    uint8_t             *empty = image.data();

    if (zero_bytes < total_size)
        memset(empty + zero_bytes, 0xF6, total_size - zero_bytes);

    empty[0x00] = 0xEB; /* Jump to make MS-DOS happy. */
    empty[0x01] = 0x3C;
    empty[0x02] = 0x90;

    memcpy(&empty[0x03], "86BOX5.0", 8); /* OEM ID. */

    put16(&empty[0x0B], (uint16_t) sector_bytes);
    empty[0x0D] = (uint8_t) disk_size.spc;
    put16(&empty[0x0E], 1);
    empty[0x10] = (uint8_t) disk_size.num_fats;
    put16(&empty[0x11], (uint16_t) disk_size.root_dir_entries);
    put16(&empty[0x13], (uint16_t) total_sectors);
    empty[0x15] = (uint8_t) disk_size.media_desc;
    put16(&empty[0x16], (uint16_t) disk_size.spfat);
    empty[0x18] = (uint8_t) disk_size.sectors;
    empty[0x1A] = (uint8_t) disk_size.sides;

    empty[0x26] = 0x29; /* ')' followed by randomly-generated volume serial number. */
    empty[0x27] = random_generate();
    empty[0x28] = random_generate();
    empty[0x29] = random_generate();
    empty[0x2A] = random_generate();

    memset(&empty[0x2B], 0x20, 11);
    memcpy(&empty[0x36], "FAT12   ", 8);

    memcpy(&empty[0x3E], floppy_boot_code, sizeof(floppy_boot_code));

    empty[0x1FE] = 0x55;
    empty[0x1FF] = 0xAA;

    empty[fat1_offs + 0x00] = empty[fat2_offs + 0x00] = empty[0x15];
    empty[fat1_offs + 0x01] = empty[fat2_offs + 0x01] = 0xFF;
    empty[fat1_offs + 0x02] = empty[fat2_offs + 0x02] = 0xFF;

    file.Write(empty, total_size);
    return file.Close();
}

static bool
create_rdisk_image(const std::string &filename, uint32_t total_sectors, uint32_t sector_bytes, bool zdi,
                   const progress_fn &progress)
{
    const uint16_t base = 0x1000;

    ImageFile file(filename);
    if (!file.IsOpen())
        return false;

    const uint64_t total_size = (uint64_t) total_sectors * sector_bytes;
    const uint32_t pbar_max   = (uint32_t) ((total_size + 1048575) >> 20);

    if (zdi) {
        std::vector<uint8_t> header(base, 0);
        put32(&header[0x08], (uint32_t) base);
        put32(&header[0x0C], (uint32_t) total_size);
        put16(&header[0x10], (uint16_t) sector_bytes);
        header[0x14] = 63;
        header[0x18] = 255;
        header[0x1C] = (uint8_t) (total_sectors / (63 * 255));
        file.Write(header.data(), base);
    }

    std::vector<uint8_t> block(1048576, 0);
    uint8_t             *empty = block.data();

    /* The PC floppy formats a drive such as the LS-120 takes get the same
       BPB as a floppy image of that size, from the floppy table. */
    const disk_size_t *fd = nullptr;
    for (int k = 0; k < KNOWN_DISK_SIZES; k++) {
        const disk_size_t &ds = disk_sizes[k];
        if ((ds.sides <= 2) && ((uint32_t) (ds.sides * ds.tracks * ds.sectors) == total_sectors)
            && ((128u << ds.sector_len) == sector_bytes)) {
            fd = &ds;
            break;
        }
    }

    if (total_sectors == ZIP_SECTORS) {
        /* ZIP 100 */
        /* MBR */
        put64(&empty[0x0000], 0x2054524150492EEBULL);
        put64(&empty[0x0008], 0x3930302065646F63ULL);
        put64(&empty[0x0010], 0x67656D6F49202D20ULL);
        put64(&empty[0x0018], 0x726F70726F432061ULL);
        put64(&empty[0x0020], 0x202D206E6F697461ULL);
        put64(&empty[0x0028], 0x30392F33322F3131ULL);

        put64(&empty[0x01AE], 0x0116010100E90644ULL);
        put64(&empty[0x01B6], 0xED08BBE5014E0135ULL);
        put64(&empty[0x01BE], 0xFFFFFE06FFFFFE80ULL);
        put64(&empty[0x01C6], 0x0002FFE000000020ULL);

        put16(&empty[0x01FE], 0xAA55);

        /* 31 sectors filled with 0x48 */
        memset(&empty[0x0200], 0x48, 0x3E00);

        /* Boot sector */
        put64(&empty[0x4000], 0x584F4236389058EBULL);
        put64(&empty[0x4008], 0x0008040200302E35ULL);
        put64(&empty[0x4010], 0x00C0F80000020002ULL);
        put64(&empty[0x4018], 0x0000002000FF003FULL);
        put32(&empty[0x4020], 0x0002FFE0);
        put16(&empty[0x4024], 0x0080);

        empty[0x4026] = 0x29; /* ')' followed by randomly-generated volume serial number. */
        empty[0x4027] = random_generate();
        empty[0x4028] = random_generate();
        empty[0x4029] = random_generate();
        empty[0x402A] = random_generate();

        memset(&empty[0x402B], 0x00, 0x000B);
        memcpy(&empty[0x4036], "FAT16   ", 8);

        empty[0x41FE] = 0x55;
        empty[0x41FF] = 0xAA;

        empty[0x5000] = empty[0x1D000] = empty[0x4015];
        empty[0x5001] = empty[0x1D001] = 0xFF;
        empty[0x5002] = empty[0x1D002] = 0xFF;
        empty[0x5003] = empty[0x1D003] = 0xFF;
    } else if (total_sectors == ZIP_250_SECTORS) {
        /* ZIP 250 */
        /* MBR */
        put64(&empty[0x0000], 0x2054524150492EEBULL);
        put64(&empty[0x0008], 0x3930302065646F63ULL);
        put64(&empty[0x0010], 0x67656D6F49202D20ULL);
        put64(&empty[0x0018], 0x726F70726F432061ULL);
        put64(&empty[0x0020], 0x202D206E6F697461ULL);
        put64(&empty[0x0028], 0x30392F33322F3131ULL);

        put64(&empty[0x01AE], 0x0116010100E900E9ULL);
        put64(&empty[0x01B6], 0x2E32A7AC014E0135ULL);

        put64(&empty[0x01EE], 0xEE203F0600010180ULL);
        put64(&empty[0x01F6], 0x000777E000000020ULL);
        put16(&empty[0x01FE], 0xAA55);

        /* 31 sectors filled with 0x48 */
        memset(&empty[0x0200], 0x48, 0x3E00);

        /* The second sector begins with some strange data in the
           reference image. */
        put64(&empty[0x0200], 0x3831393230334409ULL);
        put64(&empty[0x0208], 0x6A57766964483130ULL);
        put64(&empty[0x0210], 0x3C3A34676063653FULL);
        put64(&empty[0x0218], 0x586A56A8502C4161ULL);
        put64(&empty[0x0220], 0x6F2D702535673D6CULL);
        put64(&empty[0x0228], 0x255421B8602D3456ULL);
        put64(&empty[0x0230], 0x577B22447B52603EULL);
        put64(&empty[0x0238], 0x46412CC871396170ULL);
        put64(&empty[0x0240], 0x704F55237C5E2626ULL);
        put64(&empty[0x0248], 0x6C7932C87D5C3C20ULL);
        put64(&empty[0x0250], 0x2C50503E47543D6EULL);
        put64(&empty[0x0258], 0x46394E807721536AULL);
        put64(&empty[0x0260], 0x505823223F245325ULL);
        put64(&empty[0x0268], 0x365C79B0393B5B6EULL);

        /* Boot sector */
        put64(&empty[0x4000], 0x584F4236389058EBULL);
        put64(&empty[0x4008], 0x0001080200302E35ULL);
        put64(&empty[0x4010], 0x00EFF80000020002ULL);
        put64(&empty[0x4018], 0x0000002000400020ULL);
        put32(&empty[0x4020], 0x000777E0);
        put16(&empty[0x4024], 0x0080);

        empty[0x4026] = 0x29; /* ')' followed by randomly-generated volume serial number. */
        empty[0x4027] = random_generate();
        empty[0x4028] = random_generate();
        empty[0x4029] = random_generate();
        empty[0x402A] = random_generate();

        memset(&empty[0x402B], 0x00, 0x000B);
        memcpy(&empty[0x4036], "FAT16   ", 8);

        empty[0x41FE] = 0x55;
        empty[0x41FF] = 0xAA;

        empty[0x4200] = empty[0x22000] = empty[0x4015];
        empty[0x4201] = empty[0x22001] = 0xFF;
        empty[0x4202] = empty[0x22002] = 0xFF;
        empty[0x4203] = empty[0x22003] = 0xFF;
    } else if (fd != nullptr) {
        /* PC floppy format: boot sector, FAT1 and FAT2 start markers. */
        const uint16_t bps         = (uint16_t) sector_bytes;
        const uint16_t res_sectors = 1;

        empty[0x0000] = 0xEB;
        empty[0x0001] = 0x3C;
        empty[0x0002] = 0x90;
        memcpy(&empty[0x0003], "86BOX", 5);
        memset(&empty[0x0008], 0, 3);

        put16(&empty[0x000B], bps);
        empty[0x000D] = (uint8_t) fd->spc;
        put16(&empty[0x000E], res_sectors);
        empty[0x0010] = (uint8_t) fd->num_fats;
        put16(&empty[0x0011], (uint16_t) fd->root_dir_entries);
        put16(&empty[0x0013], (uint16_t) total_sectors);
        empty[0x0015] = (uint8_t) fd->media_desc;
        put16(&empty[0x0016], (uint16_t) fd->spfat);
        put16(&empty[0x0018], (uint16_t) fd->sectors);
        put16(&empty[0x001A], (uint16_t) fd->sides);
        put32(&empty[0x001C], 0);
        put32(&empty[0x0020], 0);

        empty[0x0024] = 0x00; /* Floppy drive number */
        empty[0x0026] = 0x29; /* Boot signature, followed by a randomly-generated volume serial number. */
        empty[0x0027] = random_generate();
        empty[0x0028] = random_generate();
        empty[0x0029] = random_generate();
        empty[0x002A] = random_generate();

        memset(&empty[0x002B], 0x20, 11);
        memcpy(&empty[0x0036], "FAT12   ", 8);

        put16(&empty[0x01FE], 0xAA55);

        for (int f = 0; f < fd->num_fats; f++) {
            const uint32_t fat_offset = (res_sectors + (f * fd->spfat)) * bps;
            empty[fat_offset]         = (uint8_t) fd->media_desc;
            empty[fat_offset + 1]     = 0xFF;
            empty[fat_offset + 2]     = 0xFF;
        }
    } else if ((total_sectors == SUPERDISK_SECTORS) || (total_sectors == SUPERDISK_240_SECTORS)
               || (total_sectors == FD32MB_SECTORS) || (total_sectors == EZ135_SECTORS)) {
        /* Superfloppy boot sector (LBA 0) for LS-120, LS-240 and compatible media. */
        const uint8_t  sec_per_cluster = (total_sectors == FD32MB_SECTORS)          ? 4
                                         : (total_sectors == SUPERDISK_SECTORS)     ? 4
                                         : (total_sectors == SUPERDISK_240_SECTORS) ? 8
                                         : (total_sectors == EZ135_SECTORS)         ? 32
                                                                                    : 1;
        const uint16_t res_sectors     = 1;
        const uint16_t fat_size_16     = (total_sectors == SUPERDISK_SECTORS)       ? 241
                                         : (total_sectors == SUPERDISK_240_SECTORS) ? 230
                                         : (total_sectors == FD32MB_SECTORS)        ? 64
                                                                                    : 0;
        /* The geometry the drive reports in its mode pages (rdisk.c). */
        const uint16_t sec_per_track   = (total_sectors == SUPERDISK_240_SECTORS) ? 56 : 32;
        const uint16_t heads           = (total_sectors == SUPERDISK_SECTORS)       ? 8
                                         : (total_sectors == SUPERDISK_240_SECTORS) ? 32
                                         : (total_sectors == FD32MB_SECTORS)        ? 2
                                         : (total_sectors == EZ135_SECTORS)         ? 16
                                                                                    : 64;

        empty[0x0000] = 0xEB;
        empty[0x0001] = 0x3E;
        empty[0x0002] = 0x90;
        memcpy(&empty[0x0003], "86BOX", 5);
        memset(&empty[0x0008], 0, 3);

        put16(&empty[0x000B], 512);
        empty[0x000D] = sec_per_cluster;
        put16(&empty[0x000E], res_sectors);
        empty[0x0010] = 2;
        put16(&empty[0x0011], 512);
        put16(&empty[0x0013], (total_sectors >= 65536) ? 0 : (uint16_t) total_sectors);
        empty[0x0015] = 0xF8;
        put16(&empty[0x0016], fat_size_16);
        put16(&empty[0x0018], sec_per_track);
        put16(&empty[0x001A], heads);
        put32(&empty[0x001C], 0);
        put32(&empty[0x0020], total_sectors);

        empty[0x0024] = 0x00;
        empty[0x0025] = 0;
        empty[0x0026] = 0x29; /* Boot signature */
        empty[0x0027] = random_generate();
        empty[0x0028] = random_generate();
        empty[0x0029] = random_generate();
        empty[0x002A] = random_generate();

        memset(&empty[0x002B], 0x20, 11);
        memcpy(&empty[0x0036], (total_sectors <= 4080) ? "FAT12   " : "FAT16   ", 8);

        put16(&empty[0x01FE], 0xAA55);

        if (fat_size_16 > 0) {
            /* FAT 1 and FAT 2 start markers. */
            put32(&empty[0x0200], 0xFFFFFFF8);
            put32(&empty[(res_sectors + fat_size_16) * 512], 0xFFFFFFF8);
        }
    }

    uint64_t written = 0;
    for (uint32_t i = 0; written < total_size; i++) {
        const uint64_t count = std::min<uint64_t>(block.size(), total_size - written);
        file.Write(empty, count);
        written += count;
        if (i == 0)
            memset(empty, 0, block.size());
        progress(i, pbar_max);
    }
    progress(pbar_max, pbar_max);

    return file.Close();
}

static bool
create_mo_image(const std::string &filename, int disk_size, bool mdi, const progress_fn &progress)
{
    const mo_type_t *dp   = &mo_types[disk_size];
    const uint16_t   base = 0x1000;

    ImageFile file(filename);
    if (!file.IsOpen())
        return false;

    const uint32_t sector_bytes  = dp->bytes_per_sector;
    const uint32_t total_sectors = dp->sectors;
    const uint64_t total_size    = (uint64_t) total_sectors * sector_bytes;
    const uint64_t total_size2   = total_size - ((total_size >> 20) << 20);
    const uint32_t blocks_num    = (uint32_t) (total_size >> 20);

    if (mdi) {
        std::vector<uint8_t> header(base, 0);
        put32(&header[0x08], (uint32_t) base);
        put32(&header[0x0C], (uint32_t) total_size);
        put16(&header[0x10], (uint16_t) sector_bytes);
        header[0x14] = 25;
        header[0x18] = 64;
        header[0x1C] = (uint8_t) ((dp->sectors / 64) / 25);
        file.Write(header.data(), base);
    }

    std::vector<uint8_t> block(1048576, 0);
    for (uint32_t i = 0; i < blocks_num; i++) {
        file.Write(block.data(), block.size());
        progress(i, blocks_num);
    }

    if (total_size2 > 0)
        file.Write(block.data(), total_size2);
    progress(blocks_num, blocks_num);

    return file.Close();
}

static bool
create_tape_image(const std::string &filename, int disk_size, const progress_fn &progress)
{
    ImageFile file(filename);
    if (!file.IsOpen())
        return false;

    /* QIC-117 floppy-tape / Ditto cartridges: the cores create and format
       their own blank images from a zero-length file. */
    if (disk_size < 7) {
        const uint32_t eod = TAPE_SIMH_EOD;
        file.Write(&eod, sizeof(eod));
    }
    progress(1, 1);

    return file.Close();
}

/* ----------------------------------------------------------- the window */

struct NewFloppyState {
    NewImageMedia type;
    int           drive_type = -1;
    sem_id        done       = -1;
    bool          accepted   = false;
    std::string   file_name;
    int           media_type = 0;
};

class NewFloppyWindow : public BWindow {
public:
    NewFloppyWindow(BWindow *parent, NewFloppyState *state);
    ~NewFloppyWindow() override;

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

private:
    void Build();
    void Browse();
    void FileChosen(BMessage *message);
    void UpdateRpm();
    void Create();
    void Done(bool ok);

    NewFloppyState         *state;
    BTextControl           *file;
    BButton                *browse;
    BButton                *ok;
    BButton                *cancel;
    std::unique_ptr<Choice> size;
    std::unique_ptr<Choice> rpm;
    BLayoutItem            *rpm_label;
    BLayoutItem            *rpm_menu;
    BStatusBar             *progress;
    ImageRefFilter          filter; /* outlives the panel that uses it */
    std::unique_ptr<BFilePanel> panel;
    std::thread             worker;
    bool                    creating;
    std::string             target;
    int                     target_type;
};

NewFloppyWindow::NewFloppyWindow(BWindow *parent, NewFloppyState *s)
    : BWindow(BRect(0, 0, 100, 100), "New Image", B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_V_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS
                  | B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE)
    , state(s)
    , rpm_label(nullptr)
    , rpm_menu(nullptr)
    , creating(false)
    , target_type(0)
{
    if (parent != nullptr)
        AddToSubset(parent);

    Build();

    if (parent != nullptr)
        CenterIn(parent->Frame());
    else
        CenterOnScreen();
}

NewFloppyWindow::~NewFloppyWindow()
{
    if (worker.joinable())
        worker.join();

    /* The caller waits for this; nothing of the window is used after it. */
    release_sem(state->done);
}

void
NewFloppyWindow::Build()
{
    const char *size_label = "Disk size:";
    switch (state->type) {
        case NewImageMedia::RDisk:
            size_label = "Removable disk type:";
            break;
        case NewImageMedia::Mo:
            size_label = "MO type:";
            break;
        case NewImageMedia::Tape:
            size_label = "Tape type:";
            break;
        default:
            break;
    }

    file = new BTextControl("file", "File name:", "", nullptr);
    file->SetModificationMessage(new BMessage(MSG_NF_NAME));
    file->TextView()->SetExplicitMinSize(BSize(be_plain_font->StringWidth("M") * 28, B_SIZE_UNSET));
    browse = new BButton("browse", "Browse" B_UTF8_ELLIPSIS, new BMessage(MSG_NF_BROWSE));

    size.reset(new Choice("size", size_label, MSG_NF_IGNORE));
    const int dt = state->drive_type;
    switch (state->type) {
        case NewImageMedia::Floppy:
            for (int i = 0; i < (int) (sizeof(floppy_types) / sizeof(floppy_types[0])); i++)
                size->Add(floppy_types[i], i);
            size->Select(0);
            break;
        case NewImageMedia::RDisk:
            for (int i = 0; i < KNOWN_RDISK_TYPES; i++) {
                if ((dt < 0) || (dt >= KNOWN_RDISK_DRIVE_TYPES) || rdisk_drive_types[dt].supported_media[i])
                    size->Add(rdisk_types[i].name, i);
            }
            size->Select(((dt >= 0) && (dt < KNOWN_RDISK_DRIVE_TYPES)) ? rdisk_drive_types[dt].default_media : -1);
            break;
        case NewImageMedia::Mo:
            for (int i = 0; i < KNOWN_MO_TYPES; i++) {
                if ((dt < 0) || (dt >= KNOWN_MO_DRIVE_TYPES) || mo_drive_types[dt].supported_media[i])
                    size->Add(mo_types[i].name, i);
            }
            size->Select(((dt >= 0) && (dt < KNOWN_MO_DRIVE_TYPES)) ? mo_drive_types[dt].default_media : -1);
            break;
        case NewImageMedia::Tape:
            for (int i = 0; i < KNOWN_TAPE_TYPES; i++) {
                if ((dt < 0) || (dt >= KNOWN_TAPE_DRIVE_TYPES) || tape_drive_types[dt].supported_media[i])
                    size->Add(tape_types[i].name, i);
            }
            size->Select(((dt >= 0) && (dt < KNOWN_TAPE_DRIVE_TYPES)) ? tape_drive_types[dt].default_media : -1);
            break;
    }

    rpm.reset(new Choice("rpm", "RPM mode:", MSG_NF_IGNORE));
    for (int i = 0; i < (int) (sizeof(rpm_modes) / sizeof(rpm_modes[0])); i++)
        rpm->Add(rpm_modes[i], i);
    rpm->Select(0);

    progress = new BStatusBar("progress", "Creating image" B_UTF8_ELLIPSIS);
    progress->SetMaxValue(100);

    ok     = new BButton("ok", "OK", new BMessage(MSG_NF_OK));
    cancel = new BButton("cancel", "Cancel", new BMessage(MSG_NF_CANCEL));

    rpm_label = rpm->field->CreateLabelLayoutItem();
    rpm_menu  = rpm->field->CreateMenuBarLayoutItem();

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_SPACING)
        .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
            .Add(file->CreateLabelLayoutItem(), 0, 0)
            .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING, 1, 0)
                .Add(file->CreateTextViewLayoutItem())
                .Add(browse)
            .End()
            .AddMenuField(size->field, 0, 1)
            .Add(rpm_label, 0, 2)
            .Add(rpm_menu, 1, 2)
        .End()
        .Add(progress)
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
            .Add(ok)
        .End();

    rpm_label->SetVisible(false);
    rpm_menu->SetVisible(false);
    progress->Hide();

    SetDefaultButton(ok);
    file->MakeFocus(true);
}

/* The RPM mode is only asked for a floppy saved as an 86F surface image. */
void
NewFloppyWindow::UpdateRpm()
{
    const bool show = (state->type == NewImageMedia::Floppy) && (file_suffix(file->Text()) == "86f");
    if (rpm_label->IsVisible() != show) {
        rpm_label->SetVisible(show);
        rpm_menu->SetVisible(show);
    }
}

void
NewFloppyWindow::Browse()
{
    switch (state->type) {
        case NewImageMedia::Floppy:
            filter.SetPatterns({ "86f", "dsk", "flp", "im?", "img", "*fd?" });
            break;
        case NewImageMedia::RDisk:
            filter.SetPatterns({ "im?", "img", "rdi", "zdi" });
            break;
        case NewImageMedia::Mo:
            filter.SetPatterns({ "im?", "img", "mdi" });
            break;
        case NewImageMedia::Tape:
            filter.SetPatterns({ "tap" });
            break;
    }

    if (!panel) {
        BMessenger target(this);
        BMessage   msg(MSG_NF_FILE);
        panel.reset(new BFilePanel(B_SAVE_PANEL, &target, nullptr, B_FILE_NODE, false, &msg, &filter, true));
        if (panel->Window()->Lock()) {
            panel->Window()->SetTitle(EMU_NAME ": New image");
            panel->Window()->Unlock();
        }
    }

    BPath current(file->Text());
    BPath dir;
    if ((file->Text()[0] == '/') && (current.InitCheck() == B_OK) && (current.GetParent(&dir) == B_OK)
        && BEntry(dir.Path()).IsDirectory()) {
        panel->SetPanelDirectory(dir.Path());
        panel->SetSaveText(current.Leaf());
    } else {
        if (usr_path[0] != '\0')
            panel->SetPanelDirectory(usr_path);
        if (file->Text()[0] != '\0')
            panel->SetSaveText(file->Text());
    }

    panel->Refresh();
    panel->Show();
}

void
NewFloppyWindow::FileChosen(BMessage *message)
{
    entry_ref   ref;
    const char *name = nullptr;

    if ((message->FindRef("directory", &ref) != B_OK) || (message->FindString("name", &name) != B_OK))
        return;

    BDirectory dir(&ref);
    BPath      path(&dir, name);
    if (path.InitCheck() == B_OK) {
        file->SetText(path.Path());
        UpdateRpm();
    }
}

/* NewFloppyDialog::onCreate(): the image is written by a worker thread
   that reports its progress; Done() gets the result. */
void
NewFloppyWindow::Create()
{
    std::string filename = file->Text();
    if (!filename.empty() && (filename[0] != '/'))
        filename = std::string(usr_path) + filename;
    file->SetText(filename.c_str());

    target      = filename;
    target_type = size->Value(0);

    const std::string   suffix   = file_suffix(filename);
    const NewImageMedia type     = state->type;
    const int           medium   = target_type;
    const int           rpm_mode = rpm->Value(0);

    creating = true;
    file->SetEnabled(false);
    browse->SetEnabled(false);
    size->SetEnabled(false);
    rpm->SetEnabled(false);
    ok->SetEnabled(false);
    cancel->SetEnabled(false);
    progress->Reset("Creating image" B_UTF8_ELLIPSIS);
    progress->SetMaxValue(100);
    progress->Show();

    BMessenger self(this);
    worker = std::thread([self, filename, suffix, type, medium, rpm_mode]() {
        const progress_fn report = [self](uint32_t value, uint32_t max) {
            BMessage msg(MSG_NF_PROGRESS);
            msg.AddInt32("value", (int32) value);
            msg.AddInt32("max", (int32) max);
            self.SendMessage(&msg);
        };

        bool result = false;
        switch (type) {
            case NewImageMedia::Floppy:
                if (suffix == "86f")
                    result = create_86f(filename, disk_sizes[medium], (uint8_t) rpm_mode);
                else
                    result = create_sector_image(filename, disk_sizes[medium], suffix == "zdi");
                report(1, 1);
                break;
            case NewImageMedia::RDisk:
                result = create_rdisk_image(filename, rdisk_types[medium].sectors, rdisk_types[medium].bytes_per_sector,
                                            suffix == "zdi", report);
                break;
            case NewImageMedia::Mo:
                result = create_mo_image(filename, medium, suffix == "mdi", report);
                break;
            case NewImageMedia::Tape:
                result = create_tape_image(filename, medium, report);
                break;
        }

        BMessage done(MSG_NF_DONE);
        done.AddBool("ok", result);
        self.SendMessage(&done);
    });
}

void
NewFloppyWindow::Done(bool result)
{
    if (worker.joinable())
        worker.join();
    creating = false;

    if (result) {
        state->accepted   = true;
        state->file_name  = target;
        state->media_type = target_type;
    } else {
        BAlert *alert = new BAlert("Unable to write file", "Make sure the file is being saved to a writable directory",
                                   "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT);
        alert->Go();
    }

    Quit();
}

bool
NewFloppyWindow::QuitRequested()
{
    /* The image being written cannot be stopped (Qt has no Cancel there). */
    return !creating;
}

void
NewFloppyWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_NF_OK:
            if (!creating)
                Create();
            break;
        case MSG_NF_CANCEL:
            if (!creating)
                Quit();
            break;
        case MSG_NF_BROWSE:
            Browse();
            break;
        case MSG_NF_FILE:
            FileChosen(message);
            break;
        case MSG_NF_NAME:
            UpdateRpm();
            break;
        case MSG_NF_PROGRESS:
            {
                const int32 max   = message->GetInt32("max", 1);
                const int32 value = message->GetInt32("value", 0);
                if (max > 0) {
                    const float at = (100.0f * value) / max;
                    progress->Update(at - progress->CurrentValue());
                }
                break;
            }
        case MSG_NF_DONE:
            Done(message->GetBool("ok", false));
            break;
        case MSG_NF_IGNORE:
            break;
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

/* ------------------------------------------------------------- entry */

bool
haiku_new_floppy_dialog(BWindow *parent, NewImageMedia type, int drive_type, std::string *file_name, int *media_type)
{
    NewFloppyState state;
    state.type       = type;
    state.drive_type = drive_type;
    state.done       = create_sem(0, "new image");
    if (state.done < B_OK)
        return false;

    NewFloppyWindow *window = new NewFloppyWindow(parent, &state);
    window->Show();

    /* Like BAlert::Go(): wait, keeping the parent window redrawn. */
    status_t err;
    if (parent != nullptr) {
        do {
            err = acquire_sem_etc(state.done, 1, B_RELATIVE_TIMEOUT, 50000);
            if ((err == B_TIMED_OUT) || (err == B_INTERRUPTED))
                parent->UpdateIfNeeded();
        } while ((err == B_TIMED_OUT) || (err == B_INTERRUPTED));
    } else {
        do {
            err = acquire_sem(state.done);
        } while (err == B_INTERRUPTED);
    }
    delete_sem(state.done);

    if (!state.accepted)
        return false;

    if (file_name != nullptr)
        *file_name = state.file_name;
    if (media_type != nullptr)
        *media_type = state.media_type;
    return true;
}
