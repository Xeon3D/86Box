/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the main window (menus, tool bar,
 *          emulated screen, status bar and keyboard input), like the Qt
 *          MainWindow.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef HAIKU_MAINWINDOW_HPP
#define HAIKU_MAINWINDOW_HPP

#include <Window.h>

#include <cstdint>

class BMenu;
class BMenuBar;
class BMenuItem;
class BStringView;
class MachineStatusView;
class MediaMenu;
class RendererView;
class ToolBarView;

class MainWindow : public BWindow {
public:
    MainWindow();
    ~MainWindow() override;

    bool QuitRequested() override;
    void MessageReceived(BMessage *message) override;
    void DispatchMessage(BMessage *message, BHandler *handler) override;
    void MenusBeginning() override;
    void WindowActivated(bool active) override;
    void FrameMoved(BPoint where) override;
    void FrameResized(float width, float height) override;
    void Zoom(BPoint origin, float width, float height) override;

    MediaMenu *Media() const { return media; }

    /* Set once the application is quitting, so closing the window no
       longer asks the application to quit again. */
    static bool app_quitting;

private:
    void BuildMenus();
    void ResizeContents(int w, int h);
    void LayoutViews();
    void SetMouseCapture(bool on);
    void UpdatePauseState();

    bool HandleKeyMessage(BMessage *message);
    void SyncModifierKeys();
    void ProcessKey(uint8_t key, bool down);
    bool MatchAccelerator(const char *name, uint16_t code) const;
    bool HandleAccelerators(uint16_t code);

    void HardReset();
    void AcpiShutdown();
    bool ConfirmExit();
    void OpenSettings();
    void SettingsClosed(bool apply);
    void ToggleFullscreen();
    void ToggleFullscreenUI();
    void SetResizable(bool resizable);
    void SpecifyDimensions();
    void VideoOptionChanged();
    void RefreshMidiMenu();
    void SwitchMidiOut(int device);
    void TakeScreenshot(int kind);
    void ToggleOsd();
    /* Around modal dialogs: free the mouse and, with "pause on dialogs",
       pause the emulation (Qt does both while the window is blocked). */
    void BeginDialog();
    void EndDialog();

    BMenuBar          *menu_bar;
    ToolBarView       *tool_bar;
    RendererView      *renderer;
    MachineStatusView *status_icons;
    MachineStatusView *status_locks; /* Num/Caps/Scroll/Kana Lock, at the right */
    BStringView       *status_text;
    BStringView       *status_hz;
    MediaMenu         *media;
    BWindow           *settings_window;

    BMenu     *media_menu;
    BMenu     *midi_menu;
    BMenu     *midi_command_menu;
    BMenu     *scale_menu;
    BMenu     *filter_menu;
    BMenu     *stretch_menu;
    BMenu     *renderer_menu;
    BMenu     *gl_stretch_menu;
    BMenu     *gl_scale_menu;
    BMenu     *input_device_menu;
    BMenu     *tablet_tool_menu;
    BMenuItem *item_pause;
    BMenuItem *item_mute;

    float   menu_height;
    float   tool_height;
    float   status_height;
    int     emu_w;
    int     emu_h;
    BRect   windowed_frame;   /* the window before going fullscreen */
    bool    fullscreen_ui;    /* menus shown in fullscreen */
    bool    zoomed;
    int     dialog_depth;
    bool    dialog_had_capture;
    uint8_t key_states[16];   /* modifier keys last sent to the guest */
};

#endif
