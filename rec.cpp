// rec.cpp â Rec.app (macOS 14.4+): index.html ã§ã¢ããªãé¸ãã§ WAV ã«é²é³
//
// æ§æ:
//   * HTTP ãµã¼ãã¼ (127.0.0.1:8765) â index.html ã® UI ã¨ JSON API ã®ã¿
//     (é³å£°ã¯ãããã«æµããªãããã©ã¦ã¶ã¯é²é³UIã¨ãã¦ã ãä½¿ã)
//   * ãã¼ã¸ã§ã¢ããªã«ãã§ãã¯ â /start â NSSavePanel ã§ä¿å­åãèã
//     â ãã­ã»ã¹ã¿ãã + éå¬é aggregate â IOProc â ring â WAV ã«æ¸ãç¶ãã
//   * ãã¼ã¸ã®ãåæ­¢ãã¾ãã¯ Dock Quit / Ctrl+C ã§ãã¡ã¤ã«ãéãã¦çµäº
//
// ä½¿ãæ¹:
//   open Rec.app          â http://127.0.0.1:8765/ ãéããããã§å¨é¨ãã
//   rec Spotify           (CLI æå®ãå¯ããã®å ´åãåããä¿å­ããã«ãåºã)
//   rec --list            é³ãåºãã¦ããã¢ããªã®ä¸è¦§
//   åºå: ä¿å­ããã«ã§é¸ãã å ´æ (<ã¢ããªå>-<æ¥ä»>.wav ãåæå¤)
//
// API (index.html ãä½¿ã):
//   GET  /processes   ã¢ããªä¸è¦§
//   GET  /info        é²é³ä¸­ãã©ãã¯ä¸è¦§
//   POST /start?apps=45,52&name=Spotify   é²é³éå§ (ä¿å­ããã«ãã«åºã)
//   POST /remove?id=N                     ãã®ãã©ãã¯ãåæ­¢ãã¦ä¿å­
//   POST /quit                            å¨é¨åæ­¢ãã¦ä¿å­ãçµäº

#define CATapDescription void  // SDK prototype uses an ObjC class; C++ sees void*
#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/AudioHardwareTapping.h>
#undef CATapDescription

extern "C" OSStatus AudioHardwareCreateProcessTap(void* inDescription, AudioObjectID* outTapID);
extern "C" OSStatus AudioHardwareDestroyProcessTap(AudioObjectID inTapID);

#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <libproc.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

static constexpr int kPort = 8765;

// ---------------------------------------------------------------------------
// ObjC runtime shorthands (this file stays plain C++)

static id oc(const char* c) { return (id)objc_getClass(c); }
static id send0(id self, const char* s) { return ((id(*)(id, SEL))objc_msgSend)(self, sel_registerName(s)); }
static id send1(id self, const char* s, id a) { return ((id(*)(id, SEL, id))objc_msgSend)(self, sel_registerName(s), a); }
static id sendU(id self, const char* s, unsigned v) { return ((id(*)(id, SEL, unsigned))objc_msgSend)(self, sel_registerName(s), v); }
static id sendI(id self, const char* s, int v) { return ((id(*)(id, SEL, int))objc_msgSend)(self, sel_registerName(s), v); }
static id nsStr(const std::string& s) {
    return ((id(*)(id, SEL, const char*))objc_msgSend)(oc("NSString"), sel_registerName("stringWithUTF8String:"), s.c_str());
}
static const char* utf8(id s) { return ((const char* (*)(id, SEL))objc_msgSend)(s, sel_registerName("UTF8String")); }

// ---------------------------------------------------------------------------
// Core Audio property helpers

static std::string cfStr(AudioObjectID obj, AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef s = nullptr;
    UInt32 sz = sizeof(s);
    if (AudioObjectGetPropertyData(obj, &a, 0, nullptr, &sz, &s) != noErr || !s) return "";
    char buf[512] = {0};
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(s);
    return buf;
}

