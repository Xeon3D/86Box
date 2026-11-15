/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the VM manager's dialogs: the add-machine
 *          wizard (qt_vmmanager_addmachine.cpp), the manager preferences
 *          (qt_preferencesmanager.cpp) and the small dialogs of
 *          qt_vmmanager_main.cpp (display name, clone, configuration file).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Alert.h>
#include <Button.h>
#include <CardLayout.h>
#include <CheckBox.h>
#include <Entry.h>
#include <FilePanel.h>
#include <Font.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <Path.h>
#include <RadioButton.h>
#include <ScrollView.h>
#include <SpaceLayoutItem.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include <sys/stat.h>

#include "haiku_dialogs.hpp"
#include "haiku_vmmanager_system.hpp"
#include "haiku_vmmanager_ui.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
}

enum : uint32 {
    MSG_DLG_OK      = 'vdok',
    MSG_DLG_CANCEL  = 'vdcn',
    MSG_DLG_CHANGED = 'vdch',
    MSG_DLG_BACK    = 'vdbk',
    MSG_DLG_NEXT    = 'vdnx',
    MSG_DLG_LOAD    = 'vdld',
    MSG_DLG_BROWSE  = 'vdbr',
    MSG_DLG_DEFAULT = 'vddf',
    MSG_DLG_REFS    = 'vdrf',
};

static bool
dir_exists(const std::string &path)
{
    struct stat st;
    return (stat(path.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
}

static std::string
clean_path(const std::string &path)
{
    std::string out;
    for (const char c : path) {
        if ((c == '/') && !out.empty() && (out[out.size() - 1] == '/'))
            continue;
        out += c;
    }
    while ((out.size() > 1) && (out[out.size() - 1] == '/'))
        out.erase(out.size() - 1);
    return out;
}

/* A window that blocks its creator until it goes away, like a QDialog's
   exec(). */
class VmmDialog : public BWindow {
public:
    VmmDialog(BWindow *parent, const char *title, uint32 extra_flags = B_NOT_RESIZABLE)
        : BWindow(BRect(0, 0, 100, 100), title, B_TITLED_WINDOW_LOOK,
                  (parent != nullptr) ? B_MODAL_SUBSET_WINDOW_FEEL : B_MODAL_APP_WINDOW_FEEL,
                  B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE | extra_flags)
        , done(create_sem(0, "86Box VMM dialog"))
        , parent(parent)
    {
        if (parent != nullptr)
            AddToSubset(parent);
    }

    ~VmmDialog() override { release_sem(done); }

    void Go()
    {
        if (parent != nullptr)
            CenterIn(parent->Frame());
        else
            CenterOnScreen();
        haiku_run_modal(this, parent, done);
    }

protected:
    sem_id   done;
    BWindow *parent;
};

/* ---------------------------------------------------------- display name */

class NameDialog : public VmmDialog {
public:
    NameDialog(BWindow *parent, std::string &name, bool &accepted)
        : VmmDialog(parent, "Set display name")
        , name(name)
        , accepted(accepted)
    {
        BStringView *label = new BStringView("label", "Enter the new display name (blank to reset)");
        label->SetAlignment(B_ALIGN_CENTER);
        label->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
        edit = new BTextControl("name", nullptr, name.c_str(), nullptr);
        edit->SetExplicitMinSize(BSize(400, B_SIZE_UNSET));

        BButton *ok = new BButton("ok", "OK", new BMessage(MSG_DLG_OK));
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .Add(label)
            .Add(edit)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(new BButton("cancel", "Cancel", new BMessage(MSG_DLG_CANCEL)))
                .Add(ok)
            .End();
        SetDefaultButton(ok);
        edit->MakeFocus(true);
        edit->TextView()->SelectAll();
    }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_DLG_OK:
                name     = edit->Text();
                accepted = true;
                Quit();
                break;
            case MSG_DLG_CANCEL:
                Quit();
                break;
            default:
                BWindow::MessageReceived(message);
                break;
        }
    }

private:
    std::string  &name;
    bool         &accepted;
    BTextControl *edit;
};

