/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: "Renderer options..." of the OpenGL 3
 *          renderer, Qt's OpenGLShaderManagerDialog (render behaviour,
 *          VSync and the shader list) and OpenGLShaderConfig (a shader's
 *          parameters).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Box.h>
#include <Button.h>
#include <CheckBox.h>
#include <Entry.h>
#include <FilePanel.h>
#include <GridView.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Node.h>
#include <Path.h>
#include <RadioButton.h>
#include <ScrollView.h>
#include <Slider.h>
#include <StringItem.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <strings.h>

#include "haiku_dialogs.hpp"
#include "haiku_renderers.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/path.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/renderdefs.h>
#include <86box/version.h>
#include <86box/qt_glslp_parser.h>

extern char gl3_shader_file[MAX_USER_SHADERS][512];
}

enum : uint32 {
    MSG_SM_OK        = 'smok',
    MSG_SM_APPLY     = 'smap',
    MSG_SM_CANCEL    = 'smcn',
    MSG_SM_SYNC      = 'smsy',
    MSG_SM_TARGET    = 'smtg',
    MSG_SM_FRAMERATE = 'smfr',
    MSG_SM_SELECT    = 'smsl',
    MSG_SM_ADD       = 'smad',
    MSG_SM_ADD_REF   = 'smar',
    MSG_SM_REMOVE    = 'smrm',
    MSG_SM_CONFIGURE = 'smcf',
    MSG_SM_UP        = 'smup',
    MSG_SM_DOWN      = 'smdn',
    MSG_SC_OK        = 'scok',
    MSG_SC_APPLY     = 'scap',
    MSG_SC_CANCEL    = 'sccn',
    MSG_SC_RESET     = 'scrs',
};

/* A window that releases a semaphore when it goes away, so that its
   creator can wait for it with haiku_run_modal() (as in haiku_dialogs.cpp). */
class GLModalWindow : public BWindow {
public:
    GLModalWindow(BWindow *parent, const char *title)
        : BWindow(BRect(0, 0, 100, 100), title, B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
                  B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS
                      | B_CLOSE_ON_ESCAPE)
        , done(create_sem(0, "86Box modal dialog"))
        , parent(parent)
    {
        if (parent != nullptr)
            AddToSubset(parent);
    }

    ~GLModalWindow() override { release_sem(done); }

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

/* ------------------------------------------------------ shader settings */

class ShaderConfigWindow : public GLModalWindow {
public:
    ShaderConfigWindow(BWindow *parent, glslp_t *shader)
        : GLModalWindow(parent, "Shader Configuration")
        , shader(shader)
    {
        glslp_read_shader_config(shader);

        BGridView   *grid   = new BGridView(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
        BGridLayout *layout = grid->GridLayout();
        layout->SetInsets(B_USE_SMALL_SPACING);
        fields = new BTextControl *[shader->num_parameters > 0 ? shader->num_parameters : 1];
        for (int i = 0; i < shader->num_parameters; i++) {
            fields[i] = new BTextControl(shader->parameters[i].id, shader->parameters[i].description, "", nullptr);
            SetField(i, shader->parameters[i].value);
            layout->AddItem(fields[i]->CreateLabelLayoutItem(), 0, i);
            layout->AddItem(fields[i]->CreateTextViewLayoutItem(), 1, i);
        }
        layout->AddItem(BSpaceLayoutItem::CreateGlue(), 0, shader->num_parameters, 2, 1);

        BScrollView *scroll = new BScrollView("parameters", grid, 0, false, true);
        scroll->SetExplicitPreferredSize(BSize(420, (shader->num_parameters > 10) ? 320 : B_SIZE_UNSET));

        BButton *ok = new BButton("ok", "OK", new BMessage(MSG_SC_OK));
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .Add(scroll)
            .AddGroup(B_HORIZONTAL)
                .Add(new BButton("reset", "Reset", new BMessage(MSG_SC_RESET)))
                .AddGlue()
                .Add(new BButton("cancel", "Cancel", new BMessage(MSG_SC_CANCEL)))
                .Add(new BButton("apply", "Apply", new BMessage(MSG_SC_APPLY)))
                .Add(ok)
            .End();
        SetDefaultButton(ok);
    }

    ~ShaderConfigWindow() override { delete[] fields; }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_SC_RESET:
                for (int i = 0; i < shader->num_parameters; i++)
                    SetField(i, shader->parameters[i].default_value);
                break;
            case MSG_SC_APPLY:
                Save();
                break;
            case MSG_SC_OK:
                Save();
                Quit();
                break;
            case MSG_SC_CANCEL:
                Quit();
                break;
            default:
                GLModalWindow::MessageReceived(message);
                break;
        }
    }

private:
    void SetField(int i, float value)
    {
        char text[64];
        snprintf(text, sizeof(text), "%.3f", value);
        fields[i]->SetText(text);
    }

