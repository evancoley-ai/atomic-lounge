// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk plugin: the editor = the tablet's web panel (panel, unchanged files served
// from binary data) in JUCE's WebBrowserComponent on WebView2, with the desktop bridge (web/desktop_shim.js).
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
    std::optional<juce::WebBrowserComponent::Resource> resource (const juce::String& url);
    juce::WebBrowserComponent::Options options();
    void js (const juce::String& code) { browser.evaluateJavascript (code); }
    void pushRestore();
    void pushPresets();
    void pickNmsv();

    VintageChonkProcessor& proc;
    std::map<juce::String, std::pair<const char*, int>> files;   // file name -> binary data
    juce::WebBrowserComponent browser;
    int seenRestore = 0;
    int ticks = 0;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VintageChonkEditor)
};
