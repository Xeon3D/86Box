/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the device configuration dialog, like
 *          the Qt DeviceConfig (qt_deviceconfig.cpp). It shows one control
 *          per entry of a device's device_config_t list and writes the
 *          values to the configuration with config_set_*().
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Beep.h>
#include <Button.h>
#include <CheckBox.h>
#include <Directory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <Node.h>
#include <OS.h>
#include <Path.h>
#include <SeparatorView.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "haiku_deviceconfig.hpp"
#include "haiku_settings_common.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/mem.h>
#include <86box/random.h>
#include <86box/rom.h>
#include <86box/thread.h>
#include <86box/network.h>
#include <86box/scsi.h>
#include <86box/version.h>
#ifdef USE_RTMIDI
#    include <86box/midi_rtmidi.h>
#endif
}

enum : uint32 {
    MSG_DC_IGNORE   = 'dcig', /* a pop-up that needs no reaction */
    MSG_DC_OK       = 'dcok',
    MSG_DC_CANCEL   = 'dccn',
    MSG_DC_BIOS     = 'dcbi', /* the BIOS pop-up changed */
    MSG_DC_BROWSE   = 'dcbr', /* "index": the file entry */
    MSG_DC_GENERATE = 'dcgn', /* "index": the MAC entry */
    MSG_DC_FILE     = 'dcfp', /* from the file panel, with "index" */
};

/* One row of the dialog: the config entry, its control and, once OK was
   pressed, the value the control held. */
struct ConfigEntry {
    const device_config_t  *config = nullptr;
    std::unique_ptr<Choice> choice;
    BCheckBox              *check = nullptr;
    BTextControl           *text  = nullptr;
    std::vector<std::string> paths; /* CONFIG_SERPORT: the path of each pop-up value */
    std::string             selected; /* the current string value */
    int                     value = 0; /* the current int value */
    int32                   result_int = 0;
    std::string             result_str;
};

struct DialogState {
    const device_t     *device = nullptr;
    device_context_t    ctx {};
    std::vector<ConfigEntry>  entries;
    ConfigEntry              *memory   = nullptr;
    ConfigEntry              *memory_2 = nullptr;
    ConfigEntry              *bios     = nullptr;
    ConfigEntry              *in530_bootlogo = nullptr;
    int                 bios_rows = 0;
    bool                accepted  = false;
    sem_id              done      = -1;
};

/* ------------------------------------------------------------- helpers */

static std::string
config_string(const device_context_t &ctx, const device_config_t *config)
{
    const char *s = config_get_string(const_cast<char *>(ctx.name), const_cast<char *>(config->name),
                                      const_cast<char *>(config->default_string));
    return (s != nullptr) ? s : "";
}

/* Lets through only the characters of `allowed` (and control keys). */
static void
restrict_chars(BTextControl *text, const char *allowed)
{
    BTextView *view = text->TextView();
    for (int c = 32; c < 128; c++) {
        if (strchr(allowed, c) == nullptr)
            view->DisallowChar(c);
    }
}

static std::string
generate_mac(bool is_oui)
{
    /* An OUI is the first half of the station address, so a generated one
       must leave the individual/group bit clear and sets the locally
       administered bit. */
    uint8_t first = random_generate();
    if (is_oui)
        first = (first & 0xfc) | 0x02;
    const uint8_t second = random_generate();
    const uint8_t third  = random_generate();
    char          buf[16];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X", first, second, third);
    return buf;
}

static bool
valid_mac(const std::string &mac)
{
    if (mac.size() != 8)
        return false;
    for (size_t i = 0; i < mac.size(); i++) {
        if ((i % 3) == 2 ? (mac[i] != ':') : !isxdigit((unsigned char) mac[i]))
            return false;
    }
    return true;
}

static std::string
to_case(std::string s, bool upper)
{
    for (char &c : s)
        c = upper ? toupper((unsigned char) c) : tolower((unsigned char) c);
    return s;
}

