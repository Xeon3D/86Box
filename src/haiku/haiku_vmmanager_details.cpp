/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the VM manager's details panel
 *          (qt_vmmanager_details.cpp and qt_vmmanager_detailsection.cpp):
 *          the machine's name, its hardware in collapsible sections, its
 *          screenshots, notes, buttons and process status.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Bitmap.h>
#include <ControlLook.h>
#include <Font.h>
#include <GridLayout.h>
#include <GridView.h>
#include <GroupLayout.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <SpaceLayoutItem.h>
#include <StringView.h>
#include <TextView.h>
#include <TranslationUtils.h>
#include <Window.h>

#include <cmath>
#include <map>

#include "haiku_icons.hpp"
#include "haiku_vmmanager_system.hpp"
#include "haiku_vmmanager_ui.hpp"

extern "C" {
#include <86box/86box.h>
}

/* ---------------------------------------------------------- tool button */

static const float TOOL_ICON_SIZE = 16;

VmmToolButton::VmmToolButton(const char *name, const char *icon, const char *label, const char *tip,
                             BMessage *message, bool show_label)
    : BControl(name, nullptr, message, B_WILL_DRAW | B_NAVIGABLE)
    , bitmap((icon != nullptr) ? haiku_icon(icon) : nullptr)
    , text(label != nullptr ? label : "")
    , show_label(show_label)
    , inside(false)
    , pressed(false)
{
    if (tip != nullptr)
        SetToolTip(tip);
}

void
VmmToolButton::SetIcon(const char *icon)
{
    bitmap = (icon != nullptr) ? haiku_icon(icon) : nullptr;
    Invalidate();
}

void
VmmToolButton::SetLabelAndTip(const char *label, const char *tip)
{
    text = (label != nullptr) ? label : "";
    SetToolTip(tip);
    if (show_label || (bitmap == nullptr))
        InvalidateLayout();
    Invalidate();
}

void
VmmToolButton::AttachedToWindow()
{
    BControl::AttachedToWindow();
    AdoptParentColors();
}

BSize
VmmToolButton::MinSize()
{
    font_height fh;
    GetFontHeight(&fh);
    const float text_height = ceilf(fh.ascent + fh.descent);
    const float text_width  = ceilf(StringWidth(text.c_str()));

    if (bitmap == nullptr)
        return BSize(text_width + 12, text_height + 8);
    if (!show_label)
        return BSize(TOOL_ICON_SIZE + 8, TOOL_ICON_SIZE + 8);
    return BSize(std::max(TOOL_ICON_SIZE, text_width) + 12, TOOL_ICON_SIZE + text_height + 12);
}

BSize
VmmToolButton::MaxSize()
{
    return MinSize();
}

BSize
VmmToolButton::PreferredSize()
{
    return MinSize();
}

void
VmmToolButton::Draw(BRect)
{
    BRect     rect  = Bounds();
    rgb_color base  = ViewColor();
    uint32    flags = 0;

    if (pressed)
        flags |= BControlLook::B_ACTIVATED;
    if (!IsEnabled())
        flags |= BControlLook::B_DISABLED;
    if (IsFocus() && Window()->IsActive())
        flags |= BControlLook::B_FOCUSED;

    if (IsEnabled() && (inside || pressed)) {
        be_control_look->DrawButtonFrame(this, rect, rect, base, base, flags);
        be_control_look->DrawButtonBackground(this, rect, rect, base, flags);
    }

    font_height fh;
    GetFontHeight(&fh);
    const rgb_color text_color = IsEnabled() ? ui_color(B_PANEL_TEXT_COLOR)
                                             : tint_color(ui_color(B_PANEL_TEXT_COLOR), B_DISABLED_LABEL_TINT);

    if (bitmap != nullptr) {
        const float x = floorf((Bounds().Width() - TOOL_ICON_SIZE) / 2) + 1;
        const float y = show_label ? 4 : floorf((Bounds().Height() - TOOL_ICON_SIZE) / 2) + 1;
        SetDrawingMode(B_OP_ALPHA);
        if (IsEnabled())
            SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        else {
            SetHighColor(0, 0, 0, 90);
            SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
        }
        DrawBitmap(bitmap, BPoint(x, y));
        SetDrawingMode(B_OP_COPY);
        if (!show_label)
            return;
        SetHighColor(text_color);
        SetLowColor(base);
        const float width = StringWidth(text.c_str());
        DrawString(text.c_str(), BPoint(floorf((Bounds().Width() - width) / 2) + 1, 4 + TOOL_ICON_SIZE + 4 + ceilf(fh.ascent)));
        return;
    }

    SetHighColor(text_color);
    SetLowColor(base);
    const float width = StringWidth(text.c_str());
    DrawString(text.c_str(), BPoint(floorf((Bounds().Width() - width) / 2) + 1,
                                    floorf((Bounds().Height() - (fh.ascent + fh.descent)) / 2 + fh.ascent) + 1));
}

