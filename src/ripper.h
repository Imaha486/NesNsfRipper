// ripper.h - sound driver tracing, candidate search and NSF generation
#pragma once
#include "nes.h"
#include <map>
#include <set>
#include <string>
#include <vector>
#include <array>
#include <functional>
#include <atomic>
#include <algorithm>

typedef std::array<uint8_t, 8> BankCfg;

struct RoutineInfo {
    uint16_t addr = 0; int kind = 0;        // kind 0=JSR target, 1=NMI handler, 2=IRQ handler
    uint64_t calls = 0, callsWithSound = 0, soundWrites = 0, depthSum = 0, cyclesSum = 0, ppuIoOwn = 0;
    uint64_t outsideCalls = 0, outsideDriverWrites = 0, outsideSoundWrites = 0, outsideAllWrites = 0;
    std::vector<uint16_t> regCnt[3];       // histogram of A/X/Y values at entry (outside calls), 256 each
    std::set<uint16_t> callers;
    std::map<BankCfg, int> cfgCount;
    bool isStatic = false;   // entry point found by scanning the ROM, never called in the trace
    BankCfg topCfg() const { BankCfg b{}; int best = -1; for (auto& kv : cfgCount) if (kv.second > best) { best = kv.second; b = kv.first; } return b; }
    void countReg(int r, uint8_t v) { if (regCnt[r].empty()) regCnt[r].assign(256, 0); if (regCnt[r][v] < 65535) regCnt[r][v]++; }
    int distinctRegVals(int r) const { if (regCnt[r].empty()) return 0; int n = 0; for (int i = 0; i < 256; i++) if (regCnt[r][i]) n++; return n; }
    std::vector<uint8_t> topRegVals(int r, int k) const { std::vector<uint8_t> out; if (regCnt[r].empty()) { out.push_back(0); return out; } std::vector<int> idx(256); for (int i = 0; i < 256; i++) idx[i] = i; std::stable_sort(idx.begin(), idx.end(), [&](int p, int q) { return regCnt[r][p] > regCnt[r][q]; }); for (int i = 0; i < k && i < 256 && regCnt[r][idx[i]]; i++) out.push_back((uint8_t)idx[i]); if (out.empty()) out.push_back(0); return out; }
};

enum RecipeType { RT_JSR = 1, RT_MAILBOX = 2, RT_MAILBOX_JSR = 3, RT_SEQ = 4 };

struct Step { int kind = 1; /* 1 = JSR routine with regs, 2 = write value to mailbox */ uint16_t routine = 0; uint8_t a = 0, x = 0, y = 0; uint16_t mailbox = 0; uint8_t value = 0; };

struct Recipe {
    int type = RT_JSR;
    uint16_t routine = 0; uint8_t a = 0, x = 0, y = 0;   // for JSR
    uint16_t mailbox = 0; uint8_t value = 0;              // for MAILBOX
    int argReg = 0;          // which register carries the song number (0=A,1=X,2=Y,3=all) for JSR type
    BankCfg initCfg{};       // bank config applied before action
    BankCfg playCfg{};       // bank config for PLAY (all zero = the driver's usual one); set by the bank sweep
    bool hasPlayCfg() const { for (int i = 0; i < 8; i++) if (playCfg[i]) return true; return false; }
    std::vector<Step> steps; // for RT_SEQ: ordered actions (observed call sequence)
    std::vector<Step> allSteps() const;
    std::string describe() const;
    std::string describeBase() const;
};

struct Candidate {
    Recipe recipe;
    uint64_t hashShort = 0, hashLong = 0, hashTail = 0; // hashTail: frames 120.. of the long run (ignores the initial transient)
    int writes = 0;
    int durationFrames = 0;   // frames until silence (0 = not ending within analysis window)
    int loopStart = 0, loopLen = 0; // detected loop (0 = none)
    bool isMusic = false;
    uint32_t channels = 0;    // channel usage bits
    bool bankSwitchesInPlay = false;
    int snapId = 0;           // 0 = the canonical snapshot (drv.snapFirst), 1 = the alternative one (drv.snapAlt)
    bool isBaseline = false;  // produces exactly what the snapshot plays on its own (the request pending in the snapshot)
    bool overlay = false;     // transient on top of music the snapshot was already playing (ends when the stream falls back onto the baseline)
    bool verified = false;    // NSF playback matched harness
    int confidence = 0;       // 2 = argument value observed in game trace, 1 = plausible, 0 = table-bounds heuristic says out of range (garbage)
    bool suspicious = false;  // PLAY read RAM never read by the driver during the game trace
    std::string note;
    std::string name;         // display label
    int index = 0;
};