template <typename T>
static bool getVal(AudioObjectID obj, AudioObjectPropertySelector sel, T& out) {
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = sizeof(T);
    return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &sz, &out) == noErr;
}

static std::vector<AudioObjectID> processObjects() {
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyProcessObjectList,
                                    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz);
    std::vector<AudioObjectID> v(sz / sizeof(AudioObjectID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, v.data());
    return v;
}

static std::string defaultOutputUID() {
    AudioObjectID dev = 0;
    getVal(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultSystemOutputDevice, dev);
    return cfStr(dev, kAudioDevicePropertyDeviceUID);
}

// ---------------------------------------------------------------------------
// Process description / name matching

static std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static std::string displayName(pid_t pid, const std::string& bundle) {
    std::string name;
    if (pid > 0) {
        id pool = send0(send0(oc("NSAutoreleasePool"), "alloc"), "init");
        id app = sendI(oc("NSRunningApplication"), "runningApplicationWithProcessIdentifier:", (int)pid);
        if (app) {
            id ns = send0(app, "localizedName");
            if (ns) {
                const char* u = utf8(ns);
                if (u) name = u;
            }
        }
        send0(pool, "drain");
        if (name.empty()) {
            char path[4096];
            if (proc_pidpath(pid, path, sizeof path) > 0) {
                const char* b = strrchr(path, '/');
                name = b ? b + 1 : path;
            }
        }
    }
    if (name.empty()) name = bundle;
    return name;
}

struct ProcInfo {
    AudioObjectID id = 0;
    pid_t pid = 0;
    std::string bundle, name, path;
    bool out = false, in = false;
};

static ProcInfo describeProcess(AudioObjectID p) {
    ProcInfo i;
    i.id = p;
    getVal(p, kAudioProcessPropertyPID, i.pid);
    i.bundle = cfStr(p, kAudioProcessPropertyBundleID);
    UInt32 f = 0;
    if (getVal(p, kAudioProcessPropertyIsRunningOutput, f)) i.out = f != 0;
    f = 0;
    if (getVal(p, kAudioProcessPropertyIsRunningInput, f)) i.in = f != 0;
    if (i.pid > 0) {
        char path[4096];
        if (proc_pidpath(i.pid, path, sizeof path) > 0) i.path = path;
    }
    i.name = displayName(i.pid, i.bundle);
    return i;
}

static AudioObjectID findProcessByName(const std::string& q, std::string& err) {
    std::string lq = lower(q);
    AudioObjectID partial = 0;
    int partials = 0;
    for (AudioObjectID p : processObjects()) {
        ProcInfo i = describeProcess(p);
        std::string n = lower(i.name), b = lower(i.bundle);
        if (n == lq || b == lq) return p;
        if (n.find(lq) != std::string::npos || b.find(lq) != std::string::npos) { partial = p; ++partials; }
    }
    err = partials ? "ambiguous name: " + q : "no audio process named: " + q;
    return 0;
}

static AudioObjectID findProcessByID(const std::string& s) {
    char* end = nullptr;
    unsigned long v = strtoul(s.c_str(), &end, 10);
    if (!end || *end != '\0' || v == 0) return 0;
    for (AudioObjectID p : processObjects())
        if (p == (AudioObjectID)v) return p;
    return 0;
}

// ---------------------------------------------------------------------------
// Tap + private aggregate

static OSStatus createTap(AudioObjectID proc, AudioObjectID& tapOut) {
    id num = sendU(oc("NSNumber"), "numberWithUnsignedInt:", proc);
    id arr = send1(oc("NSArray"), "arrayWithObject:", num);
    id desc = send1(send0(oc("CATapDescription"), "alloc"), "initStereoMixdownOfProcesses:", arr);
    return AudioHardwareCreateProcessTap(static_cast<void*>(desc), &tapOut);
}

static CFStringRef cfs(const std::string& s) {
    return CFStringCreateWithCString(nullptr, s.c_str(), kCFStringEncodingUTF8);
}

