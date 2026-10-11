// tracer.cpp - game-run tracing: find PLAY routine, driver RAM, mailboxes, INIT candidates
#include "ripper.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <sstream>

std::string hex16(uint16_t v) { char b[8]; snprintf(b, sizeof b, "$%04X", v); return b; }
std::string hex8(uint8_t v) { char b[8]; snprintf(b, sizeof b, "$%02X", v); return b; }
std::string cfgStr(const BankCfg& c) { std::string s = "["; for (int i = 0; i < 8; i++) { char b[8]; snprintf(b, sizeof b, "%s%d", i ? "," : "", c[i]); s += b; } return s + "]"; }

std::vector<Step> Recipe::allSteps() const {
    if (type == RT_SEQ) return steps;
    std::vector<Step> v;
    if (type & 2) { Step m; m.kind = 2; m.mailbox = mailbox; m.value = value; v.push_back(m); }
    if (type & 1) { Step j; j.kind = 1; j.routine = routine; j.a = a; j.x = x; j.y = y; v.push_back(j); }
    return v;
}

std::string Recipe::describe() const {
    std::string d = describeBase();
    if (hasPlayCfg()) { d += " @bank "; for (int i = 0; i < 8; i++) { if (i) d += ","; d += std::to_string(playCfg[i]); } }
    return d;
}
std::string Recipe::describeBase() const {
    char b[128];
    if (type == RT_SEQ) {
        std::string s = "SEQ:"; uint16_t last = 0xFFFF;
        for (auto& st : steps) {
            if (st.kind == 2) { snprintf(b, sizeof b, " RAM %s<-%02X;", hex16(st.mailbox).c_str(), st.value); s += b; last = 0xFFFF; continue; }
            if (st.routine != last) { snprintf(b, sizeof b, " JSR %s", hex16(st.routine).c_str()); s += b; last = st.routine; }
            snprintf(b, sizeof b, " [%02X,%02X,%02X]", st.a, st.x, st.y); s += b;
        }
        return s;
    }
    const char* reg = argReg == 0 ? "A" : argReg == 1 ? "X" : argReg == 2 ? "Y" : "A/X/Y";
    switch (type) {
    case RT_JSR: snprintf(b, sizeof b, "JSR %s (%s=$%02X) [A=%02X X=%02X Y=%02X]", hex16(routine).c_str(), reg, argReg == 1 ? x : argReg == 2 ? y : a, a, x, y); break;
    case RT_MAILBOX: snprintf(b, sizeof b, "RAM %s <- $%02X", hex16(mailbox).c_str(), value); break;
    default: snprintf(b, sizeof b, "RAM %s <- $%02X ; JSR %s [A=%02X X=%02X Y=%02X]", hex16(mailbox).c_str(), value, hex16(routine).c_str(), a, x, y); break;
    }
    return b;
}

Ripper::Ripper(const std::vector<uint8_t>& f) : romFile(f) {
    loaded = base.loadRom(f, &error);
}
void Ripper::log(const std::string& s) { drv.log += s; drv.log += "\n"; }
void Ripper::applyCfg(NES& nes, const BankCfg& c) { for (int i = 0; i < 8; i++) nes.m.prgMap[i] = c[i]; }

// deterministic auto-play input script
void Ripper::setPad(NES& nes, int f) {
    uint8_t p = 0;
    // bit layout: A=1 B=2 Select=4 Start=8 Up=16 Down=32 Left=64 Right=128
    if (padStyle == 1) {            // eager: Start early and often, then A/B mashing with directions
        if (f >= 20 && (f % 40) < 3) p |= 8;
        if (f >= 20 && (f % 10) < 2) p |= 1;
        if (f >= 20 && (f % 14) < 2) p |= 2;
        if (f >= 20) { int d = (f / 30) & 3; if (((f / 30) & 4) == 0) p |= 16 << d; }
        nes.pad[0] = p; nes.pad[1] = 0; return;
    }
    if (padStyle == 2) {            // random every 8 frames on both controllers (menus that need Select, 2-player titles)
        uint32_t r = (uint32_t)(f / 8) * 2246822519u; r ^= r >> 15; r *= 2654435761u; r ^= r >> 12;
        if (f >= 10) { p = (uint8_t)(r & 0xFF); if ((p & 0x30) == 0x30) p &= ~0x30; if ((p & 0xC0) == 0xC0) p &= ~0xC0; }
        uint32_t r2 = r * 40503u; nes.pad[0] = p; nes.pad[1] = f >= 10 ? (uint8_t)((r2 >> 8) & 0x0F) : 0; return;
    }
    if (padStyle == 3) {            // patient: nothing for 10 s (attract mode / demo music), then the default pattern
        if (f < 600) { nes.pad[0] = 0; nes.pad[1] = 0; return; }
        f -= 540;
    }
    if (f >= 60) {
        if ((f % 90) < 4) p |= 8;                 // Start periodically
        if (f >= 240) {
            uint32_t r = (uint32_t)(f / 24) * 2654435761u; r ^= r >> 13;
            if ((f % 41) < 3) p |= 1;             // A
            if ((f % 67) < 3) p |= 2;             // B
            int d = (r >> 4) & 7; if (d < 4) p |= 16 << d;
            if ((f % 300) < 3) p |= 4;            // Select
        }
    }
    nes.pad[0] = p; nes.pad[1] = 0;
}

namespace {
struct Frame { int kind; uint16_t addr; uint8_t spEntry; uint8_t a, x, y; uint64_t cyc; uint64_t sound, drvWrites, allWrites; bool isPlay; bool outside; int depth; uint64_t seq; };

struct Pass1Hooks : TraceHooks {
    NES* nes; std::map<uint32_t, RoutineInfo>* routines; std::vector<Frame> stack; uint64_t totalSound = 0; uint64_t seq = 0;
    std::vector<uint32_t> perFrameSound, perFrameNote; int curFrame = 0;
    std::map<uint32_t, std::vector<int>> callFrames; // routine -> frame numbers of calls (sampled)
    void push(int kind, uint16_t addr, uint8_t sp) {
        while (!stack.empty() && stack.back().spEntry <= sp) { Frame g = stack.back(); stack.pop_back(); pop(g); } // stale frames (SP moved above their entry)
        Frame f{}; f.kind = kind; f.addr = addr; f.spEntry = sp; f.a = nes->a; f.x = nes->x; f.y = nes->y; f.cyc = nes->cycles; f.depth = (int)stack.size(); f.seq = seq++;
        stack.push_back(f);
    }
    void pop(Frame& f) {
        RoutineInfo& r = (*routines)[(f.kind << 16) | f.addr];
        r.addr = f.addr; r.kind = f.kind; r.calls++; if (f.sound) r.callsWithSound++; r.soundWrites += f.sound; r.depthSum += f.depth; r.cyclesSum += nes->cycles - f.cyc;
        BankCfg c; for (int i = 0; i < 8; i++) c[i] = nes->m.prgMap[i]; // (exit cfg; entry cfg recorded on push below)
    }
    void onJsr(uint16_t target, uint16_t from, uint8_t sp) override {
        push(0, target, sp);
        RoutineInfo& r = (*routines)[target]; r.addr = target; r.kind = 0; r.callers.insert(from);
        BankCfg c; for (int i = 0; i < 8; i++) c[i] = nes->m.prgMap[i]; r.cfgCount[c]++;
    }
    void onRts(uint8_t spBefore) override {
        uint8_t target = spBefore + 2;
        while (!stack.empty() && stack.back().kind == 0 && (uint8_t)(stack.back().spEntry) != target && (int8_t)(stack.back().spEntry - target) < 0) { Frame f = stack.back(); stack.pop_back(); pop(f); }
        if (!stack.empty() && stack.back().kind == 0 && stack.back().spEntry == target) { Frame f = stack.back(); stack.pop_back(); pop(f); }
    }
    void onInterrupt(int kind, uint16_t vec, uint8_t sp) override {
        uint16_t h = nes->peek(vec) | (nes->peek(vec + 1) << 8);
        push(kind, h, sp);
        RoutineInfo& r = (*routines)[(kind << 16) | h]; r.addr = h; r.kind = kind;
        BankCfg c; for (int i = 0; i < 8; i++) c[i] = nes->m.prgMap[i]; r.cfgCount[c]++;
    }
    void onRti(uint8_t spBefore) override {
        uint8_t target = spBefore + 3;
        while (!stack.empty() && stack.back().kind == 0 && (int8_t)(stack.back().spEntry - target) <= 0) { Frame f = stack.back(); stack.pop_back(); pop(f); }
        if (!stack.empty() && stack.back().kind != 0 && stack.back().spEntry == target) { Frame f = stack.back(); stack.pop_back(); pop(f); }
    }
    void onSoundWrite(uint16_t addr, uint8_t val) override {
        totalSound++; if (curFrame < (int)perFrameSound.size()) { perFrameSound[curFrame]++; bool note = val != 0 && ((addr >= 0x4000 && addr <= 0x400B && (addr & 3) >= 2) || addr == 0x4012 || (addr >= 0x4018 && addr != 0x4015 && addr != 0x4017)); if (note) perFrameNote[curFrame]++; }
        for (int i = (int)stack.size() - 1; i >= 0; i--) { stack[i].sound++; if (stack[i].kind != 0) break; }
    }
    void onPpuIo(uint16_t addr) override { if (!stack.empty()) { Frame& f = stack.back(); (*routines)[(f.kind << 16) | f.addr].ppuIoOwn++; } }
};
}

