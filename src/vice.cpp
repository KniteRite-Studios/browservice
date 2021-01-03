#include "vice.hpp"

#include "globals.hpp"

#include "../vice_plugin_api.h"

#include <dlfcn.h>

struct VicePlugin::APIFuncs {
#define FOREACH_VICE_API_FUNC \
    FOREACH_VICE_API_FUNC_ITEM(isAPIVersionSupported) \
    FOREACH_VICE_API_FUNC_ITEM(getVersionString) \
    FOREACH_VICE_API_FUNC_ITEM(initContext) \
    FOREACH_VICE_API_FUNC_ITEM(destroyContext) \
    FOREACH_VICE_API_FUNC_ITEM(start) \
    FOREACH_VICE_API_FUNC_ITEM(shutdown) \
    FOREACH_VICE_API_FUNC_ITEM(pumpEvents) \
    FOREACH_VICE_API_FUNC_ITEM(closeWindow) \
    FOREACH_VICE_API_FUNC_ITEM(notifyWindowViewChanged) \
    FOREACH_VICE_API_FUNC_ITEM(getOptionDocs) \
    FOREACH_VICE_API_FUNC_ITEM(setGlobalLogCallback) \
    FOREACH_VICE_API_FUNC_ITEM(setGlobalPanicCallback)

#define FOREACH_VICE_API_FUNC_ITEM(name) \
    decltype(&vicePluginAPI_ ## name) name;

    FOREACH_VICE_API_FUNC
#undef FOREACH_VICE_API_FUNC_ITEM
};

namespace {

char* createMallocString(string val) {
    size_t size = val.size() + 1;
    char* ret = (char*)malloc(size);
    REQUIRE(ret != nullptr);
    memcpy(ret, val.c_str(), size);
    return ret;
}

#define API_CALLBACK_HANDLE_EXCEPTIONS_START try {
#define API_CALLBACK_HANDLE_EXCEPTIONS_END \
    } catch(const exception& e) { \
        PANIC("Unhandled exception traversing the vice plugin API: ", e.what()); \
        abort(); \
    } catch(...) { \
        PANIC("Unhandled exception traversing the vice plugin API"); \
        abort(); \
    }

void logCallback(
    void* filenamePtr,
    VicePluginAPI_LogLevel logLevel,
    const char* location,
    const char* msg
) {
API_CALLBACK_HANDLE_EXCEPTIONS_START

    const string& filename = *(string*)filenamePtr;

    const char* logLevelStr;
    if(logLevel == VICE_PLUGIN_API_LOG_LEVEL_ERROR) {
        logLevelStr = "ERROR";
    } else if(logLevel == VICE_PLUGIN_API_LOG_LEVEL_WARNING) {
        logLevelStr = "WARNING";
    } else {
        if(logLevel != VICE_PLUGIN_API_LOG_LEVEL_INFO) {
            WARNING_LOG(
                "Incoming log message from vice plugin ", filename,
                "with unknown log level, defaulting to INFO"
            );
        }
        logLevelStr = "INFO";
    }

    LogWriter(
        logLevelStr,
        filename + " " + location
    )(msg);

API_CALLBACK_HANDLE_EXCEPTIONS_END
}

void panicCallback(void* filenamePtr, const char* location, const char* msg) {
API_CALLBACK_HANDLE_EXCEPTIONS_START

    const string& filename = *(string*)filenamePtr;
    Panicker(filename + " " + location)(msg);

API_CALLBACK_HANDLE_EXCEPTIONS_END
}

void destructorCallback(void* filenamePtr) {
API_CALLBACK_HANDLE_EXCEPTIONS_START

    string* filename = (string*)filenamePtr;
    delete filename;

API_CALLBACK_HANDLE_EXCEPTIONS_END
}

}

