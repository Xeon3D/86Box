/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the memory viewer and debugger window,
 *          like the Qt MemoryViewer (qt_memoryviewer.cpp).
 *
 *          Everything it shows is read through the debugger core
 *          (debugger.c), which never calls a device's handlers: only the
 *          bytes on screen (and the page table entries that map them) are
 *          read, and memory only devices can answer for is shown, not read.
 *          Reads are made with the blit lock held, as in Qt.
 *
 *          The hex pane omits addresses that are not mapped (classed
 *          DEBUGGER_MEM_UNMAPPED), presenting only the mapped regions
 *          contiguously.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Beep.h>
#include <Bitmap.h>
#include <Box.h>
#include <Button.h>
#include <CheckBox.h>
#include <Clipboard.h>
#include <Directory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <Font.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageFilter.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <SplitView.h>
#include <StatusBar.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "haiku_dialogs.hpp"
#include "haiku_disasm.hpp"
#include "haiku_icons.hpp"
#include "haiku_memoryviewer.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/debugger.h>
#include <86box/mem.h>
#include <86box/io.h>
#include <86box/version.h>

/* startblit(), giving up after timeout_ms (qt_platform.cpp). Weak: where the
   platform layer does not have it, the viewer waits for the lock instead,
   which it may, as it runs in its own thread. */
int startblit_timeout(int timeout_ms) __attribute__((weak));
}

static constexpr int BYTES_PER_ROW = 16;
#ifdef USE_DISASM
static constexpr int DISASM_LINES = 32;
#endif

/* Colours for the classes of memory. */
static const rgb_color rom_bg          = { 0xff, 0xe0, 0x60, 255 };
static const rgb_color rom_fg          = { 0x00, 0x00, 0x00, 255 };
static const rgb_color unmapped_fg     = { 0x90, 0x90, 0x90, 255 };
static const rgb_color inaccessible_bg = { 0xc0, 0x00, 0x00, 255 };
static const rgb_color inaccessible_fg = { 0xff, 0xff, 0xff, 255 };
static const rgb_color breakpoint_fg   = { 0xff, 0x00, 0x00, 255 };

enum : uint32 {
    MSG_MV_INIT          = 'mvin',
    MSG_MV_ACTIVATE      = 'mvac',
    MSG_MV_POLL          = 'mvpl',
    MSG_MV_RUN           = 'mvrn',
    MSG_MV_STEP          = 'mvst',
    MSG_MV_SPACE         = 'mvsp',
    MSG_MV_GO            = 'mvgo',
    MSG_MV_DUMP          = 'mvdp',
    MSG_MV_COMMANDS      = 'mvcm',
    MSG_MV_SEARCH_TYPE   = 'mvsy',
    MSG_MV_FIND_NEXT     = 'mvfn',
    MSG_MV_FIND_PREV     = 'mvfp',
    MSG_MV_RANGE         = 'mvra',
    MSG_MV_FOCUS_SEARCH  = 'mvfs',
    MSG_MV_DISASM_GO     = 'mvdg',
    MSG_MV_FOLLOW        = 'mvfo',
    MSG_MV_COMMAND       = 'mvcd',
    MSG_MV_HISTORY       = 'mvhi',
    MSG_MV_COPY_ASCII    = 'mvca',
    MSG_MV_MENU_COPY     = 'mvmc',
    MSG_MV_MENU_COPY_ASC = 'mvma',
    MSG_MV_MENU_FILL     = 'mvmf',
    MSG_MV_MENU_ADDR     = 'mvmd',
    MSG_MV_MENU_DISASM   = 'mvmu',
    MSG_MV_MENU_DUMP     = 'mvmx',
    MSG_MV_OK            = 'mvok',
    MSG_MV_CANCEL        = 'mvcn',
    MSG_MV_BROWSE        = 'mvbr',
    MSG_MV_FILE          = 'mvfl',
};

/* ------------------------------------------------------------- helpers */

static std::string strfmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static std::string
strfmt(const char *fmt, ...)
{
    char    buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

static std::string
trim(const std::string &s)
{
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

static std::string
lower(std::string s)
{
    for (char &c : s) {
        if ((c >= 'A') && (c <= 'Z'))
            c = (char) (c - 'A' + 'a');
    }
    return s;
}

static std::string
upper(std::string s)
{
    for (char &c : s) {
        if ((c >= 'a') && (c <= 'z'))
            c = (char) (c - 'a' + 'A');
    }
    return s;
}

static std::vector<std::string>
split_ws(const std::string &s)
{
    std::vector<std::string> out;
    size_t                   pos = 0;
    for (;;) {
        pos = s.find_first_not_of(" \t", pos);
        if (pos == std::string::npos)
            break;
        const size_t end = s.find_first_of(" \t", pos);
        out.push_back(s.substr(pos, (end == std::string::npos) ? std::string::npos : (end - pos)));
        if (end == std::string::npos)
            break;
        pos = end;
    }
    return out;
}

static bool
is_hex_digit(char c)
{
    return ((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f')) || ((c >= 'A') && (c <= 'F'));
}

/* Hex number, with an optional 0x prefix or h suffix. */
static bool
parse_hex(const std::string &text, uint32_t *val)
{
    std::string t = trim(text);
    if ((t.size() >= 2) && (t[0] == '0') && ((t[1] == 'x') || (t[1] == 'X')))
        t = t.substr(2);
    else if (!t.empty() && ((t.back() == 'h') || (t.back() == 'H')))
        t.pop_back();
    if (t.empty() || (t.size() > 8))
        return false;
    for (const char c : t) {
        if (!is_hex_digit(c))
            return false;
    }
    *val = (uint32_t) strtoul(t.c_str(), nullptr, 16);
    return true;
}

/* A decimal number, all of the text. */
static bool
parse_dec(const std::string &text, int *val)
{
    const std::string t = trim(text);
    if (t.empty() || (t.size() > 9))
        return false;
    for (const char c : t) {
        if ((c < '0') || (c > '9'))
            return false;
    }
    *val = atoi(t.c_str());
    return true;
}

static bool
readable(uint8_t cls)
{
    return (cls == DEBUGGER_MEM_RAM) || (cls == DEBUGGER_MEM_ROM) || (cls == DEBUGGER_MEM_VIDEO);
}

/* What the viewer may write: RAM, and video memory (debugger_mem_write()). */
static bool
writable(uint8_t cls)
{
    return (cls == DEBUGGER_MEM_RAM) || (cls == DEBUGGER_MEM_VIDEO);
}

/* Does the pattern match at this position? Every byte of the run must be
   readable, so a match never spans a mapped gap. */
static bool
match_at(const uint8_t *data, const uint8_t *cls, size_t pos, const std::vector<uint8_t> &pattern)
{
    for (size_t i = 0; i < pattern.size(); i++) {
        if (!readable(cls[pos + i]) || (data[pos + i] != pattern[i]))
            return false;
    }
    return true;
}

static void
copy_to_clipboard(const std::string &text)
{
    if (!be_clipboard->Lock())
        return;
    be_clipboard->Clear();
    if (BMessage *clip = be_clipboard->Data()) {
        clip->AddData("text/plain", B_MIME_TYPE, text.data(), (ssize_t) text.size());
        be_clipboard->Commit();
    }
    be_clipboard->Unlock();
}

static void
alert(const char *title, const std::string &text, alert_type type = B_WARNING_ALERT)
{
    BAlert *a = new BAlert(title, text.c_str(), "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, type);
    a->SetFlags(a->Flags() | B_CLOSE_ON_ESCAPE);
    a->Go();
}

/* ---------------------------------------------------------------- lock */

/* Nested locks only count: a refresh takes the lock once for all its
   parts, so it waits for the emulator at most once. Only the viewer's
   thread takes it. */
static int lock_depth = 0;

static bool
viewer_lock(void)
{
    if (lock_depth > 0) {
        lock_depth++;
        return true;
    }
    /* The emulator holds the lock for good while it shuts down. */
    if (is_quit)
        return false;
    if (startblit_timeout != nullptr) {
        if (!startblit_timeout(250))
            return false;
    } else
        startblit();
    /* A hard reset is freeing the memory this would read. */
    if (debugger_resetting()) {
        endblit();
        return false;
    }
    lock_depth = 1;
    return true;
}

static void
viewer_unlock(void)
{
    if (--lock_depth == 0)
        endblit();
}

/* Parse an address: hex, a register, or segment:offset with either part a
   register or hex (segment:offset is a linear address). */
static bool
parse_address(const std::string &text, int space, uint32_t *addr, std::string *err)
{
    const std::string t   = lower(trim(text));
    const size_t      sep = t.find(':');
    bool              ret = false;
    uint32_t          val = 0;

    if (t.empty()) {
        *err = "Enter an address.";
        return false;
    }

    if (!viewer_lock()) {
        *err = "The emulator is busy; try again.";
        return false;
    }

    if (sep != std::string::npos) {
        const std::string seg_name = trim(t.substr(0, sep));
        const std::string off_name = trim(t.substr(sep + 1));
        uint32_t          base     = 0;
        uint32_t          off      = 0;
        uint32_t          sel;

        if (debugger_register(seg_name.c_str(), &sel, &base) && (seg_name.size() == 2) && (seg_name[1] == 's'))
            ret = true;
        else if (parse_hex(seg_name, &sel) && (sel <= 0xffff)) {
            ret = debugger_selector_base((uint16_t) sel, &base);
            if (!ret)
                *err = "The selector cannot be looked up.";
        } else
            *err = "Unknown segment \"" + seg_name + "\".";

        if (ret) {
            if (!debugger_register(off_name.c_str(), &off, nullptr) && !parse_hex(off_name, &off)) {
                *err = "Unknown offset \"" + off_name + "\".";
                ret  = false;
            }
        }

        /* seg:off is linear; the physical view wants where that is now. */
        val = base + off;
        if (ret && (space == DEBUGGER_SPACE_PHYSICAL) && !debugger_translate(val, &val)) {
            *err = "The linear address is not mapped to physical memory.";
            ret  = false;
        }
    } else {
        ret = debugger_register(t.c_str(), &val, nullptr) || parse_hex(t, &val);
        if (!ret)
            *err = "\"" + trim(text) + "\" is not an address or a register.";
    }
    viewer_unlock();

    if (ret)
        *addr = val;
    return ret;
}

/* -------------------------------------------------------------- filters */

/* Return (and, for the command line, Up and Down) in a text field post a
   message to the window, as QLineEdit::returnPressed does; a BTextControl's
   own message would also come on losing the focus. */
class KeyFilter : public BMessageFilter {
public:
    KeyFilter(uint32 enter, uint32 history = 0)
        : BMessageFilter(B_ANY_DELIVERY, B_ANY_SOURCE, B_KEY_DOWN)
        , enter(enter)
        , history(history)
    {
    }

    filter_result Filter(BMessage *message, BHandler **target) override
    {
        /* The text view's window: the filter was added before it had one. */
        BLooper    *looper = ((target != nullptr) && (*target != nullptr)) ? (*target)->Looper() : nullptr;
        const char *bytes  = nullptr;
        if ((message->FindString("bytes", &bytes) != B_OK) || (bytes == nullptr) || (looper == nullptr))
            return B_DISPATCH_MESSAGE;
        if ((bytes[0] == B_ENTER) && enter) {
            looper->PostMessage(enter);
            return B_SKIP_MESSAGE;
        }
        if (history && ((bytes[0] == B_UP_ARROW) || (bytes[0] == B_DOWN_ARROW))) {
            BMessage msg(history);
            msg.AddInt32("delta", (bytes[0] == B_UP_ARROW) ? -1 : 1);
            looper->PostMessage(&msg);
            return B_SKIP_MESSAGE;
        }
        return B_DISPATCH_MESSAGE;
    }

private:
    uint32 enter;
    uint32 history;
};

/* --------------------------------------------------------- modal dialogs */

/* A dialog of the viewer's, run with haiku_run_modal() from its thread. */
class ViewerModal : public BWindow {
public:
    ViewerModal(BWindow *parent, const char *title)
        : BWindow(BRect(0, 0, 100, 100), title, B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
                  B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS
                      | B_CLOSE_ON_ESCAPE)
        , done(create_sem(0, "86Box memory viewer dialog"))
        , parent(parent)
    {
        if (parent != nullptr)
            AddToSubset(parent);
    }

    ~ViewerModal() override { release_sem(done); }

    void Go()
    {
        if (parent != nullptr)
            CenterIn(parent->Frame());
        haiku_run_modal(this, parent, done);
    }

protected:
    sem_id   done;
    BWindow *parent;
};

/* One line of text, as QInputDialog::getText(). */
class PromptWindow : public ViewerModal {
public:
    PromptWindow(BWindow *parent, const char *title, const std::string &label, const std::string &value,
                 std::string *result, bool *accepted)
        : ViewerModal(parent, title)
        , result(result)
        , accepted(accepted)
    {
        *accepted = false;
        text      = new BTextControl("text", nullptr, value.c_str(), nullptr);
        BButton *ok     = new BButton("ok", "OK", new BMessage(MSG_MV_OK));
        BButton *cancel = new BButton("cancel", "Cancel", new BMessage(MSG_MV_CANCEL));
        BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
            .SetInsets(B_USE_WINDOW_INSETS)
            .Add(new BStringView("label", label.c_str()))
            .Add(text)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(cancel)
                .Add(ok)
            .End();
        text->SetExplicitMinSize(BSize(be_plain_font->StringWidth("0") * 30, B_SIZE_UNSET));
        SetDefaultButton(ok);
        text->MakeFocus(true);
        text->TextView()->SelectAll();
    }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_MV_OK:
                *result   = text->Text();
                *accepted = true;
                Quit();
                break;
            case MSG_MV_CANCEL:
                Quit();
                break;
            default:
                BWindow::MessageReceived(message);
                break;
        }
    }

private:
    BTextControl *text;
    std::string  *result;
    bool         *accepted;
};

static bool
prompt(BWindow *parent, const char *title, const std::string &label, const std::string &value, std::string *result)
{
    bool accepted = false;
    (new PromptWindow(parent, title, label, value, result, &accepted))->Go();
    return accepted;
}

/* What the dump dialog was given. */
struct DumpFields {
    std::string start;
    std::string length;
    int         space = DEBUGGER_SPACE_PHYSICAL;
    std::string fill;
    std::string file;
};

/* Dump memory (Qt's dialog in MemoryViewer::onDump()). */
class DumpWindow : public ViewerModal {
public:
    DumpWindow(BWindow *parent, DumpFields *fields, bool *accepted)
        : ViewerModal(parent, "Dump memory")
        , fields(fields)
        , accepted(accepted)
    {
        *accepted = false;
        start     = new BTextControl("start", "Start (hex, register or seg:off):", fields->start.c_str(), nullptr);
        length    = new BTextControl("length", "Length (hex bytes):", fields->length.c_str(), nullptr);
        fill      = new BTextControl("fill", "Fill byte for memory that cannot be read:", fields->fill.c_str(), nullptr);
        fill->TextView()->SetMaxBytes(2);
        file = new BTextControl("file", "File:", fields->file.c_str(), nullptr);

        space_menu = new BPopUpMenu("space");
        BMenuItem *physical = new BMenuItem("Physical", nullptr);
        BMenuItem *linear   = new BMenuItem("Linear", nullptr);
        space_menu->AddItem(physical);
        space_menu->AddItem(linear);
        ((fields->space == DEBUGGER_SPACE_LINEAR) ? linear : physical)->SetMarked(true);
        BMenuField *space_field = new BMenuField("space_field", "Address space:", space_menu);

        BButton *browse = new BButton("browse", "Browse" B_UTF8_ELLIPSIS, new BMessage(MSG_MV_BROWSE));
        BButton *ok     = new BButton("ok", "OK", new BMessage(MSG_MV_OK));
        BButton *cancel = new BButton("cancel", "Cancel", new BMessage(MSG_MV_CANCEL));

        BStringView *note = new BStringView("note", "Only RAM and other directly mapped memory is read. Unmapped and "
                                                    "inaccessible bytes are not read; the fill byte is written in "
                                                    "their place.");
        note->SetExplicitMaxSize(BSize(be_plain_font->StringWidth("0") * 70, B_SIZE_UNSET));
        note->SetTruncation(B_TRUNCATE_END);
        note->SetToolTip(note->Text());

        BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
            .SetInsets(B_USE_WINDOW_INSETS)
            .AddGrid(B_USE_SMALL_SPACING, B_USE_SMALL_SPACING)
                .AddTextControl(start, 0, 0)
                .AddTextControl(length, 0, 1)
                .AddMenuField(space_field, 0, 2)
                .AddTextControl(fill, 0, 3)
                .AddTextControl(file, 0, 4)
                .Add(browse, 2, 4)
                .SetColumnWeight(1, 1.0f)
            .End()
            .Add(note)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(cancel)
                .Add(ok)
            .End();
        file->SetExplicitMinSize(BSize(be_plain_font->StringWidth("0") * 40, B_SIZE_UNSET));
        SetDefaultButton(ok);
        start->MakeFocus(true);
    }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_MV_OK:
                fields->start  = start->Text();
                fields->length = length->Text();
                fields->fill   = fill->Text();
                fields->file   = file->Text();
                fields->space  = (space_menu->IndexOf(space_menu->FindMarked()) == 1) ? DEBUGGER_SPACE_LINEAR
                                                                                       : DEBUGGER_SPACE_PHYSICAL;
                *accepted      = true;
                Quit();
                break;
            case MSG_MV_CANCEL:
                Quit();
                break;
            case MSG_MV_BROWSE:
                Browse();
                break;
            case MSG_MV_FILE:
                FileChosen(message);
                break;
            default:
                BWindow::MessageReceived(message);
                break;
        }
    }

private:
    void Browse()
    {
        BMessenger target(this);
        BMessage   msg(MSG_MV_FILE);
        panel.reset(new BFilePanel(B_SAVE_PANEL, &target, nullptr, B_FILE_NODE, false, &msg, nullptr, true));
        panel->Window()->SetTitle(EMU_NAME ": Dump memory");
        BPath current(file->Text());
        BPath dir;
        if ((file->Text()[0] != '\0') && (current.InitCheck() == B_OK) && (current.GetParent(&dir) == B_OK)
            && BEntry(dir.Path()).IsDirectory()) {
            panel->SetPanelDirectory(dir.Path());
            panel->SetSaveText(current.Leaf());
        } else if (usr_path[0] != '\0')
            panel->SetPanelDirectory(usr_path);
        panel->Show();
    }

    void FileChosen(BMessage *message)
    {
        entry_ref   ref;
        const char *name = nullptr;
        if ((message->FindRef("directory", &ref) != B_OK) || (message->FindString("name", &name) != B_OK))
            return;
        BDirectory dir(&ref);
        BPath      path(&dir, name);
        if (path.InitCheck() == B_OK)
            file->SetText(path.Path());
    }

    DumpFields                 *fields;
    bool                       *accepted;
    BTextControl               *start;
    BTextControl               *length;
    BTextControl               *fill;
    BTextControl               *file;
    BPopUpMenu                 *space_menu;
    std::unique_ptr<BFilePanel> panel;
};

/* --------------------------------------------------------------- progress */

/* The window of a Progress: it has a thread of its own, so Cancel works while
   the viewer's thread is busy. */
class ProgressWindow : public BWindow {
public:
    ProgressWindow(BWindow *parent, const char *text, float max, std::atomic<bool> *canceled)
        : BWindow(BRect(0, 0, 100, 100), EMU_NAME, B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
                  B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_NOT_CLOSABLE | B_AUTO_UPDATE_SIZE_LIMITS)
        , canceled(canceled)
    {
        if (parent != nullptr)
            AddToSubset(parent);
        bar = new BStatusBar("bar", text);
        bar->SetMaxValue(max);
        bar->SetExplicitMinSize(BSize(be_plain_font->StringWidth("0") * 40, B_SIZE_UNSET));
        BButton *cancel = new BButton("cancel", "Cancel", new BMessage(MSG_MV_CANCEL));
        BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
            .SetInsets(B_USE_WINDOW_INSETS)
            .Add(bar)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(cancel)
            .End();
        if (parent != nullptr)
            CenterIn(parent->Frame());
    }

    void SetValue(float value)
    {
        if (LockLooper()) {
            bar->SetTo(value);
            UnlockLooper();
        }
    }

    void MessageReceived(BMessage *message) override
    {
        if (message->what == MSG_MV_CANCEL)
            canceled->store(true);
        else
            BWindow::MessageReceived(message);
    }

    bool QuitRequested() override
    {
        canceled->store(true);
        return false;
    }

private:
    BStatusBar        *bar;
    std::atomic<bool> *canceled;
};

/* A progress dialog with a Cancel button, shown only once the work has taken
   half a second (QProgressDialog with setMinimumDuration(500)). */
class Progress {
public:
    Progress(BWindow *parent, const char *text, int max)
        : parent(parent)
        , text(text)
        , max(std::max(1, max))
        , started(system_time())
    {
    }

    ~Progress()
    {
        if ((window != nullptr) && window->Lock())
            window->Quit();
    }

    void SetValue(int value)
    {
        if ((window == nullptr) && ((system_time() - started) >= 500000)) {
            window = new ProgressWindow(parent, text, (float) max, &canceled);
            window->Show();
        }
        if (window != nullptr)
            window->SetValue((float) value);
        /* Keep the viewer drawn while its thread is busy here. */
        if (parent != nullptr)
            parent->UpdateIfNeeded();
    }

    int  Maximum() const { return max; }
    bool WasCanceled() const { return canceled.load(); }

private:
    BWindow          *parent;
    const char       *text;
    int               max;
    bigtime_t         started;
    ProgressWindow   *window = nullptr;
    std::atomic<bool> canceled { false };
};

/* ----------------------------------------------------------- the classes */

class MemoryViewerWindow;

/* A contiguous range of mapped addresses. */
struct MappedRange {
    uint32_t start;
    uint32_t length;
};

class HexView;

/* The hex view's scroll bar. Its value is in units of scale rows, so that a
   float holds it exactly even over 4 GB of mapped memory. */
class RowScrollBar : public BScrollBar {
public:
    explicit RowScrollBar(HexView *hex)
        : BScrollBar("hex_scroll", nullptr, 0, 0, B_VERTICAL)
        , hex(hex)
    {
    }

    void ValueChanged(float value) override;

    bool syncing = false;

private:
    HexView *hex;
};

/* A hex view of one address space that reads only the rows it shows,
   omitting addresses that are not mapped. */
class HexView : public BView {
public:
    explicit HexView(MemoryViewerWindow *owner);

    void     SetScrollBar(RowScrollBar *bar) { scroll = bar; }
    void     SetSpace(int space);
    int      Space() const { return space_; }
    void     GoTo(uint32_t addr);
    uint32_t Selection() const { return sel; }
    uint32_t SelectionStart() const { return std::min(sel, anchor); }
    uint32_t SelectionEnd() const { return std::max(sel, anchor); }
    uint64_t SelectionLen() const { return (uint64_t) SelectionEnd() - (uint64_t) SelectionStart() + 1ULL; }
    std::string SelectionText() const;
    void     Refresh();
    /* Search mapped memory for a run of bytes, selecting it when found.
       From and to bound the search to an address range, both ends
       inclusive. */
    bool     Find(const std::vector<uint8_t> &pattern, bool forward, uint32_t from = 0, uint32_t to = 0xffffffffU);
    void     ScrollFromBar(float value);
    void     CopySelection(bool ascii);

    void Draw(BRect update) override;
    void FrameResized(float width, float height) override;
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage *drag) override;
    void MouseUp(BPoint where) override;
    void KeyDown(const char *bytes, int32 numBytes) override;
    void MakeFocus(bool focus) override;
    void MessageReceived(BMessage *message) override;

private:
    int      VisibleRows() const;
    int64_t  MaxTop() const;
    void     UpdateScrollBar();
    void     SetTopRow(int64_t row);
    void     EnsureVisible(uint32_t addr);
    void     SetSelection(uint32_t addr, bool extend);
    void     MoveSelection(int64_t delta, bool extend);
    bool     HitTest(BPoint pos, uint32_t *addr) const;
    bool     WriteByte(uint32_t addr, uint8_t val);
    uint64_t WriteRange(uint32_t start, uint64_t len, uint8_t val);
    void     FillSelection();
    void     EditByte(uint32_t addr);
    void     ContextMenu(BPoint where);
    void     SelectRange(uint32_t start, uint32_t len, bool cursor_at_end);
    void     RebuildMappedRanges();
    uint32_t VirtualRowToAddr(int64_t row) const;
    int64_t  AddrToVirtualRow(uint32_t addr) const;
    void     Status(const std::string &msg);
    void     ReportRange();

    MemoryViewerWindow *owner;
    RowScrollBar       *scroll = nullptr;

    int      space_   = DEBUGGER_SPACE_PHYSICAL;
    uint32_t sel      = 0;
    uint32_t anchor   = 0; /* the other end of the selection */
    int      nibble   = 0; /* 1 after the high nibble of an edit was typed */
    uint8_t  edit_val = 0;

    int64_t top_row = 0;
    int64_t scale   = 1;

    uint32_t             cache_addr = 0;
    std::vector<uint8_t> data;
    std::vector<uint8_t> cls;

    bool dragging  = false;
    bool searching = false; /* a find is under way; not re-entered */

    float char_w = 8;
    float line_h = 16;
    float ascent = 12;

    /* Dense view of only mapped memory. */
    std::vector<MappedRange> mapped;
    uint64_t                 total_mapped = 0;
};