static CFDictionaryRef mkDict(const std::vector<std::pair<const char*, CFTypeRef>>& kv) {
    std::vector<const void*> ks, vs;
    for (const auto& p : kv) {
        ks.push_back(CFStringCreateWithCString(nullptr, p.first, kCFStringEncodingUTF8));
        vs.push_back(p.second);
    }
    return CFDictionaryCreate(nullptr, ks.data(), vs.data(), (CFIndex)kv.size(),
                              &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

static CFArrayRef mkArr1(CFTypeRef v) {
    const void* x = v;
    return CFArrayCreate(nullptr, &x, 1, &kCFTypeArrayCallBacks);
}

static OSStatus createAggregate(AudioObjectID tap, const std::string& tapUID, const std::string& outUID,
                                const std::string& name, AudioObjectID& aggOut) {
    CFStringRef outCF = cfs(outUID);
    CFStringRef tapCF = cfs(tapUID);
    CFStringRef nameCF = cfs(name);
    CFStringRef uidCF = CFStringCreateWithFormat(nullptr, nullptr, CFSTR("local.rec.app.%u"), tap);
    CFDictionaryRef sub = mkDict({{kAudioSubDeviceUIDKey, outCF}});
    CFDictionaryRef tapDict = mkDict({{kAudioSubTapDriftCompensationKey, kCFBooleanTrue},
                                      {kAudioSubTapUIDKey, tapCF}});
    CFDictionaryRef desc = mkDict({
        {kAudioAggregateDeviceNameKey, nameCF},
        {kAudioAggregateDeviceUIDKey, uidCF},
        {kAudioAggregateDeviceIsPrivateKey, kCFBooleanTrue},
        {kAudioAggregateDeviceTapAutoStartKey, kCFBooleanTrue},
        {kAudioAggregateDeviceMainSubDeviceKey, outCF},
        {kAudioAggregateDeviceSubDeviceListKey, mkArr1(sub)},
        {kAudioAggregateDeviceTapListKey, mkArr1(tapDict)},
    });
    return AudioHardwareCreateAggregateDevice(desc, &aggOut);
}

// ---------------------------------------------------------------------------
// WAV writer (32-bit float, stereo)

struct Wav {
    FILE* f = nullptr;
    uint64_t frames = 0;

    bool open(const std::string& path, double sampleRate) {
        f = fopen(path.c_str(), "wb");
        if (!f) return false;
        unsigned char h[44] = {0};
        memcpy(h + 0, "RIFF", 4);
        memcpy(h + 8, "WAVE", 4);
        memcpy(h + 12, "fmt ", 4);
        uint32_t fmtSize = 16;  memcpy(h + 16, &fmtSize, 4);
        uint16_t fmt = 3;       memcpy(h + 20, &fmt, 2);          // IEEE float
        uint16_t ch = 2;       memcpy(h + 22, &ch, 2);
        uint32_t r = (uint32_t)sampleRate; memcpy(h + 24, &r, 4);
        uint32_t byteRate = (uint32_t)(sampleRate * 8); memcpy(h + 28, &byteRate, 4);
        uint16_t block = 8;    memcpy(h + 32, &block, 2);
        uint16_t bits = 32;    memcpy(h + 34, &bits, 2);
        memcpy(h + 36, "data", 4);
        fwrite(h, 1, 44, f);
        return true;
    }

    void append(const float* buf, size_t nFrames) {
        if (f && nFrames) { fwrite(buf, sizeof(float), nFrames * 2, f); frames += nFrames; }
    }

    void close() {
        if (!f) return;
        uint64_t dataBytes = frames * 8;
        uint32_t riff = (uint32_t)(36 + dataBytes);
        fseek(f, 4, SEEK_SET);   fwrite(&riff, 4, 1, f);
        fseek(f, 40, SEEK_SET);  uint32_t d = (uint32_t)dataBytes; fwrite(&d, 4, 1, f);
        fclose(f);
        f = nullptr;
    }
};

// ---------------------------------------------------------------------------
// SPSC ring (frames of interleaved stereo float32). IOProc = producer,
// writer thread = consumer.

struct Ring {
    static constexpr size_t N = 1 << 16;            // frames, power of two (~1.4s @48k)
    std::vector<float> buf = std::vector<float>(N * 2);
    std::atomic<size_t> w{0}, r{0};                 // absolute frame counters
    std::atomic<uint64_t> dropped{0};

    bool push(const float* frames, size_t n) {      // producer (realtime thread)
        size_t w0 = w.load(std::memory_order_relaxed);
        size_t space = N - (w0 - r.load(std::memory_order_acquire));
        if (n > space) { dropped.fetch_add(n - space); n = space; }
        for (size_t i = 0; i < n; i++) {
            size_t p = ((w0 + i) & (N - 1)) * 2;
            buf[p] = frames[2 * i];
            buf[p + 1] = frames[2 * i + 1];
        }
        w.fetch_add(n, std::memory_order_release);
        return true;
    }

    size_t available() const { return w.load(std::memory_order_acquire) - r.load(std::memory_order_relaxed); }

    size_t pop(float* out, size_t outCap) {         // consumer
        size_t n = available();
        if (n > outCap) n = outCap;
        for (size_t i = 0; i < n; i++) {
            size_t p = ((r.load(std::memory_order_relaxed) + i) & (N - 1)) * 2;
            out[2 * i] = buf[p];
            out[2 * i + 1] = buf[p + 1];
        }
        r.fetch_add(n, std::memory_order_release);
        return n;
    }
};

// ---------------------------------------------------------------------------
// Save panel (AppKit). runModal must run on the main thread.

static std::string askSavePath(const std::string& suggested) {
    id panel = send0(oc("NSSavePanel"), "savePanel");
    send1(panel, "setTitle:", nsStr("rec â save location"));
    send1(panel, "setNameFieldStringValue:", nsStr(suggested));
    send1(panel, "setAllowsOtherFileTypes:", (id)kCFBooleanFalse);
    send1(panel, "setAllowedFileTypes:", send1(oc("NSArray"), "arrayWithObject:", nsStr("wav")));
    long r = ((long (*)(id, SEL))objc_msgSend)(panel, sel_registerName("runModal"));
    if (r != 1) return "";
    id url = send0(panel, "URL");
    if (!url) return "";
    const char* p = utf8(send0(url, "path"));
    return p ? p : "";
}

struct PanelCtx { std::promise<std::string> pr; std::string suggested; };
static void runPanel(void* v) { PanelCtx* c = (PanelCtx*)v; c->pr.set_value(askSavePath(c->suggested)); }

// call from any thread: runs the panel on the main thread and waits for it
static std::string askSavePathMain(const std::string& suggested) {
    PanelCtx c;
    c.suggested = suggested;
    dispatch_sync_f(dispatch_get_main_queue(), &c, runPanel);
    return c.pr.get_future().get();
}

// ---------------------------------------------------------------------------
// Track: one tapped app = one wav file

struct Track {
    AudioObjectID processID = 0, tapID = 0, aggID = 0;
    std::string appName, path, err;
    pid_t pid = 0;
    double rate = 48000.0;
    Ring ring;
    Wav wav;
    std::thread writer;
    std::atomic<bool> running{true};
    AudioDeviceIOProcID procID = nullptr;
    std::vector<float> scratch;

    void startWriter() {
        writer = std::thread([this] {
            std::vector<float> out(Ring::N * 2);
            while (running || ring.available()) {
                size_t got = ring.pop(out.data(), 2048);
                if (got) wav.append(out.data(), got);
                else std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    bool start(AudioObjectID proc, const std::string& savePath) {
        processID = proc;
        ProcInfo pi = describeProcess(proc);
        pid = pi.pid;
        appName = pi.name.empty() ? (pi.bundle.empty() ? "process " + std::to_string(proc) : pi.bundle) : pi.name;

        if (createTap(proc, tapID) != noErr) {
            err = "tap failed (System Settings > Privacy & Security > System Audio Recording)";
            return false;
        }
        std::string outUID = defaultOutputUID();
        if (outUID.empty()) { err = "no default output device"; return false; }
        std::string tapUID = cfStr(tapID, kAudioTapPropertyUID);
        if (createAggregate(tapID, tapUID, outUID, "rec " + appName, aggID) != noErr) {
            err = "aggregate device failed";
            return false;
        }
        getVal(aggID, kAudioDevicePropertyNominalSampleRate, rate);

        if (savePath.empty()) { err = "no save location (cancelled)"; return false; }
        path = savePath;
        if (!wav.open(path, rate)) { err = "cannot create " + path; return false; }

        if (AudioDeviceCreateIOProcID(aggID, ioProcEntry, this, &procID) != noErr) {
            err = "cannot create IOProc";
            return false;
        }
        startWriter();
        if (AudioDeviceStart(aggID, procID) != noErr) {
            err = "cannot start device";
            return false;
        }
        printf("recording: %s\n  -> %s\n", appName.c_str(), path.c_str());
        return true;
    }

    // idempotent: safe to call twice
    void stop() {
        if (procID) {
            AudioDeviceStop(aggID, procID);
            AudioDeviceDestroyIOProcID(aggID, procID);
            procID = nullptr;
        }
        running = false;
        if (writer.joinable()) { writer.join(); }
        wav.close();
        if (aggID) { AudioHardwareDestroyAggregateDevice(aggID); aggID = 0; }
        if (tapID) { AudioHardwareDestroyProcessTap(tapID); tapID = 0; }
    }

    // ---- realtime callback ----------------------------------------------
    static OSStatus ioProcEntry(AudioObjectID, const AudioTimeStamp* /*inNow*/,
                                const AudioBufferList* inInput, const AudioTimeStamp* /*inInputTime*/,
                                AudioBufferList* inOutput, const AudioTimeStamp* /*inOutputTime*/,
                                void* inClientData) {
        Track* t = static_cast<Track*>(inClientData);

        for (UInt32 b = 0; inOutput && b < inOutput->mNumberBuffers; b++)
            memset(inOutput->mBuffers[b].mData, 0, inOutput->mBuffers[b].mDataByteSize);

        if (!inInput || inInput->mNumberBuffers == 0) return noErr;

        size_t frames;
        if (inInput->mNumberBuffers >= 2) {          // deinterleaved
            frames = inInput->mBuffers[0].mDataByteSize / (sizeof(float) * inInput->mBuffers[0].mNumberChannels);
            t->scratch.resize(frames * 2);
            const float* L = (const float*)inInput->mBuffers[0].mData;
            const float* R = (const float*)inInput->mBuffers[1].mData;
            for (size_t i = 0; i < frames; i++) { t->scratch[2 * i] = L[i]; t->scratch[2 * i + 1] = R[i]; }
        } else {                                     // interleaved
            const AudioBuffer& b = inInput->mBuffers[0];
            frames = b.mDataByteSize / (sizeof(float) * b.mNumberChannels);
            if (b.mNumberChannels == 2)
                t->scratch.assign((const float*)b.mData, (const float*)b.mData + frames * 2);
            else if (b.mNumberChannels == 1) {
                t->scratch.resize(frames * 2);
                const float* m = (const float*)b.mData;
                for (size_t i = 0; i < frames; i++) { t->scratch[2 * i] = m[i]; t->scratch[2 * i + 1] = m[i]; }
            } else return noErr;
        }
        t->ring.push(t->scratch.data(), frames);
        return noErr;
    }
};

// ---------------------------------------------------------------------------
// Track registry + app lifecycle

static std::mutex gMu;                       // guards gTracks (HTTP threads + main)
static std::vector<Track*> gTracks;
static std::atomic<bool> gRunning{true};
static std::atomic<bool> gDockQuit{false};
static std::string gIndexPath;

static std::string suggestedName(const std::string& appName) {
    char stamp[64] = {0};
    time_t now = time(nullptr);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", localtime(&now));
    std::string safe;
    for (char c : appName) safe += (strchr("\\/:*?\"<>|", c) ? '_' : c);
    return safe + "-" + stamp + ".wav";
}

static std::string trackNameFor(AudioObjectID proc) {
    ProcInfo pi = describeProcess(proc);
    return pi.name.empty() ? (pi.bundle.empty() ? "process " + std::to_string(proc) : pi.bundle) : pi.name;
}

static Track* addTrack(AudioObjectID proc, const std::string& savePath) {
    Track* t = new Track();
    if (!t->start(proc, savePath)) {
        fprintf(stderr, "%s\n", t->err.c_str());
        delete t;
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(gMu);
    gTracks.push_back(t);
    return t;
}

static bool removeTrack(size_t idx) {
    Track* t = nullptr;
    {
        std::lock_guard<std::mutex> lock(gMu);
        if (idx >= gTracks.size()) return false;
        t = gTracks[idx];
        gTracks.erase(gTracks.begin() + idx);
    }
    t->stop();
    printf("saved: %s\n", t->path.c_str());
    delete t;
    return true;
}

static void stopAll() {
    std::vector<Track*> ts;
    {
        std::lock_guard<std::mutex> lock(gMu);
        ts = gTracks;
        gTracks.clear();
    }
    for (Track* t : ts) {
        t->stop();
        printf("saved: %s\n", t->path.c_str());
        delete t;
    }
    printf("stopped\n");
}

// ---------------------------------------------------------------------------
// JSON helpers

static std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> o;
    std::string cur;
    for (char c : s) {
        if (c == d) { o.push_back(cur); cur.clear(); } else cur += c;
    }
    o.push_back(cur);
    return o;
}

static std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hx = [](char c) { return (char)strtol(std::string(1, c).c_str(), nullptr, 16); };
            o += (char)(hx(s[i + 1]) * 16 + hx(s[i + 2]));
            i += 2;
        } else if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}

static std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}

static std::string jsonStrArray(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) o += ","; o += "\"" + jsonEscape(v[i]) + "\""; }
    return o + "]";
}

// ---------------------------------------------------------------------------
// Local HTTP server (UI + JSON control only â no audio on the wire)

static bool sendAll(int fd, const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n > 0) {
        ssize_t w = send(fd, c, n, 0);
        if (w <= 0) return false;
        c += w;
        n -= (size_t)w;
    }
    return true;
}

