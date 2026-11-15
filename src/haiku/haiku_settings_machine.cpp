/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Machine Settings page (qt_settingsmachine.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Box.h>
#include <Button.h>
#include <CheckBox.h>
#include <GridView.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <RadioButton.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "haiku_deviceconfig.hpp"
#include "haiku_settings.hpp"
#include "haiku_settings_common.hpp"

/* The core headers are not self-contained: keep this order (as in Qt). */
extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/nvr.h>
#include <86box/video.h>
#include <86box/keyboard.h>
#include <86box/mouse.h>
#include <86box/sound.h>
#include <86box/midi.h>
#include <86box/version.h>
#include "../cpu/cpu.h"
}

enum : uint32 {
    MSG_MACHINE_TYPE   = 'smty',
    MSG_MACHINE        = 'smch',
    MSG_MACHINE_CFG    = 'smcf',
    MSG_CPU_FAMILY     = 'smcp',
    MSG_CPU_SPEED      = 'smsp',
    MSG_FPU            = 'smfp',
    MSG_SOFTFLOAT      = 'smsf',
    MSG_DYNAREC        = 'smdy',
    MSG_DEBUG_REGS     = 'smdr',
    MSG_TIME_SYNC      = 'smts',
    MSG_DATE_MODE      = 'smdm',
    MSG_YEARS_OFFSET   = 'smyo',
    MSG_FIXED_YEAR     = 'smfy',
};

/* Shows or hides a view once, whatever its current state. */
static void
set_shown(BView *view, bool shown)
{
    if (shown && view->IsHidden(view))
        view->Show();
    else if (!shown && !view->IsHidden(view))
        view->Hide();
}

/* A label greyed out like a disabled control's. */
static void
set_label_enabled(BStringView *label, bool enabled)
{
    label->SetHighUIColor(B_PANEL_TEXT_COLOR, enabled ? B_NO_TINT : B_DISABLED_LABEL_TINT);
    label->Invalidate();
}

/* A number field (Qt spin box): digits only, plus '-' when negative values
   are allowed. The text is set before the modification message, so filling
   it does not count as a change by the user. */
static BTextControl *
number_field(const char *name, int value, bool negative, uint32 what)
{
    char text[16];
    snprintf(text, sizeof(text), "%i", value);

    BTextControl *field = new BTextControl(name, nullptr, text, nullptr);
    field->SetExplicitMinSize(BSize(be_plain_font->StringWidth("-00000") + 10, B_SIZE_UNSET));
    field->SetExplicitMaxSize(BSize(be_plain_font->StringWidth("-00000") + 10, B_SIZE_UNSET));
    for (int c = 0; c < 256; c++) {
        if (((c < '0') || (c > '9')) && !(negative && (c == '-')))
            field->TextView()->DisallowChar(c);
    }
    field->SetModificationMessage(new BMessage(what));
    return field;
}

static int
host_year()
{
    const time_t now = time(nullptr);
    struct tm    local;
    localtime_r(&now, &local);
    return local.tm_year + 1900;
}

static int
clamp_int(int value, int min, int max)
{
    return (value < min) ? min : ((value > max) ? max : value);
}

/* --- Machine (qt_settingsmachine.cpp) --------------------------------- */