/* The serial ports Haiku publishes under /dev/ports, by path. */
static std::vector<std::string>
enumerate_serial_devices()
{
    std::vector<std::string> ports;
    BDirectory               dir("/dev/ports");
    BEntry                   entry;

    while (dir.GetNextEntry(&entry) == B_OK) {
        BPath path(&entry);
        if (path.InitCheck() == B_OK)
            ports.push_back(path.Path());
    }
    /* Numeric-aware order, so usb2 comes before usb10. */
    std::sort(ports.begin(), ports.end(), [](const std::string &a, const std::string &b) {
        size_t i = 0;
        size_t j = 0;
        while ((i < a.size()) && (j < b.size())) {
            if (isdigit((unsigned char) a[i]) && isdigit((unsigned char) b[j])) {
                const unsigned long na = strtoul(a.c_str() + i, nullptr, 10);
                const unsigned long nb = strtoul(b.c_str() + j, nullptr, 10);
                if (na != nb)
                    return na < nb;
                while ((i < a.size()) && isdigit((unsigned char) a[i]))
                    i++;
                while ((j < b.size()) && isdigit((unsigned char) b[j]))
                    j++;
            } else {
                if (a[i] != b[j])
                    return a[i] < b[j];
                i++;
                j++;
            }
        }
        return a.size() < b.size();
    });
    return ports;
}

/* The slot another EISA card is set to, read the way that card's init
   will read it: from its own section of the configuration, or from its
   default when it has never been configured. */
static int
eisa_slot_of(const device_t *dev, int instance)
{
    device_context_t ctx;
    int              def = 1;

    for (const device_config_t *c = dev->config; (c != nullptr) && (c->type != CONFIG_END); ++c) {
        if (!strcmp(c->name, "slot")) {
            def = c->default_int;
            break;
        }
    }
    device_set_context(&ctx, dev, instance);
    return config_get_int(ctx.name, const_cast<char *>("slot"), def);
}

/* One card to a slot: the slots of the other EISA SCSI and network cards
   are greyed out. The Settings pages' unsaved choices are not visible
   here, so this uses the cards of the saved configuration. */
static void
grey_out_taken_eisa_slots(Choice *choice, const device_t *cfg_dev, int instance)
{
    std::set<int> taken;

    for (int i = 0; i < SCSI_CARD_MAX; ++i) {
        const device_t *dev = scsi_card_getdevice(scsi_card_current[i]);
        if ((dev == nullptr) || !(dev->flags & DEVICE_EISA))
            continue;
        if ((dev == cfg_dev) && ((i + 1) == instance))
            continue;
        taken.insert(eisa_slot_of(dev, i + 1));
    }
    for (int i = 0; i < NET_CARD_MAX; ++i) {
        const device_t *dev = network_card_getdevice(net_cards_conf[i].device_num);
        if ((dev == nullptr) || !(dev->flags & DEVICE_EISA))
            continue;
        if ((dev == cfg_dev) && ((i + 1) == instance))
            continue;
        taken.insert(eisa_slot_of(dev, i + 1));
    }

    BMenu *menu = choice->field->Menu();
    for (int32 i = 0; i < menu->CountItems(); i++) {
        BMenuItem *item = menu->ItemAt(i);
        if ((item->Message() != nullptr) && taken.count(item->Message()->GetInt32("value", -1)))
            item->SetEnabled(false);
    }
}

/* Only the extensions of the first part of a "Description (*.a *.b)|..."
   filter, as Qt uses it. */
class ExtensionFilter : public BRefFilter {
public:
    explicit ExtensionFilter(const char *file_filter)
    {
        if (file_filter == nullptr)
            return;
        std::string filter(file_filter);
        filter = filter.substr(0, filter.find('|'));
        const size_t open  = filter.rfind('(');
        const size_t close = filter.rfind(')');
        if ((open == std::string::npos) || (close == std::string::npos) || (close < open))
            return;
        std::string list = filter.substr(open + 1, close - open - 1);
        size_t      at   = 0;
        while (at < list.size()) {
            size_t end = list.find(' ', at);
            if (end == std::string::npos)
                end = list.size();
            std::string glob = list.substr(at, end - at);
            if (glob.compare(0, 2, "*.") == 0)
                glob.erase(0, 2);
            if (glob == "*")
                all = true;
            else if (!glob.empty())
                extensions.push_back(to_case(glob, false));
            at = end + 1;
        }
        if (extensions.empty())
            all = true;
    }

    bool Filter(const entry_ref *ref, BNode *node, struct stat_beos *, const char *) override
    {
        if (all || ((node != nullptr) && node->IsDirectory()))
            return true;
        BEntry entry(ref, true);
        if (entry.IsDirectory())
            return true;
        const char *dot = strrchr(ref->name, '.');
        if (dot == nullptr)
            return false;
        const std::string ext = to_case(dot + 1, false);
        return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
    }

