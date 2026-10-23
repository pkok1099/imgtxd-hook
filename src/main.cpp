// libimgtxd_hook.so — AML plugin for GTA SA 2.10 (64-bit)
//
// Bridges ImgTxd Studio (companion app) into the running game:
//   * IPC listener on TCP loopback (127.0.0.1:38177, protocol v1.1)
//     receiving model substitution payloads streamed inline + reload
//     triggers. (v1.0 used an abstract Unix socket with SCM_RIGHTS FD
//     passing; that transport was replaced because SELinux per-app
//     isolation blocks abstract-socket connects between different apps —
//     verified on-device 2026-10-23.)
//   * Inline hook on CStreaming::ConvertBufferToObject: when a substitution is
//     pending for the modelId being converted, the game's buffer pointer is
//     replaced with our payload buffer and CStreamingInfo.CdSize is patched
//     for the duration of the call (then restored).
//   * Inline hook on OS_ScreenSwapBuffers: drains the RELOAD trigger queue and
//     the generic GL job queue on the render thread (EGL-safe point).
//     Since plugin v1.2 each drained RELOAD is gated on the model's
//     streaming state + refcount (see PerformReload) — an in-use model is
//     never force-removed.
//
// Game addresses are resolved at runtime by mangled symbol name
// (aml->GetSym, dlsym fallback). Hardcoded VAs for the reference build are a
// last-resort fallback, used only if the loaded libGTASA.so length matches
// the verified reference build exactly, and are always logged as such.

#include <mod/amlmod.h>
#include <mod/logger.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

MYMOD(net.imgtxd.hook, ImgTxd Hook, 1.2, ImgTxd Studio)

// ---------------------------------------------------------------------------
// Reference build constants (GTA SA 2.10 64-bit, CLEO/AML build).
// Verified against libGTASA-2.10.so (54,731,968 bytes, full symbol table).
// Used ONLY as a last-resort fallback when symbol resolution fails AND the
// loaded library length matches the reference build exactly.
// ---------------------------------------------------------------------------
static const size_t kRefLibLength = 54731968;

// CStreamingInfo layout (stride 20), see reference RE notes Part I.
// State offset proven from the 2.10 binary: CStreaming::RemoveModel
// (0x391FF0) does `ldrb w10, [info + 0x10]` and branches on it
// (0 = not in use, 1 = LOADED -> destructive DeleteRwObject path).
static const size_t kInfoStride   = 20;
static const size_t kInfoCdSizeOff = 0x0C;
static const size_t kInfoStateOff = 0x10;
static const uint8_t kStateLoaded = 1;
static const int    kInfoCount    = 26316; // 526320 / 20

// CBaseModelInfo refcount offset, proven from the 2.10 binary:
// CBaseModelInfo::AddRef (0x45B528) / RemoveRef (0x45B53C) do
// ldrh/strh at [modelInfo + 0x22], and the game's own purge path
// CStreaming::RemoveLeastUsedModel (0x397D24) skips any model whose
// uint16 at +0x22 is nonzero. (Note: +0x24 is NOT the refcount — it is
// the int16 TxdIndex that AddRef/RemoveRef forward to CTxdStore.)
static const size_t kModelRefCountOff = 0x22;

// IPC protocol v1.1 (see docs/PROTOCOL.md)
static const uint32_t kIpcMagic      = 0x44585449; // 'ITXD'
static const size_t   kMaxPayload    = 512ull * 1024 * 1024;
static const uint16_t kTcpPort       = 38177; // loopback only

struct IpcHeader
{
    uint32_t magic;       // +0x00 kIpcMagic
    uint16_t cmd;         // +0x04 1=SUBSTITUTE 2=RELOAD 3=PING
    uint16_t reserved;    // +0x06
    int32_t  modelId;     // +0x08
    uint32_t pad;         // +0x0C (alignment)
    uint64_t payloadSize; // +0x10 SUBSTITUTE only
};
static_assert(sizeof(IpcHeader) == 24, "IpcHeader must be 24 bytes");

