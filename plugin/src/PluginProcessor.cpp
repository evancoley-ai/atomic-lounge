// SPDX-License-Identifier: AGPL-3.0-only
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "factory_presets.h"

using namespace mfm1;

static juce::String factoryUi (int i)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("kind", "F");
    o->setProperty ("name", juce::String (mfm1_factory::names[i]));
    o->setProperty ("dirty", false);
    return juce::JSON::toString (juce::var (o), true);
}

static bool factoryPatch (int i, mfe_patch_t& p)
{
    return i >= 0 && i < mfm1_factory::count
           && mfe_preset_read (&p, mfm1_factory::images[i], mfm1_factory::size) == 0;
}

VintageChonkProcessor::VintageChonkProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    mfe_patch_t p;
    if (! factoryPatch (0, p)) mfe_patch_init (&p);
    for (int i = 0; i < numParams; i++)
    {
        const ParamDef& d = paramDefs[i];
        auto attrs = juce::AudioParameterFloatAttributes().withStringFromValueFunction (
            [&d] (float v, int) { return juce::String (paramText (d, v)); });
        auto* prm = new juce::AudioParameterFloat (juce::ParameterID { d.id, 1 }, d.name,
                                                   juce::NormalisableRange<float> (0.0f, 1.0f),
                                                   valueFromField (d, *d.field (p)), attrs);
        params[(size_t) i] = prm;
        addParameter (prm);
    }
    current = lastPanel = p;
    uiState = factoryUi (0);
    runner.setPatchHook (&VintageChonkProcessor::patchHook, this);
    runner.postPatch (current);
    events.reserve (4096);
}

VintageChonkProcessor::~VintageChonkProcessor() = default;

// ------------------------------------------------------------------------------------- audio ---
void VintageChonkProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    runner.prepare (sampleRate, samplesPerBlock);
    setLatencySamples (runner.latencySamples());
    events.reserve (4096);
}

void VintageChonkProcessor::reset() { runner.reset(); }

bool VintageChonkProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.inputBuses.size() > 0 && ! layouts.getMainInputChannelSet().isDisabled()) return false;
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

bool VintageChonkProcessor::patchHook (void* user, mfe_patch_t& p, bool)
{
    // audio thread: the engine's patch always carries the host's parameter values
    auto* self = static_cast<VintageChonkProcessor*> (user);
    bool changed = false;
    for (int i = 0; i < numParams; i++)
    {
        const ParamDef& d = paramDefs[i];
        const int16_t f = fieldFromValue (d, self->params[(size_t) i]->get());
        int16_t* fld = d.field (p);
        if (*fld != f) { *fld = f; changed = true; }
    }
    return changed;
}

void VintageChonkProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm())
                runner.setTempo (*bpm);

    events.clear();   // keeps its capacity
    for (const auto meta : midi)
    {
        if (events.size() == events.capacity()) break;
        if (meta.numBytes < 1 || meta.numBytes > 3) continue;   // SysEx etc.: not for the engine
        MidiEvent e {};
        e.offset = meta.samplePosition;
        e.size = (uint8_t) meta.numBytes;
        std::memcpy (e.bytes, meta.data, (size_t) meta.numBytes);
        events.push_back (e);
    }
    float* l = buffer.getNumChannels() > 0 ? buffer.getWritePointer (0) : nullptr;
    float* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    runner.process (l, r, n, events.data(), (int) events.size());   // r == nullptr: mono (L+R)/2 into l
    for (int c = 2; c < buffer.getNumChannels(); c++) buffer.clear (c, 0, n);
}

// ------------------------------------------------------------------------------------- state ---
juce::String VintageChonkProcessor::toB64 (const mfe_patch_t& p)
{
    uint8_t buf[2048];
    const size_t n = mfe_preset_write (&p, buf, sizeof buf);
    return juce::Base64::toBase64 (buf, n);
}

bool VintageChonkProcessor::fromB64 (const juce::String& b64, mfe_patch_t& p, int& err)
{
    juce::MemoryOutputStream mo;
    if (b64.length() > 8192 || ! juce::Base64::convertFromBase64 (mo, b64)) { err = -100; return false; }
    err = mfe_preset_read (&p, static_cast<const uint8_t*> (mo.getData()), mo.getDataSize());
    return err == 0;
}

void VintageChonkProcessor::mergeHostValues (mfe_patch_t& p) const
{
    for (int i = 0; i < numParams; i++)
        *paramDefs[i].field (p) = fieldFromValue (paramDefs[i], params[(size_t) i]->get());
}

void VintageChonkProcessor::setParamField (int i, int16_t f)
{
    auto* prm = params[(size_t) i];
    if (fieldFromValue (paramDefs[i], prm->get()) == f) return;
    prm->beginChangeGesture();
    prm->setValueNotifyingHost (valueFromField (paramDefs[i], f));
    prm->endChangeGesture();
}

void VintageChonkProcessor::loadPatch (const mfe_patch_t& p, const juce::String& uiJson)
{
    {
        const juce::ScopedLock sl (lock);
        current = lastPanel = p;
        uiState = uiJson;
        hostSerial++;
    }
    for (int i = 0; i < numParams; i++) setParamField (i, *paramDefs[i].field (const_cast<mfe_patch_t&> (p)));
    runner.postPatch (p);
    restoreCount++;
}

void VintageChonkProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    mfe_patch_t p;
    juce::String ui;
    int prog;
    {
        const juce::ScopedLock sl (lock);
        p = current;
        ui = uiState;
        prog = currentProgram;
    }
    mergeHostValues (p);
    juce::XmlElement x ("VINTAGECHONK");
    x.setAttribute ("version", 1);
    x.setAttribute ("mfp", toB64 (p));
    x.setAttribute ("ui", ui);
    x.setAttribute ("program", prog);
    copyXmlToBinary (x, destData);
}

void VintageChonkProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto x = getXmlFromBinary (data, sizeInBytes);
    if (x == nullptr || ! (x->hasTagName ("VINTAGECHONK")
                           || x->hasTagName ("MOOSIVEFM1"))) return;   // legacy name: the pre-release state tag, still read
    mfe_patch_t p;
    int err = 0;
    if (! fromB64 (x->getStringAttribute ("mfp"), p, err)) return;
    juce::String ui = x->getStringAttribute ("ui");
    if (! juce::JSON::parse (ui).isObject()) ui = "{}";
    currentProgram = juce::jlimit (0, mfm1_factory::count - 1, x->getIntAttribute ("program", 0));
    loadPatch (p, ui);
}

// ------------------------------------------------------------------------------------ programs ---
int VintageChonkProcessor::getNumPrograms() { return mfm1_factory::count; }

const juce::String VintageChonkProcessor::getProgramName (int index)
{
    return index >= 0 && index < mfm1_factory::count ? juce::String (mfm1_factory::names[index]) : juce::String();
}

void VintageChonkProcessor::setCurrentProgram (int index)
{
    // re-selecting the current program is ignored: some hosts call setCurrentProgram after restoring a
    // project, which would otherwise replace the restored (edited) sound with the factory one
    if (index == currentProgram || index < 0 || index >= mfm1_factory::count) return;
    mfe_patch_t p;
    if (! factoryPatch (index, p)) return;
    currentProgram = index;
    loadPatch (p, factoryUi (index));
}

// ------------------------------------------------------------------------------------- panel ---
juce::String VintageChonkProcessor::panelPatch (const juce::String& b64, int serial)
{
    mfe_patch_t p;
    int err = 0;
    if (! fromB64 (b64, p, err)) return "preset error " + juce::String (err);
    mfe_patch_t posted = p;
    std::array<int16_t, numParams> userSet;
    std::array<bool, numParams> isUser {};
    {
        const juce::ScopedLock sl (lock);
        const bool stale = serial != hostSerial;   // sent before the panel saw the last automation push
        for (int i = 0; i < numParams; i++)
        {
            const ParamDef& d = paramDefs[i];
            const int16_t fp = *d.field (p), fl = *d.field (lastPanel);
            if (! stale && fp != fl) { isUser[(size_t) i] = true; userSet[(size_t) i] = fp; }
            else *d.field (posted) = fieldFromValue (d, params[(size_t) i]->get());
        }
        lastPanel = p;
        current = posted;
    }
    for (int i = 0; i < numParams; i++)
        if (isUser[(size_t) i]) setParamField (i, userSet[(size_t) i]);
    runner.postPatch (posted);
    return "ok";
}

void VintageChonkProcessor::panelUiState (const juce::String& json)
{
    if (json.length() > 4096) return;
    auto v = juce::JSON::parse (json);
    if (! v.isObject()) return;
    int prog = -1;
    if (v["kind"].toString() == "F")
        for (int i = 0; i < mfm1_factory::count; i++)
            if (v["name"].toString() == mfm1_factory::names[i]) prog = i;
    const juce::ScopedLock sl (lock);
    uiState = json;
    if (prog >= 0 && prog != currentProgram)
    {
        currentProgram = prog;
        juce::MessageManager::callAsync ([this] { updateHostDisplay (ChangeDetails().withProgramChanged (true)); });
    }
}

juce::String VintageChonkProcessor::takeHostChanges (int& serialOut)
{
    const juce::ScopedLock sl (lock);
    bool changed = false;
    for (int i = 0; i < numParams; i++)
    {
        const ParamDef& d = paramDefs[i];
        const int16_t f = fieldFromValue (d, params[(size_t) i]->get());
        if (*d.field (current) != f) { *d.field (current) = f; changed = true; }
    }
    if (! changed) return {};
    hostSerial++;
    lastPanel = current;
    serialOut = hostSerial;
    return toB64 (current);
}

juce::var VintageChonkProcessor::panelState()
{
    const juce::ScopedLock sl (lock);
    mergeHostValues (current);
    lastPanel = current;
    auto* o = new juce::DynamicObject();
    o->setProperty ("b64", toB64 (current));
    o->setProperty ("ui", juce::JSON::parse (uiState));
    o->setProperty ("serial", hostSerial);
    return juce::var (o);
}

juce::var VintageChonkProcessor::status()
{
    auto* o = new juce::DynamicObject();
    const double fs = runner.hostRate();
    o->setProperty ("voices", (int) runner.activeVoices());
    o->setProperty ("midi", (int) runner.midiCount());
    o->setProperty ("sampleRate", fs);
    o->setProperty ("latencyMs", fs > 0 ? runner.latencySamples() * 1000.0 / fs : 0.0);
    o->setProperty ("underruns", 0);
    o->setProperty ("dropped", (int) runner.droppedEvents());
    o->setProperty ("midiDevices", juce::Array<juce::var> { juce::var ("HOST MIDI") });
    return juce::var (o);
}

juce::AudioProcessorEditor* VintageChonkProcessor::createEditor() { return new VintageChonkEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VintageChonkProcessor(); }