void
RowScrollBar::ValueChanged(float value)
{
    BScrollBar::ValueChanged(value);
    if (!syncing)
        hex->ScrollFromBar(value);
}

/* One line of the disassembly, as it is shown. */
struct DisasmRow {
    bool        breakpoint = false;
    bool        current    = false;
    bool        unreadable = false;
    bool        invalid    = false;
    uint32_t    lin        = 0;
    std::string address;
    std::string bytes;
    std::string text;
    std::string note;
};

/* The disassembly list (Qt's QTreeWidget with five columns). */
class DisasmListView : public BView {
public:
    explicit DisasmListView(MemoryViewerWindow *owner);

    void SetRows(const std::vector<DisasmRow> &rows);

    void Draw(BRect update) override;
    void FrameResized(float width, float height) override;
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage *drag) override;
    void TargetedByScrollView(BScrollView *view) override { scroll_view = view; }

private:
    void UpdateScrollBars();
    int  RowAt(BPoint where) const;

    MemoryViewerWindow    *owner;
    BScrollView           *scroll_view = nullptr;
    std::vector<DisasmRow> rows;
    float                  col_x[6] {};
    float                  line_h  = 16;
    float                  ascent  = 12;
    int                    tip_row = -2;
};

/* A legend entry: text in the colours of a class of memory. */
class SwatchView : public BView {
public:
    SwatchView(const char *text, rgb_color bg, rgb_color fg)
        : BView("swatch", B_WILL_DRAW)
        , text(text)
        , bg(bg)
        , fg(fg)
    {
        font_height fh;
        GetFontHeight(&fh);
        const BSize size(StringWidth(text) + 14, ceilf(fh.ascent + fh.descent) + 4);
        SetExplicitMinSize(size);
        SetExplicitMaxSize(size);
    }

    void Draw(BRect) override
    {
        BRect       r = Bounds();
        font_height fh;
        GetFontHeight(&fh);
        SetHighColor(bg);
        FillRect(r);
        SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_2_TINT));
        StrokeRect(r);
        SetHighColor(fg);
        SetLowColor(bg);
        DrawString(text, BPoint(7, 2 + ceilf(fh.ascent)));
    }

private:
    const char *text;
    rgb_color   bg;
    rgb_color   fg;
};

/* What the disassembly shows, read under the lock. */
struct DisasmSnapshot {
    debugger_cpu_info_t     info {};
    std::vector<DisasmInsn> lines;
    std::vector<bool>       bps;
    uint32_t                seg_base = 0;
};

class MemoryViewerWindow : public BWindow {
public:
    explicit MemoryViewerWindow(BRect frame);

    void MessageReceived(BMessage *message) override;
    void DispatchMessage(BMessage *message, BHandler *handler) override;
    bool QuitRequested() override;

    /* From the hex view (Qt's signals). */
    void Notice(const std::string &text);
    void RangeReport(const std::string &text);
    void DumpRequested(uint32_t start, uint32_t len);
    void DisassembleRequested(uint32_t addr);
    void MemoryWritten() { RefreshDisasm(); }
    /* From the disassembly. */
    void ToggleBreakpoint(uint32_t addr);

private:
    void OnRunToggle();
    void OnStep();
    void OnGo();
    void OnPoll();
    void OnDisasmGo();
    void OnDump(uint32_t start, uint32_t len);
    void OnSearch(bool forward);
    void RefreshAll();
    void ExecuteCommand();
    void History(int delta);

    std::vector<uint8_t> SearchPattern(std::string *err) const;
    bool                 ReadCpu(std::string *text);
    void                 ShowCpu(const std::string &text);
    bool                 ReadDisasm(DisasmSnapshot *view);
    void                 ShowDisasm(const DisasmSnapshot &view);
    void                 RefreshDisasm();
    void                 UpdateStatus();
    void                 UpdateRunButton();
    void                 RefreshMappings();
    void                 SetSpaceIndex(int index);
    void                 SetFollow(bool follow);
    void                 SetCommandsVisible(bool visible);
    void                 SetRangeVisible(bool visible);
    void                 Print(const std::string &text);
    int                  StepCount() const;