    bool all = false;

private:
    std::vector<std::string> extensions;
};

/* ---------------------------------------------------------- the window */

class DeviceConfigWindow : public BWindow {
public:
    DeviceConfigWindow(BWindow *parent, DialogState *state);
    ~DeviceConfigWindow() override;

    void MessageReceived(BMessage *message) override;

private:
    void Build();
    void AddConfig(BLayoutBuilder::Grid<> &grid, int &row, ConfigEntry &e);
    void ApplyBiosLimits();
    void Browse(int32 index);
    void FileChosen(BMessage *message);
    bool Accept();

    DialogState                    *state;
    std::unique_ptr<ExtensionFilter> filter; /* outlives the panel that uses it */
    std::unique_ptr<BFilePanel>      panel;
};

DeviceConfigWindow::DeviceConfigWindow(BWindow *parent, DialogState *s)
    : BWindow(BRect(0, 0, 100, 100), "", B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS
                  | B_CLOSE_ON_ESCAPE)
    , state(s)
{
    const std::string name  = settings_device_name(state->device, state->device->internal_name, -1);
    const std::string title = name + " Device Configuration";
    SetTitle(title.c_str());

    if (parent != nullptr)
        AddToSubset(parent);

    Build();

    if (parent != nullptr)
        CenterIn(parent->Frame());
    else
        CenterOnScreen();
}

DeviceConfigWindow::~DeviceConfigWindow()
{
    /* The caller waits for this; nothing of the window is used after it. */
    release_sem(state->done);
}

void
DeviceConfigWindow::Build()
{
    const std::string name = settings_device_name(state->device, state->device->internal_name, -1);

    BStringView *label = new BStringView("device", name.c_str());
    label->SetFont(be_bold_font);
    label->SetAlignment(B_ALIGN_CENTER);
    label->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    BButton *ok     = new BButton("ok", "OK", new BMessage(MSG_DC_OK));
    BButton *cancel = new BButton("cancel", "Cancel", new BMessage(MSG_DC_CANCEL));

    BLayoutBuilder::Grid<> grid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
    int                    row = 0;

    for (ConfigEntry &e : state->entries)
        AddConfig(grid, row, e);

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_SPACING)
        .Add(label)
        .Add(new BSeparatorView(B_HORIZONTAL))
        .Add(grid.View())
        .Add(new BSeparatorView(B_HORIZONTAL))
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
            .Add(ok)
        .End();

    SetDefaultButton(ok);
    ApplyBiosLimits();
}

