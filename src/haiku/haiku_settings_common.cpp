/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: what the Settings pages share.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Bitmap.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <String.h>
#include <TabView.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "haiku_settings_common.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/device.h>
}

/* ---------------------------------------------------------------- Choice */

Choice::Choice(const char *name, const char *label, uint32 what)
    : what(what)
{
    menu  = new BPopUpMenu("", true, true);
    field = new BMenuField(name, label, menu);
}

void
Choice::Clear()
{
    while (BMenuItem *item = menu->RemoveItem((int32) 0))
        delete item;
}

void
Choice::Add(const char *text, int32 value)
{
    BMessage *msg = new BMessage(what);
    msg->AddInt32("value", value);
    menu->AddItem(new BMenuItem(text, msg));
}

void
Choice::AddSeparator()
{
    menu->AddSeparatorItem();
}

void
Choice::Select(int32 value)
{
    BMenuItem *pick = nullptr;
    for (int32 i = 0; i < menu->CountItems(); i++) {
        BMenuItem *item = menu->ItemAt(i);
        if (item->Message() == nullptr)
            continue;
        if (pick == nullptr)
            pick = item;
        if (item->Message()->GetInt32("value", -1) == value) {
            pick = item;
            break;
        }
    }
    if (pick != nullptr)
        pick->SetMarked(true);
    field->SetEnabled(Count() > 1);
}

int32
Choice::Value(int32 fallback) const
{
    BMenuItem *item = menu->FindMarked();
    return ((item != nullptr) && (item->Message() != nullptr)) ? item->Message()->GetInt32("value", fallback) : fallback;
}

bool
Choice::Has(int32 value) const
{
    for (int32 i = 0; i < menu->CountItems(); i++) {
        const BMessage *msg = menu->ItemAt(i)->Message();
        if ((msg != nullptr) && (msg->GetInt32("value", -1) == value))
            return true;
    }
    return false;
}

int32
Choice::Count() const
{
    int32 n = 0;
    for (int32 i = 0; i < menu->CountItems(); i++) {
        if (menu->ItemAt(i)->Message() != nullptr)
            n++;
    }
    return n;
}

void
Choice::SetEnabled(bool enabled)
{
    field->SetEnabled(enabled);
}

/* ------------------------------------------------------- device names */

std::string
settings_device_name(const device_t *dev, const char *internal_name, int bus)
{
    if ((internal_name != nullptr) && !strcmp(internal_name, "none"))
        return "None";
    if ((internal_name != nullptr) && !strcmp(internal_name, "internal"))
        return "Internal device";
    if (dev == nullptr)
        return "";

    char temp[512];
    if (bus == 1) {
        device_get_name(dev, -1, temp);
        const char *bus_name = device_get_bus_name(dev);
        if (bus_name != nullptr)
            return std::string("[") + bus_name + "] " + temp;
        return temp;
    }

    device_get_name(dev, bus, temp);
    std::string name = temp;
    for (const char *suffix : { " (On-Board)", " On-Board" }) {
        const size_t at = name.find(suffix);
        if (at != std::string::npos)
            name.erase(at, strlen(suffix));
    }
    if (const char *m = device_get_machine(dev)) {
        const std::string tail = std::string(" (") + m + ")";
        const size_t      at   = name.find(tail);
        if (at != std::string::npos)
            name.erase(at, tail.size());
    }
    return name;
}

/* ---------------------------------------------------------- SettingsPage */

static int selected_machine = 0;

SettingsPage::SettingsPage(const char *name)
    : BView(name, 0)
{
    SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
}

int
SettingsPage::CurrentMachine()
{
    return selected_machine;
}

/* Set by the Machine page (haiku_settings_machine.cpp). */
void
settings_set_current_machine(int m)
{
    selected_machine = m;
}

/* ------------------------------------------------ lists with columns */

static const float kIcon = 16.0f;

/* Draw a 16x16 icon with its alpha channel. */
static void
draw_icon(BView *view, BBitmap *icon, float x, BRect row)
{
    if (icon == nullptr)
        return;
    const drawing_mode mode = view->DrawingMode();
    view->SetDrawingMode(B_OP_ALPHA);
    view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
    view->DrawBitmapAsync(icon, BPoint(x, floorf(row.top + ((row.Height() - kIcon + 1) / 2))));
    view->SetDrawingMode(mode);
}

/* The left edge of each column, and the right edge of the last. */
static std::vector<float>
column_edges(const std::vector<float> &fractions, float left, float width)
{
    std::vector<float> edges;
    float              x = left;
    edges.push_back(x);
    for (float f : fractions) {
        x += floorf(f * width);
        edges.push_back(x);
    }
    edges.back() = left + width;
    return edges;
}

static float
row_height(const BFont *font)
{
    font_height fh;
    font->GetHeight(&fh);
    return std::max(kIcon + 4, ceilf(fh.ascent + fh.descent + fh.leading) + 6);
}