    HexView        *hex;
    RowScrollBar   *hex_scroll;
    BButton        *run_btn;
    BButton        *step_btn;
    BTextControl   *step_count;
    BPopUpMenu     *space_menu;
    BTextControl   *addr_edit;
    BCheckBox      *live_box;
    BButton        *commands_btn;
    BPopUpMenu     *search_type_menu;
    BTextControl   *search_edit;
    BButton        *range_btn;
    BGroupView     *range_bar;
    BTextControl   *range_from_edit;
    BTextControl   *range_to_edit;
    BTextView      *cpu_text;
    BScrollView    *cpu_scroll;
    DisasmListView *disasm;
    BTextControl   *disasm_edit;
    BCheckBox      *follow_box;
    BStringView    *status_label;
    BStringView    *message_label;
    BStringView    *range_label;
    BSplitView     *panes;
    BTextView      *command_output;
    BScrollView    *command_scroll;
    BTextControl   *command_input;
    BMessageRunner *timer = nullptr;

    std::vector<std::string> command_history;
    std::string              command_draft;
    size_t                   history_index = 0;

    bool        range_shown    = false;
    bool        commands_shown = true;
    int         last_state     = -1;
    uint32_t    last_stops     = 0;
    int         live_ticks     = 0;
    uint32_t    disasm_addr    = 0;
    int         run_icon       = -1;
    std::string status_text;
};

/* ---------------------------------------------------------------- HexView */

HexView::HexView(MemoryViewerWindow *owner)
    : BView("hex", B_WILL_DRAW | B_NAVIGABLE | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE)
    , owner(owner)
{
    SetFont(be_fixed_font);
    font_height fh;
    GetFontHeight(&fh);
    char_w = StringWidth("0");
    line_h = ceilf(fh.ascent + fh.descent + fh.leading);
    ascent = ceilf(fh.ascent);
    SetViewColor(B_TRANSPARENT_COLOR);

    /* address, 16 bytes with a gap after 8, the characters */
    const float width = char_w * (10 + (BYTES_PER_ROW * 3) + 1 + 1 + BYTES_PER_ROW + 2);
    SetExplicitMinSize(BSize(width, line_h * 8));
    SetExplicitPreferredSize(BSize(width, line_h * 32));
}

void
HexView::Status(const std::string &msg)
{
    owner->Notice(msg);
}

void
HexView::ReportRange()
{
    owner->RangeReport(SelectionText());
}

void
HexView::RebuildMappedRanges()
{
    mapped.clear();
    total_mapped = 0;

    if (!viewer_lock())
        return;

    const uint32_t PAGE = 4096;

    auto close_range = [this](uint32_t start, uint64_t end) {
        MappedRange r;
        r.start  = start;
        r.length = (uint32_t) (end - start);
        mapped.push_back(r);
        total_mapped += end - start;
    };

    uint32_t range_start = 0;
    bool     in_range    = false;
    if (space_ == DEBUGGER_SPACE_PHYSICAL) {
        /* Physical: a page is mapped when either the read or write mapping
           is present. Scanning the arrays is far cheaper than a million
           debugger_mem_read calls. */
        for (uint32_t g = 0; g < MEM_MAPPINGS_NO; g++) {
            const bool     is_mapped = (read_mapping[g] != nullptr) || (write_mapping[g] != nullptr);
            const uint32_t page      = g * PAGE;
            if (is_mapped) {
                if (!in_range) {
                    range_start = page;
                    in_range    = true;
                }
            } else if (in_range) {
                close_range(range_start, page);
                in_range = false;
            }
        }
    } else {
        /* Linear: a page is mapped when the page walk succeeds and the
           resulting physical page is not UNMAPPED. */
        for (uint64_t page = 0; page < 0x100000000ULL; page += PAGE) {
            uint8_t byte = 0;
            uint8_t c    = DEBUGGER_MEM_UNMAPPED;
            debugger_mem_read(space_, (uint32_t) page, 1, &byte, &c);
            if (c != DEBUGGER_MEM_UNMAPPED) {
                if (!in_range) {
                    range_start = (uint32_t) page;
                    in_range    = true;
                }
            } else if (in_range) {
                close_range(range_start, page);
                in_range = false;
            }
        }
    }
    /* The last range runs to the top of the address space; its length is
       kept in 32 bits, as in Qt. */
    if (in_range) {
        MappedRange r;
        r.start  = range_start;
        r.length = (uint32_t) (0x100000000ULL - range_start);
        mapped.push_back(r);
        total_mapped += r.length;
    }

    viewer_unlock();
}

uint32_t
HexView::VirtualRowToAddr(int64_t row) const
{
    if (mapped.empty() || (row < 0))
        return 0;

    uint64_t offset = (uint64_t) row * BYTES_PER_ROW;
    for (const MappedRange &r : mapped) {
        if (offset < r.length)
            return r.start + (uint32_t) offset;
        offset -= r.length;
    }
    /* Past the end: return the end of the last range. */
    const MappedRange &last = mapped.back();
    return last.start + last.length - 1;
}

int64_t
HexView::AddrToVirtualRow(uint32_t addr) const
{
    uint64_t before = 0;
    for (const MappedRange &r : mapped) {
        if ((addr >= r.start) && ((uint64_t) addr < ((uint64_t) r.start + r.length)))
            return (int64_t) ((before + (addr - r.start)) / BYTES_PER_ROW);
        before += r.length;
    }
    return -1; /* unmapped */
}

int
HexView::VisibleRows() const
{
    const int rows = (int) ((Bounds().Height() + 1) / line_h);
    return (rows < 1) ? 1 : rows;
}

int64_t
HexView::MaxTop() const
{
    const int64_t total_rows = (int64_t) ((total_mapped + BYTES_PER_ROW - 1) / BYTES_PER_ROW);
    return std::max<int64_t>(0, total_rows - VisibleRows());
}

void
HexView::UpdateScrollBar()
{
    const int64_t total_rows = (int64_t) ((total_mapped + BYTES_PER_ROW - 1) / BYTES_PER_ROW);
    const int     vis        = VisibleRows();
    const int64_t max_top    = MaxTop();

    top_row = std::clamp<int64_t>(top_row, 0, max_top);
    if (scroll == nullptr)
        return;

    scale = 1;
    while ((max_top / scale) > (1 << 22))
        scale <<= 1;
    scroll->syncing = true;
    scroll->SetRange(0, (float) (max_top / scale));
    scroll->SetSteps(1, (float) std::max<int64_t>(1, vis / scale));
    scroll->SetProportion(total_rows ? std::min(1.0f, (float) vis / (float) total_rows) : 1.0f);
    scroll->SetValue((float) (top_row / scale));
    scroll->syncing = false;
}

void
HexView::ScrollFromBar(float value)
{
    const int64_t row = (int64_t) llroundf(value) * scale;
    /* The bar holds the top row rounded down to its scale; leave a row it
       already shows alone. */
    if ((row / scale) == (top_row / scale))
        return;
    top_row = std::clamp<int64_t>(row, 0, MaxTop());
    Refresh();
}

void
HexView::SetTopRow(int64_t row)
{
    top_row = std::clamp<int64_t>(row, 0, MaxTop());
    if (scroll != nullptr) {
        scroll->syncing = true;
        scroll->SetValue((float) (top_row / scale));
        scroll->syncing = false;
    }
    Refresh();
}

void
HexView::SetSpace(int space)
{
    space_ = space;
    nibble = 0;
    RebuildMappedRanges();
    UpdateScrollBar();
    Refresh();
}

void
HexView::GoTo(uint32_t addr)
{
    sel    = addr;
    anchor = addr;
    nibble = 0;
    const int64_t vrow = AddrToVirtualRow(addr);
    if (vrow < 0) {
        /* Unmapped: just select it. */
        Refresh();
        ReportRange();
        return;
    }
    /* Put the address a few rows down from the top, for context. */
    SetTopRow(vrow - 2);
    ReportRange();
}

void
HexView::EnsureVisible(uint32_t addr)
{
    const int64_t vrow = AddrToVirtualRow(addr);
    if (vrow < 0)
        return;
    if (vrow < top_row)
        SetTopRow(vrow);
    else if (vrow >= (top_row + VisibleRows()))
        SetTopRow(vrow - VisibleRows() + 1);
}

void
HexView::Refresh()
{
    /* Safety net: if ranges were never built (or the lock failed earlier),
       build them now so rows do not all collapse to address 0. */
    if (mapped.empty()) {
        RebuildMappedRanges();
        UpdateScrollBar();
    }

    const int            rows = VisibleRows();
    std::vector<uint8_t> new_data((size_t) rows * BYTES_PER_ROW);
    std::vector<uint8_t> new_cls((size_t) rows * BYTES_PER_ROW);

    if (!viewer_lock()) {
        Status("The emulator is busy; the memory view was not updated.");
        return;
    }

    const uint32_t first = VirtualRowToAddr(top_row);
    for (int r = 0; r < rows; r++) {
        const uint32_t row_addr = VirtualRowToAddr(top_row + r);
        debugger_mem_read(space_, row_addr, BYTES_PER_ROW, &new_data[(size_t) r * BYTES_PER_ROW],
                          &new_cls[(size_t) r * BYTES_PER_ROW]);
    }

    viewer_unlock();

    cache_addr = first;
    data.swap(new_data);
    cls.swap(new_cls);
    Invalidate();
}

/* The selection, for the status bar. */
std::string
HexView::SelectionText() const
{
    if (sel == anchor)
        return strfmt("%08X", sel);
    return strfmt("%08X-%08X (%llu bytes)", SelectionStart(), SelectionEnd(), (unsigned long long) SelectionLen());
}

void
HexView::SetSelection(uint32_t addr, bool extend)
{
    if (!extend)
        anchor = addr;
    sel    = addr;
    nibble = 0;
    ReportRange();
    Invalidate();
}

/* Select a run of bytes and scroll it into view: its middle row at
   mid-screen when the run fits down the screen, its start near the top
   when it is longer. The cursor lands at one end of it, so repeating a
   search moves on instead of finding the same run again. If either end
   is not on a mapped row, nothing changes at all. */
void
HexView::SelectRange(uint32_t start, uint32_t len, bool cursor_at_end)
{
    const uint32_t new_sel    = cursor_at_end ? (start + (len - 1)) : start;
    const uint32_t new_anchor = cursor_at_end ? start : (start + (len - 1));

    const int64_t vrow_s = AddrToVirtualRow(std::min(new_sel, new_anchor));
    const int64_t vrow_e = AddrToVirtualRow(std::max(new_sel, new_anchor));
    if ((vrow_s < 0) || (vrow_e < 0)) {
        Status("The match is not on mapped memory; the selection and the view did not move.");
        return;
    }

    sel    = new_sel;
    anchor = new_anchor;
    nibble = 0;

    /* Fit the scroll bar to the map as it is now. */
    UpdateScrollBar();

    const int     vis   = VisibleRows();
    const int64_t run   = (vrow_e > vrow_s) ? (vrow_e - vrow_s + 1) : 1;
    const int64_t first = (run <= vis) ? ((vrow_s + vrow_e + 1) / 2 - (vis / 2)) : (vrow_s - (vis / 4));
    ReportRange();
    SetTopRow(first);
    /* Straight away, so the found run is on screen as the search answers. */
    if (Window() != nullptr)
        Window()->UpdateIfNeeded();
}

void
HexView::FrameResized(float width, float height)
{
    BView::FrameResized(width, height);
    UpdateScrollBar();
    Refresh();
}

void
HexView::MakeFocus(bool focus)
{
    BView::MakeFocus(focus);
    Invalidate();
}

void
HexView::Draw(BRect)
{
    const rgb_color base      = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
    const rgb_color text      = ui_color(B_DOCUMENT_TEXT_COLOR);
    rgb_color       highlight = ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);
    const rgb_color cursor    = ui_color(B_NAVIGATION_BASE_COLOR);
    const int       rows      = VisibleRows();
    const float     hex_x     = char_w * 10;
    const float     asc_x     = hex_x + char_w * ((BYTES_PER_ROW * 3) + 2);

    SetHighColor(base);
    FillRect(Bounds());
    SetLowColor(base);
    SetDrawingMode(B_OP_OVER);

    for (int r = 0; r < rows; r++) {
        const uint32_t row_addr = VirtualRowToAddr(top_row + r);
        const float    y        = r * line_h;

        SetHighColor(text);
        DrawString(strfmt("%08X", row_addr).c_str(), BPoint(0, y + ascent));

        for (int i = 0; i < BYTES_PER_ROW; i++) {
            const size_t idx = (size_t) r * BYTES_PER_ROW + i;
            if (idx >= data.size())
                break;

            const uint8_t c       = cls[idx];
            const uint8_t val     = data[idx];
            const float   hx      = hex_x + char_w * (i * 3 + ((i >= 8) ? 1 : 0));
            const float   ax      = asc_x + char_w * i;
            /* The hex cell's fill reaches over the gap that follows it. */
            const BRect   hex_rect(hx, y, hx + char_w * ((i == 7) ? 4 : 3) - 1, y + line_h - 1);
            const BRect   asc_rect(ax, y, ax + char_w - 1, y + line_h - 1);
            std::string   hex_text;
            char          asc_char;
            rgb_color     fg = text;

            switch (c) {
                case DEBUGGER_MEM_RAM:
                case DEBUGGER_MEM_ROM:
                case DEBUGGER_MEM_VIDEO:
                    hex_text = strfmt("%02X", val);
                    asc_char = ((val >= 0x20) && (val < 0x7f)) ? (char) val : '.';
                    /* Video memory is mapped, not RAM: yellow as well. */
                    if (c != DEBUGGER_MEM_RAM) {
                        SetHighColor(rom_bg);
                        FillRect(hex_rect);
                        FillRect(asc_rect);
                        fg = rom_fg;
                    }
                    break;
                case DEBUGGER_MEM_INACCESSIBLE:
                    hex_text = "??";
                    asc_char = '?';
                    SetHighColor(inaccessible_bg);
                    FillRect(hex_rect);
                    FillRect(asc_rect);
                    fg = inaccessible_fg;
                    break;
                default:
                    hex_text = "--";
                    asc_char = ' ';
                    fg       = unmapped_fg;
                    break;
            }

            const uint32_t addr = row_addr + i;
            if ((addr >= SelectionStart()) && (addr <= SelectionEnd())) {
                /* A translucent highlight, over whatever class was drawn. */
                SetDrawingMode(B_OP_ALPHA);
                SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
                highlight.alpha = 120;
                SetHighColor(highlight);
                FillRect(hex_rect);
                FillRect(asc_rect);
                SetDrawingMode(B_OP_OVER);
            }
            if (addr == sel) {
                SetHighColor(cursor);
                SetPenSize(IsFocus() ? 2 : 1);
                StrokeRect(BRect(hx - 1, y, hx + char_w * 2, y + line_h - 1));
                StrokeRect(BRect(ax - 1, y, ax + char_w, y + line_h - 1));
                SetPenSize(1);
                if (nibble)
                    hex_text = strfmt("%X_", edit_val >> 4);
            }

            const char asc[2] = { asc_char, '\0' };
            SetHighColor(fg);
            DrawString(hex_text.c_str(), BPoint(hx, y + ascent));
            DrawString(asc, BPoint(ax, y + ascent));
        }
    }
    SetDrawingMode(B_OP_COPY);
}