void
VmmToolButton::MouseDown(BPoint)
{
    if (!IsEnabled())
        return;
    pressed = true;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    Invalidate();
}

void
VmmToolButton::MouseUp(BPoint where)
{
    if (!pressed)
        return;
    pressed = false;
    Invalidate();
    if (IsEnabled() && Bounds().Contains(where))
        Invoke();
}

void
VmmToolButton::MouseMoved(BPoint where, uint32 transit, const BMessage *)
{
    const bool now = (transit != B_EXITED_VIEW) && (transit != B_OUTSIDE_VIEW) && Bounds().Contains(where);
    if (now != inside) {
        inside = now;
        Invalidate();
    }
}

/* ------------------------------------------------------------ icon cache */

BBitmap *
vmm_machine_icon(const std::string &icon)
{
    static std::map<std::string, BBitmap *> cache;

    /* Qt's built-in system icons (":/systemicons/...") are not available. */
    if (icon.empty() || (icon[0] != '/'))
        return nullptr;

    const auto it = cache.find(icon);
    if (it != cache.end())
        return it->second;
    BBitmap *bitmap = BTranslationUtils::GetBitmap(icon.c_str());
    if ((bitmap != nullptr) && !bitmap->IsValid()) {
        delete bitmap;
        bitmap = nullptr;
    }
    cache[icon] = bitmap;
    return bitmap;
}

/* ------------------------------------------------------- detail section */

/* The clickable header of a section (CollapseButton). */
class VmmSectionHeader : public BControl {
public:
    VmmSectionHeader(const char *title, BView *content)
        : BControl("header", title, nullptr, B_WILL_DRAW)
        , content(content)
        , expanded(true)
    {
    }

    void AttachedToWindow() override
    {
        BControl::AttachedToWindow();
        AdoptParentColors();
    }

    BSize MinSize() override
    {
        BFont font;
        GetFont(&font);
        font.SetFace(B_BOLD_FACE);
        font_height fh;
        font.GetHeight(&fh);
        return BSize(font.StringWidth(Label()) + 30, ceilf(fh.ascent + fh.descent) + 6);
    }

    BSize MaxSize() override { return BSize(B_SIZE_UNLIMITED, MinSize().height); }
    BSize PreferredSize() override { return MinSize(); }

    void Draw(BRect update) override
    {
        BRect rect = Bounds();
        SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_1_TINT));
        FillRect(rect);

        BRect arrow(4, 0, 14, rect.Height());
        be_control_look->DrawArrowShape(this, arrow, update, ui_color(B_PANEL_BACKGROUND_COLOR),
                                        expanded ? BControlLook::B_DOWN_ARROW : BControlLook::B_RIGHT_ARROW);

        BFont font;
        GetFont(&font);
        font.SetFace(B_BOLD_FACE);
        SetFont(&font);
        font_height fh;
        font.GetHeight(&fh);
        SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
        SetDrawingMode(B_OP_OVER);
        DrawString(Label(), BPoint(20, floorf((rect.Height() - (fh.ascent + fh.descent)) / 2 + fh.ascent) + 1));
        SetDrawingMode(B_OP_COPY);
    }

    void MouseDown(BPoint) override
    {
        expanded = !expanded;
        if (expanded)
            content->Show();
        else
            content->Hide();
        Invalidate();
    }

    BView *content;
    bool   expanded;
};