bool Ripper::pass1(NES& nes, std::map<uint32_t, RoutineInfo>& routines, std::vector<std::pair<uint16_t,int>>& nmiHandlers) {
    Pass1Hooks h; h.nes = &nes; h.routines = &routines; h.perFrameSound.assign(opt.traceFrames, 0); h.perFrameNote.assign(opt.traceFrames, 0);
    nes.hooks = &h; nes.traceOn = true;
    for (int f = 0; f < opt.traceFrames; f++) {
        if (opt.cancel && *opt.cancel) return false;
        h.curFrame = f; setPad(nes, f); nes.runFrame();
        if (h.stack.size() > 4000) h.stack.erase(h.stack.begin(), h.stack.begin() + 2000); // runaway protection
    }
    nes.traceOn = false; nes.hooks = nullptr;
    drv.framesTraced = opt.traceFrames;
    for (int f = 0; f < opt.traceFrames; f++) if (h.perFrameSound[f] >= 3) { drv.firstAudibleFrame = f; break; }
    { int nf = 0; for (int f = 0; f < opt.traceFrames; f++) if (h.perFrameNote[f]) nf++; drv.noteFrames = nf; }
    // candidate PLAY selection
    uint64_t total = h.totalSound;
    log("trace: frames=" + std::to_string(opt.traceFrames) + " soundWrites=" + std::to_string(total) + " noteFrames=" + std::to_string(drv.noteFrames) + " firstAudible=" + std::to_string(drv.firstAudibleFrame) + " routines=" + std::to_string(routines.size()));
    if (total < 10) { error = "ゲーム実行中にサウンドレジスタ書き込みがほとんど検出されませんでした"; return false; }
    struct Cand { uint32_t key; double cov, rate, depth; bool extra = false; };
    std::vector<Cand> cs;
    for (auto& kv : routines) {
        RoutineInfo& r = kv.second; if (r.calls == 0) continue;
        double cov = (double)r.soundWrites / total, rate = (double)r.calls / opt.traceFrames, depth = (double)r.depthSum / r.calls;
        if (cov >= 0.5 && rate >= 0.15 && rate <= 32 && (r.cyclesSum / r.calls) < 60000) cs.push_back({ kv.first, cov, rate, depth });
        if (getenv("NSFRIP_DEBUG") && cov >= 0.1) { char b[160]; snprintf(b, sizeof b, "  routine %s kind=%d cov=%.2f rate=%.2f depth=%.1f ppuIo=%llu avgCyc=%llu", hex16(r.addr).c_str(), r.kind, cov, rate, depth, (unsigned long long)r.ppuIoOwn, (unsigned long long)(r.cyclesSum / r.calls)); log(b); }
    }
    if (cs.empty()) {
        for (auto& kv : routines) { RoutineInfo& r = kv.second; if (!r.calls) continue; double cov = (double)r.soundWrites / total, rate = (double)r.calls / opt.traceFrames; if (cov >= 0.25 && rate >= 0.15 && rate <= 32) cs.push_back({ kv.first, cov, rate, (double)r.depthSum / r.calls }); }
    }
    { // low-coverage per-frame routines as extra fallbacks (drivers split into music/sfx parts called from game logic)
        std::vector<Cand> extra;
        for (auto& kv : routines) { RoutineInfo& r = kv.second; if (!r.calls || r.kind != 0 || r.ppuIoOwn) continue; double cov = (double)r.soundWrites / total, rate = (double)r.calls / opt.traceFrames; bool have = false; for (auto& c : cs) if (c.key == kv.first) have = true; if (!have && cov >= 0.12 && cov < 0.5 && rate >= 0.15 && rate <= 32 && (r.cyclesSum / r.calls) < 60000) extra.push_back({ kv.first, cov, rate, (double)r.depthSum / r.calls, true }); }
        std::sort(extra.begin(), extra.end(), [](const Cand& p, const Cand& q) { return p.cov > q.cov; });
        for (size_t i = 0; i < extra.size() && i < 3; i++) cs.push_back(extra[i]);
    }
    if (cs.empty()) { error = "毎フレーム呼ばれるサウンド更新ルーチンを特定できませんでした"; return false; }
    // tier A = plain subroutines (no PPU access in their own body) with high coverage; interrupt handlers only as a fallback
    auto regular = [](double r) { if (r > 4.5) return false; double n = r < 0.75 ? 1.0 / r : r; return fabs(n - std::round(n)) < 0.12; };
    auto tierA = [&](const Cand& c) { const RoutineInfo& r = routines[c.key]; return r.kind == 0 && r.ppuIoOwn == 0 && c.cov >= 0.6; };
    bool anyA = false; for (auto& c : cs) if (tierA(c)) anyA = true;
    std::sort(cs.begin(), cs.end(), [&](const Cand& p, const Cand& q) {
        const RoutineInfo& rp = routines[p.key]; const RoutineInfo& rq = routines[q.key];
        if (p.extra != q.extra) return !p.extra;
        bool ap = tierA(p), aq = tierA(q);
        if (anyA && ap != aq) return ap;
        bool tinyp = rp.cyclesSum / rp.calls < 250, tinyq = rq.cyclesSum / rq.calls < 250;
        if (tinyp != tinyq) return !tinyp;
        bool regp = regular(p.rate), regq = regular(q.rate);
        if (regp != regq) return regp;
        if (!anyA) { bool ip = rp.kind != 0 || rp.ppuIoOwn > 0, iq = rq.kind != 0 || rq.ppuIoOwn > 0; if (ip != iq) return !ip; }
        if (fabs(p.depth - q.depth) > 0.5) return p.depth > q.depth;
        return p.cov > q.cov; });
    for (size_t i = 0; i < cs.size() && i < 8; i++) {
        RoutineInfo& r = routines[cs[i].key]; char b[200];
        snprintf(b, sizeof b, "  PLAY cand %s kind=%d cov=%.2f rate=%.2f depth=%.1f ppuIo=%llu avgCyc=%llu", hex16(r.addr).c_str(), r.kind, cs[i].cov, cs[i].rate, cs[i].depth, (unsigned long long)r.ppuIoOwn, (unsigned long long)(r.cyclesSum / r.calls));
        log(b);
    }
    drv.playAlternatives.clear();
    for (size_t i = 0; i < cs.size() && i < 6; i++) { RoutineInfo& r = routines[cs[i].key]; DriverInfo::PlayAlt pa{ r.addr, r.kind, r.topCfg(), (double)r.calls / opt.traceFrames }; pa.clean = r.ppuIoOwn * 2 <= r.calls && regular(cs[i].rate) && r.cyclesSum / r.calls >= 250 && cs[i].cov >= 0.25; /* an update routine accounts for most of the sound writes; a sub-engine (cov 0.16) would "find" game logic as requests */ drv.playAlternatives.push_back(pa); }
    RoutineInfo& best = routines[cs[0].key];
    drv.playRoutines.clear(); drv.playRoutines.push_back(best.addr); drv.playKind = best.kind; drv.playCfg = best.topCfg();
    drv.playRate = (double)best.calls / opt.traceFrames;
    log("PLAY = " + hex16(best.addr) + (best.kind ? " (interrupt handler)" : "") + " cfg=" + cfgStr(drv.playCfg) + " rate=" + std::to_string(drv.playRate));
    return true;
}