struct DriverInfo {
    std::vector<uint16_t> playRoutines;  // called in order each frame
    struct PlayCall { uint8_t a, x, y; };
    std::vector<PlayCall> playCalls;     // per-frame call sequence (registers) to the PLAY routine; empty = single call with 0,0,0
    std::vector<std::vector<PlayCall>> playCallAlts; // alternative register patterns (most common first)
    int playKind = 0;                    // 0 subroutine, 1 NMI handler, 2 IRQ handler
    struct PlayAlt { uint16_t addr; int kind; BankCfg cfg; double rate; bool clean = true; }; // clean: no PPU I/O, regular call rate, not tiny (a plausible driver update routine)
    std::vector<PlayAlt> playAlternatives; // ranked PLAY candidates (first = chosen)
    BankCfg playCfg{};
    double playRate = 1.0;               // calls per frame
    uint8_t playSp = 0xFF;               // stack pointer the game has when PLAY is entered (drivers may keep data high in the stack page)
    std::vector<uint16_t> initCandidates;
    std::vector<uint16_t> mailboxes;
    std::vector<std::pair<uint16_t, uint16_t>> mailboxJsrPairs; // (mailbox, routine)
    std::map<uint16_t, std::set<uint8_t>> mailboxVals;           // observed values written to each mailbox
    std::vector<Recipe> sequences;                                // observed multi-call sequences
    struct Burst { uint16_t routine; int reg; int len; uint8_t a, x, y; };
    std::vector<Burst> bursts;                                    // routine called repeatedly with a register counting up (per-channel track starts)
    std::vector<std::pair<uint16_t, uint8_t>> refresh;            // keep-alive variables the game rewrites every frame (addr, value)
    std::map<uint16_t, std::vector<uint16_t>> routineReads;       // INIT candidate -> non-driver RAM it reads (parameters)
    std::map<uint16_t, RoutineInfo> initInfo;
    std::vector<uint8_t> driverRead;     // 0x10000 marks: byte read inside PLAY extent
    std::vector<uint8_t> driverWrite;
    std::vector<uint8_t> anyTouched;     // read or written by anything during trace
    std::vector<uint8_t> sharedTemp;     // scratch bytes written from many sites (excluded from driver-variable logic)
    std::vector<uint8_t> romExec;        // per PRG byte: executed during the game trace
    NES::Snapshot snapFirst, snapLater, snapInit, snapAlt, snapLate2; bool haveLater = false, haveInit = false, haveAlt = false, haveLate2 = false;   // snapLate2 ("C"): second half of the trace, taken while notes were sounding
    int framesTraced = 0; int firstAudibleFrame = -1; int noteFrames = 0; uint16_t feeder = 0;
    std::string log;
};

struct NsfBuild {
    std::vector<uint8_t> file;      // complete .nsf
    std::string report;
    bool ok = false;
    int stubSlot = -1;
    int banks = 0;
    std::vector<std::string> warnings;
    std::vector<uint8_t> ramPlayTramp, ramCode;
    int maxTracksHint = 0;   // when the stub overflowed: how many tracks would fit
};

struct RipOptions {
    int traceFrames = 2400;
    int screenFrames = 60;
    int longFrames = 3600;      // 60 s
    int threads = 4;
    int maxInitRoutines = 20;
    int maxMailboxes = 48;
    bool includeSfx = true;
    bool pal = false;           // PAL (50 Hz) region: NSF header flagged PAL, playback at 50 Hz / 1.66 MHz
    std::function<void(const std::string&)> progress; // status messages
    std::atomic<bool>* cancel = nullptr;
};

