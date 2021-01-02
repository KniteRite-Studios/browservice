#pragma once

#include "browser_area.hpp"
#include "control_bar.hpp"
#include "download_manager.hpp"
#include "http.hpp"
#include "image_slice.hpp"
#include "widget.hpp"

class Session;

// Exceptionally, session event handlers are called directly instead of the
// event loop to avoid race conditions
class SessionEventHandler {
public:
    virtual void onSessionClosed(uint64_t id) = 0;
    virtual bool onIsServerFullQuery() = 0;
    virtual void onPopupSessionOpen(shared_ptr<Session> session) = 0;
};

class DownloadManager;
class ImageCompressor;
class RootWidget;
class Timeout;

class CefBrowser;

// Single browser session. Before quitting CEF message loop, call close and wait
// for onSessionClosed event. 
class Session :
    public WidgetParent,
    public ControlBarEventHandler,
    public BrowserAreaEventHandler,
    public DownloadManagerEventHandler,
    public enable_shared_from_this<Session>
{
SHARED_ONLY_CLASS(Session);
public:
    // Creates new session. The isPopup argument is only used internally to
    // create popup sessions. Returns empty pointer if creating the session
    // failed
    static shared_ptr<Session> tryCreate(
        weak_ptr<SessionEventHandler> eventHandler,
        bool allowPNG,
        bool isPopup = false
    );

    // Private constructor
    Session(CKey, CKey);

    ~Session();

    // Close browser if it is not yet closed
    void close();

    void handleHTTPRequest(shared_ptr<HTTPRequest> request);

    // Get the unique and constant ID of this session
    uint64_t id();

    // WidgetParent:
    virtual void onWidgetViewDirty() override;
    virtual void onWidgetCursorChanged() override;
    virtual void onGlobalHotkeyPressed(GlobalHotkey key) override;

    // ControlBarEventHandler:
    virtual void onAddressSubmitted(string url) override;
    virtual void onQualityChanged(int quality) override;
    virtual void onPendingDownloadAccepted() override;
    virtual void onFind(string text, bool forward, bool findNext) override;
    virtual void onStopFind(bool clearSelection) override;
    virtual void onClipboardButtonPressed() override;

    // BrowserAreaEventHandler:
    virtual void onBrowserAreaViewDirty() override;

    // DownloadManagerEventHandler:
    virtual void onPendingDownloadCountChanged(int count) override;
    virtual void onDownloadProgressChanged(vector<int> progress) override;
    virtual void onDownloadCompleted(shared_ptr<CompletedDownload> file) override;

private:
    // Class that implements CefClient interfaces for this session
    class Client;

    void updateInactivityTimeout_(bool shortened = false);

    void updateSecurityStatus_();

    // Change root viewport size if it is different than currently. Clamps
    // dimensions to sane interval.
    void updateRootViewportSize_(int width, int height);

    // Send paddedRootViewport_ to the image compressor correctly clamped such
    // that its dimensions result in signals (widthSignal_, heightSignal_).
    void sendViewportToCompressor_();

    void handleEvents_(
        uint64_t startIdx,
        string::const_iterator begin,
        string::const_iterator end
    );

    void setWidthSignal_(int newWidthSignal);
    void setHeightSignal_(int newHeightSignal);

    void addIframe_(function<void(shared_ptr<HTTPRequest>)> iframe);

    // -1 = back, 0 = refresh, 1 = forward
    void navigate_(int direction);

    weak_ptr<SessionEventHandler> eventHandler_;

    uint64_t id_;

    bool isPopup_;

    bool prePrevVisited_;
    bool preMainVisited_;

    bool prevNextClicked_;

    // How many times the main page has been requested. The main page mentions
    // its index to all the requests it makes, and we discard all the requests
    // that are not from the newest main page.
    uint64_t curMainIdx_;

    // Latest image index. We discard image requests that do not have a higher
    // image index to avoid request reordering.
    uint64_t curImgIdx_;

    // How many events we have handled for the current main index. We keep track
    // of this to avoid replaying events; the client may send the same events
    // twice as it cannot know for sure which requests make it through.
    uint64_t curEventIdx_;

    // Downloads whose iframe has been loaded, and the actual file is kept
    // available until a timeout has expired.
    map<uint64_t, pair<shared_ptr<CompletedDownload>, shared_ptr<Timeout>>> downloads_;
    uint64_t curDownloadIdx_;

    enum {Pending, Open, Closing, Closed} state_;

    // If true, browser should close as soon as it is opened
    bool closeOnOpen_;

    shared_ptr<Timeout> inactivityTimeoutLong_;
    shared_ptr<Timeout> inactivityTimeoutShort_;

    steady_clock::time_point lastSecurityStatusUpdateTime_;
    steady_clock::time_point lastNavigateOperationTime_;

    bool allowPNG_;
    shared_ptr<ImageCompressor> imageCompressor_;

    ImageSlice paddedRootViewport_;
    ImageSlice rootViewport_;
    shared_ptr<RootWidget> rootWidget_;

    queue<function<void(shared_ptr<HTTPRequest>)>> iframeQueue_;

    // We use width and height modulo WidthSignalModulus and HeightSignalModulus
    // of the part of rootViewport_ sent to imageCompressor_ to signal various
    // things to the client. We make the initial signals (1, 1) as
    // imageCompressor_ initially sends a 1x1 image.
    static constexpr int WidthSignalNewIframe = 0;
    static constexpr int WidthSignalNoNewIframe = 1;
    static constexpr int WidthSignalModulus = 2;

    // Height signals are given by *Cursor defined in widget.hpp
    static constexpr int HeightSignalModulus = CursorTypeCount;

    int widthSignal_;
    int heightSignal_;

    shared_ptr<DownloadManager> downloadManager_;

    // Only available in Open state
    CefRefPtr<CefBrowser> browser_;
};