class MachinePage : public SettingsPage {
public:
    explicit MachinePage(SettingsWindow *window)
        : SettingsPage("Machine")
        , window(window)
        , type(new Choice("type", "Machine type:", MSG_MACHINE_TYPE))
        , machine_choice(new Choice("machine", "Machine:", MSG_MACHINE))
        , configure(new BButton("configure", "Configure", new BMessage(MSG_MACHINE_CFG)))
        , cpu_family(new Choice("cpu", "CPU type:", MSG_CPU_FAMILY))
        , cpu_speed(new Choice("speed", "Frequency:", MSG_CPU_SPEED))
        , fpu(new Choice("fpu", "FPU:", MSG_FPU))
        , wait_states(new Choice("wait", "Wait states:", MSG_SETTINGS_IGNORE))
        , pit(new Choice("pit", "PIT mode:", MSG_SETTINGS_IGNORE))
        , memory(new BTextControl("memory", "Memory:", "", nullptr))
        , memory_unit(new BStringView("unit", "MB"))
        , softfloat(new BCheckBox("softfloat", "Softfloat FPU", new BMessage(MSG_SOFTFLOAT)))
        , softfloat_warning(new BStringView("softfloat_warning", "High performance impact"))
        , override_interpreter(new BCheckBox("override_interpreter",
                                             "Use the 486 interpreter for 286 and 386 processors",
                                             new BMessage(MSG_SETTINGS_IGNORE)))
        , dynarec(new BCheckBox("dynarec", "Dynamic recompiler", new BMessage(MSG_DYNAREC)))
        , accurate_fpu(new BCheckBox("accurate_fpu", "Accurate FPU environment on the dynamic recompiler",
                                     new BMessage(MSG_SETTINGS_IGNORE)))
        , block_link(new BCheckBox("block_link", "Block linking on the dynamic recompiler",
                                   new BMessage(MSG_SETTINGS_IGNORE)))
        , debug_regs(new BCheckBox("debug_regs", "Debug registers on 486 and later processors",
                                   new BMessage(MSG_DEBUG_REGS)))
        , debug_regs_warning(new BStringView("debug_regs_warning", "High performance impact"))
        , fast_forward(new BCheckBox("fast_forward", "Fast forward at startup", new BMessage(MSG_SETTINGS_IGNORE)))
        , larger_frames(new BRadioButton("larger", "Larger frames (less smooth)", new BMessage(MSG_SETTINGS_IGNORE)))
        , smaller_frames(new BRadioButton("smaller", "Smaller frames (smoother)", new BMessage(MSG_SETTINGS_IGNORE)))
        , time_sync_box(new BCheckBox("time_sync", "Synchronize with host clock", new BMessage(MSG_TIME_SYNC)))
        , time_zone_label(new BStringView("time_zone_label", "Time zone:"))
        , date_label(new BStringView("date_label", "Date:"))
        , local_time(new BRadioButton("local", "Local", new BMessage(MSG_SETTINGS_IGNORE)))
        , utc(new BRadioButton("utc", "UTC", new BMessage(MSG_SETTINGS_IGNORE)))
        , host_date(new BRadioButton("host_date", "Match the host", new BMessage(MSG_DATE_MODE)))
        , offset_years(new BRadioButton("offset_years", "Offset the year by:", new BMessage(MSG_DATE_MODE)))
        , fixed_year(new BRadioButton("fixed_year", "Always use the year:", new BMessage(MSG_DATE_MODE)))
        , keep_date(new BRadioButton("keep_date", "Do not synchronize the date", new BMessage(MSG_DATE_MODE)))
        , guest_year(new BStringView("guest_year", ""))
    {
        /* Time synchronization, as Qt sets it up: the offset is clamped so
           the guest year stays in 1980..2099; the configured values are kept
           on save unless the user edits the field. */
        this_year             = host_year();
        offset_min            = 1980 - this_year;
        offset_max            = 2099 - this_year;
        original_years_offset = time_offset_years;
        original_fixed_year   = time_fixed_year;

        int offset_value = time_offset_years;
        int fixed_value  = (time_fixed_year != 0) ? time_fixed_year : clamp_int(this_year, 1980, 2099);
        if ((time_fixed_year != 0) && (time_offset_years == 0)) {
            offset_value          = time_fixed_year - this_year;
            original_years_offset = offset_value;
        } else if ((time_offset_years != 0) && (time_fixed_year == 0)) {
            fixed_value         = this_year + time_offset_years;
            original_fixed_year = fixed_value;
        }
        years_offset_field = number_field("years_offset", clamp_int(offset_value, offset_min, offset_max), true,
                                          MSG_YEARS_OFFSET);
        fixed_year_field   = number_field("fixed_year_value", clamp_int(fixed_value, 1980, 2099), false,
                                          MSG_FIXED_YEAR);

        BuildLayout();

        wait_states->Add("Default", 0);
        for (int i = 0; i < 8; i++) {
            char label[32];
            snprintf(label, sizeof(label), "%i wait state%s", i, (i == 1) ? "" : "s");
            wait_states->Add(label, i + 1);
        }

        pit->Add("Auto", -1);
        pit->Add("Slow", 0);
        pit->Add("Fast", 1);
        pit->Select(pit_mode);

        time_sync_box->SetValue((time_sync & TIME_SYNC_ENABLED) ? B_CONTROL_ON : B_CONTROL_OFF);
        if (time_sync & TIME_SYNC_UTC)
            utc->SetValue(B_CONTROL_ON);
        else
            local_time->SetValue(B_CONTROL_ON);

        if (time_keep_date)
            keep_date->SetValue(B_CONTROL_ON);
        else if (time_fixed_year != 0)
            fixed_year->SetValue(B_CONTROL_ON);
        else if (time_offset_years != 0)
            offset_years->SetValue(B_CONTROL_ON);
        else
            host_date->SetValue(B_CONTROL_ON);

        UpdateTimeControls();
        UpdateGuestYear();

        larger_frames->SetValue(force_10ms ? B_CONTROL_ON : B_CONTROL_OFF);
        smaller_frames->SetValue(force_10ms ? B_CONTROL_OFF : B_CONTROL_ON);
        fast_forward->SetValue(fast_forward_on_start ? B_CONTROL_ON : B_CONTROL_OFF);
        override_interpreter->SetValue(cpu_override_interpreter ? B_CONTROL_ON : B_CONTROL_OFF);
        debug_regs->SetValue(cpu_debug_regs_486 ? B_CONTROL_ON : B_CONTROL_OFF);

        set_shown(softfloat_warning, false);
        set_shown(debug_regs_warning, cpu_debug_regs_486 != 0);

#ifndef USE_DYNAREC
        dynarec->SetEnabled(false);
        dynarec->Hide();
        accurate_fpu->Hide();
        block_link->Hide();
#else
        if (!codegen_has_block_link)
            block_link->Hide();
#endif
    }