bool
HexView::HitTest(BPoint pos, uint32_t *addr) const
{
    const float hex_x = char_w * 10;
    const float asc_x = hex_x + char_w * ((BYTES_PER_ROW * 3) + 2);
    const int   row   = (int) (pos.y / line_h);
    int         col   = -1;

    if (pos.y < 0)
        return false;
    if ((pos.x >= hex_x) && (pos.x < (asc_x - char_w * 2))) {
        int c = (int) ((pos.x - hex_x) / char_w);
        if (c >= 25) /* the gap after the eighth byte */
            c--;
        col = std::min(c / 3, BYTES_PER_ROW - 1);
    } else if ((pos.x >= asc_x) && (pos.x < (asc_x + char_w * BYTES_PER_ROW)))
        col = (int) ((pos.x - asc_x) / char_w);

    if (col < 0)
        return false;

    *addr = VirtualRowToAddr(top_row + row) + col;
    return true;
}

void
HexView::MouseDown(BPoint where)
{
    BMessage *msg     = Window()->CurrentMessage();
    int32     buttons = B_PRIMARY_MOUSE_BUTTON;
    int32     clicks  = 1;
    uint32_t  addr;

    if (msg != nullptr) {
        msg->FindInt32("buttons", &buttons);
        msg->FindInt32("clicks", &clicks);
    }
    MakeFocus(true);

    if (buttons & B_SECONDARY_MOUSE_BUTTON) {
        ContextMenu(where);
        return;
    }
    if (!(buttons & B_PRIMARY_MOUSE_BUTTON) || !HitTest(where, &addr))
        return;

    if (clicks == 2) {
        EditByte(addr);
        return;
    }
    dragging = true;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    SetSelection(addr, (modifiers() & B_SHIFT_KEY) != 0);
}

void
HexView::MouseMoved(BPoint where, uint32 transit, const BMessage *drag)
{
    /* Extend the selection while the left button is held. */
    if (dragging) {
        uint32_t addr;
        if (HitTest(where, &addr) && (addr != sel))
            SetSelection(addr, true);
    }
    BView::MouseMoved(where, transit, drag);
}

void
HexView::MouseUp(BPoint where)
{
    dragging = false;
    BView::MouseUp(where);
}

void
HexView::MoveSelection(int64_t delta, bool extend)
{
    /* Move in the dense virtual address space so navigation skips unmapped. */
    const int64_t vrow = AddrToVirtualRow(sel);
    if ((vrow < 0) || (total_mapped == 0)) {
        /* Currently on unmapped memory: stay. */
        SetSelection(sel, extend);
        return;
    }
    int64_t voff = vrow * BYTES_PER_ROW + (sel % BYTES_PER_ROW) + delta;
    if (voff < 0)
        voff = 0;
    const uint64_t max_off = total_mapped - 1;
    if ((uint64_t) voff > max_off)
        voff = (int64_t) max_off;

    const uint32_t new_addr = VirtualRowToAddr(voff / BYTES_PER_ROW) + (uint32_t) (voff % BYTES_PER_ROW);
    SetSelection(new_addr, extend);
    EnsureVisible(new_addr);
}

void
HexView::KeyDown(const char *bytes, int32 numBytes)
{
    const uint32 mods   = modifiers();
    const bool   extend = (mods & B_SHIFT_KEY) != 0;

    if (numBytes == 1) {
        switch (bytes[0]) {
            case B_LEFT_ARROW:
                MoveSelection(-1, extend);
                return;
            case B_RIGHT_ARROW:
                MoveSelection(1, extend);
                return;
            case B_UP_ARROW:
                MoveSelection(-BYTES_PER_ROW, extend);
                return;
            case B_DOWN_ARROW:
                MoveSelection(BYTES_PER_ROW, extend);
                return;
            case B_PAGE_UP:
                MoveSelection(-(int64_t) VisibleRows() * BYTES_PER_ROW, extend);
                return;
            case B_PAGE_DOWN:
                MoveSelection((int64_t) VisibleRows() * BYTES_PER_ROW, extend);
                return;
            case B_HOME:
                MoveSelection(-(int64_t) (sel % BYTES_PER_ROW), extend);
                return;
            case B_END:
                MoveSelection(BYTES_PER_ROW - 1 - (sel % BYTES_PER_ROW), extend);
                return;
            case B_ESCAPE:
                if (nibble) {
                    nibble = 0;
                    Invalidate();
                    return;
                }
                if (sel != anchor) {
                    anchor = sel;
                    ReportRange();
                    Invalidate();
                    return;
                }
                break;
            default:
                break;
        }
    }

    if ((numBytes != 1) || !is_hex_digit(bytes[0]) || (mods & (B_COMMAND_KEY | B_CONTROL_KEY | B_OPTION_KEY))) {
        BView::KeyDown(bytes, numBytes);
        return;
    }
    const char ch    = bytes[0];
    const int  digit = (ch <= '9') ? (ch - '0') : (((ch | 0x20) - 'a') + 10);

    /* Only RAM and video memory can be written; the cached class is what
       is on screen. */
    const uint32_t idx = sel - cache_addr;
    if ((idx >= (uint32_t) cls.size()) || !writable(cls[idx])) {
        beep();
        Status("Only RAM and video memory can be written.");
        return;
    }

    if (!nibble) {
        edit_val = (uint8_t) (digit << 4);
        nibble   = 1;
        Invalidate();
        return;
    }

    edit_val |= (uint8_t) digit;
    nibble = 0;
    if (WriteByte(sel, edit_val))
        MoveSelection(1, false);
}

void
HexView::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case B_COPY:
            CopySelection(false);
            break;
        case MSG_MV_COPY_ASCII:
            CopySelection(true);
            break;
        case B_MOUSE_WHEEL_CHANGED: {
            float dy = 0;
            if ((message->FindFloat("be:wheel_delta_y", &dy) == B_OK) && (dy != 0))
                SetTopRow(top_row + (int64_t) (dy * 3));
            break;
        }
        default:
            BView::MessageReceived(message);
            break;
    }
}

bool
HexView::WriteByte(uint32_t addr, uint8_t val)
{
    int written = 0;
    if (viewer_lock()) {
        written = debugger_mem_write(space_, addr, val);
        viewer_unlock();
    }
    if (!written) {
        beep();
        Status(strfmt("%08X: ", addr) + "not written (not writable RAM or video memory, or the emulator is busy).");
    } else
        owner->MemoryWritten();
    Refresh();
    return written != 0;
}

/* Write a byte over a range; returns how many made it through. A page at a
   time, letting the emulator run in between, so a large fill does not
   hold it up for long. */
uint64_t
HexView::WriteRange(uint32_t start, uint64_t len, uint8_t val)
{
    uint64_t written = 0;
    uint64_t done    = 0;
    while (done < len) {
        const uint64_t addr = (uint64_t) start + done;
        const uint64_t n    = std::min<uint64_t>(len - done, 0x1000 - (addr & 0xfff));
        if (!viewer_lock()) {
            beep();
            Status("The emulator is busy; try again.");
            break;
        }
        for (uint64_t i = 0; i < n; i++)
            written += debugger_mem_write(space_, (uint32_t) (addr + i), val);
        viewer_unlock();
        done += n;
    }
    if (written > 0)
        owner->MemoryWritten();
    Refresh();
    return written;
}

/* Copy the selected bytes to the clipboard, as hex or ASCII. */
void
HexView::CopySelection(bool ascii)
{
    /* A selection the whole address space would not fit; cap the copy. */
    const uint64_t sel_len    = SelectionLen();
    const uint64_t count      = std::min<uint64_t>(sel_len, 0x1000000ULL);
    const uint64_t start      = SelectionStart();
    std::string    text;
    uint64_t       unreadable = 0;

    text.reserve((size_t) (ascii ? count : (count * 3)));

    /* A chunk at a time, letting the emulator run in between. */
    constexpr uint32_t   chunk = 0x10000;
    std::vector<uint8_t> buf(chunk);
    std::vector<uint8_t> buf_cls(chunk);
    uint64_t             done = 0;
    while (done < count) {
        const uint32_t n = (uint32_t) std::min<uint64_t>(chunk, count - done);
        if (!viewer_lock()) {
            Status("The emulator is busy; try again.");
            return;
        }
        debugger_mem_read(space_, (uint32_t) (start + done), n, buf.data(), buf_cls.data());
        viewer_unlock();
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t c = buf_cls[i];
            if (!readable(c))
                unreadable++;
            if (ascii) {
                const uint8_t v = buf[i];
                text += (readable(c) && (v >= 0x20) && (v < 0x7f)) ? (char) v : '.';
            } else if (readable(c)) {
                static const char digits[] = "0123456789ABCDEF";
                text += digits[buf[i] >> 4];
                text += digits[buf[i] & 15];
                text += ' ';
            } else
                text += "-- ";
        }
        done += n;
    }

    if (!ascii && !text.empty() && (text.back() == ' '))
        text.pop_back();
    copy_to_clipboard(text);

    std::string msg = strfmt("Copied %llu bytes from %08X to the clipboard", (unsigned long long) count, (uint32_t) start);
    if (unreadable)
        msg += strfmt(" (%llu could not be read)", (unsigned long long) unreadable);
    if (count < sel_len)
        msg += strfmt(" of a %llu byte selection", (unsigned long long) sel_len);
    Status(msg + ".");
}

/* Search mapped memory for a run of bytes. Forward scans the bytes after
   the cursor onward (wrapping); backward scans the runs whose last byte
   lies before the cursor, then wraps. From and to bound the scan to an
   address range (both inclusive). A chunk at a time, letting the emulator
   run in between, with an edge of already-read bytes kept so a match
   crossing the border between two chunks is still seen. Only a hit calls
   SelectRange(), so a search that fails leaves the selection and the view
   exactly where they were. */