class Ripper {
public:
    Ripper(const std::vector<uint8_t>& romFile);
    bool ok() const { return loaded; }
    std::string error;
    NES base;                        // ROM holder (not used for execution)
    DriverInfo drv;
    std::vector<Candidate> cands;
    RipOptions opt;
    std::string romTitle;

    bool analyze();                  // pass 1+2 tracing -> drv
    double playCallsPerFrame() const; // PLAY calls per NTSC frame used for NSF speed
    bool search();                   // brute force -> cands
    bool run();                      // analyze + search, retrying alternative PLAY routines when nothing is found
    bool runAlternatives();          // the search part of run() (after analyze())

    // Harness: play recipe for n frames; returns per-frame hashes, fills soundLog if requested
    struct HarnessResult { std::vector<uint64_t> frameHash; uint64_t total = 0; int writes = 0; bool ok = true; uint32_t channels = 0; bool bankWrite = false; int foreignRamReads = 0; int codeBytesAsData = 0; int audibleFrames = 0; std::vector<uint8_t> frameAudible; };
    HarnessResult runRecipe(NES& nes, const NES::Snapshot& snap, const Recipe& r, int frames, std::vector<SoundWrite>* log = nullptr, bool collectTouch = false, std::vector<uint32_t>* initReads = nullptr);

    // NSF generation for a set of candidates (indices into cands)
    NsfBuild buildNsf(const std::vector<int>& sel, const std::string& title, const std::string& artist, const std::string& copyright);
    // verify NSF: plays track with NSF player and compares hashes with harness
    bool verifyNsf(const NsfBuild& b, int trackIdx, int candIdx, int frames, std::string* msg);

    // playback (for GUI/CLI WAV): run the NSF track and produce samples
    static std::vector<int16_t> renderNsf(const std::vector<uint8_t>& nsf, int track, double seconds);

private:
    bool loaded = false;
    std::vector<uint8_t> romFile;
    const NES::Snapshot* activeSnap = nullptr;
    int padStyle = 0;                // autoplay input pattern (0 = default; others tried when the default makes no sound)
    std::vector<uint8_t> seedRead;   // MARK_PLAY map from a previous pass2 (seeds the next pass2)
    std::vector<uint8_t> seedShared; // RAM bytes written by many different sites outside PLAY (scratch temporaries): not driver variables
    void log(const std::string& s);
    void prog(const std::string& s) { if (opt.progress) opt.progress(s); }
    void applyCfg(NES& nes, const BankCfg& c);
    bool pass1(NES& nes, std::map<uint32_t, RoutineInfo>& routines, std::vector<std::pair<uint16_t,int>>& nmiHandlers);
    bool pass2(NES& nes);
public:
    bool analyzeWithPlay(int alt);   // pass2 + parameter scan for playAlternatives[alt]
    bool searchWithCallAlts();       // search(), retrying alternative PLAY register patterns
private:
    void setPad(NES& nes, int frame);
    const NES::Snapshot& snapOf(const Candidate& c) const { return c.snapId == 1 && drv.haveAlt ? drv.snapAlt : drv.snapFirst; }
    bool buildSilencedSnapshot(const NES::Snapshot& from, const std::vector<Recipe>& recipes, NES::Snapshot& out);
    std::vector<Candidate> screen(const NES::Snapshot& snap, std::vector<Recipe>& recipes, uint64_t baselineHash, bool& any, bool baselineAudible);
};

std::string hex16(uint16_t v);
std::string hex8(uint8_t v);
std::string cfgStr(const BankCfg& c);

// simple NSF player wrapper (for verification / audition)
struct NsfPlayer {
    NES nes; int expansion = 0; uint16_t init = 0, play = 0; int songs = 0; int speed = 16666; bool pal = false; bool ok = false;
    std::string err; std::vector<uint8_t> initBanks; bool pendingInit = false;
    bool load(const std::vector<uint8_t>& nsf);
    bool selectTrack(int t);       // runs INIT
    bool frame(std::vector<SoundWrite>& writes); // runs PLAY, collects writes with cycle offsets
    double cpuClock() const { return pal ? 1662607.0 : 1789772.7; }
    int cyclesPerFrame() const { return (int)(speed * cpuClock() / 1000000.0); }
};