    /* OpenGLShaderConfig::on_OpenGLShaderConfig_accepted(). */
    void Save()
    {
        startblit();
        for (int i = 0; i < shader->num_parameters; i++) {
            struct parameter *param = &shader->parameters[i];
            char             *end   = nullptr;
            float             value = strtof(fields[i]->Text(), &end);
            if (end == fields[i]->Text())
                value = param->value;
            if (value < param->min)
                value = param->min;
            if (value > param->max)
                value = param->max;
            param->value = value;
            SetField(i, value);
        }
        glslp_write_shader_config(shader);
        config_save();
        endblit();
        haiku_renderer_reload();
    }

    glslp_t       *shader;
    BTextControl **fields;
};

/* -------------------------------------------------------- shader manager */

/* A shader in the list: its file and its parsed preset. */
class ShaderItem : public BStringItem {
public:
    ShaderItem(const char *path, glslp_t *shader)
        : BStringItem(path_get_filename(const_cast<char *>(path)))
        , file(path)
        , shader(shader)
    {
    }

    ~ShaderItem() override { glslp_free(shader); }

    std::string file;
    glslp_t    *shader;
};

class ShaderFilter : public BRefFilter {
public:
    bool Filter(const entry_ref *ref, BNode *node, struct stat_beos *, const char *) override
    {
        if ((node != nullptr) && node->IsDirectory())
            return true;
        BEntry entry(ref, true);
        if (entry.IsDirectory())
            return true;
        const char *dot = strrchr(ref->name, '.');
        return (dot != nullptr) && (!strcasecmp(dot, ".glsl") || !strcasecmp(dot, ".glslp"));
    }
};