namespace {
struct Pass2Hooks : TraceHooks {
    NES* nes; DriverInfo* drv; std::vector<Frame> stack; int playDepth = 0; uint64_t seq = 0; int curFrame = 0;
    std::map<uint16_t, RoutineInfo> info; // outside-call stats
    struct Ev { int type; uint64_t seq; uint16_t addr; uint8_t a, x, y, val; int frame; uint64_t seqEnd; }; // type 0=mailbox write,1=call(exit-time),2=play
    std::vector<Ev> events;
    std::map<uint16_t, std::pair<int,int>> mailboxWrites; // addr -> (count, distinct values approx via set size)
    std::map<uint16_t, std::set<uint8_t>> mailboxVals;
    std::map<uint16_t, std::pair<int, std::set<uint8_t>>> mailboxAfter; // writes after the first PLAY call (boot-time initialisation excluded)
    std::map<uint16_t, std::map<uint8_t, int>> mailboxValCnt;
    std::map<uint16_t, std::set<uint16_t>> pcTargets;      // writing PC -> distinct driver addresses it wrote (block fills/copies are not requests)
    std::map<uint16_t, std::map<uint16_t, int>> mailboxByPc; // addr -> (pc -> writes)
    std::map<uint16_t, std::map<uint16_t, std::set<uint8_t>>> valsByPc; // addr -> (pc -> values)
    std::set<uint8_t> valsNoFill(uint16_t addr) { std::set<uint8_t> out; for (auto& kv : valsByPc[addr]) if (pcTargets[kv.first].size() < 8) out.insert(kv.second.begin(), kv.second.end()); return out; }
    uint64_t playCalls = 0; int snapCount = 0; int laterFrame = -1; uint16_t initRoutine = 0;
    std::map<int, std::vector<DriverInfo::PlayCall>> frameCalls; std::map<uint8_t, int> spCnt;
    bool isPlayAddr(uint16_t a) { return std::find(drv->playRoutines.begin(), drv->playRoutines.end(), a) != drv->playRoutines.end(); }
    void push(int kind, uint16_t addr, uint8_t sp) {
        while (!stack.empty() && stack.back().spEntry <= sp) { Frame g = stack.back(); stack.pop_back(); pop(g); }
        Frame f{}; f.kind = kind; f.addr = addr; f.spEntry = sp; f.a = nes->a; f.x = nes->x; f.y = nes->y; f.cyc = nes->cycles; f.depth = (int)stack.size(); f.seq = seq++;
        f.isPlay = (kind == drv->playKind) && isPlayAddr(addr);
        f.outside = playDepth == 0;
        if (f.isPlay) {
            if (playDepth > 0 && dbgNest > 0) { dbgNest--; char b[160]; snprintf(b, sizeof b, "   nested PLAY push frame=%d addr=%04X sp=%02X kind=%d playDepth=%d stack=%zu pc=%04X bank=%d", curFrame, addr, sp, kind, playDepth, stack.size(), nes->pc, nes->m.prgMap[0]); drv->log += std::string(b) + "\n"; for (size_t i = stack.size() >= 6 ? stack.size() - 6 : 0; i < stack.size(); i++) { snprintf(b, sizeof b, "     [%zu] kind=%d addr=%04X sp=%02X play=%d", i, stack[i].kind, stack[i].addr, stack[i].spEntry, (int)stack[i].isPlay); drv->log += std::string(b) + "\n"; } }
            playDepth++; nes->curMark = MARK_PLAY; playCalls++;
            if (playCalls == 1) { nes->saveSnapshot(drv->snapFirst); snapCount = 1; }
            // "later" (B): 90+ frames after the first sound, preferably while notes are sounding (wait up to 300 frames for that)
            if (!drv->haveLater && drv->firstAudibleFrame >= 0 && curFrame >= drv->firstAudibleFrame + 90 && (curFrame - lastNoteFrame <= 2 || curFrame >= drv->firstAudibleFrame + 390)) { nes->saveSnapshot(drv->snapLater); drv->haveLater = true; laterFrame = curFrame; }
            // "late" (C): in the second half of the trace, the last moment (sampled every 60 frames) at which notes were sounding
            if (traceFrames > 0 && curFrame >= traceFrames / 2 && curFrame - lastNoteFrame <= 2 && curFrame - late2Frame >= 60) { nes->saveSnapshot(drv->snapLate2); drv->haveLate2 = true; late2Frame = curFrame; }
            events.push_back({ 2, f.seq, addr, 0, 0, 0, 0, curFrame, f.seq });
            if (frameCalls[curFrame].size() < 8) frameCalls[curFrame].push_back({ f.a, f.x, f.y });
            spCnt[sp]++;
        }
        if (dbgLeft > 0) { dbgLeft--; char b[96]; snprintf(b, sizeof b, "   push kind=%d addr=%04X sp=%02X play=%d depth=%zu playDepth=%d", kind, addr, sp, (int)f.isPlay, stack.size(), playDepth); drv->log += std::string(b) + "\n"; }
        stack.push_back(f);
    }
    int dbgLeft = 0; int dbgNest = 0;
    void pop(Frame& f) {
        if (dbgLeft > 0) { dbgLeft--; char b[96]; snprintf(b, sizeof b, "   pop  kind=%d addr=%04X sp=%02X play=%d playDepth=%d", f.kind, f.addr, f.spEntry, (int)f.isPlay, playDepth); drv->log += std::string(b) + "\n"; }
        if (f.isPlay) { if (--playDepth <= 0) { playDepth = 0; nes->curMark = MARK_OUT; } }
        if (f.kind == 0 && f.outside && !f.isPlay && playCalls == 0 && f.sound > 0 && !drv->haveInit) { nes->saveSnapshot(drv->snapInit); drv->haveInit = true; initRoutine = f.addr; }
        if (f.kind == 0 && f.outside && !f.isPlay) {
            RoutineInfo& r = info[f.addr]; r.addr = f.addr; r.outsideCalls++; r.outsideDriverWrites += f.drvWrites; r.outsideSoundWrites += f.sound; r.outsideAllWrites += f.allWrites; r.cyclesSum += nes->cycles - f.cyc; r.calls++;
            r.countReg(0, f.a); r.countReg(1, f.x); r.countReg(2, f.y);
            if ((f.drvWrites || f.sound) && events.size() < 2000000) events.push_back({ 1, f.seq, f.addr, f.a, f.x, f.y, 0, curFrame, seq });
        }
    }
    void onJsr(uint16_t target, uint16_t from, uint8_t sp) override {
        push(0, target, sp);
        if (playDepth == 0 || (stack.back().isPlay)) { RoutineInfo& r = info[target]; r.addr = target; r.callers.insert(from); BankCfg c; for (int i = 0; i < 8; i++) c[i] = nes->m.prgMap[i]; r.cfgCount[c]++; }
    }
    void onRts(uint8_t spBefore) override {
        uint8_t target = spBefore + 2;
        while (!stack.empty() && stack.back().kind == 0 && stack.back().spEntry != target && (int8_t)(stack.back().spEntry - target) < 0) { Frame f = stack.back(); stack.pop_back(); pop(f); }
        if (!stack.empty() && stack.back().kind == 0 && stack.back().spEntry == target) { Frame f = stack.back(); stack.pop_back(); pop(f); }
    }
    void onInterrupt(int kind, uint16_t vec, uint8_t sp) override { uint16_t h = nes->peek(vec) | (nes->peek(vec + 1) << 8); push(kind, h, sp); }
    void onRti(uint8_t spBefore) override {
        uint8_t target = spBefore + 3;
        while (!stack.empty() && stack.back().kind == 0 && (int8_t)(stack.back().spEntry - target) <= 0) { Frame f = stack.back(); stack.pop_back(); pop(f); }
        if (!stack.empty() && stack.back().kind != 0 && stack.back().spEntry == target) { Frame f = stack.back(); stack.pop_back(); pop(f); }
    }
    int lastNoteFrame = -1000, late2Frame = -1000, traceFrames = 0;
    void onSoundWrite(uint16_t addr, uint8_t val) override {
        if (val != 0 && ((addr >= 0x4000 && addr <= 0x400B && (addr & 3) >= 2) || addr == 0x4012 || (addr >= 0x4018 && addr != 0x4015 && addr != 0x4017))) lastNoteFrame = curFrame;
        for (int i = (int)stack.size() - 1; i >= 0; i--) { stack[i].sound++; if (stack[i].kind != 0) break; } }
    const std::vector<uint8_t>* shared = nullptr;
    std::map<uint16_t, std::pair<uint64_t, uint64_t>> innermost; // routine -> (driver-RAM writes, all RAM writes) made directly by it (outside PLAY)
    std::vector<uint32_t> playWriteCnt = std::vector<uint32_t>(0x800, 0); // how often PLAY itself writes each RAM byte (internal state vs. request flags)
    void onRamWrite(uint16_t addr, uint8_t val, uint16_t pc) override {
        if (playDepth) { if (addr < 0x800) playWriteCnt[addr]++; return; }
        if (!(addr >= 0x100 && addr < 0x200)) for (int i = (int)stack.size() - 1; i >= 0; i--) { stack[i].allWrites++; if (stack[i].kind != 0) break; }
        bool drvVar = (nes->ramRd[addr] & MARK_PLAY) && !(addr >= 0x100 && addr < 0x200) && !(shared && (*shared)[addr]);
        if (!stack.empty() && stack.back().kind == 0) { auto& im = innermost[stack.back().addr]; im.second++; if (drvVar) im.first++; }
        if (!(nes->ramRd[addr] & MARK_PLAY)) return; // not a driver variable (so far)
        if (addr >= 0x100 && addr < 0x200) return;
        if (shared && (*shared)[addr]) return;         // scratch byte the whole game writes (PLAY merely uses it as a temporary too)
        for (int i = (int)stack.size() - 1; i >= 0; i--) { stack[i].drvWrites++; if (stack[i].kind != 0) break; }
        auto& mw = mailboxWrites[addr]; mw.first++; mailboxVals[addr].insert(val); mailboxValCnt[addr][val]++;
        { auto& t = pcTargets[pc]; if (t.size() < 64) t.insert(addr); mailboxByPc[addr][pc]++; valsByPc[addr][pc].insert(val); }
        if (playCalls > 0) { auto& ma = mailboxAfter[addr]; ma.first++; ma.second.insert(val); }
        if (events.size() < 2000000) events.push_back({ 0, seq++, addr, nes->a, nes->x, nes->y, val, curFrame, seq });
    }
};
}