/* One section of the details (VMManagerDetailSection): "Name: value" rows,
   one row per line of each value; hidden while empty. */
class VmmDetailSection : public BGroupView {
public:
    explicit VmmDetailSection(const char *title)
        : BGroupView(B_VERTICAL, 0)
        , holder(new BGroupView(B_VERTICAL, 0))
        , grid(nullptr)
        , shown(true)
    {
        holder->GroupLayout()->SetInsets(10, 2, 0, 10);
        header = new VmmSectionHeader(title, holder);
        GroupLayout()->AddView(header);
        GroupLayout()->AddView(holder);
        SetShown(false);
    }

    void Clear() { rows.clear(); }

    void Add(const char *name, const std::string &value) { rows.emplace_back(name, value); }

    /* VMManagerDetailSection::setSections() */
    void Commit()
    {
        if (grid != nullptr) {
            holder->GroupLayout()->RemoveView(grid);
            delete grid;
        }
        grid = new BGridView(B_USE_SMALL_SPACING, 0);

        int  row   = 0;
        bool empty = true;
        for (const auto &entry : rows) {
            bool        first = true;
            std::string value = entry.second;
            size_t      start = 0;
            for (;;) {
                const size_t      pos  = value.find(VMM_SECTION_SEPARATOR, start);
                const std::string line = value.substr(start, (pos == std::string::npos) ? std::string::npos : pos - start);
                if (!line.empty()) {
                    if (first) {
                        BStringView *key = new BStringView("key", (entry.first + ":").c_str());
                        key->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT, B_ALIGN_TOP));
                        grid->GridLayout()->AddView(key, 0, row);
                        first = false;
                    }
                    BStringView *label = new BStringView("value", line.c_str());
                    label->SetTruncation(B_TRUNCATE_END);
                    label->SetExplicitMinSize(BSize(60, B_SIZE_UNSET));
                    label->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
                    label->SetToolTip(line.c_str());
                    grid->GridLayout()->AddView(label, 1, row);
                    empty = false;
                    row++;
                }
                if (pos == std::string::npos)
                    break;
                start = pos + 1;
            }
        }
        grid->GridLayout()->SetColumnWeight(0, 0);
        grid->GridLayout()->SetColumnWeight(1, 1);
        holder->GroupLayout()->AddView(grid);
        SetShown(!empty);
    }

private:
    void SetShown(bool show)
    {
        if (show == shown)
            return;
        shown = show;
        if (show)
            Show();
        else
            Hide();
    }

    typedef std::pair<std::string, std::string> row_t;

    VmmSectionHeader  *header;
    BGroupView        *holder;
    BGridView         *grid;
    std::vector<row_t> rows;
    bool               shown;
};

/* The scrolled column of sections; sets its scroll bar from its layout. */
class VmmSectionsColumn : public BGroupView {
public:
    VmmSectionsColumn()
        : BGroupView("sections", B_VERTICAL, B_USE_SMALL_SPACING)
    {
    }

    BSize MinSize() override { return BSize(BGroupView::MinSize().width, 80); }

    void DoLayout() override
    {
        BGroupView::DoLayout();
        BScrollBar *bar = ScrollBar(B_VERTICAL);
        if (bar == nullptr)
            return;
        float       height    = std::max(GroupLayout()->PreferredSize().height, GroupLayout()->MinSize().height);
        const float view      = Bounds().Height();
        if (height < view)
            height = view;
        bar->SetRange(0, std::max(0.0f, height - view));
        bar->SetProportion((height > 0) ? (view / height) : 1.0f);
        bar->SetSteps(12, std::max(12.0f, view - 12));
    }
};