static void sendHeader(int fd, const char* status, const char* type) {
    char h[512];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     status, type);
    sendAll(fd, h, (size_t)n);
}

static void respondJSON(int fd, const std::string& body) {
    sendHeader(fd, "200 OK", "application/json");
    sendAll(fd, body.data(), body.size());
}

static void respond404(int fd) { sendHeader(fd, "404 Not Found", "text/plain"); }

static void serveIndex(int fd) {
    std::ifstream f(gIndexPath);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string body = ss.str();
    if (body.empty()) { respond404(fd); return; }
    sendHeader(fd, "200 OK", "text/html; charset=utf-8");
    sendAll(fd, body.data(), body.size());
}

static std::string processesJSON() {
    std::vector<AudioObjectID> procs = processObjects();
    std::ostringstream j;
    j << "{\"ok\":true,\"processes\":[";
    for (size_t n = 0; n < procs.size(); ++n) {
        ProcInfo i = describeProcess(procs[n]);
        bool tracked = false;
        {
            std::lock_guard<std::mutex> lock(gMu);
            for (Track* t : gTracks) if (t->processID == i.id) tracked = true;
        }
        if (n) j << ",";
        j << "{\"objectID\":" << i.id
          << ",\"pid\":" << (int)i.pid
          << ",\"bundle\":\"" << jsonEscape(i.bundle) << "\""
          << ",\"name\":\"" << jsonEscape(i.name) << "\""
          << ",\"runningOutput\":" << (i.out ? "true" : "false")
          << ",\"tracked\":" << (tracked ? "true" : "false") << "}";
    }
    j << "]}";
    return j.str();
}