    ~MachinePage() override
    {
        delete type;
        delete machine_choice;
        delete cpu_family;
        delete cpu_speed;
        delete fpu;
        delete wait_states;
        delete pit;
    }

    /* Filled after the other pages exist, since it tells them the machine. */
    void Start() override
    {
        /* Machine types that have at least one available machine. The
           table has MACHINE_TYPE_MAX entries, indexed by type. */
        bool type_used[MACHINE_TYPE_MAX] = { false };
        for (int i = 0; i < machine_count(); i++) {
            const int t = machine_get_type(i);
            if (machine_available(i) && (t >= 0) && (t < MACHINE_TYPE_MAX))
                type_used[t] = true;
        }
        for (int i = 0; i < MACHINE_TYPE_MAX; i++) {
            if (type_used[i])
                type->Add(machine_types[i].name, machine_types[i].id);
        }
        type->Select(machine_get_type(machine));
        TypeChanged();
    }

    int SelectedMachine() const { return machine_choice->Value(machine); }

    bool Handle(BMessage *message) override
    {
        switch (message->what) {
            case MSG_MACHINE_TYPE:
                TypeChanged();
                return true;
            case MSG_MACHINE:
                MachineSelected();
                return true;
            case MSG_MACHINE_CFG:
                machine_cfg_changed |= haiku_device_configure(Window(), machine_get_device(SelectedMachine()), 0);
                return true;
            case MSG_CPU_FAMILY:
                CpuFamilyChanged();
                return true;
            case MSG_CPU_SPEED:
                CpuSpeedChanged();
                return true;
            case MSG_FPU:
                FpuChanged();
                return true;
            case MSG_SOFTFLOAT:
                set_shown(softfloat_warning, softfloat->Value() == B_CONTROL_ON);
                return true;
            case MSG_DEBUG_REGS:
                set_shown(debug_regs_warning, debug_regs->Value() == B_CONTROL_ON);
                return true;
            case MSG_DYNAREC:
#ifdef USE_DYNAREC
                accurate_fpu->SetEnabled(dynarec->Value() == B_CONTROL_ON);
                block_link->SetEnabled(dynarec->Value() == B_CONTROL_ON);
#endif
                return true;
            case MSG_TIME_SYNC:
            case MSG_DATE_MODE:
                UpdateTimeControls();
                return true;
            case MSG_YEARS_OFFSET:
                years_offset_manipulated = true;
                UpdateGuestYear();
                return true;
            case MSG_FIXED_YEAR:
                fixed_year_manipulated = true;
                return true;
            default:
                return false;
        }
    }