/* --------------------------------------------------------- screenshots */

class VmmScreenshotView : public BView {
public:
    VmmScreenshotView()
        : BView("screenshot", B_WILL_DRAW)
        , bitmap(nullptr)
    {
        SetExplicitMinSize(BSize(239, 159));
        SetExplicitMaxSize(BSize(239, 159));
    }

    ~VmmScreenshotView() override { delete bitmap; }

    void AttachedToWindow() override
    {
        BView::AttachedToWindow();
        AdoptParentColors();
    }

    void SetImage(const char *path)
    {
        delete bitmap;
        bitmap = (path != nullptr) ? BTranslationUtils::GetBitmap(path) : nullptr;
        if ((bitmap != nullptr) && !bitmap->IsValid()) {
            delete bitmap;
            bitmap = nullptr;
        }
        Invalidate();
    }

    void Draw(BRect) override
    {
        BRect rect = Bounds();
        if (bitmap != nullptr) {
            /* Scaled to fit, keeping the aspect ratio. */
            const BRect source = bitmap->Bounds();
            const float scale  = std::min((rect.Width() + 1) / (source.Width() + 1), (rect.Height() + 1) / (source.Height() + 1));
            const float width  = floorf((source.Width() + 1) * scale);
            const float height = floorf((source.Height() + 1) * scale);
            BRect       target(0, 0, width - 1, height - 1);
            target.OffsetTo(floorf((rect.Width() + 1 - width) / 2), floorf((rect.Height() + 1 - height) / 2));
            DrawBitmap(bitmap, source, target, B_FILTER_BITMAP_BILINEAR);
            return;
        }

        SetHighColor(tint_color(ViewColor(), B_DARKEN_2_TINT));
        StrokeRect(rect);
        SetHighColor(tint_color(ui_color(B_PANEL_TEXT_COLOR), B_DISABLED_LABEL_TINT));
        SetLowColor(ViewColor());
        font_height fh;
        GetFontHeight(&fh);
        const char *text  = "No screenshot";
        const float width = StringWidth(text);
        DrawString(text, BPoint(floorf((rect.Width() - width) / 2), floorf((rect.Height() - (fh.ascent + fh.descent)) / 2 + fh.ascent)));
    }

private:
    BBitmap *bitmap;
};

/* The notes; saved when they lose the focus, as in Qt. */
class VmmNotesView : public BTextView {
public:
    VmmNotesView()
        : BTextView("notes")
    {
    }

    void MakeFocus(bool focus) override
    {
        BTextView::MakeFocus(focus);
        if (!focus && (Window() != nullptr))
            Window()->PostMessage(MSG_VMM_NOTES);
    }
};

/* --------------------------------------------------------------- details */