class ShaderManagerWindow : public GLModalWindow {
public:
    explicit ShaderManagerWindow(BWindow *parent)
        : GLModalWindow(parent, "Shader Manager")
    {
        sync   = new BRadioButton("sync", "Synchronize with video", new BMessage(MSG_SM_SYNC));
        target = new BRadioButton("target", "Use target framerate:", new BMessage(MSG_SM_TARGET));
        rate   = new BSlider("rate", nullptr, new BMessage(MSG_SM_FRAMERATE), 15, 240, B_HORIZONTAL);
        rate->SetModificationMessage(new BMessage(MSG_SM_FRAMERATE));
        rate->SetExplicitMinSize(BSize(200, B_SIZE_UNSET));
        rate_label = new BStringView("rate label", "240 fps");
        rate_label->SetExplicitMinSize(rate_label->PreferredSize());
        vsync = new BCheckBox("vsync", "VSync", nullptr);

        vsync->SetValue(video_vsync ? B_CONTROL_ON : B_CONTROL_OFF);
        sync->SetValue(video_framerate == -1);
        target->SetValue(video_framerate != -1);
        rate->SetValue((video_framerate != -1) ? video_framerate : 60);
        rate->SetEnabled(video_framerate != -1);
        UpdateRateLabel();

        list = new BListView("shaders");
        list->SetSelectionMessage(new BMessage(MSG_SM_SELECT));
        BScrollView *scroll = new BScrollView("shader scroll", list, 0, false, true);
        scroll->SetExplicitPreferredSize(BSize(320, 200));

        add       = new BButton("add", "Add", new BMessage(MSG_SM_ADD));
        remove    = new BButton("remove", "Remove", new BMessage(MSG_SM_REMOVE));
        configure = new BButton("configure", "Configure", new BMessage(MSG_SM_CONFIGURE));
        up        = new BButton("up", "Move up", new BMessage(MSG_SM_UP));
        down      = new BButton("down", "Move down", new BMessage(MSG_SM_DOWN));

        BBox *behaviour = new BBox("behaviour");
        behaviour->SetLabel("Render behavior");
        BGroupView *behaviour_group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
        BLayoutBuilder::Group<>(behaviour_group)
            .SetInsets(B_USE_SMALL_SPACING)
            .Add(sync)
            .AddGroup(B_HORIZONTAL)
                .Add(target)
                .Add(rate)
                .Add(rate_label)
            .End()
            .Add(vsync);
        behaviour->AddChild(behaviour_group);

        BBox *shaders = new BBox("shaders box");
        shaders->SetLabel("Shaders");
        BGroupView *shaders_group = new BGroupView(B_HORIZONTAL);
        BLayoutBuilder::Group<>(shaders_group)
            .SetInsets(B_USE_SMALL_SPACING)
            .Add(scroll)
            .AddGroup(B_VERTICAL, B_USE_SMALL_SPACING)
                .Add(add)
                .Add(remove)
                .Add(configure)
                .Add(up)
                .Add(down)
                .AddGlue()
            .End();
        shaders->AddChild(shaders_group);

        BButton *ok = new BButton("ok", "OK", new BMessage(MSG_SM_OK));
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .Add(behaviour)
            .Add(shaders)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(new BButton("cancel", "Cancel", new BMessage(MSG_SM_CANCEL)))
                .Add(new BButton("apply", "Apply", new BMessage(MSG_SM_APPLY)))
                .Add(ok)
            .End();
        SetDefaultButton(ok);

        for (int i = 0; i < MAX_USER_SHADERS; i++) {
            if ((gl3_shader_file[i][0] != 0) && (path_get_filename(gl3_shader_file[i])[0] != 0)) {
                glslp_t *shader = glslp_parse(gl3_shader_file[i]);
                if (shader != nullptr)
                    list->AddItem(new ShaderItem(gl3_shader_file[i], shader));
            }
        }
        if (list->CountItems() > 0)
            list->Select(list->CountItems() - 1);
        UpdateButtons();
    }

    ~ShaderManagerWindow() override
    {
        for (int32 i = list->CountItems() - 1; i >= 0; i--)
            delete list->RemoveItem(i);
    }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_SM_SYNC:
            case MSG_SM_TARGET:
                rate->SetEnabled(target->Value() == B_CONTROL_ON);
                break;
            case MSG_SM_FRAMERATE:
                UpdateRateLabel();
                break;
            case MSG_SM_SELECT:
                UpdateButtons();
                break;
            case MSG_SM_ADD:
                Browse();
                break;
            case MSG_SM_ADD_REF:
                Add(message);
                break;
            case MSG_SM_REMOVE:
                {
                    const int32 index = list->CurrentSelection();
                    if (index >= 0) {
                        delete list->RemoveItem(index);
                        if (list->CountItems() > 0)
                            list->Select((index < list->CountItems()) ? index : (list->CountItems() - 1));
                    }
                    UpdateButtons();
                    break;
                }
            case MSG_SM_CONFIGURE:
                {
                    ShaderItem *item = dynamic_cast<ShaderItem *>(list->ItemAt(list->CurrentSelection()));
                    if ((item != nullptr) && (item->shader->num_parameters > 0))
                        (new ShaderConfigWindow(this, item->shader))->Go();
                    break;
                }
            case MSG_SM_UP:
            case MSG_SM_DOWN:
                {
                    const int32 index = list->CurrentSelection();
                    const int32 other = index + ((message->what == MSG_SM_UP) ? -1 : 1);
                    if ((index >= 0) && (other >= 0) && (other < list->CountItems())) {
                        list->SwapItems(index, other);
                        list->Select(other);
                    }
                    UpdateButtons();
                    break;
                }
            case MSG_SM_APPLY:
                Save();
                break;
            case MSG_SM_OK:
                Save();
                Quit();
                break;
            case MSG_SM_CANCEL:
                Quit();
                break;
            default:
                GLModalWindow::MessageReceived(message);
                break;
        }
    }