// ---------------------------------------------------------------------------
// Game symbols (mangled names as exported by libGTASA.so)
// ---------------------------------------------------------------------------
static const char* kSymConvertBufferToObject = "_ZN10CStreaming21ConvertBufferToObjectEPcii";
static const char* kSymSwapBuffers           = "_Z20OS_ScreenSwapBuffersv";
static const char* kSymRequestModel          = "_ZN10CStreaming12RequestModelEii";
static const char* kSymRemoveModel           = "_ZN10CStreaming11RemoveModelEi";
static const char* kSymInfoForModel          = "_ZN10CStreaming16ms_aInfoForModelE";
static const char* kSymModelInfoPtrs         = "_ZN10CModelInfo16ms_modelInfoPtrsE";
static const char* kSymVehiclePool           = "_ZN6CPools15ms_pVehiclePoolE";
static const char* kSymTxdGetCurrent         = "_Z25RwTexDictionaryGetCurrentv";
static const char* kSymTxdFindNamed          = "_Z31RwTexDictionaryFindNamedTextureP15RwTexDictionaryPKc";

struct VaFallback { const char* sym; uintptr_t va; };
static const VaFallback kVaFallbacks[] = {
    { "_ZN10CStreaming21ConvertBufferToObjectEPcii",              0x395114 },
    { "_Z20OS_ScreenSwapBuffersv",                                 0x31E2A4 },
    { "_ZN10CStreaming12RequestModelEii",                          0x3949E0 },
    { "_ZN10CStreaming11RemoveModelEi",                            0x391FF0 },
    { "_ZN10CStreaming16ms_aInfoForModelE",                        0x8F2184 },
    { "_ZN10CModelInfo16ms_modelInfoPtrsE",                        0xB666E0 },
    { "_ZN6CPools15ms_pVehiclePoolE",                              0xBC3BC0 },
    { "_Z25RwTexDictionaryGetCurrentv",                            0x27427C },
    { "_Z31RwTexDictionaryFindNamedTextureP15RwTexDictionaryPKc",  0x27417C },
};

static uintptr_t g_libBase   = 0;
static size_t    g_libLength = 0;
static void*     g_dlHandle  = nullptr;

static uint8_t* g_pInfoForModel = nullptr; // CStreaming::ms_aInfoForModel array
static uint8_t** g_ppModelInfoPtrs = nullptr; // CModelInfo::ms_modelInfoPtrs array (CBaseModelInfo* per modelId)
static void*    g_pVehiclePool  = nullptr; // resolved for the record; pool iteration is a v2 item

typedef void (*FnRemoveModel)(int);
typedef void (*FnRequestModel)(int, int);
static FnRemoveModel  g_RemoveModel  = nullptr;
static FnRequestModel g_RequestModel = nullptr;

typedef void* (*FnTxdGetCurrent)();
typedef void* (*FnTxdFindNamed)(void*, const char*);
static FnTxdGetCurrent g_TxdGetCurrent = nullptr;
static FnTxdFindNamed  g_TxdFindNamed  = nullptr;

static uintptr_t ResolveSym(const char* name)
{
    uintptr_t addr = 0;
    const char* how = "none";
    if (aml && g_libBase)
    {
        addr = aml->GetSym(g_libBase, name);
        if (addr) how = "aml->GetSym";
    }
    if (!addr && g_dlHandle)
    {
        dlerror();
        void* p = dlsym(g_dlHandle, name);
        if (p) { addr = (uintptr_t)p; how = "dlsym"; }
    }
    if (!addr && g_libBase && g_libLength == kRefLibLength)
    {
        for (const auto& fb : kVaFallbacks)
        {
            if (strcmp(fb.sym, name) == 0) { addr = g_libBase + fb.va; how = "VA-FALLBACK"; break; }
        }
    }
    logger->Info("sym %-58s -> %s via %s @ " PTRFMT, name, addr ? "OK" : "FAIL", how, addr);
    return addr;
}

// ---------------------------------------------------------------------------
// Shared state (all guarded by g_mu; actions run AFTER unlocking)
// ---------------------------------------------------------------------------
struct PendingSub
{
    uint8_t* ptr  = nullptr; // heap buffer owned by the plugin (payload copy)
    size_t   size = 0;       // payload bytes
};

struct GLJob
{
    enum Type { LOG } type;
    std::string msg;
};

static std::mutex                          g_mu;
static std::unordered_map<int, PendingSub> g_pending;
static std::deque<int>                     g_reloadQueue;
static std::deque<GLJob>                   g_glJobs;

static void FreePending(PendingSub& p)
{
    free(p.ptr);
    p.ptr = nullptr; p.size = 0;
}

// Returns true and fills `out` if a pending substitution existed.
static bool TakePending(int modelId, PendingSub& out)
{
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_pending.find(modelId);
    if (it == g_pending.end()) return false;
    out = it->second;
    g_pending.erase(it);
    return true;
}

static void EnqueueGLLog(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_glJobs.push_back(GLJob{ GLJob::LOG, msg });
}