VmmDetailsView::VmmDetailsView()
    : BView("details", 0)
    , system(nullptr)
    , screenshot_index(0)
{
    SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

    system_label = new BStringView("system", "No Machines Found!");
    BFont big(be_bold_font);
    big.SetSize(be_bold_font->Size() * 1.4f);
    system_label->SetFont(&big);
    system_label->SetTruncation(B_TRUNCATE_END);
    system_label->SetExplicitMinSize(BSize(100, B_SIZE_UNSET));
    system_label->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    VmmSectionsColumn *column = new VmmSectionsColumn();
    const char *titles[]      = { "System", "Display", "Storage", "Audio", "Network", "Input devices", "Ports", "Other devices" };
    for (const char *title : titles) {
        VmmDetailSection *section = new VmmDetailSection(title);
        sections.push_back(section);
        column->GroupLayout()->AddView(section);
    }
    column->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
    BScrollView *column_scroll = new BScrollView("sections_scroll", column, 0, false, true, B_NO_BORDER);

    screenshot      = new VmmScreenshotView();
    previous_button = new VmmToolButton("previous", nullptr, "<", "Previous screenshot", new BMessage(MSG_VMM_SHOT_PREVIOUS), false);
    next_button     = new VmmToolButton("next", nullptr, ">", "Next screenshot", new BMessage(MSG_VMM_SHOT_NEXT), false);

    notes = new VmmNotesView();
    notes->SetWordWrap(true);
    notes->SetStylable(false);
    BScrollView *notes_scroll = new BScrollView("notes_scroll", notes, 0, false, true);
    notes_scroll->SetExplicitPreferredSize(BSize(240, 120));
    notes_scroll->SetExplicitMaxSize(BSize(240, B_SIZE_UNLIMITED));

    start_pause_button = new VmmToolButton("start_pause", "run", "Start", "Start", new BMessage(MSG_VMM_START_PAUSE), false);
    reset_button       = new VmmToolButton("reset", "hard_reset", "Hard reset", "Hard reset", new BMessage(MSG_VMM_RESET), false);
    stop_button        = new VmmToolButton("stop", "acpi_shutdown", "Force shutdown", "Force shutdown", new BMessage(MSG_VMM_FORCE_SHUTDOWN), false);
    cad_button         = new VmmToolButton("cad", "send_cad", "Ctrl+Alt+Del", "Ctrl+Alt+Del", new BMessage(MSG_VMM_CAD), false);
    configure_button   = new VmmToolButton("configure", "settings", "Settings…", "Settings…", new BMessage(MSG_VMM_SETTINGS), false);

    status_label = new BStringView("status", "");
    status_label->SetAlignment(B_ALIGN_CENTER);
    status_label->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL)
            .SetInsets(10)
            .Add(system_label)
        .End()
        .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
            .SetInsets(0, 0, B_USE_SMALL_SPACING, B_USE_SMALL_SPACING)
            .Add(column_scroll, 3)
            .AddGroup(B_VERTICAL, B_USE_SMALL_SPACING, 0)
                .Add(screenshot)
                .AddGroup(B_HORIZONTAL, 0)
                    .AddGlue()
                    .Add(previous_button)
                    .Add(next_button)
                    .AddGlue()
                .End()
                .Add(notes_scroll)
                .AddGroup(B_HORIZONTAL, 0)
                    .AddGlue()
                    .Add(start_pause_button)
                    .Add(reset_button)
                    .Add(stop_button)
                    .Add(cad_button)
                    .Add(configure_button)
                    .AddGlue()
                .End()
                .Add(status_label)
            .End()
        .End();

    Reset();
}

void
VmmDetailsView::Reset()
{
    for (VmmDetailSection *section : sections) {
        section->Clear();
        section->Commit();
    }

    screenshots.clear();
    previous_button->SetEnabled(false);
    next_button->SetEnabled(false);
    screenshot->SetImage(nullptr);

    start_pause_button->SetEnabled(false);
    reset_button->SetEnabled(false);
    stop_button->SetEnabled(false);
    configure_button->SetEnabled(false);
    cad_button->SetEnabled(false);

    system_label->SetText("No Machines Found!");
    status_label->SetText("");
    notes->SetText("");
    notes->MakeEditable(false);
    system = nullptr;
}

void
VmmDetailsView::SetSystem(VmmSystem *new_system)
{
    if (new_system == nullptr) {
        Reset();
        return;
    }
    system = new_system;

    UpdateConfig();
    system_label->SetText(system->display_name.c_str());
    notes->SetText(system->notes.c_str());
    notes->MakeEditable(true);
    UpdateProcessStatus();
}