void
DeviceConfigWindow::AddConfig(BLayoutBuilder::Grid<> &grid, int &row, ConfigEntry &e)
{
    const device_config_t *config = e.config;
    const std::string      label  = std::string(config->description) + ":";
    const float            wide   = be_plain_font->StringWidth("M") * 24;
    const int32            index  = (int32) (&e - state->entries.data());

    switch (config->type) {
        default:
            return;

        case CONFIG_BINARY:
            e.check = new BCheckBox(config->name, config->description, nullptr);
            e.check->SetValue(e.value > 0 ? B_CONTROL_ON : B_CONTROL_OFF);
            grid.Add(e.check, 1, row);
            break;

#ifdef USE_RTMIDI
        case CONFIG_MIDI_OUT:
        case CONFIG_MIDI_IN:
            {
                const bool out = (config->type == CONFIG_MIDI_OUT);
                const int  n   = out ? rtmidi_out_get_num_devs() : rtmidi_in_get_num_devs();
                e.choice.reset(new Choice(config->name, label.c_str(), MSG_DC_IGNORE));
                for (int i = 0; i < n; i++) {
                    char midi_name[512] = { 0 };
                    if (out)
                        rtmidi_out_get_dev_name(i, midi_name);
                    else
                        rtmidi_in_get_dev_name(i, midi_name);
                    e.choice->Add(midi_name, i);
                }
                e.choice->Select(e.value);
                grid.AddMenuField(e.choice->field, 0, row);
                break;
            }
#endif

        case CONFIG_INT:
        case CONFIG_SELECTION:
        case CONFIG_HEX16:
        case CONFIG_HEX20:
            e.choice.reset(new Choice(config->name, label.c_str(), MSG_DC_IGNORE));
            for (const device_config_selection_t *sel = config->selection;
                 (sel->description != nullptr) && (sel->description[0] != '\0'); ++sel)
                e.choice->Add(sel->description, sel->value);
            e.choice->Select(e.value);
            if (!strcmp(config->name, "slot") && (state->device->flags & DEVICE_EISA))
                grey_out_taken_eisa_slots(e.choice.get(), state->device, state->ctx.instance);
            if (!strcmp(config->name, "memory") || !strcmp(config->name, "framebuffer_memory"))
                state->memory = &e;
            if (!strcmp(config->name, "texture_memory"))
                state->memory_2 = &e;
            if (!strcmp(config->name, "boot_logo") && !strcmp(state->device->internal_name, "in530"))
                state->in530_bootlogo = &e;
            grid.AddMenuField(e.choice->field, 0, row);
            break;

        case CONFIG_BIOS:
            {
                /* Only the BIOSes whose files are all present. */
                e.choice.reset(new Choice(config->name, label.c_str(), MSG_DC_BIOS));
                int rows    = 0;
                int current = -1;
                int q       = 0;
                for (const device_config_bios_t *bios = config->bios;
                     (bios->name != nullptr) && (bios->internal_name != nullptr) && (bios->name[0] != '\0')
                     && (bios->internal_name[0] != '\0') && (bios->files_no != 0);
                     ++bios, ++q) {
                    int p = 0;
                    for (int d = 0; d < bios->files_no; d++)
                        p += !!rom_present(const_cast<char *>(bios->files[d]));
                    if ((bios->files_no == -1) || (p == bios->files_no)) {
                        e.choice->Add(bios->name, q);
                        if ((current == -1) || (e.selected == bios->internal_name))
                            current = q;
                        rows++;
                    }
                }
                state->bios_rows = rows;
                if (rows == 0) {
                    e.choice.reset();
                    return;
                }
                e.choice->Select(current);
                state->bios = &e;
                grid.AddMenuField(e.choice->field, 0, row);
                break;
            }

        case CONFIG_SPINNER:
            {
                char buf[32];
                e.text = new BTextControl(config->name, label.c_str(), "", nullptr);
                restrict_chars(e.text, "-0123456789");
                e.text->TextView()->SetMaxBytes(7);
                snprintf(buf, sizeof(buf), "%i", e.value);
                e.text->SetText(buf);
                snprintf(buf, sizeof(buf), "%i \xe2\x80\x93 %i", config->spinner.min, config->spinner.max);
                e.text->SetToolTip(buf);
                grid.AddTextControl(e.text, 0, row);
                break;
            }

        case CONFIG_FNAME:
            {
                BMessage *msg = new BMessage(MSG_DC_BROWSE);
                msg->AddInt32("index", index);
                e.text = new BTextControl(config->name, label.c_str(), e.selected.c_str(), nullptr);
                e.text->TextView()->SetExplicitMinSize(BSize(wide, B_SIZE_UNSET));
                BButton *browse = new BButton("browse", "Browse" B_UTF8_ELLIPSIS, msg);
                grid.Add(e.text->CreateLabelLayoutItem(), 0, row)
                    .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING, 1, row)
                        .Add(e.text->CreateTextViewLayoutItem())
                        .Add(browse)
                    .End();
                break;
            }

        case CONFIG_STRING:
            e.text = new BTextControl(config->name, label.c_str(), e.selected.c_str(), nullptr);
            e.text->TextView()->SetExplicitMinSize(BSize(be_plain_font->StringWidth("M") * 12, B_SIZE_UNSET));
            grid.AddTextControl(e.text, 0, row);
            break;

        case CONFIG_SERPORT:
            {
                e.choice.reset(new Choice(config->name, label.c_str(), MSG_DC_IGNORE));
                e.paths.push_back("");
                e.choice->Add("None", 0);
                int current = 0;
                for (const std::string &path : enumerate_serial_devices()) {
                    const int32 value = (int32) e.paths.size();
                    e.paths.push_back(path);
                    e.choice->Add(path.c_str(), value);
                    if (e.selected == path)
                        current = value;
                }
                e.choice->Select(current);
                grid.AddMenuField(e.choice->field, 0, row);
                break;
            }

        case CONFIG_STRHEX:
            /* Up to four hex digits: the 16-bit range this type holds. */
            e.text = new BTextControl(config->name, label.c_str(), to_case(e.selected, true).c_str(), nullptr);
            restrict_chars(e.text, "0123456789abcdefABCDEF");
            e.text->TextView()->SetMaxBytes(4);
            grid.AddTextControl(e.text, 0, row);
            break;

        case CONFIG_MAC:
            {
                std::string mac;
                /* The current, default or a generated MAC, shown in uppercase. */
                if (config_get_mac(state->ctx.name, const_cast<char *>(config->name), config->default_int) & 0xff000000)
                    mac = (config->default_string != nullptr) ? config->default_string
                                                               : generate_mac(!strcmp(config->name, "mac_oui"));
                else
                    mac = config_string(state->ctx, config);

                BMessage *msg = new BMessage(MSG_DC_GENERATE);
                msg->AddInt32("index", index);
                e.text = new BTextControl(config->name, label.c_str(), to_case(mac, true).c_str(), nullptr);
                restrict_chars(e.text, "0123456789abcdefABCDEF:");
                e.text->TextView()->SetMaxBytes(8);
                e.text->SetToolTip("XX:XX:XX");
                BButton *generate = new BButton("generate", "Generate", msg);
                grid.Add(e.text->CreateLabelLayoutItem(), 0, row)
                    .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING, 1, row)
                        .Add(e.text->CreateTextViewLayoutItem())
                        .Add(generate)
                    .End();
                break;
            }
    }
    row++;
}