    bool Changed() override
    {
        bool changed = false;

        changed |= (machine != SelectedMachine());
        changed |= machine_cfg_changed;
        changed |= (cpu_f != &cpu_families[cpu_family->Value(0)]);
        changed |= (cpu != cpu_speed->Value(0));
        changed |= (fpu_type != fpu->Value(0));
        changed |= (cpu_override_interpreter != Checked(override_interpreter));
        changed |= (cpu_debug_regs_486 != Checked(debug_regs));
#ifdef USE_DYNAREC
        changed |= (cpu_use_dynarec != Checked(dynarec));
        changed |= (cpu_dyn_accurate_fpu_env != Checked(accurate_fpu));
        changed |= (cpu_dyn_block_link != Checked(block_link));
#endif
        changed |= (fpu_softfloat != Checked(softfloat));
        changed |= (force_10ms != Checked(larger_frames));
        changed |= (mem_size != MemorySize());
        changed |= (cpu_waitstates != (wait_states->field->IsEnabled() ? wait_states->Value(0) : 0));
        changed |= (pit_mode != pit->Value(-1));

        int sync;
        int offset;
        int fixed;
        int keep;
        TimeSyncSettings(sync, offset, fixed, keep);
        changed |= (time_sync != sync);
        changed |= (time_offset_years != offset);
        changed |= (time_fixed_year != fixed);
        changed |= (time_keep_date != keep);

        /* Qt counts this as a change that needs no hard reset. */
        changed |= (fast_forward_on_start != Checked(fast_forward));

        return changed;
    }

    void Save() override
    {
        fast_forward_on_start = Checked(fast_forward);

        machine                  = SelectedMachine();
        cpu_f                    = const_cast<cpu_family_t *>(&cpu_families[cpu_family->Value(0)]);
        cpu                      = cpu_speed->Value(0);
        fpu_type                 = fpu->Value(0);
        cpu_override_interpreter = Checked(override_interpreter);
        cpu_debug_regs_486       = Checked(debug_regs);
#ifdef USE_DYNAREC
        cpu_use_dynarec          = Checked(dynarec);
        cpu_dyn_accurate_fpu_env = Checked(accurate_fpu);
        cpu_dyn_block_link       = Checked(block_link);
#endif
        fpu_softfloat            = Checked(softfloat);
        force_10ms               = Checked(larger_frames);
        mem_size                 = MemorySize();
        cpu_waitstates           = wait_states->field->IsEnabled() ? wait_states->Value(0) : 0;
        pit_mode                 = pit->Value(-1);

        TimeSyncSettings(time_sync, time_offset_years, time_fixed_year, time_keep_date);
    }

private:
    static int Checked(const BControl *control) { return (control->Value() == B_CONTROL_ON) ? 1 : 0; }

    static void AddTab(BTabView *tabs, BView *view, const char *label)
    {
        BTab *tab = new BTab();
        tabs->AddTab(view, tab);
        tab->SetLabel(label);
    }

    void BuildLayout()
    {
        BTabView *tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);

        /* Machine tab: machine, memory and the time synchronization group.
           The radio buttons of each group need a parent of their own to
           exclude each other. */
        BGroupView *zone = new BGroupView(B_HORIZONTAL);
        BLayoutBuilder::Group<>(zone)
            .Add(local_time)
            .Add(utc)
            .AddGlue();

        BGridView *date = new BGridView(B_USE_SMALL_SPACING, B_USE_SMALL_SPACING);
        BLayoutBuilder::Grid<>(date)
            .Add(host_date, 0, 0, 3)
            .Add(offset_years, 0, 1)
            .Add(years_offset_field, 1, 1)
            .Add(guest_year, 2, 1)
            .Add(fixed_year, 0, 2)
            .Add(fixed_year_field, 1, 2)
            .Add(keep_date, 0, 3, 3)
            .SetColumnWeight(2, 1.0f);

