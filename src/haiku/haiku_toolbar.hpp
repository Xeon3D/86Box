/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the tool bar under the menus, like the Qt
 *          main window's: flat icon buttons, and the speed / mouse hint on
 *          the right (Qt's toolbar_label).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_TOOLBAR_HPP
#define HAIKU_TOOLBAR_HPP

#include <View.h>

#include <string>
#include <vector>

class IconButton;

class ToolBarView : public BView {
public:
    explicit ToolBarView(BRect frame);

    void AttachedToWindow() override;
    void Draw(BRect update) override;
    void FrameResized(float width, float height) override;

    /* Buttons send `what` to the window. */
    void AddButton(const char *icon, const char *tip, uint32 what);
    void AddSeparator();

    void SetButtonIcon(uint32 what, const char *icon, const char *tip);
    void SetButtonMarked(uint32 what, bool marked);
    void SetText(const char *text);

    static float PreferredHeight();

private:
    std::vector<IconButton *> buttons;
    std::vector<float>        separators;
    float                     next_x;
    std::string               text;
};

#endif