static std::string infoJSON() {
    std::lock_guard<std::mutex> lock(gMu);
    std::ostringstream j;
    j << "{\"ok\":true,\"tracks\":[";
    for (size_t i = 0; i < gTracks.size(); ++i) {
        if (i) j << ",";
        j << "{\"id\":" << i
          << ",\"objectID\":" << gTracks[i]->processID
          << ",\"name\":\"" << jsonEscape(gTracks[i]->appName) << "\""
          << ",\"file\":\"" << jsonEscape(gTracks[i]->path) << "\""
          << ",\"rate\":" << gTracks[i]->rate << "}";
    }
    j << "]}";
    return j.str();
}

// /start?apps=45,52&name=Spotify  â one save panel per app, then recording
static void handleStart(int fd, const std::string& query) {
    std::vector<std::string> added, failed;
    for (const std::string& kv : split(query, '&')) {
        size_t eq = kv.find('='); if (eq == std::string::npos) continue;
        std::string key = kv.substr(0, eq), val = urlDecode(kv.substr(eq + 1));
        if (key != "apps" && key != "name" && key != "id") continue;
        for (const std::string& one : split(val, ',')) {
            if (one.empty()) continue;
            AudioObjectID proc = 0;
            if (key == "apps") proc = findProcessByID(one);
            else { std::string err; proc = findProcessByName(one, err); }
            if (!proc) { failed.push_back(one + " (not found)"); continue; }
            std::string savePath = askSavePathMain(suggestedName(trackNameFor(proc)));
            if (savePath.empty()) { failed.push_back(one + " (cancelled)"); continue; }
            if (addTrack(proc, savePath)) added.push_back(one);
            else failed.push_back(one);
        }
    }
    respondJSON(fd, "{\"ok\":" + std::string(failed.empty() ? "true" : "false") +
                    ",\"added\":" + jsonStrArray(added) +
                    ",\"failed\":" + jsonStrArray(failed) + "}");
}