bool
vmm_display_name_dialog(BWindow *parent, std::string &name)
{
    bool        accepted = false;
    NameDialog *dialog   = new NameDialog(parent, name, accepted);
    dialog->Go();
    return accepted;
}

/* ----------------------------------------------------------------- clone */

class CloneDialog : public VmmDialog {
public:
    CloneDialog(BWindow *parent, const VmmSystem *system, std::string &name, bool &accepted)
        : VmmDialog(parent, "Clone")
        , name(name)
        , accepted(accepted)
    {
        const std::string text = "Virtual machine \"" + system->display_name + "\" (" + system->config_dir + ") will be cloned into:";
        BStringView *label = new BStringView("label", text.c_str());
        edit               = new BTextControl("name", nullptr, "", nullptr);
        edit->SetModificationMessage(new BMessage(MSG_DLG_CHANGED));
        error = new BStringView("error", "");
        error->SetHighUIColor(B_FAILURE_COLOR);
        error->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

        ok = new BButton("ok", "OK", new BMessage(MSG_DLG_OK));
        ok->SetEnabled(false);
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .Add(label)
            .Add(edit)
            .Add(error)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(new BButton("cancel", "Cancel", new BMessage(MSG_DLG_CANCEL)))
                .Add(ok)
            .End();
        SetDefaultButton(ok);
        edit->MakeFocus(true);
    }

    void Validate()
    {
        const std::string text       = edit->Text();
        bool              space_only = true;
        for (const char c : text) {
            if ((c == '\\') || (c == '/')) {
                error->SetText("You cannot use the following characters in the name: \\ /");
                ok->SetEnabled(false);
                return;
            }
            if (!isspace((unsigned char) c))
                space_only = false;
        }
        error->SetText("");
        const bool usable = !space_only && !text.empty();
        ok->SetEnabled(usable);
        const std::string dir = VmmSystem::Canonical(vmm_path) + "/" + text;
        if (usable && dir_exists(dir)) {
            error->SetText(("Directory " + VmmSystem::Canonical(dir) + " already exists").c_str());
            ok->SetEnabled(false);
        }
    }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_DLG_CHANGED:
                Validate();
                break;
            case MSG_DLG_OK:
                if (!ok->IsEnabled())
                    break;
                name     = edit->Text();
                accepted = true;
                Quit();
                break;
            case MSG_DLG_CANCEL:
                Quit();
                break;
            default:
                BWindow::MessageReceived(message);
                break;
        }
    }

private:
    std::string  &name;
    bool         &accepted;
    BTextControl *edit;
    BStringView  *error;
    BButton      *ok;
};

bool
vmm_clone_dialog(BWindow *parent, const VmmSystem *system, std::string &name)
{
    bool         accepted = false;
    CloneDialog *dialog   = new CloneDialog(parent, system, name, accepted);
    dialog->Go();
    return accepted;
}

/* ------------------------------------------------------- add-machine wizard */

/* A text view that reports changes to its window. */
class WatchedTextView : public BTextView {
public:
    explicit WatchedTextView(const char *name)
        : BTextView(name)
    {
    }

protected:
    void InsertText(const char *text, int32 length, int32 offset, const text_run_array *runs) override
    {
        BTextView::InsertText(text, length, offset, runs);
        if (Window() != nullptr)
            Window()->PostMessage(MSG_DLG_CHANGED);
    }

    void DeleteText(int32 from, int32 to) override
    {
        BTextView::DeleteText(from, to);
        if (Window() != nullptr)
            Window()->PostMessage(MSG_DLG_CHANGED);
    }
};

class AddMachineWizard : public VmmDialog {
public:
    enum Page {
        PAGE_INTRO,
        PAGE_EXISTING,
        PAGE_NAME,
        PAGE_CONCLUSION,
    };