bool Ripper::pass2(NES& nes) {
    Pass2Hooks h; h.nes = &nes; h.drv = &drv; h.traceFrames = opt.traceFrames;
    if (seedShared.size() == 0x10000) h.shared = &seedShared;
    if (getenv("NSFRIP_DEBUG") && atoi(getenv("NSFRIP_DEBUG")) >= 2) h.dbgLeft = 400;
    if (getenv("NSFRIP_DEBUG")) h.dbgNest = 3;
    nes.hooks = &h; nes.traceOn = true; nes.markOn = true; nes.curMark = MARK_OUT;
    std::fill(nes.ramRd.begin(), nes.ramRd.end(), 0); std::fill(nes.ramWr.begin(), nes.ramWr.end(), 0); std::fill(nes.romTouch.begin(), nes.romTouch.end(), 0); std::fill(nes.romExec.begin(), nes.romExec.end(), 0);
    // seed with the driver-RAM map of a previous run so that requests written before PLAY's first read of the variable
    // (e.g. the title tune requested during boot) are still recognised as mailbox writes
    if (seedRead.size() == nes.ramRd.size()) for (size_t i = 0; i < seedRead.size(); i++) nes.ramRd[i] |= seedRead[i] & MARK_PLAY;
    for (int f = 0; f < opt.traceFrames; f++) {
        if (opt.cancel && *opt.cancel) return false;
        h.curFrame = f; setPad(nes, f); nes.runFrame();
        if (h.stack.size() > 4000) h.stack.erase(h.stack.begin(), h.stack.begin() + 2000);
    }
    nes.traceOn = false; nes.hooks = nullptr; nes.markOn = false;
    if (h.playCalls == 0) { error = "PLAYルーチンが2回目のトレースで呼ばれませんでした"; return false; }
    if (getenv("NSFRIP_DEBUG")) log("  pass2 debug: playDepth(end)=" + std::to_string(h.playDepth) + " stack=" + std::to_string(h.stack.size()) + " mailboxWrites=" + std::to_string(h.mailboxWrites.size()) + " info=" + std::to_string(h.info.size()) + " events=" + std::to_string(h.events.size()));
    drv.driverRead = nes.ramRd; drv.driverWrite = nes.ramWr;
    drv.anyTouched.assign(0x10000, 0); for (int i = 0; i < 0x10000; i++) drv.anyTouched[i] = (nes.ramRd[i] | nes.ramWr[i]) ? 1 : 0;
    int drvBytes = 0; for (int i = 0; i < 0x800; i++) if (nes.ramRd[i] & MARK_PLAY) drvBytes++;
    int drvW = 0; for (int i = 0x6000; i < 0x8000; i++) if (nes.ramRd[i] & MARK_PLAY) drvW++;
    { std::string rs; int st = -1; for (int i = 0; i <= 0x800; i++) { bool on = i < 0x800 && (nes.ramRd[i] & MARK_PLAY); if (on && st < 0) st = i; if (!on && st >= 0) { char b[32]; snprintf(b, sizeof b, " %03X-%03X", st, i - 1); rs += b; st = -1; } } log("  PLAY reads RAM:" + rs); }
    { std::string rs; int st = -1; for (int i = 0; i <= 0x800; i++) { bool on = i < 0x800 && (nes.ramWr[i] & MARK_PLAY); if (on && st < 0) st = i; if (!on && st >= 0) { char b[32]; snprintf(b, sizeof b, " %03X-%03X", st, i - 1); rs += b; st = -1; } } log("  PLAY writes RAM:" + rs); }
    log("driver RAM bytes read by PLAY: " + std::to_string(drvBytes) + " (+" + std::to_string(drvW) + " in WRAM); snapshots: first" + (drv.haveLater ? ", later(frame " + std::to_string(h.laterFrame) + ")" : "") + (drv.haveLate2 ? ", late(frame " + std::to_string(h.late2Frame) + ")" : "") + (drv.haveInit ? ", after sound init " + hex16(h.initRoutine) : ""));
    // bytes written from many different places in the game are shared temporaries, not driver variables
    drv.sharedTemp.assign(0x10000, 0);
    { int n = 0; for (auto& kv : h.mailboxByPc) { int sites = 0, writes = 0; for (auto& pw : kv.second) if (h.pcTargets[pw.first].size() < 8) { sites++; writes += pw.second; } if (sites >= 8 && writes >= std::max(512, opt.traceFrames / 4)) { drv.sharedTemp[kv.first] = 1; n++; } } if (n) log("  shared scratch bytes (written from 8+ sites): " + std::to_string(n)); }
    // --- mailboxes: driver-read RAM written outside PLAY
    struct MB { uint16_t addr; int writes; int distinct; bool bootOnly; bool fillOnly; };
    std::vector<MB> mbs;
    int framesWithPlay = 0; { std::set<int> fr; for (auto& e : h.events) if (e.type == 2) fr.insert(e.frame); framesWithPlay = (int)fr.size(); }
    drv.refresh.clear();
    for (auto& kv : h.mailboxWrites) {
        uint16_t a = kv.first; int w = kv.second.first; int d = (int)h.mailboxVals[a].size();
        if (drv.sharedTemp[a]) continue;
        // ignore writes made by block fills / copies (one instruction writing many driver variables): RAM clearing at boot, state save/restore
        bool fillOnly = false;
        { int fill = 0; for (auto& pw : h.mailboxByPc[a]) if (h.pcTargets[pw.first].size() >= 8) fill += pw.second; if (fill >= w) fillOnly = true; else w -= fill; }
        if (w >= framesWithPlay * 0.5 && d <= 2 && framesWithPlay > 60) {
            // keep-alive variable: rewritten (almost) every frame with a constant value
            uint8_t best = 0; int bc = -1; for (auto& vc : h.mailboxValCnt[a]) if (vc.second > bc) { bc = vc.second; best = vc.first; }
            if (drv.refresh.size() < 6) { drv.refresh.push_back({ a, best }); log("  keep-alive: " + hex16(a) + " <- " + hex8(best) + " (" + std::to_string(w) + " writes)"); }
        }
        if (w > opt.traceFrames * 0.8) continue; // frame counters etc.
        // rank by the writes made while the driver was running (boot-time initialisation adds a spurious value)
        auto ia = h.mailboxAfter.find(a); bool bootOnly = fillOnly || ia == h.mailboxAfter.end() || ia->second.first == 0;
        if (!bootOnly) { w = std::min(w, ia->second.first); d = (int)ia->second.second.size(); }
        if (fillOnly) { w = 1; d = 1; }   // only ever cleared/copied as part of a block: a driver variable the game never used during the trace (ranked last)
        // a variable only initialised at boot is a request only if the driver itself also updates it (acknowledges /
        // clears the request); otherwise it is a pointer or configuration byte (writing random values there plays garbage)
        if (bootOnly && !(nes.ramWr[a] & MARK_PLAY)) { log("  mailbox " + hex16(a) + " boot-only, never written by PLAY: skipped"); continue; }
        mbs.push_back({ a, w, d, bootOnly, fillOnly });
    }
    // event-like variables (rarely written, few distinct values) first; per-frame scratch/sprite data last
    // variables the driver itself rewrites all the time are internal state; request flags are written by PLAY rarely (acknowledge/clear)
    auto mbScore = [&](const MB& m) { double s = m.writes * (1.0 + m.distinct / 8.0); if (m.distinct >= 2 && m.distinct <= 64) s *= 0.5; if (m.bootOnly) s = 1.0 + (m.addr < 0x800 ? h.playWriteCnt[m.addr] : 0) / (double)std::max(1, opt.traceFrames); return s; };
    std::sort(mbs.begin(), mbs.end(), [&](const MB& p, const MB& q) { return mbScore(p) < mbScore(q); });
    // variables that were only cleared as part of a block fill: a handful of them are request queues the game did not
    // use during the trace (worth a try), a whole block of them is the driver's working RAM (not requests)
    {
        int nFill = 0; for (auto& mb : mbs) if (mb.fillOnly) nFill++;
        if (nFill > 16) {
            auto idle = [&](uint16_t a) { if (a >= 0x800) return false; return drv.snapFirst.ram[a] == 0 && (!drv.haveLater || drv.snapLater.ram[a] == 0); };
            std::vector<MB> kept; int dropped = 0;
            for (auto& mb : mbs) { if (mb.fillOnly && !idle(mb.addr)) { dropped++; continue; } kept.push_back(mb); }
            mbs.swap(kept);
            log("  " + std::to_string(nFill) + " driver variables only touched by block fills: " + std::to_string(dropped) + " non-idle ones skipped as working RAM");
        }
    }
    // adjacent pairs that look like a 16-bit ROM pointer (lo, hi with hi in $80-$FF, each written with one or two values)
    // are data pointers, not requests: writing arbitrary values there plays garbage
    {
        std::map<uint16_t, const MB*> byAddr; for (auto& mb : mbs) byAddr[mb.addr] = &mb;
        std::set<uint16_t> ptr;
        for (auto& mb : mbs) {
            auto hi = byAddr.find(mb.addr + 1); if (hi == byAddr.end()) continue;
            if (mb.distinct > 2 || hi->second->distinct > 2) continue;
            if (h.valsNoFill(mb.addr).empty()) continue;   // the low byte must have been written explicitly too
            std::set<uint8_t> hv = h.valsNoFill(mb.addr + 1); bool allHi = !hv.empty(); for (uint8_t v : hv) if (v < 0x80) allHi = false;
            if (!allHi) continue;
            ptr.insert(mb.addr); ptr.insert(mb.addr + 1);
        }
        if (!ptr.empty()) { std::string l = "  pointer-like mailbox pairs skipped:"; for (uint16_t a : ptr) l += " " + hex16(a); log(l); }
        std::vector<MB> kept; for (auto& mb : mbs) if (!ptr.count(mb.addr)) kept.push_back(mb); mbs.swap(kept);
    }
    drv.mailboxes.clear();
    { // mailboxes written during play first; a few slots are kept for variables only written before the first PLAY call
      // (a tune requested during boot, e.g. the title music) which are otherwise indistinguishable from initialisation
        int bootSlots = 0; for (auto& mb : mbs) if (mb.bootOnly) bootSlots++; bootSlots = std::min(bootSlots, opt.maxMailboxes / 2);
        for (auto& mb : mbs) if (!mb.bootOnly && (int)drv.mailboxes.size() < opt.maxMailboxes - bootSlots) drv.mailboxes.push_back(mb.addr);
        for (auto& mb : mbs) if (mb.bootOnly && (int)drv.mailboxes.size() < opt.maxMailboxes) drv.mailboxes.push_back(mb.addr);
        if (getenv("NSFRIP_DEBUG")) for (auto& mb : mbs) { std::string l = "   writers of " + hex16(mb.addr) + ":"; int k = 0; for (auto& pw : h.mailboxByPc[mb.addr]) { if (k++ >= 6) break; l += " pc=" + hex16(pw.first) + "x" + std::to_string(pw.second); } log(l); }
        for (auto& mb : mbs) log("  mailbox " + hex16(mb.addr) + " writes=" + std::to_string(mb.writes) + " distinct=" + std::to_string(mb.distinct) + (mb.bootOnly ? " (boot only, playWr=" + std::to_string(mb.addr < 0x800 ? h.playWriteCnt[mb.addr] : 0) + ")" : "") + (std::find(drv.mailboxes.begin(), drv.mailboxes.end(), mb.addr) != drv.mailboxes.end() ? "" : " [not searched]"));
    }
    // --- INIT candidates
    struct IC { uint16_t addr; double score; };
    std::vector<IC> ics;
    for (auto& kv : h.info) {
        RoutineInfo& r = kv.second; if (!r.outsideCalls) continue;
        if (std::find(drv.playRoutines.begin(), drv.playRoutines.end(), r.addr) != drv.playRoutines.end()) continue;
        if (r.outsideDriverWrites == 0 && r.outsideSoundWrites == 0) continue;
        if (r.addr < 0x8000) continue;
        uint64_t avgCyc = r.cyclesSum / r.outsideCalls; if (avgCyc > 40000) continue;
        double score = r.callers.size() * 3.0 + std::max({ r.distinctRegVals(0), r.distinctRegVals(1), r.distinctRegVals(2) }) * 2.0 + std::min<uint64_t>(r.outsideCalls, 50);
        if (r.outsideDriverWrites) score += 5;
        if (r.outsideSoundWrites) score += 40;   // touches the APU itself (resets channels for a new tune): very likely a request API
        // a request API writes a few driver bytes per call; a routine that writes dozens per call is game logic that
        // merely clears/copies memory overlapping the driver's RAM (ranked far below the small ones)
        double wpc = (double)r.outsideDriverWrites / std::max<uint64_t>(1, r.outsideCalls);
        if (wpc > 48) score *= 0.3; else if (wpc > 16) score *= 0.7;
        // precision: what share of the routine's RAM writes go to the driver (a request API writes almost only driver RAM)
        double prec = (double)r.outsideDriverWrites / std::max<uint64_t>(1, r.outsideAllWrites);
        score *= 0.15 + prec;
        ics.push_back({ r.addr, score });
    }
    std::sort(ics.begin(), ics.end(), [](const IC& p, const IC& q) { return p.score > q.score; });
    if (getenv("NSFRIP_DEBUG")) { std::string l = "   all INIT-like routines:"; for (auto& ic : ics) { RoutineInfo& r = h.info[ic.addr]; char b[64]; snprintf(b, sizeof b, " %04X(%.0f,c%llu,w%llu/%llu,s%llu)", ic.addr, ic.score, (unsigned long long)r.outsideCalls, (unsigned long long)r.outsideDriverWrites, (unsigned long long)r.outsideAllWrites, (unsigned long long)r.outsideSoundWrites); l += b; } log(l); }
    drv.initCandidates.clear(); drv.initInfo.clear();
    for (auto& ic : ics) {
        if ((int)drv.initCandidates.size() >= opt.maxInitRoutines) break;
        drv.initCandidates.push_back(ic.addr); drv.initInfo[ic.addr] = h.info[ic.addr];
        RoutineInfo& r = h.info[ic.addr]; char b[200];
        snprintf(b, sizeof b, "  INIT cand %s score=%.0f callers=%zu outsideCalls=%llu drvW=%llu snd=%llu regs A%d X%d Y%d cfg=%s", hex16(ic.addr).c_str(), ic.score, r.callers.size(), (unsigned long long)r.outsideCalls, (unsigned long long)r.outsideDriverWrites, (unsigned long long)r.outsideSoundWrites, r.distinctRegVals(0), r.distinctRegVals(1), r.distinctRegVals(2), cfgStr(r.topCfg()).c_str());
        log(b);
    }
    // --- mailbox+JSR pairs from event timeline
    std::set<std::pair<uint16_t, uint16_t>> pairs;
    std::set<uint16_t> mbset(drv.mailboxes.begin(), drv.mailboxes.end()), icset(drv.initCandidates.begin(), drv.initCandidates.end());
    std::sort(h.events.begin(), h.events.end(), [](const Pass2Hooks::Ev& p, const Pass2Hooks::Ev& q) { return p.seq < q.seq; });
    for (size_t i = 0; i < h.events.size(); i++) {
        auto& e = h.events[i]; if (e.type != 0 || !mbset.count(e.addr)) continue;
        for (size_t j = i + 1; j < h.events.size() && j < i + 40; j++) {
            auto& g = h.events[j]; if (g.type == 2) break;
            if (g.type == 1 && icset.count(g.addr) && g.seq > e.seq) { pairs.insert({ e.addr, g.addr }); break; }
        }
    }
    drv.mailboxJsrPairs.assign(pairs.begin(), pairs.end());
    // --- observed call sequences: >=2 INIT-candidate calls in the same frame with no PLAY in between (e.g. per-channel song starts)
    {
        std::set<std::string> seen;
        std::vector<Pass2Hooks::Ev> grp; int grpFrame = -1;
        auto flush = [&]() {
            if (grp.size() >= 2 && grp.size() <= 8) {
                Recipe r; r.type = RT_SEQ;
                for (auto& e : grp) { Step st; st.kind = 1; st.routine = e.addr; st.a = e.a; st.x = e.x; st.y = e.y; r.steps.push_back(st); }
                std::string key = r.describe();
                if (!seen.count(key) && drv.sequences.size() < 64) { seen.insert(key); r.initCfg = drv.initInfo[grp[0].addr].topCfg(); drv.sequences.push_back(r); log("  sequence: " + key); }
            }
            grp.clear();
        };
        uint64_t lastEnd = 0;
        for (auto& e : h.events) {
            if (e.type == 2) { flush(); continue; }
            if (e.type != 1 || !icset.count(e.addr)) continue;
            if (e.seq < lastEnd) continue;                      // nested inside a kept call
            if (!grp.empty() && e.frame != grpFrame) flush();
            if (grp.empty()) grpFrame = e.frame;
            grp.push_back(e); lastEnd = e.seqEnd;
        }
        flush();
    }
    // --- bursts: the same INIT candidate called repeatedly with one register counting up (one call per channel track,
    // possibly spread over several frames). Recorded as (routine, register, length) so the search can try every base value.
    {
        std::map<uint16_t, std::vector<Pass2Hooks::Ev>> perRt;
        for (auto& e : h.events) if (e.type == 1 && icset.count(e.addr)) perRt[e.addr].push_back(e);
        std::set<std::string> seen;
        for (auto& kv : perRt) {
            std::vector<Pass2Hooks::Ev>& ev = kv.second; if (ev.size() < 2) continue;
            for (int reg = 0; reg < 3; reg++) {
                std::map<int, int> lenCnt; int run = 1;
                auto val = [&](const Pass2Hooks::Ev& e) { return reg == 0 ? e.a : reg == 1 ? e.x : e.y; };
                for (size_t i = 1; i <= ev.size(); i++) {
                    bool cont = i < ev.size() && val(ev[i]) == (uint8_t)(val(ev[i - 1]) + 1) && ev[i].frame - ev[i - 1].frame <= 16;
                    if (cont) run++; else { if (run >= 2 && run <= 8) lenCnt[run]++; run = 1; }
                }
                std::vector<std::pair<int, int>> ls(lenCnt.begin(), lenCnt.end()); std::sort(ls.begin(), ls.end(), [](auto& a, auto& b) { return a.second > b.second; });
                for (size_t k = 0; k < ls.size() && k < 2; k++) {
                    if (ls[k].first < 3 || ls[k].second < 2) continue;   // short runs are coincidences; a track burst repeats for every tune
                    DriverInfo::Burst b{ kv.first, reg, ls[k].first, ev.back().a, ev.back().x, ev.back().y };
                    char key[48]; snprintf(key, sizeof key, "%04X:%d:%d", b.routine, reg, b.len); if (seen.count(key)) continue; seen.insert(key);
                    drv.bursts.push_back(b); log("  burst: JSR " + hex16(b.routine) + " with " + (reg == 0 ? "A" : reg == 1 ? "X" : "Y") + " counting up x" + std::to_string(b.len) + " (" + std::to_string(ls[k].second) + " times)");
                }
            }
        }
    }
    drv.mailboxVals = h.mailboxVals;
    // feeder detection: the chosen PLAY may only copy RAM shadow registers to the APU while the tune itself is computed
    // by another per-frame routine (the engine) that writes those shadows. That engine is part of the driver.
    drv.feeder = 0;
    {
        // is PLAY alone "frozen"? (called from the later snapshot it keeps writing the very same register values every
        // frame although the game trace was full of notes -> the tune is computed elsewhere)
        bool frozen = false;
        if ((drv.haveLater || drv.haveLate2) && drv.noteFrames >= opt.traceFrames / 8 && drv.playKind == 0) {
            frozen = true;
            for (int which = 0; which < 2 && frozen; which++) {
                if (!(which ? drv.haveLate2 : drv.haveLater)) continue;
                NES t = base; t.loadSnapshot(which ? drv.snapLate2 : drv.snapLater); t.harnessMode = true; t.logSound = true; std::set<std::string> frames; bool ok = true;
                for (int f = 0; f < 120 && ok; f++) { size_t s0 = t.soundLog.size(); ok = t.callSubroutine(drv.playRoutines[0], 0, 0, 0, 200000, false, drv.playSp);
                    std::string k; for (size_t i = s0; i < t.soundLog.size(); i++) { k += (char)(t.soundLog[i].addr & 0xFF); k += (char)t.soundLog[i].val; } frames.insert(k); }
                frozen = ok && frames.size() <= 3;
            }
            if (frozen) log("  PLAY alone is frozen (same register writes every frame): the tune must be computed by another per-frame routine");
        }
        uint64_t totalDrvW = 0; for (auto& kv : h.innermost) totalDrvW += kv.second.first;
        uint16_t best = 0; double bestW = 0;
        for (auto& kv : h.innermost) {
            uint16_t rt = kv.first; if (std::find(drv.playRoutines.begin(), drv.playRoutines.end(), rt) != drv.playRoutines.end()) continue;
            auto it = h.info.find(rt); if (it == h.info.end()) continue; const RoutineInfo& r = it->second;
            if (r.outsideCalls < (uint64_t)opt.traceFrames / 2 || r.outsideCalls > (uint64_t)opt.traceFrames * 3) continue;   // per-frame routine
            if (r.cyclesSum / std::max<uint64_t>(1, r.calls) > 20000) continue;
            double perFrame = (double)kv.second.first / opt.traceFrames, prec = (double)kv.second.first / std::max<uint64_t>(1, kv.second.second);
            double share = (double)kv.second.first / std::max<uint64_t>(1, totalDrvW);   // share of all outside writes to driver RAM
            // only when PLAY alone is provably not the engine (frozen output); a precise shadow-register writer is
            // otherwise just the game's sound-request code and would drag game logic into the NSF
            bool good = frozen && perFrame >= 0.5 && (prec >= 0.5 || share >= 0.6);
            if (good && kv.second.first > bestW) { bestW = kv.second.first; best = rt; }
        }
        // the engine routine must be callable from the harness (a main loop that waits for the NMI would hang) and must
        // actually make PLAY's output move
        if (best) {
            NES t = base; t.loadSnapshot(drv.haveLate2 ? drv.snapLate2 : drv.snapLater); t.harnessMode = true; t.logSound = true; std::set<std::string> frames; bool ok = true;
            for (int f = 0; f < 60 && ok; f++) { size_t s0 = t.soundLog.size(); ok = t.callSubroutine(best, 0, 0, 0, 200000, false, drv.playSp) && t.callSubroutine(drv.playRoutines[0], 0, 0, 0, 200000, false, drv.playSp);
                std::string k; for (size_t i = s0; i < t.soundLog.size(); i++) { k += (char)(t.soundLog[i].addr & 0xFF); k += (char)t.soundLog[i].val; } frames.insert(k); }
            if (!ok || frames.size() <= 3) { log("  engine candidate " + hex16(best) + " rejected (" + (ok ? "output still frozen" : "not callable from the harness") + ")"); best = 0; }
        }
        if (best) { drv.feeder = best; log("  feeder: " + hex16(best) + " writes " + std::to_string((int)bestW) + " driver bytes per trace directly (shadow registers) -> part of PLAY"); }
    }
    // per-frame PLAY call registers: the triple present in most frames is the periodic update call; other triples are requests (INIT API)
    {
        std::map<uint32_t, int> presence, tripleCnt; int total = 0; std::map<uint32_t, std::vector<int>> perFrameCount;
        for (auto& kv : h.frameCalls) { total++; std::map<uint32_t, int> local; for (auto& c : kv.second) { uint32_t k = (c.a << 16) | (c.x << 8) | c.y; local[k]++; tripleCnt[k]++; } for (auto& l : local) { presence[l.first]++; perFrameCount[l.first].push_back(l.second); } }
        uint32_t best = 0; int bc = 0; for (auto& kv : presence) if (kv.second > bc) { bc = kv.second; best = kv.first; }
        drv.playCalls.clear(); drv.playCallAlts.clear();
        { std::vector<std::pair<int, uint32_t>> order; for (auto& kv : presence) order.push_back({ kv.second, kv.first }); std::sort(order.rbegin(), order.rend());
          for (size_t i = 0; i < order.size() && i < 3; i++) if (order[i].first >= total * 0.3) { std::vector<int>& pc = perFrameCount[order[i].second]; std::sort(pc.begin(), pc.end()); int med = std::min(4, pc[pc.size() / 2]); DriverInfo::PlayCall c{ (uint8_t)(order[i].second >> 16), (uint8_t)(order[i].second >> 8), (uint8_t)order[i].second }; drv.playCallAlts.push_back(std::vector<DriverInfo::PlayCall>(med, c)); } }
        if (!drv.playCallAlts.empty()) drv.playCallAlts.insert(drv.playCallAlts.begin(), std::vector<DriverInfo::PlayCall>(1, DriverInfo::PlayCall{ 0, 0, 0 }));
        if (total > 30 && bc >= total * 0.6) {
            std::vector<int>& pc = perFrameCount[best]; std::sort(pc.begin(), pc.end()); int med = pc[pc.size() / 2]; if (med > 4) med = 4;
            DriverInfo::PlayCall c{ (uint8_t)(best >> 16), (uint8_t)(best >> 8), (uint8_t)best };
            // default stays A=X=Y=0 (works for most drivers); the observed triple is tried when that finds nothing
            if (med > 1) drv.playRate = 1.0 * std::max(1.0, (double)h.playCalls / opt.traceFrames / med);
            char b[96]; snprintf(b, sizeof b, "  PLAY call registers: A=%02X X=%02X Y=%02X x%d per frame (%d/%d frames)", c.a, c.x, c.y, med, bc, total); log(b);
            // calls to PLAY with other registers are requests: make PLAY itself an INIT candidate
            RoutineInfo ri; ri.addr = drv.playRoutines[0]; ri.kind = 0; ri.cfgCount[drv.playCfg] = 1;
            for (auto& kv : tripleCnt) if (kv.first != best) { ri.outsideCalls += kv.second; ri.calls += kv.second; for (int i = 0; i < kv.second; i++) { ri.countReg(0, kv.first >> 16); ri.countReg(1, (kv.first >> 8) & 0xFF); ri.countReg(2, kv.first & 0xFF); } }
            if (ri.outsideCalls > 0 && drv.playKind == 0) { ri.callers.insert(0); drv.initCandidates.insert(drv.initCandidates.begin(), ri.addr); drv.initInfo[ri.addr] = ri; log("  PLAY routine also used as request API (" + std::to_string(ri.outsideCalls) + " calls with other registers)"); }
        }
    }
    drv.romExec = nes.romExec;
    // --- static candidates: subroutine entry points next to the driver that the game never called during the trace
    // (a music-start API when the automatic play never left the title screen, FamiTone's music_play next to its
    // update routine...). Every JSR target inside the driver's code neighbourhood that is not a known routine is
    // added as a low-priority INIT candidate (A and X scanned); the brute force sorts the garbage out.
    {
        int lo = 0x10000, hi = -1; const BankCfg& cfg = drv.playCfg;
        for (int slot = 0; slot < 8; slot++) { size_t base = (size_t)cfg[slot] << 12; if (base + 0x1000 > nes.romExec.size()) continue;
            for (int i = 0; i < 0x1000; i++) if (nes.romExec[base + i] & MARK_PLAY) { int a = 0x8000 + (slot << 12) + i; lo = std::min(lo, a); hi = std::max(hi, a); } }
        if (hi >= 0 && hi - lo < 0x3000) {
            int from = std::max(0x8000, lo - 0x800), to = std::min(0xFFFF, hi + 0x800);
            std::map<uint16_t, int> hits;
            for (int slot = 0; slot < 8; slot++) { size_t base = (size_t)cfg[slot] << 12; if (base + 0x1000 > nes.prg.size()) continue;
                for (int i = 0; i + 2 < 0x1000; i++) { if (nes.prg[base + i] != 0x20) continue; int t = nes.prg[base + i + 1] | (nes.prg[base + i + 2] << 8);
                    if (t < from || t > to) continue; if (h.info.count((uint16_t)t)) continue; if (t == drv.playRoutines[0]) continue;
                    // the target must not be in the middle of code we saw executing (a JSR operand that merely looks like an address)
                    size_t tb = ((size_t)cfg[(t >> 12) & 7] << 12) | (t & 0xFFF); if (tb < nes.romExec.size() && (nes.romExec[tb] & (MARK_PLAY | MARK_OUT | MARK_INIT))) continue;
                    uint8_t op = nes.prg[tb]; if (op == 0x00 || op == 0xFF || op == 0x40 || op == 0x60) continue;   // BRK/garbage/RTI/RTS entry: no
                    hits[(uint16_t)t]++; } }
            std::vector<std::pair<int, uint16_t>> order; for (auto& kv : hits) order.push_back({ kv.second, kv.first }); std::sort(order.rbegin(), order.rend());
            int added = 0; std::string l;
            for (auto& o : order) { if (added >= 12) break; RoutineInfo ri; ri.addr = o.second; ri.kind = 0; ri.cfgCount[cfg] = 1; ri.callers.insert(0); ri.countReg(1, 0); ri.countReg(1, 1); ri.isStatic = true;
                drv.initCandidates.push_back(ri.addr); drv.initInfo[ri.addr] = ri; added++; l += " " + hex16(o.second); }
            if (added) log("  static (uncalled) routines near the driver " + hex16((uint16_t)lo) + "-" + hex16((uint16_t)hi) + " added as INIT candidates:" + l);
        }
    }
    { uint8_t bestSp = 0xFF; int bc = -1; for (auto& kv : h.spCnt) if (kv.second > bc) { bc = kv.second; bestSp = kv.first; } drv.playSp = bestSp; if (bestSp < 0xF0) log("  PLAY entered with SP=" + hex8(bestSp) + " (stack relocated in NSF)"); }
    for (auto& p : drv.mailboxJsrPairs) log("  pair: RAM " + hex16(p.first) + " then JSR " + hex16(p.second));
    return true;
}