bool
HexView::Find(const std::vector<uint8_t> &pattern, bool forward, uint32_t from, uint32_t to)
{
    const size_t plen = pattern.size();
    if (!plen || searching)
        return false;

    /* The ranges to scan: the mapped ones, clipped to the wanted span. */
    std::vector<MappedRange> scan;
    const bool               full = (from == 0) && (to == 0xffffffffU);
    for (const MappedRange &r : mapped) {
        const uint64_t start = std::max<uint64_t>(r.start, from);
        const uint64_t last  = std::min<uint64_t>((uint64_t) r.start + r.length - 1, to);
        if (last >= start)
            scan.push_back(MappedRange { (uint32_t) start, (uint32_t) (last - start + 1) });
    }

    if (scan.empty()) {
        Status(full ? "There is no mapped memory to search." : "There is no mapped memory in the requested range.");
        return false;
    }
    if (plen > 0x10000) {
        Status(strfmt("The search is limited to %d bytes.", 0x10000));
        return false;
    }

    searching = true;

    uint64_t scan_total = 0;
    for (const MappedRange &r : scan)
        scan_total += r.length;
    const int nr = (int) scan.size();

    /* The range and address where the scan starts. Forward starts at the
       first scanned byte after the cursor, backward before it; past either
       end, wrap round. */
    int      start_idx = -1;
    uint32_t start_pos = 0;
    if (forward) {
        const uint32_t after = sel + 1;
        for (int i = 0; i < nr; i++) {
            const MappedRange &r = scan[i];
            if ((uint64_t) after < (uint64_t) r.start) {
                start_idx = i;
                start_pos = r.start;
                break;
            }
            if (((uint64_t) r.start + r.length) > (uint64_t) after) {
                start_idx = i;
                start_pos = after;
                break;
            }
        }
        if (start_idx < 0) { /* at or past the last scanned byte */
            start_idx = 0;
            start_pos = scan[0].start;
        }
    } else {
        const uint32_t limit = sel - 1; /* matches end before the cursor */
        for (int i = nr - 1; i >= 0; i--) {
            const MappedRange &r = scan[i];
            if (((uint64_t) limit >= (uint64_t) r.start) && ((uint64_t) limit < ((uint64_t) r.start + r.length))) {
                start_idx = i;
                start_pos = limit;
                break;
            }
        }
        if (start_idx < 0) { /* before the first scanned byte */
            start_idx = nr - 1;
            start_pos = (uint32_t) ((uint64_t) scan[nr - 1].start + scan[nr - 1].length - 1);
        }
    }

    std::vector<uint8_t> rdata(0x10000);
    std::vector<uint8_t> rcls(0x10000);
    std::vector<uint8_t> buf;
    std::vector<uint8_t> cls_buf;
    uint32_t             buf_base = 0; /* the address of buf[0] */
    const size_t         keep     = plen - 1;

    Progress progress(Window(), "Searching memory" B_UTF8_ELLIPSIS,
                      (int) std::min<uint64_t>(0x7fffffff, (scan_total + 0xffff) / 0x10000));

    int  chunk_no = 0;
    bool found    = false;

    /* Through the ranges once, in the scan's direction: from the cursor to
       the end of the range, then the beginning. The first range is cut to
       the scan's side of the cursor, so no match reaches across it. */
    for (int k = 0; !found && (k < nr); k++) {
        const int          idx   = forward ? ((start_idx + k) % nr) : ((start_idx - k + nr) % nr);
        const MappedRange &range = scan[idx];
        uint64_t           lo    = (uint64_t) range.start;
        uint64_t           hi    = (uint64_t) range.start + range.length;

        if (k == 0) {
            if (forward)
                lo = std::max<uint64_t>(lo, start_pos);
            else
                hi = std::min<uint64_t>(hi, (uint64_t) start_pos + 1);
        }

        if (hi <= lo)
            continue;

        buf.clear();
        cls_buf.clear();
        buf_base = (uint32_t) lo;

        if (forward) {
            for (uint64_t pos = lo; pos < hi;) {
                const uint32_t n = (uint32_t) std::min<uint64_t>(hi - pos, 0x10000ULL);
                if (!viewer_lock()) {
                    Status("The emulator is busy; try again.");
                    searching = false;
                    return false;
                }
                debugger_mem_read(space_, (uint32_t) pos, n, rdata.data(), rcls.data());
                viewer_unlock();
                buf.insert(buf.end(), rdata.begin(), rdata.begin() + n);
                cls_buf.insert(cls_buf.end(), rcls.begin(), rcls.begin() + n);
                progress.SetValue(std::min(++chunk_no, progress.Maximum()));
                if (progress.WasCanceled()) {
                    Status("The search was cancelled. The selection did not move.");
                    searching = false;
                    return false;
                }

                for (size_t p = 0; (p + plen) <= buf.size(); p++) {
                    if (match_at(buf.data(), cls_buf.data(), p, pattern)) {
                        const uint32_t at = buf_base + (uint32_t) p;
                        SelectRange(at, (uint32_t) plen, true);
                        Status(strfmt("Found at %08X.", at));
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;

                if (buf.size() > keep) {
                    const size_t drop = buf.size() - keep;
                    buf.erase(buf.begin(), buf.begin() + (ptrdiff_t) drop);
                    cls_buf.erase(cls_buf.begin(), cls_buf.begin() + (ptrdiff_t) drop);
                    buf_base += (uint32_t) drop;
                }
                pos += n;
            }
        } else {
            for (uint64_t top = hi; top > lo;) {
                const uint32_t n = (uint32_t) std::min<uint64_t>(top - lo, 0x10000ULL);
                top -= n;
                if (!viewer_lock()) {
                    Status("The emulator is busy; try again.");
                    searching = false;
                    return false;
                }
                debugger_mem_read(space_, (uint32_t) top, n, rdata.data(), rcls.data());
                viewer_unlock();
                buf.insert(buf.begin(), rdata.begin(), rdata.begin() + n);
                cls_buf.insert(cls_buf.begin(), rcls.begin(), rcls.begin() + n);
                buf_base = (uint32_t) top;
                progress.SetValue(std::min(++chunk_no, progress.Maximum()));
                if (progress.WasCanceled()) {
                    Status("The search was cancelled. The selection did not move.");
                    searching = false;
                    return false;
                }

                if (buf.size() >= plen) {
                    for (size_t p = buf.size() - plen + 1; p-- > 0;) {
                        if (match_at(buf.data(), cls_buf.data(), p, pattern)) {
                            const uint32_t at = buf_base + (uint32_t) p;
                            SelectRange(at, (uint32_t) plen, false);
                            Status(strfmt("Found at %08X.", at));
                            found = true;
                            break;
                        }
                    }
                }
                if (found)
                    break;

                if (buf.size() > keep) {
                    buf.resize(keep);
                    cls_buf.resize(keep);
                }
            }
        }
    }

    searching = false;
    if (!found)
        Status("Not found. The selection did not move.");
    return found;
}

/* Fill the selected bytes with one value. */
void
HexView::FillSelection()
{
    const uint64_t sel_len = SelectionLen();
    const uint64_t count   = std::min<uint64_t>(sel_len, 0x1000000ULL);
    const uint32_t start   = SelectionStart();

    std::string text;
    if (!prompt(Window(), "Fill selection",
                strfmt("Fill %llu bytes from %08X with (hex):", (unsigned long long) count, start), "00", &text))
        return;
    uint32_t val;
    if (!parse_hex(text, &val) || (val > 0xff)) {
        Status("Enter a hex byte from 00 to FF.");
        return;
    }

    const uint64_t written = WriteRange(start, count, (uint8_t) val);
    std::string    msg     = strfmt("Filled %llu of %llu bytes with %02X (%llu were not writable RAM or video memory).",
                                    (unsigned long long) written, (unsigned long long) count, val,
                                    (unsigned long long) (count - written));
    if (count < sel_len)
        msg += strfmt(" Selected: %llu bytes (%llu were skipped).", (unsigned long long) sel_len,
                      (unsigned long long) (sel_len - count));
    Status(msg + ".");
}

/* Double-click: edit one byte in a prompt. */
void
HexView::EditByte(uint32_t addr)
{
    SetSelection(addr, false);

    const uint32_t idx = addr - cache_addr;
    if ((idx >= (uint32_t) cls.size()) || !writable(cls[idx])) {
        beep();
        Status("Only RAM and video memory can be written.");
        return;
    }

    std::string text;
    if (!prompt(Window(), "Edit byte", strfmt("%08X:", addr), strfmt("%02X", data[idx]), &text))
        return;
    uint32_t val;
    if (!parse_hex(text, &val) || (val > 0xff)) {
        Status("Enter a hex byte from 00 to FF.");
        return;
    }
    WriteByte(addr, (uint8_t) val);
}

void
HexView::ContextMenu(BPoint where)
{
    uint32_t addr;
    /* Clicking outside the selection starts a new one; inside keeps it. */
    if (HitTest(where, &addr) && ((addr < SelectionStart()) || (addr > SelectionEnd())))
        SetSelection(addr, false);

    BPopUpMenu menu("hex_menu", false, false);
    menu.AddItem(new BMenuItem("Copy selection (hex)", new BMessage(MSG_MV_MENU_COPY)));
    menu.AddItem(new BMenuItem("Copy selection (ASCII)", new BMessage(MSG_MV_MENU_COPY_ASC)));
    menu.AddItem(new BMenuItem("Fill selection" B_UTF8_ELLIPSIS, new BMessage(MSG_MV_MENU_FILL)));
    menu.AddSeparatorItem();
    menu.AddItem(new BMenuItem("Copy address", new BMessage(MSG_MV_MENU_ADDR)));
    BMenuItem *dis = new BMenuItem("Disassemble here", new BMessage(MSG_MV_MENU_DISASM));
    dis->SetEnabled(space_ == DEBUGGER_SPACE_LINEAR);
    menu.AddItem(dis);
    menu.AddItem(new BMenuItem("Dump memory from here" B_UTF8_ELLIPSIS, new BMessage(MSG_MV_MENU_DUMP)));

    BMenuItem *chosen = menu.Go(ConvertToScreen(where), false, true);
    if ((chosen == nullptr) || (chosen->Message() == nullptr))
        return;
    switch (chosen->Message()->what) {
        case MSG_MV_MENU_COPY:
            CopySelection(false);
            break;
        case MSG_MV_MENU_COPY_ASC:
            CopySelection(true);
            break;
        case MSG_MV_MENU_FILL:
            FillSelection();
            break;
        case MSG_MV_MENU_ADDR:
            copy_to_clipboard(strfmt("%08X", sel));
            break;
        case MSG_MV_MENU_DISASM:
            owner->DisassembleRequested(sel);
            break;
        case MSG_MV_MENU_DUMP:
            owner->DumpRequested(SelectionStart(), (uint32_t) std::min<uint64_t>(SelectionLen(), 0xffffffffULL));
            break;
        default:
            break;
    }
}

/* --------------------------------------------------------- DisasmListView */

DisasmListView::DisasmListView(MemoryViewerWindow *owner)
    : BView("disasm", B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE)
    , owner(owner)
{
    SetFont(be_fixed_font);
    font_height fh;
    GetFontHeight(&fh);
    line_h = ceilf(fh.ascent + fh.descent + fh.leading) + 2;
    ascent = ceilf(fh.ascent) + 1;
    SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
    SetToolTip("Double-click a line to set or clear a breakpoint there");
    SetRows(std::vector<DisasmRow>());
}

void
DisasmListView::SetRows(const std::vector<DisasmRow> &new_rows)
{
    static const char *const headers[5] = { "", "Address", "Bytes", "Instruction", "Note" };
    const float              pad        = StringWidth("0") * 2;

    rows = new_rows;

    /* The columns, fitted to their contents. */
    float x = 4;
    for (int c = 0; c < 5; c++) {
        float w = StringWidth(headers[c]);
        if (c == 0)
            w = std::max(w, StringWidth("\xe2\x97\x8f"));
        for (const DisasmRow &r : rows) {
            const std::string &s = (c == 1) ? r.address : (c == 2) ? r.bytes : (c == 3) ? r.text : r.note;
            if (c > 0)
                w = std::max(w, StringWidth(s.c_str()));
        }
        col_x[c] = x;
        x += w + pad;
    }
    col_x[5] = x;

    UpdateScrollBars();
    Invalidate();

    /* Keep the current line in view. */
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i].current) {
            const float top    = (i + 1) * line_h;
            const BRect bounds = Bounds();
            if ((top < bounds.top) || ((top + line_h) > bounds.bottom))
                ScrollTo(bounds.left, std::max(0.0f, top - line_h));
            break;
        }
    }
}

void
DisasmListView::UpdateScrollBars()
{
    if (scroll_view == nullptr)
        return;
    const BRect  bounds = Bounds();
    const float  height = (rows.size() + 1) * line_h;
    BScrollBar  *v      = scroll_view->ScrollBar(B_VERTICAL);
    BScrollBar  *h      = scroll_view->ScrollBar(B_HORIZONTAL);
    if (v != nullptr) {
        const float range = std::max(0.0f, height - bounds.Height());
        v->SetRange(0, range);
        v->SetProportion((height > 0) ? std::min(1.0f, bounds.Height() / height) : 1.0f);
        v->SetSteps(line_h, std::max(line_h, bounds.Height() - line_h));
    }
    if (h != nullptr) {
        const float range = std::max(0.0f, col_x[5] - bounds.Width());
        h->SetRange(0, range);
        h->SetProportion((col_x[5] > 0) ? std::min(1.0f, bounds.Width() / col_x[5]) : 1.0f);
        h->SetSteps(StringWidth("0") * 4, std::max(10.0f, bounds.Width() / 2));
    }
}

void
DisasmListView::FrameResized(float width, float height)
{
    BView::FrameResized(width, height);
    UpdateScrollBars();
}

void
DisasmListView::Draw(BRect update)
{
    static const char *const headers[5] = { "", "Address", "Bytes", "Instruction", "Note" };
    const rgb_color          base       = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
    const rgb_color          text       = ui_color(B_DOCUMENT_TEXT_COLOR);
    const rgb_color          sel_bg     = ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);
    const rgb_color          sel_fg     = ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR);
    const rgb_color          panel      = ui_color(B_PANEL_BACKGROUND_COLOR);
    const BRect              bounds     = Bounds();

    SetHighColor(base);
    FillRect(update);
    SetDrawingMode(B_OP_OVER);

    /* The header. */
    SetHighColor(panel);
    FillRect(BRect(bounds.left, 0, bounds.right, line_h - 1));
    SetHighColor(tint_color(panel, B_DARKEN_2_TINT));
    StrokeLine(BPoint(bounds.left, line_h - 1), BPoint(bounds.right, line_h - 1));
    SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
    for (int c = 1; c < 5; c++)
        DrawString(headers[c], BPoint(col_x[c], ascent));

    for (size_t i = 0; i < rows.size(); i++) {
        const DisasmRow &r = rows[i];
        const float      y = (i + 1) * line_h;
        if (((y + line_h) < update.top) || (y > update.bottom))
            continue;

        if (r.current) {
            SetHighColor(sel_bg);
            FillRect(BRect(col_x[1] - 2, y, std::max(bounds.right, col_x[5]), y + line_h - 1));
        }
        if (r.breakpoint) {
            SetHighColor(breakpoint_fg);
            DrawString("\xe2\x97\x8f", BPoint(col_x[0], y + ascent));
        }
        for (int c = 1; c < 5; c++) {
            rgb_color fg = text;
            if (r.current)
                fg = sel_fg;
            else if ((c == 3) && r.unreadable)
                fg = unmapped_fg;
            else if ((c == 3) && r.invalid)
                fg = inaccessible_bg;
            SetHighColor(fg);
            const std::string &s = (c == 1) ? r.address : (c == 2) ? r.bytes : (c == 3) ? r.text : r.note;
            DrawString(s.c_str(), BPoint(col_x[c], y + ascent));
        }
    }
    SetDrawingMode(B_OP_COPY);
}

int
DisasmListView::RowAt(BPoint where) const
{
    const int row = (int) (where.y / line_h) - 1;
    return ((row >= 0) && (row < (int) rows.size())) ? row : -1;
}

void
DisasmListView::MouseDown(BPoint where)
{
    BMessage *msg    = Window()->CurrentMessage();
    int32     clicks = 1;
    if (msg != nullptr)
        msg->FindInt32("clicks", &clicks);
    const int row = RowAt(where);
    if ((clicks == 2) && (row >= 0))
        owner->ToggleBreakpoint(rows[row].lin);
}

void
DisasmListView::MouseMoved(BPoint where, uint32 transit, const BMessage *drag)
{
    /* The note of the line under the mouse, which may be cut off. */
    const int row = ((transit == B_EXITED_VIEW) || (transit == B_OUTSIDE_VIEW)) ? -2 : RowAt(where);
    if (row != tip_row) {
        tip_row = row;
        if ((row >= 0) && !rows[row].note.empty())
            SetToolTip(rows[row].note.c_str());
        else
            SetToolTip("Double-click a line to set or clear a breakpoint there");
    }
    BView::MouseMoved(where, transit, drag);
}

/* ------------------------------------------------------ MemoryViewerWindow */

static BScrollView *
text_scroll(const char *name, BTextView **view)
{
    *view = new BTextView(name);
    (*view)->MakeEditable(false);
    (*view)->SetStylable(false);
    (*view)->SetWordWrap(false);
    (*view)->SetFontAndColor(be_fixed_font);
    (*view)->SetInsets(4, 2, 4, 2);
    return new BScrollView(name, *view, 0, true, true);
}

static BBox *
labelled_box(const char *name, const char *label, BView *content)
{
    BBox *box = new BBox(name);
    box->SetLabel(label);
    box->AddChild(content);
    return box;
}