/* The memory pop-ups offer only what the selected BIOS supports, and the
   IN530's boot logo pop-up the logos of that BIOS (on_comboIndexChanged). */
void
DeviceConfigWindow::ApplyBiosLimits()
{
    if ((state->bios == nullptr) || !state->bios->choice)
        return;

    const int32 bios_idx = state->bios->choice->Value(-1);
    if (bios_idx < 0)
        return;

    const device_config_t *cfg_bios   = state->bios->config;
    const char            *bios_name  = cfg_bios->bios[bios_idx].internal_name;
    const uint64_t         bios_flags = device_get_bios_flags(state->device, bios_name);

    for (int second = 0; second < 2; second++) {
        ConfigEntry *e = second ? state->memory_2 : state->memory;
        if ((e == nullptr) || !e->choice)
            continue;

        int      mem     = e->choice->Value(e->value);
        uint16_t min_mem = 0;
        uint16_t max_mem = 65535;

        if (!second) {
            if (bios_flags & BIOS_LIMIT_MIN_MEMORY)
                min_mem = (bios_flags & 0xff);
            if (bios_flags & BIOS_LIMIT_MAX_MEMORY)
                max_mem = ((bios_flags >> 8) & 0xff);
            if (bios_flags & BIOS_FIXED_MEMORY)
                min_mem = max_mem = e->config->default_int;
        } else {
            if (bios_flags & BIOS_LIMIT_MIN_MEMORY_2)
                min_mem = ((bios_flags >> 16) & 0xff);
            if (bios_flags & BIOS_LIMIT_MAX_MEMORY_2)
                max_mem = ((bios_flags >> 24) & 0xff);
        }

        mem = std::max<int>(mem, min_mem);
        mem = std::min<int>(mem, max_mem);

        e->choice->Clear();
        for (const device_config_selection_t *sel = e->config->selection;
             (sel->description != nullptr) && (sel->description[0] != '\0'); ++sel) {
            if ((sel->value >= min_mem) && (sel->value <= max_mem))
                e->choice->Add(sel->description, sel->value);
        }
        e->choice->Select(mem);
    }

    if (ConfigEntry *e = state->in530_bootlogo) {
        const int32 selector = e->choice->Value(e->config->default_int);

        e->choice->Clear();
        e->choice->Add("Disabled", 4);
        if (!strcmp(bios_name, "in530_pb_111j")) {
            e->choice->Add("Packard Bell", 1);
            e->choice->Add("NEC", 2);
            e->choice->Add("PowerMate", 3);
        } else if (!strcmp(bios_name, "in530_pb_129")) {
            e->choice->Add("Packard Bell", 1);
            e->choice->Add("Japaq", 2);
            e->choice->Add("PowerMate", 3);
        } else {
            /* Only one logo */
            e->choice->Add("Enabled", 1);
        }
        e->choice->Select(e->choice->Has(selector) ? selector : e->config->default_int);
    }
}

