#include "globals.hpp"
#include "server.hpp"
#include "xvfb.hpp"

#include <csignal>
#include <cstdlib>

#include "include/wrapper/cef_closure_task.h"
#include "include/cef_app.h"

#include <X11/Xlib.h>

#include <dlfcn.h>

namespace {

class AppServerEventHandler : public ServerEventHandler {
SHARED_ONLY_CLASS(AppServerEventHandler);
public:
    AppServerEventHandler(CKey) {}

    virtual void onServerShutdownComplete() override {
        LOG(INFO) << "Quitting CEF message loop";
        CefQuitMessageLoop();
    }
};

class App :
    public CefApp,
    public CefBrowserProcessHandler
{
public:
    App() {
        serverEventHandler_ = AppServerEventHandler::create();
        shutdown_ = false;
    }

    void shutdown() {
        REQUIRE_UI_THREAD();

        if(server_) {
            server_->shutdown();
        } else {
            shutdown_ = true;
        }
    }

    // CefApp:
    virtual CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }
    virtual void OnBeforeCommandLineProcessing(
        const CefString& processType,
        CefRefPtr<CefCommandLine> commandLine
    ) override {
        commandLine->AppendSwitch("disable-smooth-scrolling");
        commandLine->AppendSwitchWithValue("use-gl", "desktop");
    }

    // CefBrowserProcessHandler:
    virtual void OnContextInitialized() override {
        REQUIRE_UI_THREAD();
        CHECK(!server_);

        server_ = Server::create(serverEventHandler_);
        if(shutdown_) {
            server_->shutdown();
        }
    }

private:
    shared_ptr<Server> server_;
    shared_ptr<AppServerEventHandler> serverEventHandler_;
    bool shutdown_;

    IMPLEMENT_REFCOUNTING(App);
};

CefRefPtr<App> app;
bool termSignalReceived = false;

void handleTermSignalSetFlag(int signalID) {
    LOG(INFO) << "Got signal " << signalID << ", initiating shutdown";
    termSignalReceived = true;
}

void handleTermSignalInApp(int signalID) {
    LOG(INFO) << "Got signal " << signalID << ", initiating shutdown";
    CefPostTask(TID_UI, base::Bind(&App::shutdown, app));
}

}

int main(int argc, char* argv[]) {
    CefMainArgs mainArgs(argc, argv);

    int exitCode = CefExecuteProcess(mainArgs, nullptr, nullptr);
    if(exitCode >= 0) {
        return exitCode;
    }

    signal(SIGINT, handleTermSignalSetFlag);
    signal(SIGTERM, handleTermSignalSetFlag);

    void* vice = dlopen("retrowebvice.so", RTLD_NOW | RTLD_LOCAL);
    if(vice == nullptr) {
        cerr << "Loading vice plugin failed\n";
        return 1;
    }
    void* sym = dlsym(vice, "vicePlugin_createContext");
    if(sym == nullptr) {
        cerr << "Loading vicePlugin_createContext symbol failed\n";
        return 1;
    }
    void* (*vicePlugin_createContext)(uint64_t) = (void* (*)(uint64_t))sym;
    void* ctx = vicePlugin_createContext(1);
    if(ctx == nullptr) {
        cerr << "Call to vicePlugin_createContext failed\n";
        return 1;
    }

    shared_ptr<Config> config = Config::read(argc, argv);
    if(!config) {
        return 1;
    }

    shared_ptr<Xvfb> xvfb;
    if(config->useDedicatedXvfb) {
        xvfb = Xvfb::create();
        xvfb->setupEnv();
    }

    globals = Globals::create(config);

    if(!termSignalReceived) {
        // Ignore non-fatal X errors
        XSetErrorHandler([](Display*, XErrorEvent*) { return 0; });
        XSetIOErrorHandler([](Display*) { return 0; });

        app = new App;

        CefSettings settings;
        settings.windowless_rendering_enabled = true;
        settings.command_line_args_disabled = true;
        CefString(&settings.cache_path).FromString(globals->config->dataDir);
        CefString(&settings.user_agent).FromString(globals->config->userAgent);

        CefInitialize(mainArgs, settings, app, nullptr);

        signal(SIGINT, handleTermSignalInApp);
        signal(SIGTERM, handleTermSignalInApp);

        if(termSignalReceived) {
            app->shutdown();
        }

        setRequireUIThreadEnabled(true);
        CefRunMessageLoop();
        setRequireUIThreadEnabled(false);

        signal(SIGINT, [](int) {});
        signal(SIGTERM, [](int) {});

        CefShutdown();

        app = nullptr;
    }

    globals.reset();
    xvfb.reset();

    return 0;
}