MemoryViewerWindow::MemoryViewerWindow(BRect frame)
    : BWindow(frame, "Memory viewer", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS)
{
    /* A single toggle button, matching the main tool bar: the pause icon
       while the guest is running, depressed with the play (run) icon while
       it is paused. */
    run_btn = new BButton("run", nullptr, new BMessage(MSG_MV_RUN));
    run_btn->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);

    step_btn = new BButton("step", nullptr, new BMessage(MSG_MV_STEP));
    if (BBitmap *icon = haiku_icon("fast_forward"))
        step_btn->SetIcon(icon);
    else
        step_btn->SetLabel("Step");
    step_btn->SetToolTip("Run the given number of instructions (F11)");
    step_count = new BTextControl("step_count", nullptr, "1", nullptr);
    step_count->SetExplicitMaxSize(BSize(be_plain_font->StringWidth("0") * 10, B_SIZE_UNSET));
    step_count->SetToolTip("Instructions per step");

    space_menu = new BPopUpMenu("space");
    for (int i = 0; i < 2; i++) {
        BMessage *msg = new BMessage(MSG_MV_SPACE);
        msg->AddInt32("index", i);
        space_menu->AddItem(new BMenuItem(i ? "Linear" : "Physical", msg));
    }
    space_menu->ItemAt(0)->SetMarked(true);
    BMenuField *space_field = new BMenuField("space_field", nullptr, space_menu);

    addr_edit = new BTextControl("addr", nullptr, "", nullptr);
    addr_edit->SetToolTip("Address, register or seg:off");
    addr_edit->TextView()->AddFilter(new KeyFilter(MSG_MV_GO));
    BButton *go_btn = new BButton("go", "Go", new BMessage(MSG_MV_GO));
    live_box        = new BCheckBox("live", "Live update", nullptr);
    live_box->SetValue(B_CONTROL_ON);
    BButton *dump_btn = new BButton("dump", "Dump" B_UTF8_ELLIPSIS, new BMessage(MSG_MV_DUMP));
    commands_btn      = new BButton("commands", "Commands", new BMessage(MSG_MV_COMMANDS));
    commands_btn->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    commands_btn->SetValue(B_CONTROL_ON);
    commands_btn->SetToolTip("Show or hide debugger commands (Alt+D)");

    hex        = new HexView(this);
    hex_scroll = new RowScrollBar(hex);
    hex->SetScrollBar(hex_scroll);

    /* The search bar. Bytes can be given as raw hex pairs, as text, or as
       a byte, word or dword value in the guest's little-endian order. */
    search_type_menu = new BPopUpMenu("search_type");
    static const char *const search_types[5] = { "Hex bytes", "ASCII text", "Value, byte", "Value, word", "Value, dword" };
    for (const char *t : search_types)
        search_type_menu->AddItem(new BMenuItem(t, new BMessage(MSG_MV_SEARCH_TYPE)));
    search_type_menu->ItemAt(0)->SetMarked(true);
    BMenuField *search_type_field = new BMenuField("search_type_field", nullptr, search_type_menu);
    search_type_field->SetToolTip("What to look for: raw hex bytes, text, or a hex value stored little-endian, as in "
                                  "the hex view.");
    search_edit = new BTextControl("search", nullptr, "", nullptr);
    search_edit->SetToolTip("Memory search. Alt+F focuses this box; Enter or F3 finds the next match, Shift+F3 the "
                            "previous.");
    search_edit->TextView()->AddFilter(new KeyFilter(MSG_MV_FIND_NEXT));
    BButton *search_prev_btn = new BButton("find_prev", "Find previous", new BMessage(MSG_MV_FIND_PREV));
    search_prev_btn->SetToolTip("Search upwards from the selection (Shift+F3)");
    BButton *search_next_btn = new BButton("find_next", "Find next", new BMessage(MSG_MV_FIND_NEXT));
    search_next_btn->SetToolTip("Search downwards from the selection (F3)");
    range_btn = new BButton("range", "Range" B_UTF8_ELLIPSIS, new BMessage(MSG_MV_RANGE));
    range_btn->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    range_btn->SetToolTip("Search within an address range instead of all memory");
    range_from_edit = new BTextControl("range_from", nullptr, "", nullptr);
    range_from_edit->SetToolTip("Where the search starts (hex, register or seg:off; empty means the first mapped "
                                "byte; . means the selection start).");
    range_from_edit->TextView()->AddFilter(new KeyFilter(MSG_MV_FIND_NEXT));
    range_to_edit = new BTextControl("range_to", nullptr, "", nullptr);
    range_to_edit->SetToolTip("Where the search ends, that address included (hex, register or seg:off; empty means "
                              "the last mapped byte; . means the selection end).");
    range_to_edit->TextView()->AddFilter(new KeyFilter(MSG_MV_FIND_NEXT));

    range_bar = new BGroupView(B_HORIZONTAL, B_USE_SMALL_SPACING);
    BLayoutBuilder::Group<>(range_bar)
        .Add(new BStringView("range_from_label", "Range from"))
        .Add(range_from_edit, 1.0f)
        .Add(new BStringView("range_to_label", "to"))
        .Add(range_to_edit, 1.0f);

    /* The legend. */
    BStringView *legend_note = new BStringView("legend_note", "Inaccessible: only device handlers answer there "
                                                              "(reading would have side effects), or the linear page "
                                                              "is not present.");
    legend_note->SetHighColor(tint_color(ui_color(B_PANEL_TEXT_COLOR), B_LIGHTEN_1_TINT));
    legend_note->SetTruncation(B_TRUNCATE_END);
    legend_note->SetToolTip(legend_note->Text());
    legend_note->SetExplicitMinSize(BSize(50, B_SIZE_UNSET));

    BGroupView *left = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
    BLayoutBuilder::Group<>(left)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(search_type_field)
            .Add(search_edit, 1.0f)
            .Add(range_btn)
            .Add(search_prev_btn)
            .Add(search_next_btn)
        .End()
        .Add(range_bar)
        .AddGroup(B_HORIZONTAL, 0, 1.0f)
            .Add(hex, 1.0f)
            .Add(hex_scroll)
        .End()
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(new SwatchView("RAM", ui_color(B_DOCUMENT_BACKGROUND_COLOR), ui_color(B_DOCUMENT_TEXT_COLOR)))
            .Add(new SwatchView("Mapped, not RAM", rom_bg, rom_fg))
            .Add(new SwatchView("Unmapped (--)", ui_color(B_PANEL_BACKGROUND_COLOR), unmapped_fg))
            .Add(new SwatchView("Inaccessible (?\?)", inaccessible_bg, inaccessible_fg))
            .AddGlue()
        .End()
        .Add(legend_note);
    range_bar->Hide();

    BScrollView *cpu_box_scroll = text_scroll("cpu_text", &cpu_text);
    cpu_scroll                  = cpu_box_scroll;
    BGroupView *cpu_group       = new BGroupView(B_VERTICAL);
    BLayoutBuilder::Group<>(cpu_group).SetInsets(B_USE_SMALL_INSETS).Add(cpu_box_scroll);
    BBox *cpu_box = labelled_box("cpu_box", "Processor", cpu_group);

    disasm_edit = new BTextControl("disasm_addr", nullptr, "", nullptr);
    disasm_edit->SetToolTip("Linear address, register or seg:off");
    disasm_edit->TextView()->AddFilter(new KeyFilter(MSG_MV_DISASM_GO));
    follow_box = new BCheckBox("follow", "Follow CS:IP", new BMessage(MSG_MV_FOLLOW));
    follow_box->SetValue(B_CONTROL_ON);
    BButton *dis_go = new BButton("disasm_go", "Go", new BMessage(MSG_MV_DISASM_GO));
    disasm          = new DisasmListView(this);
    BScrollView *disasm_scroll = new BScrollView("disasm_scroll", disasm, 0, true, true);
    BGroupView  *dis_group     = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
    BLayoutBuilder::Group<>(dis_group)
        .SetInsets(B_USE_SMALL_INSETS)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(disasm_edit, 1.0f)
            .Add(dis_go)
            .Add(follow_box)
        .End()
        .Add(disasm_scroll, 1.0f);
#ifdef USE_DISASM
    BBox *dis_box = labelled_box("disasm_box", "Disassembly", dis_group);
#else
    BBox *dis_box = labelled_box("disasm_box", "Disassembly (not available in this build)", dis_group);
    disasm_edit->SetEnabled(false);
    dis_go->SetEnabled(false);
    follow_box->SetEnabled(false);
#endif

    BSplitView *right = new BSplitView(B_VERTICAL, B_USE_SMALL_SPACING);
    right->AddChild(cpu_box, 360);
    right->AddChild(dis_box, 440);

    BSplitView *split = new BSplitView(B_HORIZONTAL, B_USE_SMALL_SPACING);
    split->AddChild(left, 1.0f);
    split->AddChild(right, 1.0f);

    BScrollView *cmd_scroll = text_scroll("command_output", &command_output);
    command_scroll          = cmd_scroll;
    command_input           = new BTextControl("command_input", nullptr, "", nullptr);
    command_input->TextView()->SetFontAndColor(be_fixed_font);
    command_input->SetToolTip("help: commands; Up/Down: history; . is the selected address");
    command_input->TextView()->AddFilter(new KeyFilter(MSG_MV_COMMAND, MSG_MV_HISTORY));
    BGroupView *cmd_group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
    BLayoutBuilder::Group<>(cmd_group).SetInsets(B_USE_SMALL_INSETS).Add(cmd_scroll, 1.0f).Add(command_input);
    BBox *command_box = labelled_box("command_box", "Debugger commands", cmd_group);
    command_output->SetText("Commands share the memory viewer's address space and selection. Type help for "
                            "commands.\n");

    panes = new BSplitView(B_VERTICAL, B_USE_SMALL_SPACING);
    panes->AddChild(split, 580);
    panes->AddChild(command_box, 180);

    /* The status row: the guest's state at the left; at the right, the
       latest message (an error, warning or result) then the selection's
       range at the corner. The message is cut off when long; its tool tip
       carries it whole. */
    status_label  = new BStringView("status", "");
    message_label = new BStringView("message", "");
    range_label   = new BStringView("range", "");
    status_label->SetTruncation(B_TRUNCATE_END);
    status_label->SetExplicitMinSize(BSize(50, B_SIZE_UNSET));
    message_label->SetTruncation(B_TRUNCATE_END);
    message_label->SetExplicitMinSize(BSize(50, B_SIZE_UNSET));
    message_label->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    message_label->SetAlignment(B_ALIGN_RIGHT);

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
        .SetInsets(B_USE_SMALL_INSETS)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(run_btn)
            .Add(step_btn)
            .Add(step_count)
            .AddStrut(12)
            .Add(space_field)
            .Add(addr_edit, 1.0f)
            .Add(go_btn)
            .Add(live_box)
            .Add(dump_btn)
            .Add(commands_btn)
        .End()
        .Add(panes, 1.0f)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(status_label, 1.0f)
            .Add(message_label, 1.0f)
            .AddStrut(12)
            .Add(range_label)
        .End();

    AddShortcut('F', B_COMMAND_KEY, new BMessage(MSG_MV_FOCUS_SEARCH));
    AddShortcut('D', B_COMMAND_KEY, new BMessage(MSG_MV_COMMANDS));
    AddShortcut('C', B_COMMAND_KEY | B_SHIFT_KEY, new BMessage(MSG_MV_COPY_ASCII), hex);

    /* Attaching turns off the recompiler for as long as the window is open. */
    debugger_attach(1);
    last_stops = debugger_stop_count();

    BMessage poll(MSG_MV_POLL);
    timer = new BMessageRunner(BMessenger(this), &poll, 100000);

    /* Memory is read from the viewer's own thread only: the rest is done
       there, once the window has its size. */
    UpdateRunButton();
    PostMessage(MSG_MV_INIT);
}

bool
MemoryViewerWindow::QuitRequested()
{
    delete timer;
    timer = nullptr;
    debugger_attach(0);
    return true;
}

void
MemoryViewerWindow::DispatchMessage(BMessage *message, BHandler *handler)
{
    /* The function key shortcuts (QPushButton::setShortcut in Qt): F5 runs
       or stops, F11 steps, F3 and Shift+F3 search. */
    if ((message->what == B_KEY_DOWN) || (message->what == B_UNMAPPED_KEY_DOWN)) {
        int32 raw = 0;
        int32 key = 0;
        if ((message->FindInt32("raw_char", &raw) == B_OK) && (raw == B_FUNCTION_KEY)
            && (message->FindInt32("key", &key) == B_OK)) {
            switch (key) {
                case B_F5_KEY:
                    OnRunToggle();
                    return;
                case B_F11_KEY:
                    OnStep();
                    return;
                case B_F3_KEY:
                    OnSearch((modifiers() & B_SHIFT_KEY) == 0);
                    return;
                default:
                    break;
            }
        }
    }
    BWindow::DispatchMessage(message, handler);
}

void
MemoryViewerWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_MV_INIT:
            /* Build the mapped-range list for the initial (Physical) space
               before the first GoTo()/Refresh(), otherwise every row
               collapses to address 0. */
            hex->SetSpace(DEBUGGER_SPACE_PHYSICAL);
            hex->GoTo(0);
            RefreshAll();
            hex->MakeFocus(true);
            break;
        case MSG_MV_ACTIVATE:
            Activate(true);
            break;
        case MSG_MV_POLL:
            OnPoll();
            break;
        case MSG_MV_RUN:
            OnRunToggle();
            break;
        case MSG_MV_STEP:
            OnStep();
            break;
        case MSG_MV_SPACE:
            SetSpaceIndex(message->GetInt32("index", 0));
            break;
        case MSG_MV_GO:
            OnGo();
            break;
        case MSG_MV_DUMP:
            OnDump(hex->Selection(), (uint32_t) std::min<uint64_t>(hex->SelectionLen(), 0xffffffffULL));
            break;
        case MSG_MV_COMMANDS:
            SetCommandsVisible(!commands_shown);
            break;
        case MSG_MV_SEARCH_TYPE:
            break;
        case MSG_MV_FIND_NEXT:
            OnSearch(true);
            break;
        case MSG_MV_FIND_PREV:
            OnSearch(false);
            break;
        case MSG_MV_RANGE:
            SetRangeVisible(range_btn->Value() == B_CONTROL_ON);
            break;
        case MSG_MV_FOCUS_SEARCH:
            search_edit->MakeFocus(true);
            search_edit->TextView()->SelectAll();
            break;
        case MSG_MV_DISASM_GO:
            OnDisasmGo();
            break;
        case MSG_MV_FOLLOW:
            RefreshDisasm();
            break;
        case MSG_MV_COMMAND:
            ExecuteCommand();
            break;
        case MSG_MV_HISTORY:
            History(message->GetInt32("delta", 0));
            break;
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

void
MemoryViewerWindow::Notice(const std::string &text)
{
    message_label->SetText(text.c_str());
    message_label->SetToolTip(text.c_str());
}

void
MemoryViewerWindow::RangeReport(const std::string &text)
{
    range_label->SetText(text.c_str());
    range_label->SetToolTip(text.c_str());
}

void
MemoryViewerWindow::DumpRequested(uint32_t start, uint32_t len)
{
    OnDump(start, len);
}

void
MemoryViewerWindow::DisassembleRequested(uint32_t addr)
{
    follow_box->SetValue(B_CONTROL_OFF);
    disasm_addr = addr;
    disasm_edit->SetText(strfmt("%08X", addr).c_str());
    RefreshDisasm();
}

void
MemoryViewerWindow::ToggleBreakpoint(uint32_t addr)
{
    if (debugger_breakpoint_toggle(addr) < 0)
        Notice("Too many breakpoints.");
    RefreshDisasm();
}

void
MemoryViewerWindow::SetSpaceIndex(int index)
{
    if ((index < 0) || (index > 1))
        return;
    space_menu->ItemAt(index)->SetMarked(true);
    const int space = index ? DEBUGGER_SPACE_LINEAR : DEBUGGER_SPACE_PHYSICAL;
    if (space != hex->Space())
        hex->SetSpace(space);
}

void
MemoryViewerWindow::SetFollow(bool follow)
{
    if ((follow_box->Value() == B_CONTROL_ON) == follow)
        return;
    follow_box->SetValue(follow ? B_CONTROL_ON : B_CONTROL_OFF);
    RefreshDisasm();
}

void
MemoryViewerWindow::SetCommandsVisible(bool visible)
{
    commands_shown = visible;
    commands_btn->SetValue(visible ? B_CONTROL_ON : B_CONTROL_OFF);
    panes->SetItemCollapsed(1, !visible);
    if (visible)
        command_input->MakeFocus(true);
}

void
MemoryViewerWindow::SetRangeVisible(bool visible)
{
    if (visible == range_shown)
        return;
    range_shown = visible;
    range_btn->SetValue(visible ? B_CONTROL_ON : B_CONTROL_OFF);
    if (visible) {
        range_bar->Show();
        range_from_edit->MakeFocus(true);
    } else
        range_bar->Hide();
}

int
MemoryViewerWindow::StepCount() const
{
    int count = 1;
    if (!parse_dec(step_count->Text(), &count))
        count = 1;
    return std::clamp<int>(count, 1, 1000000);
}