bool Ripper::analyzeWithPlay(int alt) {
    if (alt < 0 || alt >= (int)drv.playAlternatives.size()) return false;
    const DriverInfo::PlayAlt& pa = drv.playAlternatives[alt];
    drv.playRoutines.clear(); drv.playRoutines.push_back(pa.addr); drv.playKind = pa.kind; drv.playCfg = pa.cfg; drv.playRate = pa.rate;
    log("PLAY = " + hex16(pa.addr) + (pa.kind ? " (interrupt handler)" : "") + " cfg=" + cfgStr(pa.cfg) + " rate=" + std::to_string(pa.rate));
    prog("トレース2回目: ドライバRAM・メールボックス・INIT候補を収集中...");
    for (int round = 0; round < 2; round++) {
    for (int run = 0; run < 2; run++) {
        NES nes2 = base; nes2.reset(); nes2.powerOnRam();
        drv.mailboxes.clear(); drv.initCandidates.clear(); drv.initInfo.clear(); drv.mailboxJsrPairs.clear(); drv.mailboxVals.clear(); drv.sequences.clear(); drv.bursts.clear(); drv.routineReads.clear(); drv.refresh.clear(); drv.haveLater = false; drv.haveInit = false; drv.haveLate2 = false;
        size_t logLen = drv.log.size();
        if (!pass2(nes2)) return false;
        if (run == 0) { seedRead = drv.driverRead; seedShared = drv.sharedTemp; drv.log.resize(logLen); }   // first run only learns the driver's RAM map; second run (seeded) is the one we keep
    }
    seedRead.clear(); seedShared.clear();
    if (round == 0 && drv.feeder && drv.playKind == 0) { log("  PLAY extended with the engine routine " + hex16(drv.feeder) + " (called before the register flush)"); drv.playRoutines.insert(drv.playRoutines.begin(), drv.feeder); drv.playRate = drv.playRate; continue; }
    break;
    }
    // --- parameters of INIT candidates: non-driver RAM they read (game state such as stage number)
    {
        NES n3 = base;
        for (uint16_t rt : drv.initCandidates) {
            RoutineInfo& ri = drv.initInfo[rt];
            n3.loadSnapshot(drv.snapFirst); applyCfg(n3, ri.topCfg());
            std::fill(n3.ramRd.begin(), n3.ramRd.end(), 0); n3.markOn = true; n3.curMark = MARK_INIT;
            bool ok = n3.callSubroutine(rt, ri.topRegVals(0, 1)[0], ri.topRegVals(1, 1)[0], ri.topRegVals(2, 1)[0], 400000);
            n3.markOn = false;
            if (!ok) continue;
            std::vector<uint16_t> v;
            for (int a = 0; a < 0x800; a++) { if (a >= 0x100 && a < 0x200) continue; if (!(n3.ramRd[a] & MARK_INIT)) continue; if (drv.driverRead[a] & MARK_PLAY) continue; v.push_back((uint16_t)a); }
            if (v.size() > 24) v.resize(24);
            if (!v.empty()) { drv.routineReads[rt] = v; std::string l = "  params of " + hex16(rt) + ":"; for (uint16_t a : v) l += " " + hex16(a); log(l); }
        }
    }
    return true;
}