static void handleClient(int fd) {
    char req[2048] = {0};
    ssize_t r = recv(fd, req, sizeof req - 1, 0);
    if (r <= 0) { close(fd); return; }
    const std::string s(req);

    size_t sp1 = s.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : s.find(' ', sp1 + 1);
    std::string full = (sp2 == std::string::npos) ? "" : s.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qm = full.find('?');
    std::string path = full.substr(0, qm);
    std::string query = (qm == std::string::npos) ? "" : full.substr(qm + 1);

    if (path == "/")                serveIndex(fd);
    else if (path == "/health")    respondJSON(fd, "{\"ok\":true}");
    else if (path == "/info")      respondJSON(fd, infoJSON());
    else if (path == "/processes") respondJSON(fd, processesJSON());
    else if (path == "/start")     handleStart(fd, query);
    else if (path == "/remove") {
        size_t eq = query.find("id=");
        bool ok = eq != std::string::npos &&
                  removeTrack((size_t)strtoul(query.c_str() + eq + 3, nullptr, 10));
        respondJSON(fd, std::string("{\"ok\":") + (ok ? "true" : "false") + "}");
    }
    else if (path == "/quit")      { respondJSON(fd, "{\"ok\":true}"); gRunning = false; }
    else respond404(fd);

    close(fd);
}