void
MemoryViewerWindow::OnRunToggle()
{
    /* Toggle based on the current debugger state, not the button's value
       (which the click has already flipped). */
    if (debugger_get_state() == DEBUGGER_BREAK)
        debugger_continue();
    else
        debugger_pause();
    UpdateStatus();
}

void
MemoryViewerWindow::OnStep()
{
    debugger_step(StepCount());
    UpdateStatus();
}

/* Raised with the pause icon while running; depressed with the play icon
   while paused, matching the main window tool bar. */
void
MemoryViewerWindow::UpdateRunButton()
{
    const bool stopped = (debugger_get_state() == DEBUGGER_BREAK);
    run_btn->SetValue(stopped ? B_CONTROL_ON : B_CONTROL_OFF);
    if (run_icon == (int) stopped)
        return;
    run_icon = stopped;
    if (BBitmap *icon = haiku_icon(stopped ? "run" : "pause"))
        run_btn->SetIcon(icon);
    else
        run_btn->SetLabel(stopped ? "Run" : "Pause");
    run_btn->SetToolTip(stopped ? "Let the processor run (F5)" : "Stop the processor at the next instruction (F5)");
}

void
MemoryViewerWindow::OnGo()
{
    uint32_t    addr;
    std::string err;
    if (!parse_address(addr_edit->Text(), hex->Space(), &addr, &err)) {
        Notice(err);
        return;
    }
    hex->GoTo(addr);
}

void
MemoryViewerWindow::OnDisasmGo()
{
#ifdef USE_DISASM
    uint32_t    addr;
    std::string err;
    if (!parse_address(disasm_edit->Text(), DEBUGGER_SPACE_LINEAR, &addr, &err)) {
        Notice(err);
        return;
    }
    follow_box->SetValue(B_CONTROL_OFF);
    disasm_addr = addr;
    RefreshDisasm();
#endif
}

void
MemoryViewerWindow::OnPoll()
{
    const int      state = debugger_get_state();
    const uint32_t stops = debugger_stop_count();

    if ((state != last_state) || (stops != last_stops)) {
        const bool stopped = (stops != last_stops);
        last_state         = state;
        last_stops         = stops;
        UpdateStatus();
        if (stopped) {
            RefreshAll();
            Print("Stopped: " + status_text);
            if (debugger_stop_reason() == DEBUGGER_STOP_BREAKPOINT)
                Activate(true);
        }
        return;
    }

    if ((state != DEBUGGER_BREAK) && (live_box->Value() == B_CONTROL_ON) && (++live_ticks >= 5)) {
        live_ticks = 0;
        RefreshAll();
    }
}

void
MemoryViewerWindow::RefreshAll()
{
    std::string    cpu;
    DisasmSnapshot view;
    bool           have_cpu;
    bool           have_disasm;

    /* One lock for every part, so they agree with each other; the views
       are updated after it, as the emulator waits for as long as it is
       held. */
    if (!viewer_lock()) {
        Notice("The emulator is busy; the view was not updated.");
        return;
    }
    hex->Refresh();
    have_cpu    = ReadCpu(&cpu);
    have_disasm = ReadDisasm(&view);
    viewer_unlock();

    if (have_cpu)
        ShowCpu(cpu);
    if (have_disasm)
        ShowDisasm(view);
    UpdateStatus();
}

void
MemoryViewerWindow::UpdateStatus()
{
    std::string text;

    switch (debugger_get_state()) {
        case DEBUGGER_BREAK:
            switch (debugger_stop_reason()) {
                case DEBUGGER_STOP_BREAKPOINT:
                    text = "Stopped at a breakpoint.";
                    break;
                case DEBUGGER_STOP_STEP:
                    text = "Stopped after stepping.";
                    break;
                default:
                    text = "Stopped.";
                    break;
            }
            break;
        case DEBUGGER_STEP:
            text = "Stepping" B_UTF8_ELLIPSIS;
            break;
        default:
            text = "Running.";
            break;
    }

    if (dopause)
        text += " The emulator is paused; resume it to run or step.";
    text += " The dynamic recompiler is off while this window is open.";
    if (text != status_text) {
        status_text = text;
        status_label->SetText(text.c_str());
        status_label->SetToolTip(text.c_str());
    }
    UpdateRunButton();
}

bool
MemoryViewerWindow::ReadCpu(std::string *text)
{
    char buf[4096];

    buf[0] = '\0';
    if (!viewer_lock())
        return false;
    debugger_cpu_format(buf, sizeof(buf));
    viewer_unlock();

    *text = buf;
    return true;
}

void
MemoryViewerWindow::ShowCpu(const std::string &text)
{
    if (text == cpu_text->Text())
        return;
    BScrollBar *v   = cpu_scroll->ScrollBar(B_VERTICAL);
    BScrollBar *h   = cpu_scroll->ScrollBar(B_HORIZONTAL);
    const float pos = (v != nullptr) ? v->Value() : 0;
    const float hp  = (h != nullptr) ? h->Value() : 0;
    cpu_text->SetText(text.c_str());
    if (v != nullptr)
        v->SetValue(pos);
    if (h != nullptr)
        h->SetValue(hp);
}

bool
MemoryViewerWindow::ReadDisasm(DisasmSnapshot *view)
{
#ifdef USE_DISASM
    const bool follow = (follow_box->Value() == B_CONTROL_ON);

    if (!viewer_lock())
        return false;
    debugger_cpu_info(&view->info);
    {
        Disassembler dis;
        uint32_t     ip = follow ? view->info.ip : (disasm_addr - dis.csBase());
        if (!follow && dis.wraps() && (ip > 0xffff)) {
            /* Out of CS's reach where IP wraps at 64 KB: decode in the
               segment the address starts. */
            dis.setBase(disasm_addr & ~0x0f);
            ip = disasm_addr & 0x0f;
        }
        view->seg_base = dis.csBase();
        for (int i = 0; i < DISASM_LINES; i++) {
            /* The instruction at CS:IP may already be in the prefetch queue. */
            DisasmInsn d = dis.decode(ip, (dis.csBase() == view->info.cs_base) && (ip == view->info.ip));
            view->bps.push_back(debugger_breakpoint_present(d.lin) != 0);
            view->lines.push_back(d);
            /* Past something unreadable, go on at the next byte. */
            ip = dis.next(ip, d.len);
        }
    }
    viewer_unlock();
    return true;
#else
    (void) view;
    return false;
#endif
}

void
MemoryViewerWindow::ShowDisasm(const DisasmSnapshot &view)
{
    const debugger_cpu_info_t &info = view.info;

    if (follow_box->Value() == B_CONTROL_ON)
        disasm_edit->SetText(strfmt("%04X:%0*X", info.cs_sel, info.code32 ? 8 : 4, info.ip).c_str());

    std::vector<DisasmRow> rows;
    for (size_t n = 0; n < view.lines.size(); n++) {
        const DisasmInsn &l = view.lines[n];
        DisasmRow         r;
        for (int i = 0; (i < l.len) && (i < 16); i++)
            r.bytes += strfmt("%02X", l.bytes[i]);
        r.breakpoint = view.bps[n];
        r.current    = (l.lin == info.pc_linear);
        r.unreadable = (l.len <= 0);
        r.invalid    = l.invalid;
        r.lin        = l.lin;
        if (view.seg_base == info.cs_base)
            r.address = strfmt("%08X  %04X:%0*X", l.lin, info.cs_sel, info.code32 ? 8 : 4,
                               info.code32 ? l.ip : (l.ip & 0xffff));
        else if (!info.protected_mode)
            r.address = strfmt("%08X  %04X:%04X", l.lin, view.seg_base >> 4, l.ip & 0xffff);
        else
            r.address = strfmt("%08X", l.lin);
        r.text = l.text;
        r.note = l.note;
        rows.push_back(r);
    }
    disasm->SetRows(rows);
}

void
MemoryViewerWindow::RefreshDisasm()
{
    DisasmSnapshot view;

    if (ReadDisasm(&view))
        ShowDisasm(view);
}

void
MemoryViewerWindow::RefreshMappings()
{
    const uint32_t selected = hex->Selection();
    hex->SetSpace(hex->Space());
    hex->GoTo(selected);
    RefreshAll();
}

void
MemoryViewerWindow::Print(const std::string &text)
{
    std::string line = text;
    if (line.empty() || (line.back() != '\n'))
        line += '\n';
    command_output->Insert(command_output->TextLength(), line.c_str(), (int32) line.size());

    /* Keep the last 4096 lines (setMaximumBlockCount in Qt). */
    const int32 lines = command_output->CountLines();
    if (lines > 4097)
        command_output->Delete(0, command_output->OffsetAt(lines - 4097));
    command_output->ScrollToOffset(command_output->TextLength());
}

void
MemoryViewerWindow::History(int delta)
{
    if (history_index == command_history.size())
        command_draft = command_input->Text();
    const int64_t index = std::clamp<int64_t>((int64_t) history_index + delta, 0, (int64_t) command_history.size());
    history_index       = (size_t) index;
    const std::string &text = (history_index == command_history.size()) ? command_draft : command_history[history_index];
    command_input->SetText(text.c_str());
    command_input->TextView()->Select(command_input->TextView()->TextLength(), command_input->TextView()->TextLength());
}