/* VMManagerDetails::updateConfig() and updateScreenshots() */
void
VmmDetailsView::UpdateConfig()
{
    if (system == nullptr)
        return;

    const struct {
        int         section;
        const char *name;
        VmmDisplay  value;
    } rows[] = {
        { 0, "Machine", DISPLAY_MACHINE },
        { 0, "CPU", DISPLAY_CPU },
        { 0, "Memory", DISPLAY_MEMORY },
        { 1, "Video", DISPLAY_VIDEO },
        { 1, "Voodoo", DISPLAY_VOODOO },
        { 2, "Disks", DISPLAY_DISKS },
        { 2, "Floppy", DISPLAY_FLOPPY },
        { 2, "CD-ROM", DISPLAY_CD },
        { 2, "Removable disks", DISPLAY_RDISK },
        { 2, "MO", DISPLAY_MO },
        { 2, "Tape drives", DISPLAY_TAPE },
        { 2, "SCSI", DISPLAY_SCSI_CONTROLLER },
        { 2, "Controllers", DISPLAY_STORAGE_CONTROLLER },
        { 3, "Audio", DISPLAY_AUDIO },
        { 3, "MIDI Out", DISPLAY_MIDI_OUT },
        { 4, "NIC", DISPLAY_NIC },
        { 5, "Keyboard", DISPLAY_KEYBOARD },
        { 5, "Mouse", DISPLAY_MOUSE },
        { 5, "Tablet", DISPLAY_TABLET },
        { 5, "Joystick", DISPLAY_JOYSTICK },
        { 6, "Serial ports", DISPLAY_SERIAL },
        { 6, "Parallel ports", DISPLAY_PARALLEL },
        { 7, "ISA RTC", DISPLAY_ISA_RTC },
        { 7, "ISA RAM", DISPLAY_ISA_MEM },
        { 7, "ISA ROM", DISPLAY_ISA_ROM },
    };

    for (VmmDetailSection *section : sections)
        section->Clear();
    for (const auto &row : rows)
        sections[row.section]->Add(row.name, system->Display(row.value));
    for (VmmDetailSection *section : sections)
        section->Commit();

    system_label->SetText(system->display_name.c_str());
    UpdateScreenshots();
}

void
VmmDetailsView::UpdateScreenshots()
{
    screenshots = system->Screenshots();
    previous_button->SetEnabled(screenshots.size() > 1);
    next_button->SetEnabled(screenshots.size() > 1);
    screenshot_index = screenshots.empty() ? 0 : screenshots.size() - 1;
    ShowScreenshot();
}

void
VmmDetailsView::ShowScreenshot()
{
    screenshot->SetImage(screenshots.empty() ? nullptr : screenshots[screenshot_index].c_str());
}

void
VmmDetailsView::NextScreenshot()
{
    if (screenshots.empty())
        return;
    screenshot_index = (screenshot_index + 1) % screenshots.size();
    ShowScreenshot();
}

void
VmmDetailsView::PreviousScreenshot()
{
    if (screenshots.empty())
        return;
    screenshot_index = (screenshot_index == 0) ? screenshots.size() - 1 : screenshot_index - 1;
    ShowScreenshot();
}

/* VMManagerDetails::updateProcessStatus() */
void
VmmDetailsView::UpdateProcessStatus()
{
    if (system == nullptr)
        return;

    const bool  running = system->IsRunning();
    std::string text    = running ? "Running: PID " + std::to_string(system->pid) : std::string("Not running");
    if (system->window_obscured)
        text += " (Waiting)";
    status_label->SetText(text.c_str());

    reset_button->SetEnabled(running && !system->window_obscured);
    stop_button->SetEnabled(running && !system->window_obscured);
    cad_button->SetEnabled(running && !system->window_obscured);
    if (running && (system->status == VmmStatus::Running)) {
        start_pause_button->SetIcon("pause");
        start_pause_button->SetLabelAndTip("Pause", "Pause");
    } else {
        start_pause_button->SetIcon("run");
        start_pause_button->SetLabelAndTip(running ? "Continue" : "Start", running ? "Continue" : "Start");
    }
    start_pause_button->SetEnabled(!system->window_obscured);
    configure_button->SetEnabled(!system->window_obscured);
}

void
VmmDetailsView::SaveNotes()
{
    if ((system == nullptr) || !notes->IsEditable())
        return;
    const std::string text = notes->Text();
    if (text != system->notes)
        system->SetNotes(text);
}