static void serve() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kPort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr*)&a, sizeof a) < 0 || listen(s, 8) < 0) {
        perror("bind/listen");
        return;
    }
    printf("rec page: http://127.0.0.1:%d/  (open Rec.app and pick apps there)\n", kPort);
    fflush(stdout);

    while (gRunning) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        timeval tv{0, 200 * 1000};
        int r = select(s + 1, &fds, nullptr, nullptr, &tv);
        if (r > 0) {
            int c = accept(s, nullptr, nullptr);
            if (c >= 0) std::thread(handleClient, c).detach();
        }
    }
    close(s);
}

// ---------------------------------------------------------------------------
// Quit: Dock "Quit" / AppleScript quit arrive as applicationShouldTerminate:.
// SIGINT/SIGTERM cover the terminal.

static unsigned long appShouldTerminate(id, SEL, id) {
    gDockQuit = true;
    gRunning = false;
    return 2;                   // NSTerminateLater
}

static void replyTerminateYES(void*) {
    id app = send0(oc("NSApplication"), "sharedApplication");
    ((void (*)(id, SEL, BOOL))objc_msgSend)(app, sel_registerName("replyToApplicationShouldTerminate:"), YES);
}

static void listProcesses() {
    for (AudioObjectID p : processObjects()) {
        ProcInfo i = describeProcess(p);
        printf("%u\tpid=%d\t%s\t%s%s\n", p, (int)i.pid, i.bundle.c_str(), i.name.c_str(),
               i.out ? "\t[playing]" : "");
    }
}