void
MemoryViewerWindow::ExecuteCommand()
{
    const std::string line = trim(command_input->Text());
    if (line.empty())
        return;
    command_input->SetText("");
    if (command_history.empty() || (command_history.back() != line))
        command_history.push_back(line);
    if (command_history.size() > 200)
        command_history.erase(command_history.begin());
    history_index = command_history.size();
    command_draft.clear();

    Print(strfmt("[%s:%08X] > %s", (hex->Space() == DEBUGGER_SPACE_LINEAR) ? "Linear" : "Physical", hex->Selection(),
                 line.c_str()));
    const std::vector<std::string> args = split_ws(line);
    const std::string              cmd  = lower(args[0]);
    const int                      n    = (int) args.size() - 1;
    auto usage = [this](const char *text) { Print(std::string("Usage: ") + text); };
    auto address = [this](const std::string &text, int space, uint32_t *value) {
        if (text == ".") {
            if (space != hex->Space()) {
                Print("The selection is physical; use a linear address or switch to linear space.");
                return false;
            }
            *value = hex->Selection();
            return true;
        }
        std::string error;
        if (parse_address(text, space, value, &error))
            return true;
        Print(error);
        return false;
    };
    /* Never hold the emulator lock while updating views or opening a dialog. */
    auto acquire = [this]() {
        if (viewer_lock())
            return true;
        Print("The emulator is busy; try again.");
        return false;
    };

    if ((cmd == "help") && (n == 1) && (lower(args[1]) == "registers")) {
        Print("r name [hex-value]: read or edit a register supported by the current processor.\n"
              "EAX ECX EDX EBX ESP EBP ESI EDI; AX CX DX BX SP BP SI DI; AL CL DL BL AH CH DH BH\n"
              "IP EIP FLAGS EFLAGS; CS SS DS ES FS GS; MSW CR0 CR2 CR3 CR4; DR0..DR7\n"
              "GDTR.base GDTR.limit IDTR.base IDTR.limit LDTR TR\n"
              "CS/SS/DS/ES/FS/GS/LDTR/TR: .base .limit .access .flags (hidden descriptor cache)\n"
              "Selectors load descriptors without executing guest instructions; cache fields are literal edits.\n"
              "Writes require pause at a complete instruction boundary. NEC 8080-mode register edits are unavailable.");
    } else if ((cmd == "help") && !n) {
        Print("Numbers are hexadecimal; step counts are decimal. Addresses accept registers and seg:off.\n"
              ". means the hex selection; omitted memory addresses use it.\n"
              "help | clear | refresh | regs\n"
              "run | pause | step [count]\n"
              "space physical|linear | goto address | db [address [length]] (max 1000 hex bytes)\n"
              "write address byte [byte ...] | dump (opens selection export)\n"
              "dbio [address [length]] | writeio address byte [byte ...] (device side effects; requires pause)\n"
              "u address (linear disassembly) | follow on|off\n"
              "bp address | bc address|all | bl (linear breakpoints)\n"
              "r [register [value]] (read/set; values are hex); help registers lists names\n"
              "peek port [b|w|d] | poke port value [b|w|d]\n"
              "Register/memory writes and I/O require pause. I/O invokes device handlers, including read side effects.");
    } else if ((cmd == "clear") && !n) {
        command_output->SetText("");
    } else if ((cmd == "refresh") && !n) {
        RefreshMappings();
    } else if (((cmd == "regs") || (cmd == "r")) && !n) {
        std::string text;
        if (ReadCpu(&text)) {
            ShowCpu(text);
            Print(text);
        } else
            Print("The emulator is busy; try again.");
    } else if ((cmd == "r") && ((n == 1) || (n == 2))) {
        uint32_t value = 0;
        if ((n == 2) && !parse_hex(args[2], &value)) {
            usage("r register hexadecimal-value");
            return;
        }
        if (!acquire())
            return;
        const std::string name   = lower(args[1]);
        const int         result = (n == 2) ? debugger_register_write(name.c_str(), value)
                                            : debugger_register(name.c_str(), &value, nullptr);
        if ((result > 0) && (n == 2))
            debugger_register(name.c_str(), &value, nullptr);
        viewer_unlock();
        if (result > 0) {
            Print(strfmt("%s = %08X", upper(args[1]).c_str(), value));
            if (n == 2)
                RefreshMappings();
        } else
            Print("Register unavailable, value invalid, or processor not paused at a complete instruction boundary.");
    } else if (((cmd == "run") || (cmd == "pause")) && !n) {
        if (cmd == "run")
            debugger_continue();
        else
            debugger_pause();
        UpdateStatus();
        Print(status_text);
    } else if ((cmd == "step") && (n <= 1)) {
        int count = 1;
        if ((n && !parse_dec(args[1], &count)) || (count < 1) || (count > 1000000)) {
            usage("step [1..1000000] (decimal)");
            return;
        }
        step_count->SetText(strfmt("%d", count).c_str());
        OnStep();
    } else if ((cmd == "space") && (n == 1)) {
        const std::string space = lower(args[1]);
        if ((space != "physical") && (space != "linear")) {
            usage("space physical|linear");
            return;
        }
        SetSpaceIndex((space == "physical") ? 0 : 1);
    } else if ((cmd == "goto") && (n == 1)) {
        uint32_t addr;
        if (address(args[1], hex->Space(), &addr)) {
            addr_edit->SetText(strfmt("%08X", addr).c_str());
            hex->GoTo(addr);
        }
    } else if (((cmd == "db") || (cmd == "dbio")) && (n <= 2)) {
        uint32_t addr = hex->Selection();
        uint32_t len  = 0x80;
        if (n && !address(args[1], hex->Space(), &addr))
            return;
        if (((n == 2) && !parse_hex(args[2], &len)) || !len || (len > 0x1000) || (((uint64_t) addr + len) > 0x100000000ULL)) {
            usage("db [address [length]] (1..1000 hex bytes; no address wrap)");
            return;
        }
        std::vector<uint8_t> buf(len);
        std::vector<uint8_t> buf_cls(len);
        if (!acquire())
            return;
        if (cmd == "dbio") {
            if (!debugger_stopped()) {
                viewer_unlock();
                Print("Pause the processor before side-effecting memory access.");
                return;
            }
            for (uint32_t i = 0; i < len; i++) {
                uint32_t   physical = addr + i;
                const bool is_mapped = (hex->Space() == DEBUGGER_SPACE_PHYSICAL) || debugger_translate(physical, &physical);
                buf_cls[i] = is_mapped ? DEBUGGER_MEM_RAM : DEBUGGER_MEM_INACCESSIBLE;
                buf[i]     = is_mapped ? (uint8_t) mem_view_read(MEM_VIEW_CPU, physical, 1) : 0;
            }
        } else
            debugger_mem_read(hex->Space(), addr, len, buf.data(), buf_cls.data());
        viewer_unlock();
        for (uint32_t offset = 0; offset < len; offset += 16) {
            std::string row = strfmt("%08X  ", addr + offset);
            std::string ascii;
            for (uint32_t i = offset; i < (offset + 16); i++) {
                if (i >= len) {
                    row += "   ";
                    continue;
                }
                row += readable(buf_cls[i]) ? strfmt("%02X ", buf[i])
                     : (buf_cls[i] == DEBUGGER_MEM_UNMAPPED) ? std::string("-- ") : std::string("?? ");
                ascii += (readable(buf_cls[i]) && (buf[i] >= 32) && (buf[i] < 127)) ? (char) buf[i] : '.';
            }
            Print(row + " " + ascii);
        }
        addr_edit->SetText(strfmt("%08X", addr).c_str());
        hex->GoTo(addr);
        if (cmd == "dbio")
            RefreshMappings();
    } else if (((cmd == "write") || (cmd == "writeio")) && (n >= 2)) {
        uint32_t addr;
        if (!address(args[1], hex->Space(), &addr))
            return;
        std::vector<uint8_t> bytes;
        for (size_t i = 2; i < args.size(); i++) {
            uint32_t byte;
            if (!parse_hex(args[i], &byte) || (byte > 0xff)) {
                usage("write address byte [byte ...] (bytes 00..FF)");
                return;
            }
            bytes.push_back((uint8_t) byte);
        }
        if ((bytes.size() > 4096) || (((uint64_t) addr + bytes.size()) > 0x100000000ULL)) {
            Print("Write exceeds 4096 bytes or wraps the address space.");
            return;
        }
        if (!acquire())
            return;
        int        written = 0;
        const bool stopped = debugger_stopped();
        if (stopped) {
            for (size_t i = 0; i < bytes.size(); i++) {
                if (cmd == "writeio") {
                    uint32_t physical = addr + (uint32_t) i;
                    if ((hex->Space() == DEBUGGER_SPACE_PHYSICAL) || debugger_translate(physical, &physical)) {
                        mem_view_write(MEM_VIEW_CPU, physical, bytes[i], 1);
                        written++;
                    }
                } else
                    written += !!debugger_mem_write(hex->Space(), addr + (uint32_t) i, bytes[i]);
            }
        }
        viewer_unlock();
        if (!stopped)
            Print("Pause the processor before writing memory.");
        else {
            Print((cmd == "writeio")
                      ? strfmt("Issued %d of %d byte writes; devices may ignore writes. Untranslated addresses are "
                               "skipped.", written, (int) bytes.size())
                      : strfmt("Wrote %d of %d bytes; read-only or inaccessible bytes are skipped.", written,
                               (int) bytes.size()));
            RefreshMappings();
        }
    } else if ((cmd == "dump") && !n) {
        OnDump(hex->SelectionStart(), (uint32_t) std::min<uint64_t>(hex->SelectionLen(), UINT32_MAX));
    } else if ((cmd == "u") && (n == 1)) {
#ifdef USE_DISASM
        uint32_t addr;
        if (address(args[1], DEBUGGER_SPACE_LINEAR, &addr)) {
            SetFollow(false);
            disasm_addr = addr;
            disasm_edit->SetText(strfmt("%08X", addr).c_str());
            RefreshDisasm();
        }
#else
        Print("Disassembly is not available in this build.");
#endif
    } else if ((cmd == "follow") && (n == 1) && ((lower(args[1]) == "on") || (lower(args[1]) == "off"))) {
        SetFollow(lower(args[1]) == "on");
    } else if (((cmd == "bp") || (cmd == "bc")) && (n == 1)) {
        if ((cmd == "bc") && (lower(args[1]) == "all")) {
            debugger_breakpoints_clear();
        } else {
            uint32_t addr;
            if (!address(args[1], DEBUGGER_SPACE_LINEAR, &addr))
                return;
            const bool wanted = (cmd == "bp");
            if (((debugger_breakpoint_present(addr) != 0) != wanted) && (debugger_breakpoint_toggle(addr) < 0)) {
                Print("Too many breakpoints.");
                return;
            }
            Print(strfmt("Breakpoint %s at %08X.", wanted ? "set" : "cleared", addr));
        }
        RefreshDisasm();
    } else if ((cmd == "bl") && !n) {
        uint32_t  list[DEBUGGER_MAX_BREAKPOINTS];
        const int count = debugger_breakpoints(list, DEBUGGER_MAX_BREAKPOINTS);
        Print(strfmt("%d breakpoint(s), linear addresses:", count));
        for (int i = 0; i < count; i++)
            Print(strfmt("%08X", list[i]));
    } else if (((cmd == "peek") && ((n == 1) || (n == 2))) || ((cmd == "poke") && ((n == 2) || (n == 3)))) {
        const bool        write       = (cmd == "poke");
        const int         width_index = write ? 3 : 2;
        const std::string width       = (n == width_index) ? lower(args[width_index]) : std::string("b");
        uint32_t          port;
        uint32_t          value = 0;
        const uint32_t    max   = (width == "b") ? 0xff : (width == "w") ? 0xffff : UINT32_MAX;
        if (!parse_hex(args[1], &port) || (port > 0xffff) || ((width != "b") && (width != "w") && (width != "d"))
            || (write && (!parse_hex(args[2], &value) || (value > max)))) {
            usage(write ? "poke port value [b|w|d]" : "peek port [b|w|d]");
            return;
        }
        if (!acquire())
            return;
        if (!debugger_stopped()) {
            viewer_unlock();
            Print("Pause the processor before accessing I/O ports.");
            return;
        }
        if (write) {
            if (width == "b")
                outb((uint16_t) port, (uint8_t) value);
            else if (width == "w")
                outw((uint16_t) port, (uint16_t) value);
            else
                outl((uint16_t) port, value);
        } else {
            if (width == "b")
                value = inb((uint16_t) port);
            else if (width == "w")
                value = inw((uint16_t) port);
            else
                value = inl((uint16_t) port);
        }
        viewer_unlock();
        Print(strfmt("I/O %04X %s %08X (%s)", port, write ? "<-" : "->", value, width.c_str()));
        RefreshMappings();
    } else {
        Print("Unknown command or invalid arguments. Type help for syntax.");
    }
}

void
MemoryViewerWindow::OnDump(uint32_t start, uint32_t len)
{
    DumpFields fields;
    fields.start  = strfmt("%08X", start);
    fields.length = len ? strfmt("%X", len) : std::string("10000");
    fields.space  = hex->Space();
    fields.fill   = "FF";
    if (usr_path[0] != '\0') {
        BPath path(usr_path, strfmt("mem_%08X.bin", start).c_str());
        if (path.InitCheck() == B_OK)
            fields.file = path.Path();
    }

    uint32_t addr     = 0;
    uint32_t dump_len = 0;
    uint32_t fill     = 0;
    for (;;) {
        bool accepted = false;
        (new DumpWindow(this, &fields, &accepted))->Go();
        if (!accepted)
            return;

        std::string err;
        if (!parse_address(fields.start, fields.space, &addr, &err)) {
            alert("Dump memory", err);
            continue;
        }
        if (!parse_hex(fields.length, &dump_len) || !dump_len) {
            alert("Dump memory", "The length must be a hex number above 0.");
            continue;
        }
        if (!parse_hex(fields.fill, &fill) || (fill > 0xff)) {
            alert("Dump memory", "The fill byte must be a hex number from 00 to FF.");
            continue;
        }
        if (trim(fields.file).empty()) {
            alert("Dump memory", "Choose a file to write the dump to.");
            continue;
        }
        if (((uint64_t) addr + dump_len) > 0x100000000ULL)
            dump_len = (uint32_t) (0x100000000ULL - addr);
        break;
    }

    FILE *fp = fopen(fields.file.c_str(), "wb");
    if (fp == nullptr) {
        alert("Dump memory", "Could not open " + fields.file + " for writing.", B_STOP_ALERT);
        return;
    }

    constexpr uint32_t   chunk = 0x10000;
    std::vector<uint8_t> buf(chunk);
    std::vector<uint8_t> buf_cls(chunk);
    uint64_t             unreadable = 0;
    uint64_t             done       = 0;
    Progress             progress(this, "Dumping memory" B_UTF8_ELLIPSIS, (int) (((uint64_t) dump_len + chunk - 1) / chunk));

    while (done < dump_len) {
        const uint32_t n = (uint32_t) std::min<uint64_t>(chunk, dump_len - done);
        if (!viewer_lock()) {
            /* A reset is under way; it ends, or the dump can be cancelled. */
            progress.SetValue((int) (done / chunk));
            if (progress.WasCanceled() || is_quit)
                break;
            snooze(10000);
            continue;
        }
        debugger_mem_read(fields.space, addr + (uint32_t) done, n, buf.data(), buf_cls.data());
        viewer_unlock();

        for (uint32_t i = 0; i < n; i++) {
            if (!readable(buf_cls[i])) {
                buf[i] = (uint8_t) fill;
                unreadable++;
            }
        }
        if (fwrite(buf.data(), 1, n, fp) != n) {
            fclose(fp);
            alert("Dump memory", "Could not write to " + fields.file + ".", B_STOP_ALERT);
            return;
        }
        done += n;
        progress.SetValue((int) (done / chunk));
        if (progress.WasCanceled())
            break;
    }
    fclose(fp);

    Notice(strfmt("Dumped %llu bytes from %08X to %s (%llu bytes could not be read and were filled).",
                  (unsigned long long) done, addr, fields.file.c_str(), (unsigned long long) unreadable));
}

/* Build the bytes to look for from the search box: raw hex pairs, a line
   of text, or a hex value in the guest's little-endian byte order. */
std::vector<uint8_t>
MemoryViewerWindow::SearchPattern(std::string *err) const
{
    const int         type = search_type_menu->IndexOf(search_type_menu->FindMarked());
    const std::string text = trim(search_edit->Text());

    if (type == 1) {
        /* Latin-1, as QString::toLatin1(): a character outside it is '?'. */
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i < text.size();) {
            const uint8_t c = (uint8_t) text[i];
            uint32_t      cp;
            int           len;
            if (c < 0x80) {
                cp  = c;
                len = 1;
            } else if ((c & 0xe0) == 0xc0) {
                cp  = c & 0x1f;
                len = 2;
            } else if ((c & 0xf0) == 0xe0) {
                cp  = c & 0x0f;
                len = 3;
            } else {
                cp  = c & 0x07;
                len = 4;
            }
            for (int k = 1; k < len; k++)
                cp = (cp << 6) | (((i + k) < text.size()) ? ((uint8_t) text[i + k] & 0x3f) : 0);
            bytes.push_back((cp < 0x100) ? (uint8_t) cp : (uint8_t) '?');
            i += len;
        }
        return bytes;
    }

    if (type == 0) {
        std::string t;
        for (const char c : text) {
            if ((c != ' ') && (c != ','))
                t += c;
        }
        if (t.empty() || (t.size() & 1)) {
            *err = "Enter hex bytes in pairs, e.g. \"8B 44 24 08\".";
            return std::vector<uint8_t>();
        }
        for (const char c : t) {
            if (!is_hex_digit(c)) {
                *err = "\"" + text + "\" is not made of hex bytes.";
                return std::vector<uint8_t>();
            }
        }
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i < t.size(); i += 2)
            bytes.push_back((uint8_t) strtoul(t.substr(i, 2).c_str(), nullptr, 16));
        return bytes;
    }

    /* A byte, word or dword value, stored little-endian like the hex pane. */
    const int      width = (type == 2) ? 1 : (type == 3) ? 2 : 4;
    const uint32_t max   = (width == 1) ? 0xff : (width == 2) ? 0xffff : UINT32_MAX;
    uint32_t       value;
    if (!parse_hex(text, &value)) {
        *err = strfmt("Enter a hex value from 0 to %X.", max);
        return std::vector<uint8_t>();
    }
    if (value > max) {
        *err = strfmt("The value is too large for %d bytes.", width);
        return std::vector<uint8_t>();
    }
    std::vector<uint8_t> bytes((size_t) width);
    for (int i = 0; i < width; i++)
        bytes[i] = (uint8_t) ((value >> (i * 8)) & 0xff);
    return bytes;
}

void
MemoryViewerWindow::OnSearch(bool forward)
{
    std::string                err;
    const std::vector<uint8_t> pattern = SearchPattern(&err);

    if (pattern.empty()) {
        Notice(err.empty() ? std::string("Enter something to search for.") : err);
        return;
    }
    if (pattern.size() > 4096) {
        Notice(strfmt("The search is limited to %d bytes.", 4096));
        return;
    }

    /* An optional range, in whatever form the Go field takes. The ends
       left empty mean the whole space; . means the selection. */
    uint32_t from = 0;
    uint32_t to   = 0xffffffffU;
    if (range_shown) {
        const std::string from_text = trim(range_from_edit->Text());
        const std::string to_text   = trim(range_to_edit->Text());
        if (!from_text.empty()) {
            if (from_text == ".")
                from = hex->SelectionStart();
            else if (!parse_address(from_text, hex->Space(), &from, &err)) {
                Notice(err);
                return;
            }
        }
        if (!to_text.empty()) {
            if (to_text == ".")
                to = hex->SelectionEnd();
            else if (!parse_address(to_text, hex->Space(), &to, &err)) {
                Notice(err);
                return;
            }
        }
        if (from > to) {
            Notice("The range ends before it starts.");
            return;
        }
    }

    hex->Find(pattern, forward, from, to);
}

/* ------------------------------------------------------------------ entry */

static BMessenger viewer_messenger;

void
haiku_memory_viewer_open(BWindow *main_window)
{
    if (viewer_messenger.IsValid() && (viewer_messenger.SendMessage(MSG_MV_ACTIVATE) == B_OK))
        return;

    BRect frame(0, 0, 1399, 819);
    if (main_window != nullptr) {
        BRect main = main_window->Frame();
        frame.OffsetTo(main.left + 40, main.top + 40);
    } else
        frame.OffsetTo(80, 80);

    MemoryViewerWindow *window = new MemoryViewerWindow(frame);
    viewer_messenger           = BMessenger(window);
    window->Show();
}
