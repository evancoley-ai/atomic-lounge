// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk plugin: the user preset folder (%APPDATA%\Vintage Chonk\presets\*.mfp; presets in the
// pre-rename folder are copied in, never overwriting: PresetStore::oldFolder), the desktop
// counterpart of the tablet's app-private presets folder. Same contract as MainActivity.Bridge.
#pragma once
#include <juce_core/juce_core.h>

namespace mfm1 {

struct PresetStore
{
    static juce::File folder();
    static juce::File oldFolder();                              // pre-rename folder (read for migration)
    static int migrate (const juce::File& from, const juce::File& to);   // copies missing *.mfp; count
    static juce::String safeName (const juce::String& name);   // as the tablet: [A-Za-z0-9 _.-], trimmed
    static juce::var list();                                    // [{name, b64}]
    static juce::String save (const juce::String& name, const juce::String& b64);   // "ok" | error
    static juce::String remove (const juce::String& name);                          // "ok" | "not found"
};

}
