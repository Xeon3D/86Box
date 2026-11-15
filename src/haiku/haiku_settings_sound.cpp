/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Native Haiku frontend: the Sound Settings page (qt_settingssound.cpp).
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <atomic> /* before the core headers: plat.h and video.h include it under C++ */
#include <Button.h>
#include <CheckBox.h>
#include <GridView.h>
#include <LayoutBuilder.h>
#include <TabView.h>

#include <cstdio>
#include <cstring>
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
#include <86box/machine.h>
#include <86box/sound.h>
#include <86box/midi.h>
#include <86box/snd_mpu401.h>
#include <86box/snd_opl.h>
}

/* The slot is added to the per-slot codes ('ssc0' + i ...). */
enum : uint32 {
    MSG_SS_CARD         = 'ssc0',
    MSG_SS_CARD_CFG     = 'ssC0',
    MSG_SS_MIDI_OUT     = 'ssmo',
    MSG_SS_MIDI_OUT_CFG = 'ssmO',
    MSG_SS_MIDI_IN      = 'ssmi',
    MSG_SS_MIDI_IN_CFG  = 'ssmI',
    MSG_SS_MPU401       = 'ssmp',
    MSG_SS_MPU401_CFG   = 'ssmP',
    MSG_SS_OUTPUT       = 'ssao',
    MSG_SS_INPUT_ENABLE = 'ssie',
};

/* The host's devices as a double-NUL-terminated list (openal.c). */
static void
add_device_list(std::vector<std::string> &names, const char *list)
{
    if (list == nullptr)
        return;
    for (const char *dev = list; *dev != '\0'; dev += strlen(dev) + 1)
        names.push_back(dev);
}

/* --- Sound (qt_settingssound.cpp) ------------------------------------- */