private:
    void UpdateRateLabel()
    {
        char text[32];
        snprintf(text, sizeof(text), "%d fps", (int) rate->Value());
        rate_label->SetText(text);
    }

    void UpdateButtons()
    {
        const int32 index = list->CurrentSelection();
        ShaderItem *item  = dynamic_cast<ShaderItem *>(list->ItemAt(index));
        remove->SetEnabled(item != nullptr);
        configure->SetEnabled((item != nullptr) && (item->shader->num_parameters > 0));
        up->SetEnabled((item != nullptr) && (index > 0));
        down->SetEnabled((item != nullptr) && (index < (list->CountItems() - 1)));
        add->SetEnabled(list->CountItems() < MAX_USER_SHADERS);
    }

    void Browse()
    {
        BMessenger me(this);
        BMessage   msg(MSG_SM_ADD_REF);
        panel.reset(new BFilePanel(B_OPEN_PANEL, &me, nullptr, B_FILE_NODE, false, &msg, &filter, true));
        panel->Window()->SetTitle(EMU_NAME ": Add shader");
        if (usr_path[0] != '\0')
            panel->SetPanelDirectory(usr_path);
        panel->Show();
    }

    void Add(BMessage *message)
    {
        entry_ref ref;
        BPath     path;
        if ((message->FindRef("refs", &ref) != B_OK) || (path.SetTo(&ref) != B_OK))
            return;

        glslp_t *shader = glslp_parse(path.Path());
        if (shader == nullptr) {
            char text[1100];
            snprintf(text, sizeof(text), "Could not load file %s", path.Path());
            BAlert *alert = new BAlert("GLSL error", text, "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT);
            alert->Go();
            return;
        }
        list->AddItem(new ShaderItem(path.Path(), shader));
        list->Select(list->CountItems() - 1);
        UpdateButtons();
    }

    /* OpenGLShaderManagerDialog::on_OpenGLShaderManagerDialog_accepted(). */
    void Save()
    {
        memset(gl3_shader_file, 0, sizeof(gl3_shader_file));
        for (int32 i = 0; (i < list->CountItems()) && (i < MAX_USER_SHADERS); i++) {
            ShaderItem *item = static_cast<ShaderItem *>(list->ItemAt(i));
            snprintf(gl3_shader_file[i], sizeof(gl3_shader_file[i]), "%s", item->file.c_str());
        }
        startblit();
        video_vsync     = (vsync->Value() == B_CONTROL_ON);
        video_framerate = (target->Value() == B_CONTROL_ON) ? (int) rate->Value() : -1;
        config_save();
        endblit();
        haiku_renderer_reload();
    }

    BRadioButton               *sync;
    BRadioButton               *target;
    BSlider                    *rate;
    BStringView                *rate_label;
    BCheckBox                  *vsync;
    BListView                  *list;
    BButton                    *add;
    BButton                    *remove;
    BButton                    *configure;
    BButton                    *up;
    BButton                    *down;
    ShaderFilter                filter;
    std::unique_ptr<BFilePanel> panel;
};

bool
haiku_renderer_options_available(int vid_api)
{
    return vid_api == RENDERER_OPENGL3;
}

void
haiku_renderer_options_dialog(BWindow *parent, int vid_api)
{
    if (!haiku_renderer_options_available(vid_api))
        return;

    (new ShaderManagerWindow(parent))->Go();
}