shared_ptr<VicePlugin> VicePlugin::load(string filename) {
    REQUIRE_UI_THREAD();

    void* lib = dlopen(filename.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if(lib == nullptr) {
        const char* err = dlerror();
        ERROR_LOG(
            "Loading vice plugin library '", filename,
            "' failed: ", err != nullptr ? err : "Unknown error"
        );
        return {};
    }

    unique_ptr<APIFuncs> apiFuncs = make_unique<APIFuncs>();

    void* sym;

#define FOREACH_VICE_API_FUNC_ITEM(name) \
    sym = dlsym(lib, "vicePluginAPI_" #name); \
    if(sym == nullptr) { \
        const char* err = dlerror(); \
        ERROR_LOG( \
            "Loading symbol 'vicePluginAPI_" #name "' from vice plugin ", \
            filename, " failed: ", err != nullptr ? err : "Unknown error" \
        ); \
        REQUIRE(dlclose(lib) == 0); \
        return {}; \
    } \
    apiFuncs->name = (decltype(apiFuncs->name))sym;

    FOREACH_VICE_API_FUNC
#undef FOREACH_VICE_API_FUNC_ITEM

    uint64_t apiVersion = 1000000;

    if(!apiFuncs->isAPIVersionSupported(apiVersion)) {
        ERROR_LOG(
            "Vice plugin ", filename,
            " does not support API version ", apiVersion
        );
        REQUIRE(dlclose(lib) == 0);
        return {};
    }

    apiFuncs->setGlobalLogCallback(
        apiVersion,
        logCallback,
        new string(filename),
        destructorCallback
    );
    apiFuncs->setGlobalPanicCallback(
        apiVersion,
        panicCallback,
        new string(filename),
        destructorCallback
    );

    return VicePlugin::create(
        CKey(),
        filename,
        lib,
        apiVersion,
        move(apiFuncs)
    );
}

VicePlugin::VicePlugin(CKey, CKey,
    string filename,
    void* lib,
    uint64_t apiVersion,
    unique_ptr<APIFuncs> apiFuncs
) {
    filename_ = filename;
    lib_ = lib;
    apiVersion_ = apiVersion;
    apiFuncs_ = move(apiFuncs);
}

VicePlugin::~VicePlugin() {
    REQUIRE(dlclose(lib_) == 0);
}

string VicePlugin::getVersionString() {
    REQUIRE_UI_THREAD();

    char* raw = apiFuncs_->getVersionString();
    string val = raw;
    free(raw);
    return val;
}

vector<VicePlugin::OptionDocsItem> VicePlugin::getOptionDocs() {
    REQUIRE_UI_THREAD();

    vector<VicePlugin::OptionDocsItem> ret;

    apiFuncs_->getOptionDocs(
        apiVersion_,
        [](
            void* data,
            const char* name,
            const char* valSpec,
            const char* desc,
            const char* defaultValStr
        ) {
        API_CALLBACK_HANDLE_EXCEPTIONS_START

            vector<VicePlugin::OptionDocsItem>& ret =
                *(vector<VicePlugin::OptionDocsItem>*)data;
            ret.push_back({name, valSpec, desc, defaultValStr});

        API_CALLBACK_HANDLE_EXCEPTIONS_END
        },
        (void*)&ret
    );

    return ret;
}

namespace {

thread_local ViceContext* threadActivePumpEventsContext = nullptr;

}

// Convenience macro for passing callbacks to the plugin in ViceContext,
// handling appropriate checks and resolving the callback data back to a
// reference to the ViceContext object
#define CTX_CALLBACK_WITHOUT_PUMPEVENTS_CHECK(ret, argDef, ...) \
    [](void* callbackData, auto ...args) -> ret { \
    API_CALLBACK_HANDLE_EXCEPTIONS_START \
        shared_ptr<ViceContext> self = getContext_(callbackData); \
        return ([&self]argDef -> ret { __VA_ARGS__ })(args...); \
    API_CALLBACK_HANDLE_EXCEPTIONS_END \
    }

#define CTX_CALLBACK(ret, argDef, ...) \
    CTX_CALLBACK_WITHOUT_PUMPEVENTS_CHECK(ret, argDef, { \
        if(threadActivePumpEventsContext != self.get()) { \
            PANIC( \
                "Vice plugin unexpectedly called a callback in a thread that " \
                "is not currently executing vicePluginAPI_pumpEvents" \
            ); \
        } \
        REQUIRE_UI_THREAD(); \
        { __VA_ARGS__ } \
    })

shared_ptr<ViceContext> ViceContext::init(
    shared_ptr<VicePlugin> plugin,
    vector<pair<string, string>> options
) {
    REQUIRE_UI_THREAD();

    vector<const char*> optionNames;
    vector<const char*> optionValues;
    for(const pair<string, string>& opt : options) {
        optionNames.push_back(opt.first.c_str());
        optionValues.push_back(opt.second.c_str());
    }

    char* initErrorMsg = nullptr;
    VicePluginAPI_Context* ctx = plugin->apiFuncs_->initContext(
        plugin->apiVersion_,
        optionNames.data(),
        optionValues.data(),
        options.size(),
        &initErrorMsg
    );

    if(ctx == nullptr) {
        REQUIRE(initErrorMsg != nullptr);
        cerr
            << "ERROR: Vice plugin " << plugin->filename_ <<
            " initialization failed: " << initErrorMsg << "\n";
        free(initErrorMsg);
        return {};
    }

    return ViceContext::create(CKey(), plugin, ctx);
}

ViceContext::ViceContext(CKey, CKey,
    shared_ptr<VicePlugin> plugin,
    VicePluginAPI_Context* ctx
) {
    plugin_ = plugin;
    ctx_ = ctx;

    state_ = Pending;
    shutdownPending_ = false;
    pumpEventsInQueue_.store(false);

    nextWindowHandle_ = 1;
}

ViceContext::~ViceContext() {
    REQUIRE(state_ == Pending || state_ == ShutdownComplete);
    plugin_->apiFuncs_->destroyContext(ctx_);
}

void ViceContext::start(shared_ptr<ViceContextEventHandler> eventHandler) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Pending);
    REQUIRE(eventHandler);

    state_ = Running;
    eventHandler_ = eventHandler;
    self_ = shared_from_this();

    // For release builds, we simply use a raw pointer to this object as the
    // callback data. For debug builds, we leak a weak pointer to this object on
    // purpose so that we can catch misbehaving plugins calling callbacks after
    // shutdown (see function getContext_).
#ifdef NDEBUG
    void* callbackData = (void*)this;
#else
    void* callbackData = (void*)(new weak_ptr<ViceContext>(shared_from_this()));
#endif

    VicePluginAPI_Callbacks callbacks;
    memset(&callbacks, 0, sizeof(VicePluginAPI_Callbacks));

    callbacks.eventNotify = CTX_CALLBACK_WITHOUT_PUMPEVENTS_CHECK(void, (), {
        if(!self->pumpEventsInQueue_.exchange(true)) {
            postTask(self, &ViceContext::pumpEvents_);
        }
    });

    callbacks.shutdownComplete = CTX_CALLBACK(void, (), {
        self->shutdownComplete_();
    });

    callbacks.createWindow = CTX_CALLBACK(uint64_t, (char** msg), {
        string reason = "Unknown reason";
        uint64_t window =
            self->eventHandler_->onViceContextCreateWindowRequest(reason);
        if(window || msg == nullptr) {
            REQUIRE(self->openWindows_.insert(window).second);
            return window;
        } else {
            *msg = createMallocString(reason);
            return 0;
        }
    });

    callbacks.closeWindow = CTX_CALLBACK(void, (uint64_t window), {
        REQUIRE(self->openWindows_.erase(window));
        self->eventHandler_->onViceContextCloseWindow(window);
    });

    callbacks.resizeWindow = CTX_CALLBACK(void, (
        uint64_t window,
        size_t width,
        size_t height
    ), {
    });

    callbacks.fetchWindowImage = CTX_CALLBACK(void, (
        uint64_t window,
        void (*putImageFunc)(
            void* putImageFuncData,
            const uint8_t* image,
            size_t width,
            size_t height,
            size_t pitch
        ),
        void* putImageFuncData
    ), {
        REQUIRE(self->openWindows_.count(window));

        bool putImageCalled = false;
        self->eventHandler_->onViceContextFetchWindowImage(
            window,
            [&](
                const uint8_t* image,
                size_t width,
                size_t height,
                size_t pitch
            ) {
                REQUIRE(!putImageCalled);
                putImageCalled = true;

                REQUIRE(width);
                REQUIRE(height);
                putImageFunc(putImageFuncData, image, width, height, pitch);
            }
        );
        REQUIRE(putImageCalled);
    });

    callbacks.mouseDown = CTX_CALLBACK(void, (
        uint64_t window, int x, int y, int button
    ), {
    });

    callbacks.mouseUp = CTX_CALLBACK(void, (
        uint64_t window, int x, int y, int button
    ), {
    });

    callbacks.mouseMove = CTX_CALLBACK(void, (uint64_t window, int x, int y), {
    });

    callbacks.mouseDoubleClick = CTX_CALLBACK(void, (
        uint64_t window, int x, int y, int button
    ), {
    });

    callbacks.mouseWheel = CTX_CALLBACK(void, (
        uint64_t window, int x, int y, int dx, int dy
    ), {
    });

    callbacks.mouseLeave = CTX_CALLBACK(void, (uint64_t window, int x, int y), {
    });

    callbacks.keyDown = CTX_CALLBACK(void, (uint64_t window, int key), {
    });

    callbacks.keyUp = CTX_CALLBACK(void, (uint64_t window, int key), {
    });

    callbacks.loseFocus = CTX_CALLBACK(void, (uint64_t window), {
    });

    plugin_->apiFuncs_->start(
        ctx_,
        callbacks,
        callbackData
    );
}