// ---------------------------------------------------------------------------
// Hook: CStreaming::ConvertBufferToObject(char* lpBuffer, int modelId, int channel)
// ---------------------------------------------------------------------------
DECL_HOOKv(ConvertBufferToObject, char* lpBuffer, int modelId, int channel)
{
    PendingSub sub;
    if (TakePending(modelId, sub))
    {
        bool usable = sub.ptr != nullptr && sub.size > 0 &&
                      modelId >= 0 && modelId < kInfoCount && g_pInfoForModel != nullptr;
        if (usable)
        {
            uint32_t* pCdSize = reinterpret_cast<uint32_t*>(
                g_pInfoForModel + (size_t)modelId * kInfoStride + kInfoCdSizeOff);
            uint32_t oldCdSize = *pCdSize;
            uint32_t newCdSize = (uint32_t)((sub.size + 2047) / 2048);
            *pCdSize = newCdSize;
            logger->Info("CBO: model %d substitute %zu bytes (CdSize %u -> %u), channel %d",
                         modelId, sub.size, oldCdSize, newCdSize, channel);
            ConvertBufferToObject((char*)sub.ptr, modelId, channel);
            *pCdSize = oldCdSize;
            logger->Info("CBO: model %d done, CdSize restored to %u", modelId, oldCdSize);
            FreePending(sub);
            return;
        }
        logger->Error("CBO: model %d pending substitution unusable (ptr=%p size=%zu), passing through",
                      modelId, sub.ptr, sub.size);
        FreePending(sub);
    }
    ConvertBufferToObject(lpBuffer, modelId, channel);
}

// ---------------------------------------------------------------------------
// Gated reload (plugin v1.2)
//
// CStreaming::RemoveModel on a LOADED model destroys its RwObject without
// consulting the model refcount (proven by disassembly of the 2.10
// binary), so calling it on a model that live instances still reference
// is a use-after-free and force-closes the game. The game itself only
// reaches RemoveModel through refcount-based purge paths, so the drain
// now gates on the same two facts those paths check first: the
// CStreamingInfo state byte and the CBaseModelInfo refcount.
// Fail-safe: anything unreadable means "skip", never the old behaviour.
// ---------------------------------------------------------------------------
static void PerformReload(int modelId)
{
    if (modelId < 0 || modelId >= kInfoCount || !g_pInfoForModel || !g_RequestModel)
    {
        logger->Error("RELOAD: model %d skipped (id out of range or streaming state unresolved)", modelId);
        return;
    }
    const uint8_t* info = g_pInfoForModel + (size_t)modelId * kInfoStride;
    const uint8_t state = info[kInfoStateOff];
    if (state != kStateLoaded)
    {
        logger->Info("RELOAD: model %d state=%u refcount=n/a decision=request-only "
                     "(not loaded, RequestModel(0x14))",
                     modelId, (unsigned)state);
        g_RequestModel(modelId, 0x14);
        return;
    }
    // state == LOADED: the refcount decides whether removal is safe.
    if (!g_ppModelInfoPtrs)
    {
        logger->Error("RELOAD: model %d state=%u refcount=unreadable (ms_modelInfoPtrs unresolved) "
                      "decision=skip (fail-safe)",
                      modelId, (unsigned)state);
        return;
    }
    const uint8_t* modelInfo = g_ppModelInfoPtrs[modelId];
    if (!modelInfo)
    {
        logger->Error("RELOAD: model %d state=%u refcount=unreadable (modelInfo is null) "
                      "decision=skip (fail-safe)",
                      modelId, (unsigned)state);
        return;
    }
    const uint16_t refCount = *reinterpret_cast<const uint16_t*>(modelInfo + kModelRefCountOff);
    if (refCount > 0)
    {
        logger->Info("RELOAD: model %d state=%u refcount=%u decision=skip (in use; "
                     "pending substitution stays armed)",
                     modelId, (unsigned)state, (unsigned)refCount);
        return;
    }
    if (!g_RemoveModel)
    {
        logger->Error("RELOAD: model %d state=%u refcount=0 decision=skip (RemoveModel unresolved)",
                      modelId, (unsigned)state);
        return;
    }
    logger->Info("RELOAD: model %d state=%u refcount=0 decision=remove+request "
                 "(RemoveModel + RequestModel(0x14)) on render thread",
                 modelId, (unsigned)state);
    g_RemoveModel(modelId);
    g_RequestModel(modelId, 0x14);
}

