// SPDX-License-Identifier: AGPL-3.0-only
#include "PluginEditor.h"
#include "PresetStore.h"
#include "MFM1Www.h"

static juce::String quoted (const juce::String& s) { return juce::JSON::toString (juce::var (s)); }
static juce::String json (const juce::var& v) { return juce::JSON::toString (v, true); }

static std::map<juce::String, std::pair<const char*, int>> wwwFiles()
{
    std::map<juce::String, std::pair<const char*, int>> m;
    for (int i = 0; i < MFM1Www::namedResourceListSize; i++)
    {
        int size = 0;
        const char* data = MFM1Www::getNamedResource (MFM1Www::namedResourceList[i], size);
        m[juce::String (MFM1Www::getNamedResourceOriginalFilename (MFM1Www::namedResourceList[i]))] = { data, size };
    }
    return m;
}

VintageChonkEditor::VintageChonkEditor (VintageChonkProcessor& p)
    : AudioProcessorEditor (p), proc (p), files (wwwFiles()), browser (options())
{
    addAndMakeVisible (browser);
    seenRestore = proc.restoreSerial();
    // the panel is drawn for 1920 x 1200 and scales to fit: keep that shape
    setResizable (true, true);
    setResizeLimits (720, 450, 3840, 2400);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio (1920.0 / 1200.0);
    setSize (1440, 900);
    browser.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    startTimerHz (30);
}

VintageChonkEditor::~VintageChonkEditor() { stopTimer(); }

void VintageChonkEditor::paint (juce::Graphics& g) { g.fillAll (juce::Colour (0xff141312)); }
void VintageChonkEditor::resized() { browser.setBounds (getLocalBounds()); }

juce::WebBrowserComponent::Options VintageChonkEditor::options()
{
    auto* init = new juce::DynamicObject();
    init->setProperty ("state", proc.panelState());
    init->setProperty ("presets", mfm1::PresetStore::list());
    init->setProperty ("status", proc.status());
    int n = 0;
    const char* shim = MFM1Www::getNamedResource ("desktop_shim_js", n);
    const juce::String script = "window.MFHostInit = " + json (juce::var (init)) + ";\n" + juce::String::fromUTF8 (shim, n);

    const auto dataFolder = juce::File::getSpecialLocation (juce::File::windowsLocalAppData)
                                .getChildFile ("Vintage Chonk").getChildFile ("WebView2");
    dataFolder.createDirectory();
    using O = juce::WebBrowserComponent::Options;
    return O {}
        .withBackend (O::Backend::webview2)
        .withWinWebView2Options (O::WinWebView2 {}
                                     .withUserDataFolder (dataFolder)
                                     .withBackgroundColour (juce::Colour (0xff141312))
                                     .withStatusBarDisabled()
                                     .withBuiltInErrorPageDisabled())
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withUserScript (script)
        .withEventListener ("mfm1", [this] (const juce::var& v) { onPanelEvent (v); })
        .withResourceProvider ([this] (const juce::String& url) { return resource (url); });
}

std::optional<juce::WebBrowserComponent::Resource> VintageChonkEditor::resource (const juce::String& url)
{
    auto path = url.upToFirstOccurrenceOf ("?", false, false).upToFirstOccurrenceOf ("#", false, false);
    path = path.trimCharactersAtStart ("/");
    if (path.isEmpty()) path = "index.html";
    if (path == "desktop_shim.js") return std::nullopt;     // injected, not served
    const auto name = path.fromLastOccurrenceOf ("/", false, false);
    const auto it = files.find (name);
    if (it == files.end()) return std::nullopt;
    juce::String mime = "application/octet-stream";
    if (name.endsWith (".html")) mime = "text/html";
    else if (name.endsWith (".css")) mime = "text/css";
    else if (name.endsWith (".js")) mime = "text/javascript";
    const auto* b = reinterpret_cast<const std::byte*> (it->second.first);
    return juce::WebBrowserComponent::Resource { std::vector<std::byte> (b, b + it->second.second), mime };
}

void VintageChonkEditor::pushRestore()
{
    js ("window.MFHost && MFHost.restore(" + json (proc.panelState()) + ")");
}

void VintageChonkEditor::pushPresets()
{
    js ("window.MFHost && MFHost.presets(" + json (mfm1::PresetStore::list()) + ")");
}

void VintageChonkEditor::timerCallback()
{
    if (proc.restoreSerial() != seenRestore)
    {
        seenRestore = proc.restoreSerial();
        pushRestore();
    }
    else
    {
        int serial = 0;
        const auto b64 = proc.takeHostChanges (serial);
        if (b64.isNotEmpty()) js ("window.MFHost && MFHost.patch(" + quoted (b64) + "," + juce::String (serial) + ")");
    }
    if (++ticks % 3 == 0) js ("window.MFHost && MFHost.status(" + json (proc.status()) + ")");
}

void VintageChonkEditor::onPanelEvent (const juce::var& v)
{
    const auto op = v["op"].toString();
    const auto* a = v["a"].getArray();
    auto arg = [a] (int i) { return a != nullptr && i < a->size() ? (*a)[i] : juce::var(); };
    if (op == "patch")
    {
        const auto res = proc.panelPatch (arg (0).toString(), (int) arg (1));
        if (res != "ok") js ("window.MFHost && MFHost.toast(" + quoted ("Engine: " + res) + ")");
    }
    else if (op == "midi")
    {
        uint8_t m[3] = { (uint8_t) (int) arg (0), (uint8_t) ((int) arg (1) & 127), (uint8_t) ((int) arg (2) & 127) };
        const uint8_t st = m[0] & 0xF0;
        if (st == 0x80 || st == 0x90 || st == 0xB0 || st == 0xE0) proc.panelMidi (m, 3);
    }
    else if (op == "ui") proc.panelUiState (arg (0).toString());
    else if (op == "save")
    {
        const auto res = mfm1::PresetStore::save (arg (0).toString(), arg (1).toString());
        if (res != "ok") js ("window.MFHost && MFHost.toast(" + quoted ("Not saved: " + res) + ")");
        pushPresets();
    }
    else if (op == "delete")
    {
        mfm1::PresetStore::remove (arg (0).toString());
        pushPresets();
    }
    else if (op == "pick") pickNmsv();
    else if (op == "hello")
    {
        pushPresets();
        pushRestore();
    }
}

void VintageChonkEditor::pickNmsv()
{
    chooser = std::make_unique<juce::FileChooser> ("Import a Massive preset (.nmsv)", juce::File(), "*.nmsv");
    juce::Component::SafePointer<VintageChonkEditor> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr) return;
                              const auto f = fc.getResult();
                              if (f == juce::File())
                              {
                                  safe->js ("window.onNmsvPicked && onNmsvPicked(null, 'cancelled')");
                                  return;
                              }
                              juce::MemoryBlock mb;
                              if (f.getSize() > (4 << 20) || ! f.loadFileAsData (mb))
                              {
                                  safe->js ("window.onNmsvPicked && onNmsvPicked(null, " + quoted ("Could not read " + f.getFileName()) + ")");
                                  return;
                              }
                              safe->js ("window.onNmsvPicked && onNmsvPicked(" + quoted (f.getFileName()) + ", "
                                        + quoted (juce::Base64::toBase64 (mb.getData(), mb.getSize())) + ")");
                          });
}