bool Ripper::analyze() {
    if (!loaded) return false;
    drv = DriverInfo();
    prog("トレース1回目: ゲームを自動実行してサウンドルーチンを探索中...");
    // the default input pattern may never reach a state that makes sound (or may skip the title tune); try other styles
    // a trace in which notes were actually played (period registers written in many frames) is what we want;
    // keep the best input style, retrying while the game stayed (nearly) silent
    int bestStyle = -1, bestNotes = -1; DriverInfo bestDrv; std::string allLog;
    for (padStyle = 0; padStyle < 4; padStyle++) {
        drv = DriverInfo();
        if (padStyle) { prog("別の操作パターンで再トレース中 (" + std::to_string(padStyle) + ")..."); log("retrying the game trace with input style " + std::to_string(padStyle)); }
        NES nes = base; nes.reset(); nes.powerOnRam();
        std::map<uint32_t, RoutineInfo> routines; std::vector<std::pair<uint16_t,int>> nmis;
        bool ok = pass1(nes, routines, nmis);
        if (opt.cancel && *opt.cancel) return false;
        // another style replaces the default only when it is clearly better (random input changes the game's
        // dynamics, so a marginal gain in notes is not worth the different trace)
        if (ok && (bestStyle < 0 || drv.noteFrames >= std::max(bestNotes * 2, bestNotes + 60))) { bestNotes = drv.noteFrames; bestStyle = padStyle; bestDrv = drv; }
        allLog += drv.log;
        if (ok && drv.noteFrames >= opt.traceFrames / 8) break;   // plenty of actual notes: good enough
    }
    if (bestStyle < 0) { padStyle = 0; drv.log = allLog; return false; }
    padStyle = bestStyle; drv = bestDrv; drv.log = allLog;
    if (bestStyle) log("using input style " + std::to_string(bestStyle) + " (" + std::to_string(bestNotes) + " frames with notes)");
    return analyzeWithPlay(0);
}