// ---------------------------------------------------------------------------
// Hook: OS_ScreenSwapBuffers() — render-thread drain point
// ---------------------------------------------------------------------------
DECL_HOOKv(OS_ScreenSwapBuffers)
{
    // Move queues out under the lock; call game code only after unlocking.
    std::deque<int>   reloads;
    std::deque<GLJob> jobs;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        reloads.swap(g_reloadQueue);
        jobs.swap(g_glJobs);
    }
    for (int modelId : reloads)
    {
        PerformReload(modelId);
    }
    for (const GLJob& job : jobs)
    {
        if (job.type == GLJob::LOG)
            logger->Info("GL job (render thread): %s", job.msg.c_str());
    }
    OS_ScreenSwapBuffers();
}

// ---------------------------------------------------------------------------
// IPC listener thread
// ---------------------------------------------------------------------------
static bool RecvFull(int fd, void* buf, size_t len)
{
    char* p = (char*)buf;
    size_t got = 0;
    while (got < len)
    {
        ssize_t r = recv(fd, p + got, len - got, 0);
        if (r == 0) return false;
        if (r < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
        got += (size_t)r;
    }
    return true;
}

static void HandleSubstitute(const IpcHeader& hdr, int cfd)
{
    if (hdr.payloadSize == 0 || hdr.payloadSize > kMaxPayload)
    {
        logger->Error("IPC SUBSTITUTE model %d: bad payloadSize %llu, ignored",
                      hdr.modelId, (unsigned long long)hdr.payloadSize);
        return;
    }
    // Protocol v1.1: payload is streamed inline right after the header.
    // Read it fully (the socket already carries the 5s receive timeout);
    // a short read (timeout / EOF) rejects the message with no state change.
    uint8_t* buf = (uint8_t*)malloc((size_t)hdr.payloadSize);
    if (!buf)
    {
        logger->Error("IPC SUBSTITUTE model %d: malloc(%llu) failed",
                      hdr.modelId, (unsigned long long)hdr.payloadSize);
        return;
    }
    if (!RecvFull(cfd, buf, (size_t)hdr.payloadSize))
    {
        logger->Error("IPC SUBSTITUTE model %d: short payload (wanted %llu bytes), ignored",
                      hdr.modelId, (unsigned long long)hdr.payloadSize);
        free(buf);
        return;
    }
    PendingSub fresh{ buf, (size_t)hdr.payloadSize };
    PendingSub replaced;
    bool hadOld = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_pending.find(hdr.modelId);
        if (it != g_pending.end()) { replaced = it->second; hadOld = true; }
        g_pending[hdr.modelId] = fresh;
    }
    if (hadOld) FreePending(replaced);
    logger->Info("IPC SUBSTITUTE model %d: %llu bytes buffered @ %p%s",
                 hdr.modelId, (unsigned long long)hdr.payloadSize, (void*)buf,
                 hadOld ? " (replaced previous pending)" : "");
}

static void HandleClient(int cfd)
{
    timeval tv{ 5, 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    IpcHeader hdr;
    if (!RecvFull(cfd, &hdr, sizeof(hdr)))
    {
        logger->Error("IPC: short header, dropped");
        close(cfd);
        return;
    }

    if (hdr.magic != kIpcMagic)
    {
        logger->Error("IPC: bad magic 0x%08X, dropped", hdr.magic);
        close(cfd);
        return;
    }

    switch (hdr.cmd)
    {
    case 3: // PING
    {
        const uint8_t ok = 0x01;
        send(cfd, &ok, 1, MSG_NOSIGNAL);
        logger->Info("IPC: PING -> pong");
        break;
    }
    case 1: // SUBSTITUTE
        HandleSubstitute(hdr, cfd);
        break;
    case 2: // RELOAD
    {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_reloadQueue.push_back(hdr.modelId);
        }
        EnqueueGLLog("reload requested for model " + std::to_string(hdr.modelId));
        logger->Info("IPC: RELOAD model %d queued", hdr.modelId);
        break;
    }
    default:
        logger->Error("IPC: unknown cmd %u, dropped", hdr.cmd);
        break;
    }
    close(cfd);
}