void
DeviceConfigWindow::Browse(int32 index)
{
    if ((index < 0) || (index >= (int32) state->entries.size()) || (state->entries[index].text == nullptr))
        return;

    ConfigEntry           &e      = state->entries[index];
    const bool       create = !!e.config->default_int;
    BMessenger       target(this);
    BMessage         msg(MSG_DC_FILE);
    msg.AddInt32("index", index);

    panel.reset();
    filter.reset(new ExtensionFilter(e.config->file_filter));
    panel.reset(new BFilePanel(create ? B_SAVE_PANEL : B_OPEN_PANEL, &target, nullptr, B_FILE_NODE, false, &msg,
                               filter->all ? nullptr : filter.get(), true));

    const std::string title = std::string(EMU_NAME ": ") + e.config->description;
    panel->Window()->SetTitle(title.c_str());

    BPath current(e.text->Text());
    BPath dir;
    if ((e.text->Text()[0] != '\0') && (current.InitCheck() == B_OK) && (current.GetParent(&dir) == B_OK)
        && BEntry(dir.Path()).IsDirectory()) {
        panel->SetPanelDirectory(dir.Path());
        if (create)
            panel->SetSaveText(current.Leaf());
    } else if (usr_path[0] != '\0')
        panel->SetPanelDirectory(usr_path);

    panel->Show();
}

void
DeviceConfigWindow::FileChosen(BMessage *message)
{
    const int32 index = message->GetInt32("index", -1);
    if ((index < 0) || (index >= (int32) state->entries.size()) || (state->entries[index].text == nullptr))
        return;

    entry_ref   ref;
    const char *name = nullptr;
    BPath       path;

    if (message->FindRef("refs", &ref) == B_OK)
        path.SetTo(&ref);
    else if ((message->FindRef("directory", &ref) == B_OK) && (message->FindString("name", &name) == B_OK)) {
        BDirectory dir(&ref);
        path.SetTo(&dir, name);
    } else
        return;

    if (path.InitCheck() == B_OK)
        state->entries[index].text->SetText(path.Path());
}

/* Reads every control into its entry; false (and nothing kept) when a
   value is not acceptable yet. */
bool
DeviceConfigWindow::Accept()
{
    for (ConfigEntry &e : state->entries) {
        if (e.check != nullptr)
            e.result_int = (e.check->Value() == B_CONTROL_ON) ? 1 : 0;
        else if (e.choice) {
            e.result_int = e.choice->Value((e.config->type == CONFIG_BIOS) ? -1 : e.value);
            if (e.config->type == CONFIG_SERPORT)
                e.result_str = ((e.result_int >= 0) && (e.result_int < (int32) e.paths.size()))
                    ? e.paths[e.result_int] : e.selected;
        } else if (e.text != nullptr) {
            e.result_str = e.text->Text();
            if (e.config->type == CONFIG_SPINNER) {
                long v = strtol(e.result_str.c_str(), nullptr, 10);
                v      = std::max<long>(v, e.config->spinner.min);
                v      = std::min<long>(v, e.config->spinner.max);
                e.result_int = (int32) v;
            } else if ((e.config->type == CONFIG_MAC) && !valid_mac(e.result_str)) {
                beep();
                e.text->MakeFocus(true);
                e.text->TextView()->SelectAll();
                return false;
            }
        }
    }
    return true;
}

void
DeviceConfigWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_DC_OK:
            if (Accept()) {
                state->accepted = true;
                Quit();
            }
            break;

        case MSG_DC_CANCEL:
            Quit();
            break;

        case MSG_DC_BIOS:
            ApplyBiosLimits();
            break;

        case MSG_DC_BROWSE:
            Browse(message->GetInt32("index", -1));
            break;

        case MSG_DC_FILE:
            FileChosen(message);
            break;

        case MSG_DC_GENERATE:
            {
                const int32 index = message->GetInt32("index", -1);
                if ((index >= 0) && (index < (int32) state->entries.size()) && (state->entries[index].text != nullptr)) {
                    ConfigEntry &e = state->entries[index];
                    e.text->SetText(generate_mac(!strcmp(e.config->name, "mac_oui")).c_str());
                }
                break;
            }

        case MSG_DC_IGNORE:
            break;

        default:
            BWindow::MessageReceived(message);
            break;
    }
}

/* --------------------------------------------------------- the interface */

bool
haiku_device_has_config(const device_t *device)
{
    return device_has_config(device) != 0;
}