    AddMachineWizard(BWindow *parent, VmmNewMachine &result, bool &accepted)
        : VmmDialog(parent, "Add new system wizard", 0)
        , result(result)
        , accepted(accepted)
        , page(PAGE_INTRO)
        , panel(nullptr)
    {
        title = new BStringView("title", "");
        BFont bold(be_bold_font);
        bold.SetSize(be_bold_font->Size() * 1.2f);
        title->SetFont(&bold);
        subtitle = new BStringView("subtitle", "");

        /* Introduction */
        new_config      = new BRadioButton("new", "New configuration", new BMessage(MSG_DLG_CHANGED));
        existing_config = new BRadioButton("existing", "Use existing configuration", new BMessage(MSG_DLG_CHANGED));
        new_config->SetValue(B_CONTROL_ON);
        BGroupView *intro = new BGroupView(B_VERTICAL);
        BLayoutBuilder::Group<>(intro)
            .Add(new BStringView("intro", "This will help you add a new system to 86Box."))
            .Add(new_config)
            .Add(existing_config)
            .AddGlue();

        /* Existing configuration */
        configuration = new WatchedTextView("configuration");
        configuration->SetFontAndColor(be_fixed_font);
        configuration->SetStylable(false);
        BScrollView *configuration_scroll = new BScrollView("configuration_scroll", configuration, 0, true, true);
        configuration_scroll->SetExplicitMinSize(BSize(360, 200));
        BGroupView *existing = new BGroupView(B_VERTICAL);
        BLayoutBuilder::Group<>(existing)
            .Add(configuration_scroll)
            .AddGroup(B_HORIZONTAL)
                .Add(new BButton("load", "Load configuration from file…", new BMessage(MSG_DLG_LOAD)))
                .AddGlue()
            .End();

        /* Name */
        system_name = new BTextControl("system_name", "System name:", "", new BMessage(MSG_DLG_NEXT));
        system_name->SetModificationMessage(new BMessage(MSG_DLG_CHANGED));
        validation = new BStringView("validation", "");
        validation->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
        display_name = new BTextControl("display_name", "Display name (optional):", "", new BMessage(MSG_DLG_NEXT));
        BGroupView *name_page = new BGroupView(B_VERTICAL);
        BLayoutBuilder::Group<>(name_page)
            .AddStrut(20)
            .AddGrid()
                .AddTextControl(system_name, 0, 0)
                .Add(validation, 0, 1, 2)
                .Add(BSpaceLayoutItem::CreateVerticalStrut(20), 0, 2)
                .AddTextControl(display_name, 0, 3)
            .End()
            .AddGlue();

        /* Conclusion */
        conclusion_name         = new BStringView("conclusion_name", "");
        conclusion_display_label = new BStringView("conclusion_display_label", "Display name:");
        conclusion_display      = new BStringView("conclusion_display", "");
        BGroupView *conclusion  = new BGroupView(B_VERTICAL);
        BLayoutBuilder::Group<>(conclusion)
            .Add(new BStringView("conclusion", "The wizard will now launch the configuration for the new system."))
            .AddStrut(20)
            .AddGrid()
                .Add(new BStringView("conclusion_name_label", "System name:"), 0, 0)
                .Add(conclusion_name, 1, 0)
                .Add(conclusion_display_label, 0, 1)
                .Add(conclusion_display, 1, 1)
            .End()
            .AddGlue();

        cards = new BCardLayout();
        BView *pages = new BView("pages", 0, cards);
        cards->AddView(intro);
        cards->AddView(existing);
        cards->AddView(name_page);
        cards->AddView(conclusion);

        back   = new BButton("back", "< Back", new BMessage(MSG_DLG_BACK));
        next   = new BButton("next", "Next >", new BMessage(MSG_DLG_NEXT));
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .Add(title)
            .Add(subtitle)
            .Add(pages)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(back)
                .Add(next)
                .Add(new BButton("cancel", "Cancel", new BMessage(MSG_DLG_CANCEL)))
            .End();
        SetDefaultButton(next);
        ShowPage(PAGE_INTRO);
    }

