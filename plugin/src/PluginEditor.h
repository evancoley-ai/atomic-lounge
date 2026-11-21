// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk plugin: the editor = a web panel in JUCE's WebBrowserComponent on WebView2.
//   CHONK: the tablet's panel (panel, unchanged files served from binary data) with the desktop
//          bridge (web/desktop_shim.js, which also adds the CHONK | ORBIT switch).
//   ORBIT: web/orbit/orbit.html (the "Streamline Lounge" panel), bridged through the same 'mfm1' events.
// Switching the mode navigates between the two pages.
#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include <map>
#include "PluginProcessor.h"

class VintageChonkEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VintageChonkEditor (VintageChonkProcessor&);
    ~VintageChonkEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void onPanelEvent (const juce::var& v);
    void onOrbitEvent (const juce::String& op, const juce::Array<juce::var>& a);
    std::optional<juce::WebBrowserComponent::Resource> resource (const juce::String& url);
    juce::WebBrowserComponent::Options options();
    void js (const juce::String& code) { browser.evaluateJavascript (code); }
    void pushRestore();
    void pushPresets();
    void pushOrbitState();
    void pickNmsv();
    void pickSample (int layer, bool impulse);
    void showMode();

    VintageChonkProcessor& proc;
    std::map<juce::String, std::pair<const char*, int>> files;   // file name -> binary data
    juce::WebBrowserComponent browser;
    int seenRestore = 0;
    int ticks = 0;
    std::unique_ptr<juce::FileChooser> chooser;

    // ORBIT page
    int seenMode = -1;
    bool orbitPage = false;          // the ORBIT page said hello (and is the one loaded)
    int seenOrbitRestore = 0, seenViews = 0;
    std::vector<float> sentHost;
    juce::MemoryBlock drop;          // a file dropped on the panel, arriving in chunks
    juce::String dropName;
    int dropLayer = 0;
    bool dropImpulse = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VintageChonkEditor)
};
