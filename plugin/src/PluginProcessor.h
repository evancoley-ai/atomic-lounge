// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk plugin: the audio processor (engine, host parameters, state, programs).
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include "EngineRunner.h"
#include "Params.h"

class VintageChonkProcessor final : public juce::AudioProcessor
{
public:
    VintageChonkProcessor();
    ~VintageChonkProcessor() override;

    // juce::AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Vintage Chonk"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- the editor's side (message thread) ---------------------------------------------------
    // a patch from the panel (.mfp as base64). hostSerial: the last host push the panel had applied.
    juce::String panelPatch (const juce::String& b64, int hostSerial);
    void panelUiState (const juce::String& json);
    void panelMidi (const uint8_t* msg, int n) { runner.postMidi (msg, n); }
    // host automation not yet shown on the panel: the patch to push (base64), else empty
    juce::String takeHostChanges (int& serialOut);
    // everything the panel needs to show the current sound: {b64, ui, serial}
    juce::var panelState();
    int restoreSerial() const { return restoreCount.load(); }   // bumps when the panel must reload (state, program)
    juce::var status();

    static juce::String toB64 (const mfe_patch_t& p);
    static bool fromB64 (const juce::String& b64, mfe_patch_t& p, int& err);

    mfm1::EngineRunner& engine() { return runner; }

private:
    static bool patchHook (void* user, mfe_patch_t& p, bool isNew);
    void loadPatch (const mfe_patch_t& p, const juce::String& uiJson);   // state / program
    void mergeHostValues (mfe_patch_t& p) const;
    void setParamField (int i, int16_t f);

    mfm1::EngineRunner runner;
    std::array<juce::AudioParameterFloat*, mfm1::numParams> params {};

    juce::CriticalSection lock;
    mfe_patch_t current {};        // what is playing (host values merged in when pushed or saved)
    mfe_patch_t lastPanel {};      // the patch the panel last had (sent or pushed): its edits are the diff
    juce::String uiState;          // panel JSON: current preset entry, edited flag
    int hostSerial = 0;            // pushes of host automation to the panel
    int currentProgram = 0;
    std::atomic<int> restoreCount { 0 };
    bool applyingFromPanel = false;

    std::vector<mfm1::MidiEvent> events;   // reserved in prepareToPlay; never grows on the audio thread

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VintageChonkProcessor)
};