    ~AddMachineWizard() override { delete panel; }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_DLG_CHANGED:
                UpdateButtons();
                break;
            case MSG_DLG_NEXT:
                Next();
                break;
            case MSG_DLG_BACK:
                if (!history.empty()) {
                    const Page previous = history.back();
                    history.pop_back();
                    ShowPage(previous);
                }
                break;
            case MSG_DLG_LOAD:
                if (panel == nullptr) {
                    BMessenger messenger(this);
                    panel = new BFilePanel(B_OPEN_PANEL, &messenger, nullptr, B_FILE_NODE, false,
                                           new BMessage(MSG_DLG_REFS), nullptr, true, true);
                    panel->Window()->SetTitle("Choose configuration file");
                    panel->SetPanelDirectory(vmm_path);
                }
                panel->Show();
                break;
            case MSG_DLG_REFS:
                {
                    entry_ref ref;
                    if (message->FindRef("refs", &ref) == B_OK)
                        LoadFile(ref);
                    break;
                }
            case MSG_DLG_CANCEL:
                Quit();
                break;
            default:
                BWindow::MessageReceived(message);
                break;
        }
    }

private:
    void LoadFile(const entry_ref &ref)
    {
        BPath         path(&ref);
        std::ifstream file(path.Path() != nullptr ? path.Path() : "");
        if (!file.is_open()) {
            const std::string text = std::string("Unable to open the selected configuration file for reading: ") + strerror(errno);
            BAlert *alert = new BAlert("Configuration read failed", text.c_str(), "OK", nullptr, nullptr,
                                       B_WIDTH_AS_USUAL, B_STOP_ALERT);
            alert->Go();
            return;
        }
        std::stringstream contents;
        contents << file.rdbuf();
        configuration->SetText(contents.str().c_str());
        UpdateButtons();
    }

    /* NameAndLocationPage::isComplete() */
    bool NameComplete()
    {
        const std::string name = system_name->Text();
        if (name.empty())
            validation->SetText("Please enter a system name");
        else if (name.find('/') != std::string::npos)
            validation->SetText("System name cannot contain certain characters");
        else if (dir_exists(clean_path(std::string(vmm_path) + "/" + name)))
            validation->SetText("System name already exists");
        else {
            validation->SetText("");
            return true;
        }
        return false;
    }

    bool PageComplete()
    {
        switch (page) {
            case PAGE_EXISTING:
                return configuration->TextLength() > 0;
            case PAGE_NAME:
                return NameComplete();
            default:
                return true;
        }
    }

    void UpdateButtons()
    {
        back->SetEnabled(!history.empty());
        next->SetEnabled(PageComplete());
    }

    void ShowPage(Page new_page)
    {
        page = new_page;
        cards->SetVisibleItem((int32) page);
        switch (page) {
            case PAGE_INTRO:
                title->SetText("Introduction");
                subtitle->SetText("");
                break;
            case PAGE_EXISTING:
                title->SetText("Use existing configuration");
                subtitle->SetText("Paste the contents of the existing configuration file into the box below.");
                configuration->MakeFocus(true);
                break;
            case PAGE_NAME:
                title->SetText("System name");
                subtitle->SetText("Enter the name of the system");
                system_name->MakeFocus(true);
                break;
            case PAGE_CONCLUSION:
                title->SetText("Complete");
                subtitle->SetText("");
                conclusion_name->SetText(system_name->Text());
                conclusion_display->SetText(display_name->Text());
                if (display_name->Text()[0] != '\0') {
                    if (conclusion_display->IsHidden()) {
                        conclusion_display->Show();
                        conclusion_display_label->Show();
                    }
                } else if (!conclusion_display->IsHidden()) {
                    conclusion_display->Hide();
                    conclusion_display_label->Hide();
                }
                break;
        }
        next->SetLabel((page == PAGE_CONCLUSION) ? "Finish" : "Next >");
        UpdateButtons();
    }

    void Next()
    {
        if (!PageComplete())
            return;
        switch (page) {
            case PAGE_INTRO:
                history.push_back(page);
                ShowPage((existing_config->Value() == B_CONTROL_ON) ? PAGE_EXISTING : PAGE_NAME);
                break;
            case PAGE_EXISTING:
                history.push_back(page);
                ShowPage(PAGE_NAME);
                break;
            case PAGE_NAME:
                history.push_back(page);
                ShowPage(PAGE_CONCLUSION);
                break;
            case PAGE_CONCLUSION:
                result.name          = system_name->Text();
                result.display_name  = display_name->Text();
                result.configuration = (existing_config->Value() == B_CONTROL_ON) ? configuration->Text() : "";
                accepted             = true;
                Quit();
                break;
        }
    }

    VmmNewMachine     &result;
    bool              &accepted;
    Page               page;
    std::vector<Page>  history;
    BFilePanel        *panel;
    BCardLayout       *cards;
    BStringView       *title;
    BStringView       *subtitle;
    BRadioButton      *new_config;
    BRadioButton      *existing_config;
    BTextView         *configuration;
    BTextControl      *system_name;
    BStringView       *validation;
    BTextControl      *display_name;
    BStringView       *conclusion_name;
    BStringView       *conclusion_display_label;
    BStringView       *conclusion_display;
    BButton           *back;
    BButton           *next;
};