void ViceContext::shutdown() {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Running);
    REQUIRE(!shutdownPending_);

    shutdownPending_ = true;
    plugin_->apiFuncs_->shutdown(ctx_);
}

void ViceContext::closeWindow(uint64_t window) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Running);
    REQUIRE(threadActivePumpEventsContext == nullptr);

    REQUIRE(openWindows_.erase(window));
    plugin_->apiFuncs_->closeWindow(ctx_, window);
}

void ViceContext::notifyWindowViewChanged(uint64_t window) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Running);
    REQUIRE(threadActivePumpEventsContext == nullptr);
    REQUIRE(openWindows_.count(window));

    plugin_->apiFuncs_->notifyWindowViewChanged(ctx_, window);
}

shared_ptr<ViceContext> ViceContext::getContext_(void* callbackData) {
    if(callbackData == nullptr) {
        PANIC("Vice plugin sent unexpected NULL pointer as callback data");
    }

#ifdef NDEBUG
    return ((ViceContext*)callbackData)->shared_from_this();
#else
    if(shared_ptr<ViceContext> self = ((weak_ptr<ViceContext>*)callbackData)->lock()) {
        if(self->state_ == ShutdownComplete) {
            PANIC("Vice plugin called a callback for a context that has shut down");
        }
        REQUIRE(self->state_ == Running);
        return self;
    } else {
        PANIC("Vice plugin called a callback for a context that has been destroyed");
        abort();
    }
#endif
}

void ViceContext::pumpEvents_() {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Running);
    REQUIRE(threadActivePumpEventsContext == nullptr);

    pumpEventsInQueue_.store(false);

    threadActivePumpEventsContext = this;
    plugin_->apiFuncs_->pumpEvents(ctx_);
    threadActivePumpEventsContext = nullptr;
}

void ViceContext::shutdownComplete_() {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Running);
    REQUIRE(shutdownPending_);

    state_ = ShutdownComplete;
    shutdownPending_ = false;
    self_.reset();

    REQUIRE(eventHandler_);
    postTask(
        eventHandler_,
        &ViceContextEventHandler::onViceContextShutdownComplete
    );
    eventHandler_.reset();
}