class SoundPage : public SettingsPage {
public:
    SoundPage()
        : SettingsPage("Sound")
        , machine_id(0)
        , fm(new Choice("fm", "FM synth driver:", MSG_SETTINGS_IGNORE))
        , float32(new BCheckBox("float32", "Use FLOAT32 sound", nullptr))
        , output(new Choice("output", "Audio output device:", MSG_SS_OUTPUT))
        , sample_rate(new Choice("sample_rate", "Sample rate:", MSG_SETTINGS_IGNORE))
        , input_enable(new BCheckBox("input_enable", "Enable audio input (mic/line-in)",
                                     new BMessage(MSG_SS_INPUT_ENABLE)))
        , input(new Choice("input", "Audio input device:", MSG_SETTINGS_IGNORE))
        , input_rate(new Choice("input_rate", "Audio input sample rate:", MSG_SETTINGS_IGNORE))
        , midi_out(new Choice("midi_out", "MIDI Out Device:", MSG_SS_MIDI_OUT))
        , midi_out_button(new BButton("midi_out_cfg", "Configure", new BMessage(MSG_SS_MIDI_OUT_CFG)))
        , midi_in(new Choice("midi_in", "MIDI In Device:", MSG_SS_MIDI_IN))
        , midi_in_button(new BButton("midi_in_cfg", "Configure", new BMessage(MSG_SS_MIDI_IN_CFG)))
        , mpu401(new BCheckBox("mpu401", "Standalone MPU-401", new BMessage(MSG_SS_MPU401)))
        , mpu401_button(new BButton("mpu401_cfg", "Configure", new BMessage(MSG_SS_MPU401_CFG)))
        , mpu401_enabled(mpu401_standalone_enable > 0)
        , mpu401_cfg_changed(false)
        , midi_out_cfg_changed(false)
        , midi_in_cfg_changed(false)
    {
        /* General tab. */
        BGridView *general = new BGridView("General", B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
        BLayoutBuilder::Grid<> grid(general);
        grid.SetInsets(B_USE_DEFAULT_SPACING);
        for (int i = 0; i < SOUND_CARD_MAX; i++) {
            char name[32];
            char label[32];
            snprintf(name, sizeof(name), "card%i", i);
            snprintf(label, sizeof(label), "Sound card %i:", i + 1);
            cards[i] = new Choice(name, label, MSG_SS_CARD + i);
            snprintf(name, sizeof(name), "card%i_cfg", i);
            card_button[i]      = new BButton(name, "Configure", new BMessage(MSG_SS_CARD_CFG + i));
            card_cfg_changed[i] = false;
            grid.AddMenuField(cards[i]->field, 0, i).Add(card_button[i], 2, i);
        }
        const int r = SOUND_CARD_MAX;
        grid.AddMenuField(fm->field, 0, r, B_ALIGN_HORIZONTAL_UNSET, 1, 2)
            .Add(float32, 0, r + 1, 2)
            .AddMenuField(output->field, 0, r + 2, B_ALIGN_HORIZONTAL_UNSET, 1, 2)
            .AddMenuField(sample_rate->field, 0, r + 3, B_ALIGN_HORIZONTAL_UNSET, 1, 2)
            .Add(input_enable, 0, r + 4, 2)
            .AddMenuField(input->field, 0, r + 5, B_ALIGN_HORIZONTAL_UNSET, 1, 2)
            .AddMenuField(input_rate->field, 0, r + 6, B_ALIGN_HORIZONTAL_UNSET, 1, 2)
            .AddGlue(0, r + 7, 3);
        general->GridLayout()->SetColumnWeight(1, 1.0f);

        /* MIDI tab. */
        BGridView *midi = new BGridView("MIDI", B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
        BLayoutBuilder::Grid<>(midi)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddMenuField(midi_out->field, 0, 0)
            .Add(midi_out_button, 2, 0)
            .AddMenuField(midi_in->field, 0, 1)
            .Add(midi_in_button, 2, 1)
            .Add(mpu401, 0, 2, 2)
            .Add(mpu401_button, 2, 2)
            .AddGlue(0, 3, 3);
        midi->GridLayout()->SetColumnWeight(1, 1.0f);

        BTabView *tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);
        tabs->AddTab(general);
        tabs->AddTab(midi);

        BLayoutBuilder::Group<>(this, B_VERTICAL)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .Add(tabs);

        float32->SetValue((sound_is_float > 0) ? B_CONTROL_ON : B_CONTROL_OFF);

        /* The FM driver does not change with the machine. */
        fm->Add("Nuked (more accurate)", FM_DRV_NUKED);
        fm->Add("YMFM (faster)", FM_DRV_YMFM);
        fm->Select((fm_driver == FM_DRV_YMFM) ? FM_DRV_YMFM : FM_DRV_NUKED);

        /* The host's audio devices do not change with the machine either:
           they are asked for once, here. Item values index the name lists;
           "" is the system default. */
        out_devices.push_back("");
        add_device_list(out_devices, sound_get_output_devices());
        int selected = 0;
        for (size_t i = 0; i < out_devices.size(); i++) {
            output->Add((i == 0) ? "System Default" : out_devices[i].c_str(), (int32) i);
            if (out_devices[i] == sound_output_device)
                selected = (int) i;
        }
        output->Select(selected);
        output->SetEnabled(true);
        FillSampleRates();

        in_devices.push_back("");
        add_device_list(in_devices, sound_get_input_devices());
        selected = -1;
        for (size_t i = 0; i < in_devices.size(); i++) {
            input->Add((i == 0) ? "System Default" : in_devices[i].c_str(), (int32) i);
            if (in_devices[i] == sound_input_dev_name)
                selected = (int) i;
        }
        if (selected < 0) {
            const std::string missing = std::string(sound_input_dev_name) + " (not detected)";
            selected                  = (int) in_devices.size();
            in_devices.push_back(sound_input_dev_name);
            input->Add(missing.c_str(), selected);
        }
        input->Select(selected);

        for (const int rate : { FREQ_44100, FREQ_48000 }) {
            const std::string text = std::to_string(rate) + " Hz";
            input_rate->Add(text.c_str(), rate);
        }
        input_rate->Select(sb_input_rate);

        input_enable->SetValue((sound_input_enabled > 0) ? B_CONTROL_ON : B_CONTROL_OFF);

        /* The MIDI devices do not depend on the machine. */
        for (int c = 0;; c++) {
            const device_t   *dev  = midi_out_device_getdevice(c);
            const std::string name = settings_device_name(dev, midi_out_device_get_internal_name(c), 0);
            if (name.empty())
                break;
            if (midi_out_device_available(c))
                midi_out->Add(name.c_str(), c);
        }
        midi_out->Select(midi_output_device_current);

        for (int c = 0;; c++) {
            const device_t   *dev  = midi_in_device_getdevice(c);
            const std::string name = settings_device_name(dev, midi_in_device_get_internal_name(c), 0);
            if (name.empty())
                break;
            if (midi_in_device_available(c))
                midi_in->Add(name.c_str(), c);
        }
        midi_in->Select(midi_input_device_current);
    }

    ~SoundPage() override
    {
        for (int i = 0; i < SOUND_CARD_MAX; i++)
            delete cards[i];
        delete fm;
        delete output;
        delete sample_rate;
        delete input;
        delete input_rate;
        delete midi_out;
        delete midi_in;
    }

    void MachineChanged(int m) override
    {
        machine_id = m;

        const bool has_sound = machine_has_flags(m, MACHINE_SOUND);
        int        current[SOUND_CARD_MAX];

        for (int i = 0; i < SOUND_CARD_MAX; i++) {
            current[i] = cards[i]->Count() ? cards[i]->Value(sound_card_current[i]) : sound_card_current[i];
            cards[i]->Clear();
        }

        for (int c = 0;; c++) {
            const device_t   *dev  = sound_card_getdevice(c);
            const std::string name = settings_device_name(dev, sound_card_get_internal_name(c), 1);
            if (name.empty())
                break;
            if (!sound_card_available(c) || !device_is_valid(dev, m))
                continue;

            for (int i = 0; i < SOUND_CARD_MAX; i++) {
                /* "Internal" only in the first slot, on machines with
                   on-board sound. */
                if ((c == SOUND_INTERNAL) && ((i != 0) || !has_sound))
                    continue;
                std::string label = name;
                const device_t *onboard = machine_get_snd_device(m);
                if ((c == SOUND_INTERNAL) && (onboard != nullptr))
                    label += " (" + settings_device_name(onboard, onboard->internal_name, 0) + ")";
                cards[i]->Add(label.c_str(), c);
            }
        }

        for (int i = 0; i < SOUND_CARD_MAX; i++)
            cards[i]->Select(current[i]);

        /* Standalone MPU-401: kept checked only while it can be used. */
        SetCheck(mpu401, mpu401_enabled && AllowMpu401());
        mpu401_enabled = (mpu401->Value() == B_CONTROL_ON);

        UpdateCardButtons();
        UpdateMidiControls();
        UpdateSoundInputEnabled();
    }

    bool Handle(BMessage *message) override
    {
        const uint32 what = message->what;

        switch (what) {
            case MSG_SS_OUTPUT:
                FillSampleRates();
                return true;
            case MSG_SS_INPUT_ENABLE:
                UpdateSoundInputEnabled();
                return true;
            case MSG_SS_MIDI_OUT:
            case MSG_SS_MIDI_IN:
                UpdateMidiControls();
                return true;
            case MSG_SS_MIDI_OUT_CFG:
                midi_out_cfg_changed |= haiku_device_configure(Window(), midi_out_device_getdevice(midi_out->Value(0)));
                return true;
            case MSG_SS_MIDI_IN_CFG:
                midi_in_cfg_changed |= haiku_device_configure(Window(), midi_in_device_getdevice(midi_in->Value(0)));
                return true;
            case MSG_SS_MPU401:
                mpu401_enabled = (mpu401->Value() == B_CONTROL_ON);
                mpu401_button->SetEnabled(mpu401_enabled);
                return true;
            case MSG_SS_MPU401_CFG:
                if (machine_has_bus(machine_id, MACHINE_BUS_MCA) > 0)
                    mpu401_cfg_changed |= haiku_device_configure(Window(), &mpu401_mca_device);
                else
                    mpu401_cfg_changed |= haiku_device_configure(Window(), &mpu401_device);
                return true;
            default:
                break;
        }

        if ((what >= MSG_SS_CARD) && (what < MSG_SS_CARD + SOUND_CARD_MAX)) {
            UpdateCardButtons();
            UpdateSoundInputEnabled();
            return true;
        }
        if ((what >= MSG_SS_CARD_CFG) && (what < MSG_SS_CARD_CFG + SOUND_CARD_MAX)) {
            const int i    = what - MSG_SS_CARD_CFG;
            const int card = cards[i]->Value(0);
            if ((i == 0) && (card == SOUND_INTERNAL))
                card_cfg_changed[0] |= haiku_device_configure(Window(), machine_get_snd_device(machine_id));
            else
                card_cfg_changed[i] |= haiku_device_configure(Window(), sound_card_getdevice(card), i + 1);
            return true;
        }
        return false;
    }

    bool Changed() override
    {
        bool changed = false;

        for (int i = 0; i < SOUND_CARD_MAX; i++)
            changed |= (sound_card_current[i] != cards[i]->Value(sound_card_current[i])) || card_cfg_changed[i];

        changed |= (fm_driver != fm->Value(fm_driver));
        changed |= (mpu401_standalone_enable != Checked(mpu401)) || mpu401_cfg_changed;
        changed |= (sound_is_float != Checked(float32));
        changed |= (sound_sample_rate != sample_rate->Value(sound_sample_rate));

        changed |= (OutputDevice() != sound_output_device);
        changed |= (sound_input_enabled != Checked(input_enable));
        changed |= (InputDevice() != sound_input_dev_name);
        changed |= (sb_input_rate != input_rate->Value(sb_input_rate));

        changed |= (midi_output_device_current != midi_out->Value(midi_output_device_current)) || midi_out_cfg_changed;
        changed |= (midi_input_device_current != midi_in->Value(midi_input_device_current)) || midi_in_cfg_changed;

        return changed;
    }

    /* Qt's save(0): the machine is hard-reset afterwards, which reopens the
       host output (sound_reset()). */
    void Save() override
    {
        midi_output_device_current = midi_out->Value(midi_output_device_current);
        midi_input_device_current  = midi_in->Value(midi_input_device_current);

        snprintf(sound_output_device, sizeof(sound_output_device), "%s", OutputDevice().c_str());
        sound_input_enabled = Checked(input_enable);
        snprintf(sound_input_dev_name, sizeof(sound_input_dev_name), "%s", InputDevice().c_str());

        sb_input_rate = input_rate->Value(sb_input_rate);
        if ((sb_input_rate != FREQ_44100) && (sb_input_rate != FREQ_48000))
            sb_input_rate = FREQ_44100;

        for (int i = 0; i < SOUND_CARD_MAX; i++)
            sound_card_current[i] = cards[i]->Value(sound_card_current[i]);

        fm_driver                = fm->Value(fm_driver);
        mpu401_standalone_enable = Checked(mpu401);
        sound_is_float           = Checked(float32);
        sound_sample_rate        = sample_rate->Value(sound_sample_rate);
    }

private:
    static void SetCheck(BCheckBox *box, bool on) { box->SetValue(on ? B_CONTROL_ON : B_CONTROL_OFF); }
    static int  Checked(const BCheckBox *box) { return (box->Value() == B_CONTROL_ON) ? 1 : 0; }

    std::string OutputDevice() const
    {
        const int32 i = output->Value(0);
        return ((i >= 0) && (i < (int32) out_devices.size())) ? out_devices[i] : std::string();
    }

    std::string InputDevice() const
    {
        const int32 i = input->Value(0);
        return ((i >= 0) && (i < (int32) in_devices.size())) ? in_devices[i] : std::string();
    }

    /* on_comboBoxAudioOutputDevice_currentIndexChanged(): the rates the
       device supports; the current choice (else the saved rate) is kept,
       marked as resampled by the host when the device lacks it. */
    void FillSampleRates()
    {
        int       rates[16] = { 0 };
        const int count     = sound_get_device_supported_rates(OutputDevice().c_str(), rates, 16);

        int target = sample_rate->Value(0);
        if (target == 0)
            target = sound_sample_rate;

        sample_rate->Clear();
        bool offered = false;
        for (int i = 0; i < count; i++) {
            const std::string text = std::to_string(rates[i]) + " Hz";
            sample_rate->Add(text.c_str(), rates[i]);
            offered |= (rates[i] == target);
        }
        if (!offered && (target > 0)) {
            const std::string text = std::to_string(target) + " Hz (host will resample)";
            sample_rate->Add(text.c_str(), target);
        }
        sample_rate->Select(target);
        sample_rate->SetEnabled(true);
    }

    /* allowMpu401(): a MIDI out device exists, and MIDI out or in is set. */
    bool AllowMpu401() const
    {
        const char *out = midi_out_device_get_internal_name(midi_out->Value(0));
        const char *in  = midi_in_device_get_internal_name(midi_in->Value(0));

        if ((out == nullptr) || (out[0] == '\0'))
            return false;
        if (!strcmp(out, "none") && ((in == nullptr) || !strcmp(in, "none")))
            return false;
        return true;
    }

    /* on_comboBoxSoundCard*_currentIndexChanged(). */
    void UpdateCardButtons()
    {
        for (int i = 0; i < SOUND_CARD_MAX; i++) {
            const int card = cards[i]->Value(0);
            bool      on;
            if ((i == 0) && (card == SOUND_INTERNAL))
                on = machine_has_flags(machine_id, MACHINE_SOUND)
                    && haiku_device_has_config(machine_get_snd_device(machine_id));
            else
                on = (cards[i]->Count() > 0) && sound_card_has_config(card);
            card_button[i]->SetEnabled(on);
        }
    }

    /* on_comboBoxMidiOut/In_currentIndexChanged(). */
    void UpdateMidiControls()
    {
        const bool allow = AllowMpu401();
        const bool bus   = (machine_has_bus(machine_id, MACHINE_BUS_ISA) > 0)
            || (machine_has_bus(machine_id, MACHINE_BUS_MCA) > 0);

        midi_out_button->SetEnabled((midi_out->Count() > 0) && midi_out_device_has_config(midi_out->Value(0)));
        midi_in_button->SetEnabled((midi_in->Count() > 0) && midi_in_device_has_config(midi_in->Value(0)));
        mpu401->SetEnabled(allow && bus);
        mpu401_button->SetEnabled(allow && (mpu401->Value() == B_CONTROL_ON));
    }

    /* updateSoundInputEnabled(): input is offered when a selected card (or
       the on-board sound) can record. */
    void UpdateSoundInputEnabled()
    {
        bool has_input = false;

        for (int i = 0; i < SOUND_CARD_MAX; i++) {
            if (cards[i]->Count() == 0)
                continue;
            const int card = cards[i]->Value(0);
            if (card == SOUND_INTERNAL) {
                const device_t *dev = machine_get_snd_device(machine_id);
                if (machine_has_flags(machine_id, MACHINE_SOUND) && (dev != nullptr) && (dev->flags & DEVICE_AUDIO_IN))
                    has_input = true;
            } else if (sound_card_has_input(card))
                has_input = true;
        }

        input_enable->SetEnabled(has_input);
        const bool on = has_input && (input_enable->Value() == B_CONTROL_ON);
        input->SetEnabled(on);
        input_rate->SetEnabled(on);
    }

    int       machine_id;
    Choice   *cards[SOUND_CARD_MAX];
    BButton  *card_button[SOUND_CARD_MAX];
    bool      card_cfg_changed[SOUND_CARD_MAX];
    Choice   *fm;
    BCheckBox *float32;
    Choice   *output;
    Choice   *sample_rate;
    BCheckBox *input_enable;
    Choice   *input;
    Choice   *input_rate;
    Choice   *midi_out;
    BButton  *midi_out_button;
    Choice   *midi_in;
    BButton  *midi_in_button;
    BCheckBox *mpu401;
    BButton  *mpu401_button;
    bool      mpu401_enabled;
    bool      mpu401_cfg_changed;
    bool      midi_out_cfg_changed;
    bool      midi_in_cfg_changed;

    std::vector<std::string> out_devices;
    std::vector<std::string> in_devices;
};


SettingsPage *
create_sound_page()
{
    return new SoundPage();
}