/* The texts of one row (or the header), each cut to its column. */
static void
draw_texts(BView *view, BRect row, const std::vector<std::string> &texts, const std::vector<float> &edges, BBitmap *icon,
           bool with_icon_room)
{
    font_height fh;
    view->GetFontHeight(&fh);
    const float baseline = floorf(row.top + ((row.Height() + fh.ascent - fh.descent) / 2));

    for (size_t i = 0; (i < texts.size()) && (i + 1 < edges.size()); i++) {
        float x = edges[i] + 4;
        if ((i == 0) && with_icon_room) {
            draw_icon(view, icon, x, row);
            x += kIcon + 4;
        }
        BString text(texts[i].c_str());
        view->TruncateString(&text, B_TRUNCATE_END, edges[i + 1] - x - 4);
        view->DrawString(text.String(), BPoint(x, baseline));
    }
}

SettingsColumnItem::SettingsColumnItem(const std::vector<float> &f)
    : fractions(f)
    , texts(f.size())
    , icon(nullptr)
{
}

void
SettingsColumnItem::SetText(size_t column, const std::string &text)
{
    if (column < texts.size())
        texts[column] = text;
}

void
SettingsColumnItem::DrawItem(BView *owner, BRect frame, bool)
{
    owner->SetLowColor(ui_color(IsSelected() ? B_LIST_SELECTED_BACKGROUND_COLOR : B_LIST_BACKGROUND_COLOR));
    owner->FillRect(frame, B_SOLID_LOW);
    owner->SetHighColor(ui_color(IsSelected() ? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR));
    draw_texts(owner, frame, texts, column_edges(fractions, frame.left, frame.Width()), icon, true);
}

void
SettingsColumnItem::Update(BView *owner, const BFont *font)
{
    BListItem::Update(owner, font);
    SetHeight(row_height(font));
}

namespace {

/* The titles above a column list, lined up with its columns. */
class ColumnTitles : public BView {
public:
    ColumnTitles(const std::vector<const char *> &t, const std::vector<float> &f)
        : BView("titles", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE)
        , fractions(f)
    {
        for (const char *title : t)
            titles.push_back(title);
        SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
        font_height fh;
        be_plain_font->GetHeight(&fh);
        const float height = ceilf(fh.ascent + fh.descent + fh.leading) + 6;
        SetExplicitMinSize(BSize(B_SIZE_UNSET, height));
        SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, height));
    }

    void Draw(BRect) override
    {
        const BRect bounds = Bounds();
        SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
        SetHighUIColor(B_PANEL_TEXT_COLOR);
        /* The list sits in a scroll view: a border, and the scroll bar. The
           first title starts where the rows' icons do, as in Qt. */
        const std::vector<float> edges = column_edges(fractions, 1, bounds.Width() - B_V_SCROLL_BAR_WIDTH - 2);
        draw_texts(this, bounds, titles, edges, nullptr, false);
        SetHighUIColor(B_PANEL_BACKGROUND_COLOR, B_DARKEN_2_TINT);
        for (size_t i = 1; i + 1 < edges.size(); i++)
            StrokeLine(BPoint(edges[i], bounds.top + 3), BPoint(edges[i], bounds.bottom - 3));
        StrokeLine(BPoint(bounds.left, bounds.bottom), BPoint(bounds.right, bounds.bottom));
    }

private:
    std::vector<std::string> titles;
    std::vector<float>       fractions;
};

/* A tab drawing an icon in the room left by spaces before its label. */
class IconTab : public BTab {
public:
    IconTab(BView *view, BBitmap *bitmap)
        : BTab(view)
        , icon(bitmap)
    {
    }

    void DrawLabel(BView *owner, BRect frame) override
    {
        BTab::DrawLabel(owner, frame);
        if (Label() == nullptr)
            return;
        /* BTab centres the label: the icon goes where the spaces are. */
        const float width = owner->StringWidth(Label());
        draw_icon(owner, icon, floorf(frame.left + ((frame.Width() - width) / 2)), frame);
    }

private:
    BBitmap *icon;
};

} /* namespace */

BView *
settings_column_list(const char *name, BListView *list, const std::vector<const char *> &titles,
                     const std::vector<float> &fractions)
{
    BView *holder = new BView(name, 0);
    BLayoutBuilder::Group<>(holder, B_VERTICAL, 0)
        .Add(new ColumnTitles(titles, fractions))
        .Add(new BScrollView((std::string(name) + "_scroll").c_str(), list, 0, false, true), 1);
    return holder;
}

SettingsIconItem::SettingsIconItem(const char *t, BBitmap *bitmap)
    : text(t)
    , icon(bitmap)
{
}

void
SettingsIconItem::DrawItem(BView *owner, BRect frame, bool)
{
    owner->SetLowColor(ui_color(IsSelected() ? B_LIST_SELECTED_BACKGROUND_COLOR : B_LIST_BACKGROUND_COLOR));
    owner->FillRect(frame, B_SOLID_LOW);
    owner->SetHighColor(ui_color(IsSelected() ? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR));
    draw_texts(owner, frame, { text }, { frame.left, frame.right }, icon, true);
}

void
SettingsIconItem::Update(BView *owner, const BFont *font)
{
    BListItem::Update(owner, font);
    SetHeight(row_height(font));
}

BTab *
settings_icon_tab(BView *view, BBitmap *icon)
{
    /* Spaces before the name make the tab wide enough for the icon. */
    std::string label;
    while (be_plain_font->StringWidth(label.c_str()) < kIcon + 4)
        label += ' ';
    label += view->Name();

    IconTab *tab = new IconTab(view, icon);
    tab->SetLabel(label.c_str());
    return tab;
}