static void* IpcThreadMain(void*)
{
    // TCP loopback (protocol v1.1): cross-app abstract Unix sockets are
    // blocked by SELinux per-app isolation; loopback TCP is not, and the
    // game APK holds the INTERNET permission.
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0)
    {
        logger->Error("IPC: socket() failed errno=%d; listener thread stops", errno);
        return nullptr;
    }
    int reuse = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
    in.sin_port = htons(kTcpPort);
    if (bind(srv, (struct sockaddr*)&in, sizeof(in)) != 0)
    {
        logger->Error("IPC: bind(tcp 127.0.0.1:%u) failed errno=%d (port in use?); listener thread stops",
                      (unsigned)kTcpPort, errno);
        close(srv);
        return nullptr;
    }
    if (listen(srv, 8) != 0)
    {
        logger->Error("IPC: listen() failed errno=%d; listener thread stops", errno);
        close(srv);
        return nullptr;
    }
    logger->Info("IPC: listening on tcp 127.0.0.1:%u", (unsigned)kTcpPort);
    for (;;)
    {
        int cfd = accept(srv, nullptr, nullptr);
        if (cfd < 0)
        {
            if (errno == EINTR) continue;
            logger->Error("IPC: accept() failed errno=%d", errno);
            break;
        }
        HandleClient(cfd); // one message per connection
    }
    close(srv);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Mod entry
// ---------------------------------------------------------------------------
ON_MOD_LOAD()
{
    logger->SetTag("ImgTxdHook");
    logger->Info("ImgTxdHook v1.2 loading (GTA SA 2.10 arm64 target)");

    g_libBase = aml->GetLib("libGTASA.so");
    if (!g_libBase)
    {
        logger->Error("libGTASA.so base not found via AML, trying dlopen");
        g_dlHandle = dlopen("libGTASA.so", RTLD_NOW | RTLD_NOLOAD);
        if (g_dlHandle) g_libBase = aml->GetLibXDL(g_dlHandle);
    }
    else
    {
        g_dlHandle = dlopen("libGTASA.so", RTLD_NOW | RTLD_NOLOAD);
    }
    g_libLength = aml->GetLibLength("libGTASA.so");
    logger->Info("libGTASA.so base=" PTRFMT " length=%zu (reference %zu)",
                 g_libBase, g_libLength, kRefLibLength);
    if (!g_libBase)
    {
        logger->Error("Cannot resolve game base; hooks disabled, IPC still starts");
    }

    uintptr_t aCBO   = ResolveSym(kSymConvertBufferToObject);
    uintptr_t aSwap  = ResolveSym(kSymSwapBuffers);
    uintptr_t aReq   = ResolveSym(kSymRequestModel);
    uintptr_t aRem   = ResolveSym(kSymRemoveModel);
    uintptr_t aInfo  = ResolveSym(kSymInfoForModel);
    uintptr_t aPtrs  = ResolveSym(kSymModelInfoPtrs);
    uintptr_t aPool  = ResolveSym(kSymVehiclePool);
    uintptr_t aTxdCur  = ResolveSym(kSymTxdGetCurrent);
    uintptr_t aTxdFind = ResolveSym(kSymTxdFindNamed);

    if (aInfo) g_pInfoForModel = (uint8_t*)aInfo;
    if (aPtrs) g_ppModelInfoPtrs = (uint8_t**)aPtrs;
    if (aPool) g_pVehiclePool = *(void**)aPool; // CPools::ms_pVehiclePool is a pointer object
    if (aReq)  g_RequestModel = (FnRequestModel)aReq;
    if (aRem)  g_RemoveModel  = (FnRemoveModel)aRem;
    if (aTxdCur)  g_TxdGetCurrent = (FnTxdGetCurrent)aTxdCur;
    if (aTxdFind) g_TxdFindNamed  = (FnTxdFindNamed)aTxdFind;
    // NOTE (stretch, intentionally not wired in v1): named-texture
    // glTexSubImage2D updates need a TEXUPDATE IPC command (texture name +
    // pixels) which the v1 protocol does not define. Both Rw symbols above
    // are resolved and logged so the capability status is visible in logcat;
    // the GL job queue drained in OS_ScreenSwapBuffers is the execution point
    // ready for that job type. See README "TODO".

    if (aCBO)
    {
        bool ok = aml->Hook((void*)aCBO, (void*)&HookOf_ConvertBufferToObject, (void**)&ConvertBufferToObject);
        logger->Info("hook ConvertBufferToObject: %s", ok ? "OK" : "FAIL");
    }
    if (aSwap)
    {
        bool ok = aml->Hook((void*)aSwap, (void*)&HookOf_OS_ScreenSwapBuffers, (void**)&OS_ScreenSwapBuffers);
        logger->Info("hook OS_ScreenSwapBuffers: %s", ok ? "OK" : "FAIL");
    }

    EnqueueGLLog("ImgTxdHook GL job queue alive (self-test job)");

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &attr, IpcThreadMain, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) logger->Error("IPC: pthread_create failed rc=%d", rc);

    logger->Info("ImgTxdHook v1.2 load complete");
}