// quality of an attempt: observed requests count most, plausible tunes next (at most 8 per request form, so a
// variable where every value "works" cannot outscore a real song table); demoted/parametric variants not at all
static double attemptScore(const std::vector<Candidate>& cands, int* observed) {
    double sc = 0; int c2 = 0; std::map<std::string, int> perGroup;
    for (auto& c : cands) {
        if (c.confidence == 2) { sc += 3; c2++; continue; }
        if (c.confidence != 1 || c.suspicious) continue;
        const Recipe& r = c.recipe; char k[48];
        if (r.type == RT_SEQ) snprintf(k, sizeof k, "S%04X", r.steps.empty() ? 0 : r.steps[0].routine); else if (r.type == RT_JSR) snprintf(k, sizeof k, "J%04X", r.routine); else snprintf(k, sizeof k, "M%04X:%04X", r.mailbox, r.type == RT_MAILBOX_JSR ? r.routine : 0);
        if (perGroup[k]++ >= 12) continue;
        sc += c.isMusic ? 1.0 : 0.5;
    }
    if (observed) *observed = c2; return sc;
}

bool Ripper::searchWithCallAlts() {
    // PLAY called with A=X=Y=0 works for most drivers; some use the registers as a mode ("A=1: update, else request")
    // and produce nothing (or only silence) with zeros. When the zero call finds no music, retry with the observed
    // per-frame registers and keep the best attempt.
    bool ok = search();
    auto hasMusic = [&]() { for (auto& c : cands) if (c.isMusic && c.confidence >= 1 && !c.suspicious) return true; return false; };
    auto hasObserved = [&]() { for (auto& c : cands) if (c.confidence == 2 && !c.suspicious) return true; return false; };
    if (ok && (hasMusic() || hasObserved())) return true;
    std::vector<Candidate> bestCands = cands; DriverInfo bestDrv = drv; bool have = ok; double bestScore = ok ? attemptScore(cands, nullptr) : -1;
    std::vector<DriverInfo::PlayCall> zero = drv.playCalls;
    for (size_t k = 1; k < drv.playCallAlts.size(); k++) {
        if (opt.cancel && *opt.cancel) return false;
        drv.playCalls = drv.playCallAlts[k];
        char b[96]; snprintf(b, sizeof b, "retry with PLAY registers A=%02X X=%02X Y=%02X", drv.playCalls[0].a, drv.playCalls[0].x, drv.playCalls[0].y); log(b); prog("別のPLAY呼び出しレジスタで再試行...");
        if (!search()) continue;
        double sc = attemptScore(cands, nullptr) + (hasMusic() ? 2 : 0);
        log("  PLAY-register attempt: " + std::to_string(cands.size()) + " candidates, score " + std::to_string((int)sc));
        if (!have || sc > bestScore) { bestCands = cands; bestDrv = drv; bestScore = sc; have = true; }
        if (hasMusic()) break;
    }
    if (!have) return false;
    std::string all = drv.log; cands = bestCands; drv = bestDrv; drv.log = all;
    return true;
}