        BBox *time_box = new BBox("time_box");
        time_box->SetLabel(time_sync_box);
        BGridView *time_box_view = new BGridView(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
        BLayoutBuilder::Grid<>(time_box_view)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(time_zone_label, 0, 0)
            .Add(zone, 1, 0)
            .Add(date_label, 0, 1)
            .Add(date, 1, 1)
            .SetColumnWeight(1, 1.0f);
        /* A child view, so the box keeps room for its label. */
        time_box->AddChild(time_box_view);
        time_zone_label->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT, B_ALIGN_TOP));
        date_label->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT, B_ALIGN_TOP));

        BGroupView *machine_tab = new BGroupView(B_VERTICAL);
        BLayoutBuilder::Group<>(machine_tab, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
                .AddMenuField(type->field, 0, 0, B_ALIGN_LEFT, 1, 2)
                .AddMenuField(machine_choice->field, 0, 1)
                .Add(configure, 2, 1)
                .AddTextControl(memory, 0, 2)
                .Add(memory_unit, 2, 2)
                .SetColumnWeight(1, 1.0f)
            .End()
            .Add(time_box)
            .AddGlue();
        AddTab(tabs, machine_tab, "Machine");

        /* Processor tab. */
        BGroupView *cpu_tab = new BGroupView(B_VERTICAL);
        BLayoutBuilder::Group<>(cpu_tab, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
                .AddMenuField(cpu_family->field, 0, 0)
                .AddMenuField(cpu_speed->field, 0, 1)
                .AddMenuField(fpu->field, 0, 2)
                .AddMenuField(wait_states->field, 0, 3)
                .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING, 1, 4)
                    .Add(softfloat)
                    .Add(softfloat_warning)
                    .AddGlue()
                .End()
                .SetColumnWeight(1, 1.0f)
            .End()
            .AddGlue();
        AddTab(tabs, cpu_tab, "Processor");

        /* Performance tab. */
        BBox *frame_box = new BBox("frame_box");
        frame_box->SetLabel("CPU frame size");
        BGroupView *frame_box_view = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
        BLayoutBuilder::Group<>(frame_box_view)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(larger_frames)
            .Add(smaller_frames);
        /* A child view, so the box keeps room for its label. */
        frame_box->AddChild(frame_box_view);

        BGroupView *perf_tab = new BGroupView(B_VERTICAL);
        BLayoutBuilder::Group<>(perf_tab, B_VERTICAL, B_USE_SMALL_SPACING)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
                .AddMenuField(pit->field, 0, 0)
                .AddGlue(2, 0)
            .End()
            .Add(override_interpreter)
            .Add(dynarec)
            .Add(accurate_fpu)
            .Add(block_link)
            .AddGroup(B_HORIZONTAL)
                .Add(debug_regs)
                .Add(debug_regs_warning)
                .AddGlue()
            .End()
            .Add(fast_forward)
            .Add(frame_box)
            .AddGlue();
        AddTab(tabs, perf_tab, "Performance");

        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(tabs);
    }

    void TypeChanged()
    {
        const int t = type->Value(0);

        machine_choice->Clear();
        for (int i = 0; i < machine_count(); i++) {
            if ((machine_get_type(i) == t) && machine_available(i))
                machine_choice->Add(machines[i].name, i);
        }
        machine_choice->Select(machine);
        MachineSelected();
    }

    void MachineSelected()
    {
        const int m = SelectedMachine();

        configure->SetEnabled(haiku_device_has_config(machine_get_device(m)));

        cpu_family->Clear();
        for (int i = 0; cpu_families[i].package != 0; i++) {
            if (cpu_family_is_eligible(&cpu_families[i], m)) {
                char label[256];
                snprintf(label, sizeof(label), "%s %s", cpu_families[i].manufacturer, cpu_families[i].name);
                cpu_family->Add(label, i);
            }
        }
        cpu_family->Select(static_cast<int32>(cpu_f - cpu_families));
        CpuFamilyChanged();

        /* Memory, in KB or MB depending on the machine's step. */
        const int gran = machine_get_ram_granularity(m);
        divisor        = (gran < 1024) ? 1 : 1024;
        memory_unit->SetText((divisor == 1) ? "KB" : "MB");

        uint32_t size = mem_size;
        if (size < (uint32_t) machine_get_min_ram(m))
            size = machine_get_min_ram(m);
        if (size > (uint32_t) machine_get_max_ram(m))
            size = machine_get_max_ram(m);
        char text[32];
        snprintf(text, sizeof(text), "%u", (unsigned) (size / divisor));
        memory->SetText(text);
        memory->SetEnabled(machine_get_min_ram(m) != machine_get_max_ram(m));

        char tip[96];
        snprintf(tip, sizeof(tip), "%i to %i %s, in steps of %i", machine_get_min_ram(m) / divisor,
                 machine_get_max_ram(m) / divisor, (divisor == 1) ? "KB" : "MB", gran / divisor);
        memory->SetToolTip(tip);

        settings_set_current_machine(m);
        window->MachineChanged(m);
    }

    void CpuFamilyChanged()
    {
        const int           m      = SelectedMachine();
        const cpu_family_t *family = &cpu_families[cpu_family->Value(0)];

        cpu_speed->Clear();
        for (int i = 0; family->cpus[i].cpu_type != 0; i++) {
            if (cpu_is_eligible(family, i, m))
                cpu_speed->Add(family->cpus[i].name, i);
        }
        cpu_speed->Select(cpu);
        CpuSpeedChanged();
    }

    void CpuSpeedChanged()
    {
        const int           m      = SelectedMachine();
        const cpu_family_t *family = &cpu_families[cpu_family->Value(0)];
        const int           id     = cpu_speed->Value(0);
        const uint32_t      type   = family->cpus[id].cpu_type;

        /* Wait states only exist on 286-class CPUs (not on the Tandy VIS). */
        const bool waits = (type >= CPU_286) && (type < CPU_386DX) && (machines[m].init != machine_at_tandy_vis_init);
        wait_states->Select(waits ? cpu_waitstates : 0);
        wait_states->field->SetEnabled(waits);

#ifdef USE_DYNAREC
        const uint8_t flags = family->cpus[id].cpu_flags;
        if (!(flags & CPU_SUPPORTS_DYNAREC)) {
            dynarec->SetValue(B_CONTROL_OFF);
            dynarec->SetEnabled(false);
            accurate_fpu->SetEnabled(false);
            block_link->SetEnabled(false);
        } else if ((flags & CPU_REQUIRES_DYNAREC) && !cpu_override) {
            dynarec->SetValue(B_CONTROL_ON);
            dynarec->SetEnabled(false);
            accurate_fpu->SetEnabled(true);
            block_link->SetEnabled(true);
        } else {
            dynarec->SetValue(cpu_use_dynarec ? B_CONTROL_ON : B_CONTROL_OFF);
            dynarec->SetEnabled(true);
            accurate_fpu->SetEnabled(cpu_use_dynarec != 0);
            block_link->SetEnabled(cpu_use_dynarec != 0);
        }

        accurate_fpu->SetValue(cpu_dyn_accurate_fpu_env ? B_CONTROL_ON : B_CONTROL_OFF);
        block_link->SetValue(cpu_dyn_block_link ? B_CONTROL_ON : B_CONTROL_OFF);
#endif

        /* The 486 interpreter only replaces the 286/386 ones of Intel-style
           CPUs. */
        override_interpreter->SetEnabled(!((type < CPU_286) || (type == CPU_IBM486SLC) || (type == CPU_IBM486BL)
                                           || !strcmp(family->manufacturer, "Cyrix")
                                           || !strcmp(family->manufacturer, "ST") || (type > CPU_486DLC)));

        /* A board without an FPU socket only offers the CPU's own FPU. */
        const bool no_socket = machine_has_flags(m, MACHINE_NO_FPU_SOCKET);
        fpu->Clear();
        for (int i = 0; fpu_get_name_from_index(family, id, i) != nullptr; i++) {
            const int t = fpu_get_type_from_index(family, id, i);
            if (no_socket && (t != FPU_NONE) && (t != FPU_INTERNAL))
                continue;
            fpu->Add(fpu_get_name_from_index(family, id, i), t);
        }
        fpu->Select(fpu_type);
        FpuChanged();
    }

    void FpuChanged()
    {
        const int m = SelectedMachine();

        if (fpu->Value(FPU_NONE) == FPU_NONE) {
            softfloat->SetValue(B_CONTROL_OFF);
            softfloat->SetEnabled(false);
        } else {
            const bool only = machine_has_flags(m, MACHINE_SOFTFLOAT_ONLY);
            softfloat->SetValue((only || fpu_softfloat) ? B_CONTROL_ON : B_CONTROL_OFF);
            softfloat->SetEnabled(!only);
        }
        set_shown(softfloat_warning, softfloat->Value() == B_CONTROL_ON);
    }

    /* Qt: updateTimeControlsEnabled(), plus what the checkable group box
       does to its children. */
    void UpdateTimeControls()
    {
        const bool sync = (time_sync_box->Value() == B_CONTROL_ON);

        for (BControl *control : { (BControl *) local_time, (BControl *) utc, (BControl *) host_date,
                                   (BControl *) offset_years, (BControl *) fixed_year, (BControl *) keep_date })
            control->SetEnabled(sync);
        set_label_enabled(time_zone_label, sync);
        set_label_enabled(date_label, sync);

        const bool offset_mode = sync && (offset_years->Value() == B_CONTROL_ON);
        const bool fixed_mode  = sync && (fixed_year->Value() == B_CONTROL_ON);
        years_offset_field->SetEnabled(offset_mode);
        set_label_enabled(guest_year, offset_mode);
        fixed_year_field->SetEnabled(fixed_mode);
    }

    /* The values the fields stand for, clamped like Qt's spin boxes. */
    int YearsOffsetValue() const
    {
        return clamp_int((int) strtol(years_offset_field->Text(), nullptr, 10), offset_min, offset_max);
    }

    int FixedYearValue() const
    {
        return clamp_int((int) strtol(fixed_year_field->Text(), nullptr, 10), 1980, 2099);
    }

    void UpdateGuestYear()
    {
        char text[64];
        snprintf(text, sizeof(text), "(currently %i)", this_year + YearsOffsetValue());
        guest_year->SetText(text);
    }

    /* Qt: timeSyncSettings(). At most one date mode applies; the others
       are cleared. */
    void TimeSyncSettings(int &sync, int &offset, int &fixed, int &keep) const
    {
        sync = TIME_SYNC_DISABLED;
        if (time_sync_box->Value() == B_CONTROL_ON)
            sync = TIME_SYNC_ENABLED | ((utc->Value() == B_CONTROL_ON) ? TIME_SYNC_UTC : 0);

        offset = 0;
        fixed  = 0;
        keep   = (keep_date->Value() == B_CONTROL_ON) ? 1 : 0;
        if (fixed_year->Value() == B_CONTROL_ON)
            fixed = fixed_year_manipulated ? FixedYearValue()
                                           : ((original_fixed_year != 0) ? original_fixed_year : FixedYearValue());
        else if (offset_years->Value() == B_CONTROL_ON)
            offset = years_offset_manipulated ? YearsOffsetValue() : original_years_offset;
    }

    /* The typed size, clamped to the machine's range and rounded down to
       its step, as Qt does. */
    uint32_t MemorySize() const
    {
        const int m    = SelectedMachine();
        const int gran = machine_get_ram_granularity(m);
        const int min  = machine_get_min_ram(m);
        const int max  = machine_get_max_ram(m);

        int64_t size = strtoll(memory->Text(), nullptr, 10) * divisor;
        if (size > min)
            size = min + ((size - min) / gran) * gran;
        if (size < min)
            size = min;
        else if (size > max)
            size = max;
        return static_cast<uint32_t>(size);
    }

    SettingsWindow *window;
    Choice         *type;
    Choice         *machine_choice;
    BButton        *configure;
    Choice         *cpu_family;
    Choice         *cpu_speed;
    Choice         *fpu;
    Choice         *wait_states;
    Choice         *pit;
    BTextControl   *memory;
    BStringView    *memory_unit;
    BCheckBox      *softfloat;
    BStringView    *softfloat_warning;
    BCheckBox      *override_interpreter;
    BCheckBox      *dynarec;
    BCheckBox      *accurate_fpu;
    BCheckBox      *block_link;
    BCheckBox      *debug_regs;
    BStringView    *debug_regs_warning;
    BCheckBox      *fast_forward;
    BRadioButton   *larger_frames;
    BRadioButton   *smaller_frames;
    BCheckBox      *time_sync_box;
    BStringView    *time_zone_label;
    BStringView    *date_label;
    BRadioButton   *local_time;
    BRadioButton   *utc;
    BRadioButton   *host_date;
    BRadioButton   *offset_years;
    BRadioButton   *fixed_year;
    BRadioButton   *keep_date;
    BStringView    *guest_year;
    BTextControl   *years_offset_field = nullptr;
    BTextControl   *fixed_year_field   = nullptr;
    int             divisor            = 1024;
    bool            machine_cfg_changed = false;

    int  this_year                = 0;
    int  offset_min               = 0;
    int  offset_max               = 0;
    int  original_years_offset    = 0;
    int  original_fixed_year      = 0;
    bool years_offset_manipulated = false;
    bool fixed_year_manipulated   = false;
};


SettingsPage *
create_machine_page(SettingsWindow *window)
{
    return new MachinePage(window);
}
