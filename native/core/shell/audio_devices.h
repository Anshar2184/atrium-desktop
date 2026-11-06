// The sound output page of the control centre, after the Windows taskbar's:
// the playback devices (the default one chosen there) and the volume of each
// app playing sound. Core Audio for both; switching the default device goes
// through IPolicyConfig, the interface Windows' own Sound settings use (there
// is no public one; EarTrumpet and SoundSwitch use the same). Every call
// blocks briefly: the lists are read on a worker thread.
#pragma once

#include "shell/task_model.h"

#include <string>
#include <vector>

namespace atrium {

struct AudioOutput {
    std::wstring id;        // the endpoint's id
    std::wstring name;      // "扬声器"
    std::wstring adapter;   // "Realtek(R) Audio"
    enum Kind { Speakers, Headphones, Display, Other } kind = Other;
    bool isDefault = false;
};

struct AudioApp {
    std::wstring key;  // the program's path in lower case; "#system" for system sounds
    std::wstring name;
    std::wstring path;
    int volume = 100;  // 0..100
    bool muted = false;
    bool active = false; // playing now
    IconImage icon;    // filled by the reader
};

// Active playback devices, the default one marked.
std::vector<AudioOutput> ListAudioOutputs();
// Makes `id` the default playback device for every role, as Windows' panel does.
bool SetDefaultOutput(const std::wstring& id);
// The sessions on the default device, one entry per program; system sounds first.
// iconPx == 0 reads volume/state only; the caller keeps its existing icons.
std::vector<AudioApp> ListAudioApps(int iconPx);
void SetAppVolume(const std::wstring& key, int percent);
void SetAppMute(const std::wstring& key, bool mute);

} // namespace atrium