bool
vmm_add_machine_wizard(BWindow *parent, VmmNewMachine &result)
{
    bool              accepted = false;
    AddMachineWizard *wizard   = new AddMachineWizard(parent, result, accepted);
    wizard->Go();
    return accepted;
}

/* --------------------------------------------------- manager preferences */

class PreferencesDialog : public VmmDialog {
public:
    PreferencesDialog(BWindow *parent, bool machines_running, bool &accepted)
        : VmmDialog(parent, "VM manager preferences")
        , accepted(accepted)
        , panel(nullptr)
    {
        std::string dir = vmm_path_cfg;
        if (dir.empty())
            dir = vmm_path;
        directory = new BTextControl("directory", "System directory:", dir.c_str(), nullptr);
        directory->SetExplicitMinSize(BSize(380, B_SIZE_UNSET));
        BButton *browse      = new BButton("browse", "Browse…", new BMessage(MSG_DLG_BROWSE));
        BButton *default_dir = new BButton("default", "Default", new BMessage(MSG_DLG_DEFAULT));
        if (machines_running) {
            directory->SetEnabled(false);
            browse->SetEnabled(false);
            default_dir->SetEnabled(false);
            browse->SetToolTip("To change the system directory, stop all running machines.");
        }

        remember = new BCheckBox("remember", "Remember size & position", nullptr);
        remember->SetValue(vmm_window_remember ? B_CONTROL_ON : B_CONTROL_OFF);
        update_check = new BCheckBox("update", "Check for updates on startup", nullptr);
        update_check->SetValue(vmm_update_check ? B_CONTROL_ON : B_CONTROL_OFF);
        regex = new BCheckBox("regex", "Use regular expressions in search box", nullptr);
        regex->SetValue(vmm_regex_search ? B_CONTROL_ON : B_CONTROL_OFF);
        trash = new BCheckBox("trash", "Delete virtual machines to the Trash", nullptr);
        trash->SetValue(vmm_delete_to_trash ? B_CONTROL_ON : B_CONTROL_OFF);

        BButton *ok = new BButton("ok", "OK", new BMessage(MSG_DLG_OK));
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .AddGroup(B_HORIZONTAL)
                .Add(directory)
                .Add(browse)
                .Add(default_dir)
            .End()
            .Add(remember)
            .Add(update_check)
            .Add(regex)
            .Add(trash)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(new BButton("cancel", "Cancel", new BMessage(MSG_DLG_CANCEL)))
                .Add(ok)
            .End();
        SetDefaultButton(ok);
    }

    ~PreferencesDialog() override { delete panel; }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case MSG_DLG_BROWSE:
                if (panel == nullptr) {
                    BMessenger messenger(this);
                    panel = new BFilePanel(B_OPEN_PANEL, &messenger, nullptr, B_DIRECTORY_NODE, false,
                                           new BMessage(MSG_DLG_REFS), nullptr, true, true);
                    panel->Window()->SetTitle("Choose directory");
                }
                panel->SetPanelDirectory(directory->Text());
                panel->Show();
                break;
            case MSG_DLG_REFS:
                {
                    entry_ref ref;
                    if (message->FindRef("refs", &ref) == B_OK) {
                        BPath path(&ref);
                        if (path.Path() != nullptr)
                            directory->SetText(path.Path());
                    }
                    break;
                }
            case MSG_DLG_DEFAULT:
                {
                    char temp[1024];
                    plat_get_vmm_dir(temp, sizeof(temp));
                    directory->SetText(clean_path(temp).c_str());
                    break;
                }
            case MSG_DLG_OK:
                Save();
                accepted = true;
                Quit();
                break;
            case MSG_DLG_CANCEL:
                Quit();
                break;
            default:
                BWindow::MessageReceived(message);
                break;
        }
    }