bool
haiku_device_configure(BWindow *parent, const device_t *device, int instance)
{
    if ((device == nullptr) || (device->config == nullptr))
        return false;

    DialogState state;
    state.device = device;
    device_set_context(&state.ctx, device, instance);

    /* The current values, read here in the caller's thread (ProcessConfig).
       Entries of the "dependent" class are left out, as in Qt. */
    for (const device_config_t *config = device->config; config->type != CONFIG_END; ++config) {
        if (config->type & CONFIG_DEP)
            continue;

        ConfigEntry e;
        e.config = config;

        const int major = ((config->type & CONFIG_TYPE_MASK) >> CONFIG_SHIFT) << CONFIG_SHIFT;
        switch (major) {
            case CONFIG_TYPE_INT:
                e.value = config_get_int(state.ctx.name, const_cast<char *>(config->name), config->default_int);
                break;
            case CONFIG_TYPE_HEX16:
                e.value = config_get_hex16(state.ctx.name, const_cast<char *>(config->name), config->default_int);
                break;
            case CONFIG_TYPE_HEX20:
                e.value = config_get_hex20(state.ctx.name, const_cast<char *>(config->name), config->default_int);
                break;
            case CONFIG_TYPE_STRING:
            case CONFIG_TYPE_STRHEX:
                e.selected = config_string(state.ctx, config);
                break;
            default:
                break;
        }

        /* A BIOS saved by its old index is migrated to its name first. */
        if ((config->type == CONFIG_BIOS) && (e.selected.size() == 1)) {
            device_migrate_config_bios((const void *) config, state.ctx.name);
            e.selected = config_string(state.ctx, config);
        }

        state.entries.push_back(std::move(e));
    }

    state.done = create_sem(0, "device config");
    if (state.done < B_OK)
        return false;

    DeviceConfigWindow *window = new DeviceConfigWindow(parent, &state);
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

    /* Write back what changed (ConfigureDevice after exec()). */
    bool  changed = false;
    char *section = state.ctx.name;
    for (ConfigEntry &e : state.entries) {
        char *name = const_cast<char *>(e.config->name);
        if ((e.check == nullptr) && !e.choice && (e.text == nullptr))
            continue;

        switch (e.config->type) {
            case CONFIG_BINARY:
            case CONFIG_SPINNER:
            case CONFIG_MIDI_OUT:
            case CONFIG_MIDI_IN:
            case CONFIG_INT:
            case CONFIG_SELECTION:
                if (config_get_int(section, name, e.config->default_int) != e.result_int) {
                    config_set_int(section, name, e.result_int);
                    changed = true;
                }
                break;

            case CONFIG_HEX16:
                if (e.value != e.result_int) {
                    config_set_hex16(section, name, e.result_int);
                    changed = true;
                }
                break;

            case CONFIG_HEX20:
                if (e.value != e.result_int) {
                    config_set_hex20(section, name, e.result_int);
                    changed = true;
                }
                break;

            case CONFIG_BIOS:
                if ((state.bios_rows > 0) && (e.result_int >= 0)) {
                    const char *bios_name = e.config->bios[e.result_int].internal_name;
                    if (e.selected != bios_name) {
                        config_set_string(section, name, const_cast<char *>(bios_name));
                        changed = true;
                    }
                }
                break;

            case CONFIG_SERPORT:
            case CONFIG_STRING:
            case CONFIG_FNAME:
                if (e.selected != e.result_str) {
                    config_set_string(section, name, const_cast<char *>(e.result_str.c_str()));
                    changed = true;
                }
                break;

            case CONFIG_STRHEX:
                {
                    const std::string hex = to_case(e.result_str, true);
                    if (strcasecmp(config_string(state.ctx, e.config).c_str(), hex.c_str())) {
                        config_set_string(section, name, const_cast<char *>(hex.c_str()));
                        changed = true;
                    }
                    break;
                }

            case CONFIG_MAC:
                {
                    /* Stored in lowercase; an unset MAC is always written. */
                    const std::string mac = to_case(e.result_str, false);
                    if ((config_get_mac(section, name, e.config->default_int) & 0xff000000)
                        || strcasecmp(config_string(state.ctx, e.config).c_str(), mac.c_str())) {
                        config_set_string(section, name, const_cast<char *>(mac.c_str()));
                        changed = true;
                    }
                    break;
                }

            default:
                break;
        }
    }

    return changed;
}