bool Ripper::run() { return analyze() && runAlternatives(); }

bool Ripper::runAlternatives() {
    // try PLAY alternatives; keep the attempt that yields the most candidates (stop early when clearly enough)
    std::vector<Candidate> bestCands; DriverInfo bestDrv; bool have = false; double bestScore = -1; int bestObs = -1;
    // quality of an attempt: observed requests count most, plausible tunes next; demoted/parametric variants not at all
    auto score = [&]() { int c2 = 0; double sc = attemptScore(cands, &c2); return std::make_pair(sc, c2); };
    for (int alt = 0; alt < (int)drv.playAlternatives.size() && alt < 5; alt++) {
        // alternatives are only worth trying when they look like a driver update routine themselves (no PPU I/O,
        // regular call rate): the NMI handler or a sound-effect sub-routine would "find" garbage requests
        // (when the primary PLAY produced nothing at all - e.g. it cannot even be called from the harness - every
        // alternative is worth a try)
        if (alt > 0 && have && bestObs >= 1 && bestCands.size() >= 5 && !drv.playAlternatives[alt].clean) { log("  PLAY alternative " + hex16(drv.playAlternatives[alt].addr) + " skipped (not a clean update routine)"); continue; }
        if (alt > 0) { prog("別のPLAYルーチンでも試行..."); if (!analyzeWithPlay(alt)) continue; }
        bool ok = searchWithCallAlts();
        if (opt.cancel && *opt.cancel) return false;
        auto sc = ok ? score() : std::make_pair(0.0, 0);
        if (ok) log("  attempt with PLAY " + hex16(drv.playRoutines.empty() ? 0 : drv.playRoutines[0]) + ": " + std::to_string(cands.size()) + " candidates, score " + std::to_string((int)sc.first) + " (" + std::to_string(sc.second) + " observed)");
        // the primary PLAY is replaced only by a clearly better alternative (more observed requests, or a much higher score)
        bool better = !have || (alt == 0) || sc.second > bestObs || (sc.second == bestObs && sc.first > bestScore * 1.5);
        if (ok && better) { bestCands = cands; bestDrv = drv; have = true; bestScore = sc.first; bestObs = sc.second; }
        if (ok && (sc.second >= 4 || sc.first >= 24)) break;   // clearly a working driver interface
    }
    if (!have) return false;
    std::string all = drv.log;   // keep the log of every attempt, not only the winning one
    cands = bestCands; drv = bestDrv; drv.log = all;
    return true;
}
