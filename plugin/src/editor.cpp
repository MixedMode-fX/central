#include "editor.h"
#include "BinaryData.h"
#include "version.h"

namespace {
    // The page, for whatever the root URL is asked as.
    std::optional<juce::WebBrowserComponent::Resource> page(const juce::String& url){
        const juce::String path = url.trimCharactersAtStart("/");
        if (path.isNotEmpty() && path != "index.html") return std::nullopt;
        const auto* data = reinterpret_cast<const std::byte*>(mmmc_page::index_html);
        return juce::WebBrowserComponent::Resource{
            std::vector<std::byte>(data, data + mmmc_page::index_htmlSize), "text/html"};
    }

    // Bytes out of a payload the page sent as a JSON array of numbers.
    void bytesOf(const juce::var& payload, std::vector<uint8_t>& out){
        out.clear();
        if (const auto* array = payload.getArray()){
            out.reserve((size_t)array->size());
            for (const auto& v : *array) out.push_back((uint8_t)((int)v & 0xFF));
        }
    }
}

MmmcEditor::MmmcEditor(MmmcProcessor& p) :
    AudioProcessorEditor(p), processor(p), browser(options()), bytes(), replies()
{
    addAndMakeVisible(browser);
    browser.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
    setResizable(true, true);
    setResizeLimits(480, 400, 4096, 4096);
    setSize(1200, 800);
    startTimer(POLL_MS);
}

MmmcEditor::~MmmcEditor(){ stopTimer(); }

void MmmcEditor::resized(){ browser.setBounds(getLocalBounds()); }

juce::WebBrowserComponent::Options MmmcEditor::options(){
    using Options = juce::WebBrowserComponent::Options;
    Options o = Options{}
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withResourceProvider(page)
        // What tells the page it is inside this plugin, and which build.
        .withInitialisationData("mmmcHost", juce::var(juce::String(MMMC_BUILD)))
        .withEventListener("mmmc.sysex", [this](const juce::var& payload){ onSysex(payload); })
        .withEventListener("mmmc.midi", [this](const juce::var& payload){ onMidi(payload); });
#if JUCE_WINDOWS
    // WebView2 is the browser that runs the page; the legacy control cannot.
    // Its profile goes under the user's temporary files, one per plugin,
    // which is what lets the page keep its library between sessions.
    const auto profile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("MMMC");
    o = o.withBackend(Options::Backend::webview2)
         .withWinWebView2Options(Options::WinWebView2{}
             .withUserDataFolder(profile)
             .withBackgroundColour(juce::Colours::black));
#endif
    return o;
}

// One SysEx message from the page, answered on the spot.
void MmmcEditor::onSysex(const juce::var& payload){
    bytesOf(payload, bytes);
    if (bytes.empty()) return;
    processor.editorSysex(bytes.data(), bytes.size(), replies);
    emit(replies);
}

// A channel message from the page's keyboard or surface: [cable, type,
// channel, d1, d2], on its way to the next pass.
void MmmcEditor::onMidi(const juce::var& payload){
    bytesOf(payload, bytes);
    if (bytes.size() < 5) return;
    processor.editorMidi(bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
}

// Each F0..F7 run in `runs` goes to the page as one message.
void MmmcEditor::emit(const std::vector<uint8_t>& runs){
    size_t start = 0;
    for (size_t i = 0; i < runs.size(); i++){
        if (runs[i] != 0xF7) continue;
        juce::Array<juce::var> message;
        message.ensureStorageAllocated((int)(i + 1 - start));
        for (size_t k = start; k <= i; k++) message.add((int)runs[k]);
        browser.emitEventIfBrowserIsVisible("mmmc.message", juce::var(message));
        start = i + 1;
    }
}

void MmmcEditor::timerCallback(){
    processor.drainEditor(replies);
    if (!replies.empty()) emit(replies);
}