private:
    /* PreferencesManager::save() */
    void Save()
    {
        const std::string dir = clean_path(directory->Text());
        strncpy(vmm_path_cfg, dir.c_str(), sizeof(vmm_path_cfg) - 1);
        vmm_path_cfg[sizeof(vmm_path_cfg) - 1] = '\0';
        vmm_update_check    = (update_check->Value() == B_CONTROL_ON) ? 1 : 0;
        vmm_window_remember = (remember->Value() == B_CONTROL_ON) ? 1 : 0;
        vmm_regex_search    = (regex->Value() == B_CONTROL_ON) ? 1 : 0;
        vmm_delete_to_trash = (trash->Value() == B_CONTROL_ON) ? 1 : 0;
    }

    bool         &accepted;
    BFilePanel   *panel;
    BTextControl *directory;
    BCheckBox    *remember;
    BCheckBox    *update_check;
    BCheckBox    *regex;
    BCheckBox    *trash;
};

bool
vmm_preferences_dialog(BWindow *parent, bool machines_running)
{
    bool               accepted = false;
    PreferencesDialog *dialog   = new PreferencesDialog(parent, machines_running, accepted);
    dialog->Go();
    return accepted;
}

/* ----------------------------------------------------------- text viewer */

class TextFileWindow : public BWindow {
public:
    TextFileWindow(const std::string &title, const std::string &contents)
        : BWindow(BRect(0, 0, 539, 359), title.c_str(), B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    {
        BTextView *text = new BTextView("text");
        text->SetStylable(true);
        text->SetFontAndColor(be_fixed_font);
        text->SetText(contents.c_str());
        text->MakeEditable(false);
        text->SetWordWrap(false);
        BScrollView *scroll = new BScrollView("scroll", text, 0, true, true);
        scroll->SetExplicitMinSize(BSize(300, 200));

        BButton *ok = new BButton("ok", "OK", new BMessage(B_QUIT_REQUESTED));
        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_WINDOW_SPACING)
            .Add(scroll)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(ok)
            .End();
        SetDefaultButton(ok);
        ResizeTo(539, 359);
    }
};

void
vmm_show_text_file(BWindow *parent, const std::string &title, const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        pclog("VMM: couldn't open %s\n", path.c_str());
        return;
    }
    std::stringstream contents;
    contents << file.rdbuf();

    const size_t   slash  = path.rfind('/');
    const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    TextFileWindow *window = new TextFileWindow(title + " - " + name, contents.str());
    if (parent != nullptr)
        window->CenterIn(parent->Frame());
    window->Show();
}

/* -------------------------------------------------------------- progress */

bool
vmm_run_with_progress(BWindow *parent, const char *title, const char *text, bool (*work)(void *), void *data)
{
    BWindow *window = new BWindow(BRect(0, 0, 100, 100), title, B_MODAL_WINDOW_LOOK,
                                  (parent != nullptr) ? B_MODAL_SUBSET_WINDOW_FEEL : B_MODAL_APP_WINDOW_FEEL,
                                  B_NOT_CLOSABLE | B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS);
    if (parent != nullptr)
        window->AddToSubset(parent);
    BLayoutBuilder::Group<>(window, B_VERTICAL)
        .SetInsets(B_USE_WINDOW_SPACING)
        .Add(new BStringView("text", text));
    if (parent != nullptr)
        window->CenterIn(parent->Frame());
    window->Show();

    std::atomic<bool> finished { false };
    bool              result = false;
    std::thread       worker([&]() {
        result   = work(data);
        finished = true;
    });
    while (!finished) {
        snooze(50000);
        if (parent != nullptr)
            parent->UpdateIfNeeded();
    }
    worker.join();

    if (window->Lock())
        window->Quit();
    return result;
}