int main(int argc, char** argv) {
    std::string self = argv[0];
    size_t slash = self.rfind('/');
    std::string dir = (slash == std::string::npos ? "." : self.substr(0, slash));
    gIndexPath = dir + "/../Resources/index.html";

    if (argc >= 2 && !strcmp(argv[1], "--list")) {
        listProcesses();
        return 0;
    }

    // NSApplication must exist before AppKit panels can run
    id app = send0(oc("NSApplication"), "sharedApplication");
    sendI(app, "setActivationPolicy:", 0);            // Regular: Dock icon + Quit menu

    // CLI args: objectID or app name, same behaviour as the page (panel per app)
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "-psn", 4)) continue;
        std::string a = argv[i];
        AudioObjectID proc = findProcessByID(a);
        if (!proc) {
            std::string err;
            proc = findProcessByName(a, err);
            if (!proc) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        }
        std::string savePath = askSavePath(suggestedName(trackNameFor(proc)));
        if (savePath.empty()) { fprintf(stderr, "cancelled\n"); return 0; }
        if (!addTrack(proc, savePath)) return 1;
    }

    struct sigaction sa{};
    sa.sa_handler = [](int) { gRunning = false; };  // no SA_RESTART
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    Class cls = objc_allocateClassPair(objc_getClass("NSObject"), "RecAppDelegate", 0);
    class_addMethod(cls, sel_registerName("applicationShouldTerminate:"),
                    (IMP)appShouldTerminate, "Q@:@");
    objc_registerClassPair(cls);
    id delegate = send0(send0((id)cls, "alloc"), "init");
    send1(app, "setDelegate:", delegate);

    std::thread(serve).detach();

    // no CLI args: this is the normal "open Rec.app" flow â open the page
    if (argc < 2 || (argc == 2 && !strncmp(argv[1], "-psn", 4))) {
        char cmd[128];
        snprintf(cmd, sizeof cmd, "open http://127.0.0.1:%d/", kPort);
        system(cmd);
    }

    // finalize as soon as gRunning goes false, from any trigger
    std::thread([] {
        while (gRunning) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stopAll();
        fflush(stdout);
        if (gDockQuit) dispatch_async_f(dispatch_get_main_queue(), nullptr, replyTerminateYES);
        else _exit(0);                                // Ctrl+C / SIGTERM / /quit path
    }).detach();

    id pool = send0(send0(oc("NSAutoreleasePool"), "alloc"), "init");
    send0(app, "run");                                 // blocks until Dock Quit is replied to
    send0(pool, "drain");
    return 0;
}