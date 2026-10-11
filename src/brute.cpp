// brute.cpp - harness execution and brute-force candidate search
#include "ripper.h"
#include <chrono>
#include "synth.h"
#include <cmath>
#include <algorithm>
#include <thread>
#include <mutex>
#include <cstdio>

static inline uint64_t fnv(uint64_t h, uint32_t v) { h ^= v; h *= 1099511628211ULL; return h; }

double Ripper::playCallsPerFrame() const {
    double r = drv.playRate;
    if (r > 4.5) return 1.0;                     // called from a spinning main loop: once per frame
    if (r >= 1.5) return std::min(4.0, std::round(r));
    if (r < 0.75) return 1.0 / std::max(1.0, std::round(1.0 / r)); // every N frames
    return 1.0;
}

static bool dbgAud = getenv("NSFRIP_DEBUG") && atoi(getenv("NSFRIP_DEBUG")) >= 3;
Ripper::HarnessResult Ripper::runRecipe(NES& nes, const NES::Snapshot& snap, const Recipe& r, int frames, std::vector<SoundWrite>* logOut, bool collectTouch, std::vector<uint32_t>* initReads) {
    HarnessResult res;
    nes.loadSnapshot(snap);
    nes.apuFrameIrqInhibit = (drv.playKind != 2); nes.irqLine = 0;   // same APU frame-IRQ visibility as in an NSF player ($4017 = $40)
    nes.logSound = true; nes.soundLog.clear(); nes.logBaseCycle = (uint32_t)nes.cycles;
    nes.markOn = collectTouch; nes.curMark = MARK_INIT;
    if (initReads) { initReads->clear(); nes.readLog = initReads; }
    // action
    applyCfg(nes, r.initCfg);
    for (const Step& st : r.allSteps()) {
        if (st.kind == 2) { if (st.mailbox < 0x800) nes.ram[st.mailbox] = st.value; else if (st.mailbox >= 0x6000 && st.mailbox < 0x8000) nes.wram[st.mailbox & 0x1FFF] = st.value; }
        else if (!nes.callSubroutine(st.routine, st.a, st.x, st.y, 400000, false, drv.playSp)) { res.ok = false; nes.logSound = false; nes.markOn = false; nes.readLog = nullptr; return res; }
    }
    // frames
    int cpf = (int)(29780.5 / playCallsPerFrame());
    nes.curMark = MARK_PLAY;
    size_t logPos = 0; uint64_t total = 1469598103934665603ULL;
    uint8_t chActive = 0;
    // cheap audibility model
    // (length counters, envelopes and the triangle's linear counter are modelled per frame so that a channel left
    //  with a stale period but an expired length/envelope is not reported as sounding)
    static const uint8_t LENTAB[32] = { 10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
    uint8_t ar[0x18]; memcpy(ar, snap.apuRegs, sizeof ar); int lenCnt[4] = {}, envVol[4] = {}, envDiv[4] = {}, linCnt = 0; bool linReload = false; uint8_t enable = 0x1F; bool expTouched = false;
    for (int f = 0; f < frames; f++) {
        applyCfg(nes, r.hasPlayCfg() ? r.playCfg : drv.playCfg);
        uint64_t cycStart = nes.cycles;
        if (drv.playKind == 2) nes.irqLine |= 2; // APU frame IRQ flag visible to IRQ-driven drivers
        for (auto& kv : drv.refresh) { if (kv.first < 0x800) nes.ram[kv.first] = kv.second; else if (kv.first >= 0x6000 && kv.first < 0x8000) nes.wram[kv.first & 0x1FFF] = kv.second; }
        for (uint16_t pr : drv.playRoutines) {
            if (drv.playCalls.empty()) { if (!nes.callSubroutine(pr, 0, 0, 0, 200000, drv.playKind != 0, drv.playSp)) { res.ok = false; nes.logSound = false; nes.markOn = false; nes.readLog = nullptr; return res; } }
            else for (auto& pcl : drv.playCalls) { if (!nes.callSubroutine(pr, pcl.a, pcl.x, pcl.y, 200000, drv.playKind != 0, drv.playSp)) { res.ok = false; nes.logSound = false; nes.markOn = false; nes.readLog = nullptr; return res; } }
        }
        // pad to frame length so DMC/timestamps are regular
        if (nes.cycles - cycStart < (uint64_t)cpf) nes.cycles = cycStart + cpf;
        nes.readLog = nullptr;
        uint64_t fh = 1469598103934665603ULL;
        for (; logPos < nes.soundLog.size(); logPos++) {
            SoundWrite& w = nes.soundLog[logPos];
            if (w.addr == 0x4017 || (w.addr & 0xF800) == 0x4800 || (w.addr & 0xF800) == 0xF800) { if (w.addr != 0x4017 && w.addr != N163_READ_MARK) { chActive |= 32; expTouched = true; } continue; }
            fh = fnv(fh, ((uint32_t)w.addr << 8) | w.val); res.writes++;
            if (w.addr >= 0x4000 && w.addr <= 0x4013) {
                int ch = (w.addr - 0x4000) >> 2; if (ch < 5) chActive |= 1 << ch; ar[w.addr & 0x1F] = w.val;
                if ((w.addr & 3) == 3 && ch < 4) { if (enable & (1 << ch)) lenCnt[ch] = LENTAB[w.val >> 3]; if (ch != 2) { envVol[ch] = 15; envDiv[ch] = (ar[ch * 4] & 15) + 1; } else linReload = true; }
            }
            else if (w.addr != 0x4015 && w.addr != 0x4017) { chActive |= 32; expTouched = true; }
            if (w.addr == 0x4015) { res.channels |= (w.val & 0x1F) << 8; enable = w.val; for (int ch = 0; ch < 4; ch++) if (!(enable & (1 << ch))) lenCnt[ch] = 0; }
        }
        {
            bool aud = false;
            auto volOf = [&](int ch) -> int { uint8_t v = ar[ch * 4]; return (v & 0x10) ? (v & 15) : envVol[ch]; };
            for (int ch = 0; ch < 2; ch++) { uint16_t per = ar[ch * 4 + 2] | ((ar[ch * 4 + 3] & 7) << 8); if (lenCnt[ch] > 0 && per >= 8 && volOf(ch) > 0) aud = true; }
            { uint16_t per = ar[0x0A] | ((ar[0x0B] & 7) << 8); if (lenCnt[2] > 0 && linCnt > 0 && per >= 2) aud = true; }
            if (lenCnt[3] > 0 && volOf(3) > 0) aud = true;
            // clock the frame sequencer: 2 half-frames (length counters) and 4 quarter-frames (envelopes, linear counter)
            for (int ch = 0; ch < 4; ch++) { uint8_t v = ar[ch * 4]; bool halt = ch == 2 ? (v & 0x80) != 0 : (v & 0x20) != 0; if (!halt) lenCnt[ch] = std::max(0, lenCnt[ch] - 2); }
            for (int ch : { 0, 1, 3 }) { uint8_t v = ar[ch * 4]; if (v & 0x10) continue; int period = (v & 15) + 1; for (int q = 0; q < 4; q++) { if (--envDiv[ch] <= 0) { envDiv[ch] = period; if (envVol[ch] > 0) envVol[ch]--; else if (v & 0x20) envVol[ch] = 15; } } }
            for (int q = 0; q < 4; q++) { if (linReload) linCnt = ar[8] & 0x7F; else if (linCnt > 0) linCnt--; if (!(ar[8] & 0x80)) linReload = false; }
            if ((enable & 0x10) && ar[0x13]) aud = true;
            if (expTouched) aud = true;
            if (aud) res.audibleFrames++;
            res.frameAudible.push_back(aud ? 1 : 0);
            if (dbgAud && f < 6) fprintf(stderr, "AUD f=%d aud=%d en=%02X len=%d/%d/%d/%d env=%d/%d/%d lin=%d regs=%02X %02X %02X %02X | %02X %02X %02X %02X | %02X %02X %02X %02X\n", f, (int)aud, enable, lenCnt[0], lenCnt[1], lenCnt[2], lenCnt[3], envVol[0], envVol[1], envVol[3], linCnt, ar[0], ar[1], ar[2], ar[3], ar[4], ar[5], ar[6], ar[7], ar[8], ar[9], ar[10], ar[11]);
        }
        res.frameHash.push_back(fh); total = fnv(total, (uint32_t)fh) ^ (fh >> 32);
    }
    res.channels |= chActive;
    res.total = total;
    if (collectTouch) {
        for (int a = 0; a < 0x800; a++) if ((nes.ramRd[a] & MARK_PLAY) && !(drv.driverRead[a] & MARK_PLAY) && a != r.mailbox) res.foreignRamReads++;
        if (drv.romExec.size() == nes.romData.size()) for (size_t i = 0; i < nes.romData.size(); i++) if ((nes.romData[i] & MARK_PLAY) && drv.romExec[i]) res.codeBytesAsData++;
    }
    if (logOut) *logOut = nes.soundLog;
    nes.logSound = false; nes.markOn = false;
    return res;
}

// screening of recipes: run short, dedupe by hash, drop baseline/silent
std::vector<Candidate> Ripper::screen(const NES::Snapshot& snap, std::vector<Recipe>& recipes, uint64_t baselineHash, bool& any, bool baselineAudible) {
    std::vector<Candidate> out;
    std::mutex mu; std::atomic<size_t> next{ 0 }; std::atomic<int> done{ 0 };
    std::vector<std::pair<int, HarnessResult>> results(recipes.size());
    int nth = std::max(1, opt.threads);
    std::vector<std::thread> ths;
    for (int t = 0; t < nth; t++) ths.emplace_back([&]() {
        NES nes = base;
        for (;;) {
            size_t i = next++; if (i >= recipes.size()) break;
            if (opt.cancel && *opt.cancel) break;
            results[i] = { (int)i, runRecipe(nes, snap, recipes[i], opt.screenFrames) };
            int d = ++done; if ((d & 255) == 0) prog("候補スクリーニング " + std::to_string(d) + "/" + std::to_string(recipes.size()));
        }
    });
    for (auto& t : ths) t.join();
    std::set<uint64_t> seen; if (!baselineAudible) seen.insert(baselineHash);
    for (size_t i = 0; i < recipes.size(); i++) {
        HarnessResult& hr = results[i].second;
        if (!hr.ok) continue;
        // a request that reproduces the baseline exactly is kept once when the snapshot itself is playing music
        // (the pending request of the snapshot is then this very candidate, e.g. the title tune requested during boot)
        if (hr.total == baselineHash && !baselineAudible) continue;
        if (hr.writes < 6) continue;
        if ((hr.channels & 0x3F) == 0) continue;
        if (hr.audibleFrames < 2) continue;
        if (seen.count(hr.total)) continue;
        seen.insert(hr.total);
        Candidate c; c.recipe = recipes[i]; c.hashShort = hr.total; c.writes = hr.writes; c.channels = hr.channels; c.isBaseline = (hr.total == baselineHash);
        out.push_back(c); any = true;
    }
    return out;
}

static void analyzeLoop(Candidate& c, const std::vector<uint64_t>& fh, int longFrames, const std::vector<uint64_t>* baseFh = nullptr, const std::vector<uint8_t>* audible = nullptr) {
    const uint64_t EMPTY = 1469598103934665603ULL;
    int last = -1; for (int i = (int)fh.size() - 1; i >= 0; i--) if (fh[i] != EMPTY) { last = i; break; }
    c.durationFrames = 0; c.loopLen = 0; c.loopStart = 0;
    int n = (int)fh.size();
    if (last >= 0 && last < n - 120) { c.durationFrames = last + 1; return; } // ends with ≥2s silence
    // the snapshot may already be playing music: a request whose write stream falls back onto the baseline's
    // (same per-frame hashes from some frame on) was only a transient on top of it (sound effect, jingle)
    if (baseFh && baseFh->size() == fh.size() && n > 0) {
        int conv = -1; for (int i = n - 1; i >= 0; i--) { if (fh[i] != (*baseFh)[i]) { conv = i; break; } }
        if (conv >= 0 && conv < n - 120) { c.durationFrames = conv + 1; c.overlay = true; return; }
    }
    // loop detection: find period p so that fh[i]==fh[i-p] for all i in [n-W, n)
    int W = std::min(n / 2, 900);
    for (int p = 8; p <= n / 2; p++) {
        bool ok = true;
        for (int i = n - W; i < n; i++) if (fh[i] != fh[i - p]) { ok = false; break; }
        if (ok) {
            // make sure not trivially constant (all equal)
            int start = n - W; while (start - 1 >= 0 && start - 1 + p < n && fh[start - 1] == fh[start - 1 + p]) start--;
            // a short inaudible loop is the driver idling (periodic housekeeping writes) after the tune ended
            if (p < 64 && audible && audible->size() == fh.size()) { bool quiet = true; for (int i = start; i < n; i++) if ((*audible)[i]) { quiet = false; break; } if (quiet) { c.durationFrames = start; return; } }
            c.loopLen = p; c.loopStart = start; return;
        }
    }
}

// Find a request that silences the sound driver when applied to snapshot `from`; on success `out` is the state a little
// after silence is reached. The stop request must not disable the driver: a music candidate must still sound afterwards.
bool Ripper::buildSilencedSnapshot(const NES::Snapshot& from, const std::vector<Recipe>& recipes, NES::Snapshot& out) {
    if (recipes.empty()) return false;
    prog("無音化リクエストを探索中...");
    const int FR = 180, QUIET = 60;
    std::vector<int> silentAt(recipes.size(), -1); std::vector<uint64_t> stopHash(recipes.size(), 0);
    std::atomic<size_t> next{ 0 }; std::vector<std::thread> ths;
    for (int t = 0; t < std::max(1, opt.threads); t++) ths.emplace_back([&]() {
        NES nes = base;
        for (;;) {
            size_t i = next++; if (i >= recipes.size()) break;
            if (opt.cancel && *opt.cancel) break;
            if (recipes[i].type == RT_SEQ) continue;
            HarnessResult hr = runRecipe(nes, from, recipes[i], FR);
            if (!hr.ok || (int)hr.frameAudible.size() != FR) continue;
            int last = -1; for (int f = 0; f < FR; f++) if (hr.frameAudible[f]) last = f;
            if (last < FR - QUIET) { silentAt[i] = last + 1; stopHash[i] = hr.total; }
        }
    });
    for (auto& t : ths) t.join();
    std::vector<size_t> order; for (size_t i = 0; i < recipes.size(); i++) if (silentAt[i] >= 0) order.push_back(i);
    if (order.empty()) { log("  no silencing request found"); return false; }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { int sa = silentAt[a] + (recipes[a].type == RT_MAILBOX ? 0 : 20), sb = silentAt[b] + (recipes[b].type == RT_MAILBOX ? 0 : 20); if (sa != sb) return sa < sb; return a < b; });
    // a music candidate of the original search used to validate that the driver still accepts requests
    // several probes, different routines first: a "request" that merely re-enters PLAY (keeps the base tune going)
    // would be silent after the stop and wrongly condemn every stop request
    std::vector<const Recipe*> probes;
    { std::set<std::string> seen; auto add = [&](const Candidate& c) { if (probes.size() >= 8) return; std::string k = c.recipe.type == RT_JSR ? "J" + std::to_string(c.recipe.routine) : c.recipe.type == RT_MAILBOX ? "M" + std::to_string(c.recipe.mailbox) : c.recipe.describe(); if (seen.count(k)) return; seen.insert(k); probes.push_back(&c.recipe); };
      for (auto& c : cands) if (c.isMusic && !c.isBaseline && c.confidence == 2) add(c);
      for (auto& c : cands) if (c.isMusic && !c.isBaseline) add(c);
      for (auto& c : cands) if (!c.isBaseline) add(c); }
    // among the stop requests, prefer the one after which the most probes still sound (a pause/mute flag may let one
    // request through while blocking the others)
    NES nes = base; std::set<uint64_t> tried; int tries = 0; int bestSound = 0; NES::Snapshot bestS; std::string bestDesc;
    for (size_t k = 0; k < order.size() && tries < 12; k++) {
        const Recipe& r = recipes[order[k]];
        if (tried.count(stopHash[order[k]])) continue; tried.insert(stopHash[order[k]]); tries++;
        HarnessResult hr = runRecipe(nes, from, r, silentAt[order[k]] + 30);
        if (!hr.ok) continue;
        NES::Snapshot s; nes.saveSnapshot(s);
        int sounds = probes.empty() ? 1 : 0;
        for (const Recipe* probe : probes) { NES n2 = base; HarnessResult p = runRecipe(n2, s, *probe, 150); if (p.ok && p.audibleFrames >= 2) sounds++; }
        if (sounds == 0) { log("  stop request " + r.describe() + " disables the driver (all probes silent); skipped"); continue; }
        if (sounds > bestSound) { bestSound = sounds; bestS = s; bestDesc = r.describe() + " (silent after " + std::to_string(silentAt[order[k]]) + " frames, " + std::to_string(sounds) + "/" + std::to_string(probes.size()) + " probes sound)"; }
        if (sounds == (int)probes.size()) break;
    }
    if (bestSound == 0) return false;
    out = bestS; log("  silenced snapshot S built with " + bestDesc);
    return true;
}

bool Ripper::search() {
    cands.clear();
    if (drv.playRoutines.empty()) return false;
    // local copies: the loop below rewrites drv.snapFirst while later snapshots may still be tried
    NES::Snapshot snapI = drv.snapInit, snapA = drv.snapFirst, snapB = drv.snapLater, snapC = drv.snapLate2;
    const int NSNAP = 4;
    const NES::Snapshot* snaps[NSNAP] = { drv.haveInit ? &snapI : nullptr, &snapA, drv.haveLater ? &snapB : nullptr, drv.haveLate2 ? &snapC : nullptr };
    bool lastBaselineAudible = false; std::vector<Recipe> lastRecipes;
    const std::vector<Recipe>* recipesOverride = nullptr; BankCfg sweepCfg{};   // set by the bank sweep
    auto searchOne = [&](const NES::Snapshot& snap, const char* label, int snapId) -> bool {
        cands.clear();
        prog(std::string("総当たり探索 (スナップショット") + label + ")...");
        // baseline
        NES nes = base; Recipe none; none.type = RT_MAILBOX; none.mailbox = 0x7FF; none.value = snap.ram[0x7FF]; none.initCfg = drv.playCfg;
        if (recipesOverride) { none.initCfg = sweepCfg; none.playCfg = sweepCfg; }
        HarnessResult bl = runRecipe(nes, snap, none, opt.screenFrames);
        uint64_t baseline = bl.ok ? bl.total : 0;
        bool baselineAudible = bl.ok && bl.writes >= 6 && (bl.channels & 0x3F) != 0 && bl.audibleFrames >= 2;
        lastBaselineAudible = baselineAudible;
        HarnessResult blLong = runRecipe(nes, snap, none, opt.longFrames);   // for overlay detection (requests on top of music already playing)
        log(std::string("baseline hash ok=") + (bl.ok ? "1" : "0") + " writes=" + std::to_string(bl.writes) + " audibleFrames=" + std::to_string(bl.audibleFrames) + "/" + std::to_string(opt.screenFrames) + " ch=" + hex8((uint8_t)bl.channels) + (baselineAudible ? " (snapshot is playing sound)" : ""));
        // build recipes
        std::vector<Recipe> recipes;
        if (recipesOverride) recipes = *recipesOverride;
        else for (size_t ci = 0; ci < drv.initCandidates.size(); ci++) {
            uint16_t rt = drv.initCandidates[ci];
            RoutineInfo& ri = drv.initInfo[rt]; BankCfg cfg = ri.topCfg(); bool allZero = true; for (int i = 0; i < 8; i++) if (cfg[i]) allZero = false; if (allZero) cfg = drv.playCfg;
            // which registers vary at entry?
            int dA = ri.distinctRegVals(0), dX = ri.distinctRegVals(1), dY = ri.distinctRegVals(2);
            std::vector<int> regs;
            if (ri.isStatic) { regs = { 0, 1 }; }
            else {
                if (dA > 1 || (dX <= 1 && dY <= 1)) regs.push_back(0);
                if (dX > 1) regs.push_back(1);
                if (dY > 1) regs.push_back(2);
            }
            int k = ci < 6 ? 3 : 1;
            std::vector<uint8_t> top[3] = { ri.topRegVals(0, k), ri.topRegVals(1, k), ri.topRegVals(2, k) };
            for (int reg : regs) {
                std::vector<uint8_t>& o1 = top[reg == 0 ? 1 : 0]; std::vector<uint8_t>& o2 = top[reg == 2 ? 1 : 2];
                for (uint8_t v1 : o1) for (uint8_t v2 : o2) for (int v = 0; v < 256; v++) {
                    Recipe r; r.type = RT_JSR; r.routine = rt; r.initCfg = cfg; r.argReg = reg;
                    if (reg == 0) { r.a = v; r.x = v1; r.y = v2; } else if (reg == 1) { r.x = v; r.a = v1; r.y = v2; } else { r.y = v; r.a = v1; r.x = v2; }
                    recipes.push_back(r);
                }
            }
            // also A=X=Y=v variant for the top few routines
            if (ci < 4) for (int v = 0; v < 256; v++) { Recipe r; r.type = RT_JSR; r.routine = rt; r.initCfg = cfg; r.argReg = 3; r.a = r.x = r.y = v; recipes.push_back(r); }
        }
        if (!recipesOverride) {
        for (uint16_t mb : drv.mailboxes) for (int v = 0; v < 256; v++) { Recipe r; r.type = RT_MAILBOX; r.mailbox = mb; r.value = v; r.initCfg = drv.playCfg; recipes.push_back(r); }
        for (auto& pr : drv.mailboxJsrPairs) {
            RoutineInfo& ri = drv.initInfo[pr.second]; BankCfg cfg = ri.topCfg();
            for (int v = 0; v < 256; v++) { Recipe r; r.type = RT_MAILBOX_JSR; r.mailbox = pr.first; r.value = v; r.routine = pr.second; r.initCfg = cfg; r.a = ri.topRegVals(0, 1)[0]; r.x = ri.topRegVals(1, 1)[0]; r.y = ri.topRegVals(2, 1)[0]; recipes.push_back(r); }
        }
        // routine parameters: write a value to RAM the INIT candidate reads, then call it with its usual registers
        for (auto& kv : drv.routineReads) {
            RoutineInfo& ri = drv.initInfo[kv.first];
            for (uint16_t a : kv.second) for (int v = 0; v < 256; v++) { Recipe r; r.type = RT_MAILBOX_JSR; r.mailbox = a; r.value = v; r.routine = kv.first; r.initCfg = ri.topCfg(); r.a = ri.topRegVals(0, 1)[0]; r.x = ri.topRegVals(1, 1)[0]; r.y = ri.topRegVals(2, 1)[0]; recipes.push_back(r); }
        }
        // bursts: JSR routine with reg = base, base+1, ... base+len-1 for every base
        for (auto& b : drv.bursts) {
            BankCfg cfg = drv.initInfo.count(b.routine) ? drv.initInfo[b.routine].topCfg() : drv.playCfg;
            for (int base = 0; base + b.len <= 256; base++) {
                Recipe r; r.type = RT_SEQ; r.initCfg = cfg;
                for (int k = 0; k < b.len; k++) { Step st; st.kind = 1; st.routine = b.routine; st.a = b.a; st.x = b.x; st.y = b.y; uint8_t v = (uint8_t)(base + k); if (b.reg == 0) st.a = v; else if (b.reg == 1) st.x = v; else st.y = v; r.steps.push_back(st); }
                recipes.push_back(r);
            }
        }
        // observed call sequences + shifted variants
        for (const Recipe& sq : drv.sequences) {
            recipes.push_back(sq);
            bool va = false, vx = false, vy = false;
            for (size_t k = 1; k < sq.steps.size(); k++) { if (sq.steps[k].a != sq.steps[0].a) va = true; if (sq.steps[k].x != sq.steps[0].x) vx = true; if (sq.steps[k].y != sq.steps[0].y) vy = true; }
            if (!va && !vx && !vy) continue;
            for (int d = -255; d <= 255; d++) {   // the whole byte range: per-channel track numbers of other tunes can be far from the observed ones
                if (d == 0) continue; Recipe v = sq; bool ok = true;
                for (auto& st : v.steps) { if (va) { int n = st.a + d; if (n < 0 || n > 255) ok = false; st.a = (uint8_t)n; } if (vx) { int n = st.x + d; if (n < 0 || n > 255) ok = false; st.x = (uint8_t)n; } if (vy) { int n = st.y + d; if (n < 0 || n > 255) ok = false; st.y = (uint8_t)n; } }
                if (ok) recipes.push_back(v);
            }
        }
        }
        log("recipes to screen: " + std::to_string(recipes.size()));
        if (!recipesOverride) lastRecipes = recipes;
        bool any = false;
        std::vector<Candidate> sc = screen(snap, recipes, baseline, any, baselineAudible);
        log("screen survivors: " + std::to_string(sc.size()));
        if (sc.empty()) return false;
        // long run for survivors
        prog("候補の長時間解析 (" + std::to_string(sc.size()) + "件)...");
        std::atomic<size_t> next{ 0 }; std::vector<std::thread> ths; std::vector<HarnessResult> lr(sc.size());
        for (int t = 0; t < std::max(1, opt.threads); t++) ths.emplace_back([&]() { NES n2 = base; for (;;) { size_t i = next++; if (i >= sc.size()) break; if (opt.cancel && *opt.cancel) break; lr[i] = runRecipe(n2, snap, sc[i].recipe, opt.longFrames); } });
        for (auto& t : ths) t.join();
        std::set<uint64_t> seenLong;
        for (size_t i = 0; i < sc.size(); i++) {
            if (!lr[i].ok) continue;
            if (seenLong.count(lr[i].total)) continue; seenLong.insert(lr[i].total);
            Candidate c = sc[i]; c.hashLong = lr[i].total; c.writes = lr[i].writes; c.channels = lr[i].channels; c.snapId = snapId;
            analyzeLoop(c, lr[i].frameHash, opt.longFrames, blLong.ok ? &blLong.frameHash : nullptr, &lr[i].frameAudible);
            { uint64_t t = 1469598103934665603ULL; for (size_t f = 120; f < lr[i].frameHash.size() && f < 480; f++) t = fnv(t, (uint32_t)lr[i].frameHash[f]) ^ (lr[i].frameHash[f] >> 32); c.hashTail = t; }
            if (c.durationFrames && c.durationFrames <= 2) continue; // stop/no-op commands
            if (lr[i].audibleFrames < 2) continue;
            c.isMusic = (c.durationFrames == 0) || c.durationFrames >= 240 || c.loopLen > 0;
            if (c.durationFrames && c.durationFrames < 240) c.isMusic = false;
            if (c.loopLen > 0 && c.loopLen < 64) c.isMusic = false;   // a sub-second loop is a continuous sound effect (engine, footsteps), not a tune
            // confidence from observed argument values
            Recipe& r = c.recipe;
            if (r.type == RT_SEQ) {
                bool obs = false; for (const Recipe& sq : drv.sequences) if (sq.describe() == r.describe()) obs = true;
                c.confidence = obs ? 2 : 1;
            } else if (r.type == RT_JSR) {
                RoutineInfo& ri = drv.initInfo[r.routine];
                int reg = r.argReg == 3 ? 0 : r.argReg; uint8_t v = reg == 0 ? r.a : reg == 1 ? r.x : r.y;
                if (!ri.regCnt[reg].empty()) {
                    int maxObs = 0; for (int k = 0; k < 256; k++) if (ri.regCnt[reg][k]) maxObs = k;
                    c.confidence = ri.regCnt[reg][v] ? 2 : (v <= maxObs ? 1 : 0);
                }
            } else {
                auto it = drv.mailboxVals.find(r.mailbox);
                if (it != drv.mailboxVals.end()) { int maxObs = it->second.empty() ? 0 : *it->second.rbegin(); c.confidence = it->second.count(r.value) ? 2 : (r.value <= maxObs ? 1 : 0); }
                else if (r.mailbox < 0x800 && snap.ram[r.mailbox] == r.value) c.confidence = 2;
            }
            cands.push_back(c);
        }
        // real audibility: render 2.5 s through the synth and drop near-silent candidates
        {
            std::vector<Candidate> kept; NES n4 = base;
            int cpf = (int)(29780.5 / playCallsPerFrame());
            for (auto& c : cands) {
                std::vector<SoundWrite> lg; HarnessResult t = runRecipe(n4, snap, c.recipe, 150, &lg);
                if (!t.ok) continue;
                Synth syn; syn.reset(base.expansion); NES* np = &n4; syn.setMemReader([np](uint16_t a) { return np->peek(a); });
                for (uint16_t a = 0x4000; a <= 0x4013; a++) syn.write(a, snap.apuRegs[a & 0x1F]); syn.write(0x4015, snap.apuRegs[0x15] | 0x0F); syn.write(0x4017, snap.apuRegs[0x17]);
                std::vector<int16_t> out; size_t li = 0; uint32_t base0 = 0;
                for (int f = 0; f < 150; f++) {
                    uint32_t fstart = (uint32_t)f * cpf, fend = fstart + cpf, tcur = fstart;
                    while (li < lg.size() && lg[li].cycle < fend) { uint32_t at = std::max(lg[li].cycle, fstart); if (at > tcur) { syn.run(at - tcur, out); tcur = at; } syn.write(lg[li].addr, lg[li].val); li++; }
                    if (tcur < fend) syn.run(fend - tcur, out);
                }
                double e = 0; for (int16_t v : out) e += (double)v * v; double rms = sqrt(e / std::max<size_t>(1, out.size()));
                if (rms < 40) continue;
                kept.push_back(c);
            }
            cands.swap(kept);
        }
        // garbage detection: table-bounds heuristic + RAM/code heuristics (needs touch collection)
        {
            struct Info { std::vector<uint32_t> initReads; std::set<uint32_t> rinit, rplay; };
            std::vector<Info> infos(cands.size());
            std::vector<int> codeAsData(cands.size(), 0), foreignRam(cands.size(), 0);
            NES n3 = base;
            for (size_t i = 0; i < cands.size(); i++) {
                Candidate& c = cands[i];
                std::fill(n3.ramRd.begin(), n3.ramRd.end(), 0); std::fill(n3.romData.begin(), n3.romData.end(), 0);
                HarnessResult t = runRecipe(n3, snap, c.recipe, std::min(opt.longFrames, 600), nullptr, true, &infos[i].initReads);
                codeAsData[i] = t.codeBytesAsData; foreignRam[i] = t.foreignRamReads;
                for (size_t k = 0; k < n3.romData.size(); k++) { if (n3.romData[k] & MARK_INIT) infos[i].rinit.insert((uint32_t)k); if (n3.romData[k] & MARK_PLAY) infos[i].rplay.insert((uint32_t)k); }
                char b[96]; snprintf(b, sizeof b, "codeAsData=%d foreignRam=%d%s", t.codeBytesAsData, t.foreignRamReads, c.overlay ? " overlay" : ""); c.note = b;
            }
            // calibrate against observed (certainly valid) candidates
            { int mc = 0, mf = 0; for (size_t i = 0; i < cands.size(); i++) if (cands[i].confidence == 2) { mc = std::max(mc, codeAsData[i]); mf = std::max(mf, foreignRam[i]); }
              int tc = std::max(16, mc * 2 + 8), tf = std::max(48, mf * 2 + 16);
              for (size_t i = 0; i < cands.size(); i++) cands[i].suspicious = cands[i].confidence != 2 && (codeAsData[i] >= tc || foreignRam[i] >= tf); }
            // first argument-dependent ROM read per candidate (table entry); D = all other data reads
            std::map<std::string, std::vector<size_t>> groups;
            for (size_t i = 0; i < cands.size(); i++) { Recipe& r = cands[i].recipe; char k[48]; if (r.type == RT_SEQ) snprintf(k, sizeof k, "S%04X:%zu", r.steps.empty() ? 0 : r.steps[0].routine, r.steps.size()); else if (r.type == RT_JSR) snprintf(k, sizeof k, "J%04X:%d", r.routine, r.argReg == 3 ? 0 : r.argReg); else snprintf(k, sizeof k, "M%04X:%04X", r.mailbox, r.type == RT_MAILBOX_JSR ? r.routine : 0); groups[k].push_back(i); }
            std::vector<uint32_t> first(cands.size(), 0xFFFFFFFF);
            for (auto& g : groups) {
                // common = reads shared by (nearly) all members of the group
                std::map<uint32_t, int> cnt; for (size_t i : g.second) for (uint32_t a : infos[i].rinit) cnt[a]++;
                int n = (int)g.second.size();
                std::set<uint32_t> common; for (auto& kv : cnt) if (kv.second >= std::max(2, (n * 3 + 3) / 4) || (n == 1)) common.insert(kv.first);
                if (n == 1) common.clear();
                for (size_t i : g.second) { for (uint32_t a : infos[i].initReads) if (!common.count(a)) { first[i] = a; break; } }
            }
            std::vector<bool> bad(cands.size(), false);
            std::set<uint32_t> firsts; for (size_t i = 0; i < cands.size(); i++) if (first[i] != 0xFFFFFFFF && cands[i].confidence == 2) firsts.insert(first[i]);
            for (int iter = 0; iter < 4; iter++) {
                std::set<uint32_t> D; for (size_t i = 0; i < cands.size(); i++) { for (uint32_t a : infos[i].rinit) if (!firsts.count(a) && a != first[i]) D.insert(a); for (uint32_t a : infos[i].rplay) if (!firsts.count(a) && a != first[i]) D.insert(a); }
                std::set<uint32_t> nf; bool changed = false;
                for (size_t i = 0; i < cands.size(); i++) {
                    uint32_t f = first[i]; bool b = false;
                    if (cands[i].confidence != 2 && f != 0xFFFFFFFF) b = D.count(f) || (f < drv.romExec.size() && drv.romExec[f]);
                    if (b != bad[i]) changed = true; bad[i] = b;
                    if (!b && f != 0xFFFFFFFF) nf.insert(f);
                }
                if (nf == firsts && !changed) break;
                firsts = nf;
            }
            if (getenv("NSFRIP_DEBUG")) for (size_t i = 0; i < cands.size() && i < 12; i++) {
                fprintf(stderr, "DBG cand %zu first=%05X exec=%d bad=%d initReads:", i, first[i], first[i] < drv.romExec.size() ? drv.romExec[first[i]] : -1, (int)bad[i]); for (size_t k = 0; k < infos[i].initReads.size() && k < 12; k++) fprintf(stderr, " %05X", infos[i].initReads[k]); fprintf(stderr, "\n");
                if (first[i] != 0xFFFFFFFF) for (size_t j = 0; j < cands.size(); j++) if (j != i && (infos[j].rinit.count(first[i]) || infos[j].rplay.count(first[i]))) fprintf(stderr, "   read by cand %zu (%s) init=%d play=%d\n", j, cands[j].recipe.describe().c_str(), (int)infos[j].rinit.count(first[i]), (int)infos[j].rplay.count(first[i]));
            }
            for (size_t i = 0; i < cands.size(); i++) {
                Candidate& c = cands[i];
                if (c.confidence == 2) continue;
                c.confidence = bad[i] ? 0 : 1;
                if (first[i] != 0xFFFFFFFF) { char b[64]; snprintf(b, sizeof b, " tableRead=%05X%s", first[i], bad[i] ? "(in data/code)" : ""); c.note += b; }
            }
            // parametric APIs: an argument that yields distinct output for almost every value is a parameter (tempo, pitch...), not a song index
            std::vector<bool> drop(cands.size(), false);
            // many large groups at once = the mailboxes are the driver's per-channel registers (period, volume...) written
            // by game logic, and every value "works": keep only observed values and the lowest few of each big group
            { int big = 0; for (auto& g : groups) { int n = 0; for (size_t i : g.second) if (cands[i].confidence >= 1) n++; if (n >= 160) big++; }
              if (big >= 3) { int demoted = 0; for (auto& g : groups) { std::vector<size_t> pl; for (size_t i : g.second) if (cands[i].confidence >= 1) pl.push_back(i); if (pl.size() < 160) continue;
                    std::sort(pl.begin(), pl.end(), [&](size_t a, size_t b) { return cands[a].recipe.describe() < cands[b].recipe.describe(); });
                    for (size_t k = 8; k < pl.size(); k++) if (cands[pl[k]].confidence != 2) { cands[pl[k]].confidence = 0; cands[pl[k]].note += " (register-like)"; drop[pl[k]] = true; demoted++; } }
                log("  " + std::to_string(big) + " variables with 160+ working values each: register-like mailboxes, " + std::to_string(demoted) + " variants dropped"); } }
            auto argOf = [&](const Candidate& c) -> int { const Recipe& r = c.recipe; if (r.type == RT_JSR) return r.argReg == 1 ? r.x : r.argReg == 2 ? r.y : r.a; if (r.type == RT_SEQ) return r.steps.empty() ? 0 : r.steps[0].a; return r.value; };
            for (auto& g : groups) {
                std::vector<size_t> plausible; for (size_t i : g.second) if (cands[i].confidence >= 1) plausible.push_back(i);
                size_t n = plausible.size();
                // a variable whose every value yields the same tune with marginal differences (same structure, same
                // number of register writes) is driver state (tempo, pitch, position...), not a song selector
                if (n >= 30) {
                    std::vector<int> ws; std::set<std::tuple<int, int, uint32_t>> structs;
                    for (size_t i : plausible) { const Candidate& c = cands[i]; ws.push_back(c.writes); structs.insert({ c.durationFrames == 0 ? 0 : c.durationFrames < 240 ? 1 : 2, c.loopLen == 0 ? 0 : c.loopLen < 64 ? 1 : 2, c.channels & 0x3F }); }
                    std::sort(ws.begin(), ws.end()); double spread = (double)(ws.back() - ws.front()) / std::max(1, ws[ws.size() / 2]);
                    if (spread < 0.1 && structs.size() <= 3) {
                        int demoted = 0; for (size_t i : plausible) if (cands[i].confidence != 2) { cands[i].confidence = 0; cands[i].note += " (parametric)"; demoted++; }
                        log("  group " + g.first + ": " + std::to_string(n) + " variants with identical structure -> parameter, not a song index (" + std::to_string(demoted) + " demoted)");
                        continue;
                    }
                }
                if (n <= 48) continue;
                // song indices are normally contiguous from the bottom: keep the lowest 48 values (and everything up to the largest observed value + 16) plausible
                int maxObs = -1; for (size_t i : g.second) if (cands[i].confidence == 2) maxObs = std::max(maxObs, argOf(cands[i]));
                std::sort(plausible.begin(), plausible.end(), [&](size_t a, size_t b) { return argOf(cands[a]) < argOf(cands[b]); });
                for (size_t k = 0; k < plausible.size(); k++) { size_t i = plausible[k]; if (cands[i].confidence == 2) continue; if (k < 48 || argOf(cands[i]) <= maxObs + 16) continue; cands[i].confidence = 0; cands[i].note += " (many variants)"; if (n > 160) drop[i] = true; }
                log("  group " + g.first + ": " + std::to_string(n) + " plausible variants -> values beyond the lowest 48 demoted" + (n > 160 ? " (dropped)" : ""));
            }
            std::vector<Candidate> kept; for (size_t i = 0; i < cands.size(); i++) if (!drop[i]) kept.push_back(cands[i]); cands.swap(kept);
        }
        activeSnap = &snap;
        // sort: music first, then by recipe order (routine, arg)
        std::stable_sort(cands.begin(), cands.end(), [](const Candidate& p, const Candidate& q) { if (p.isMusic != q.isMusic) return p.isMusic; int cp = p.confidence - (p.suspicious ? 1 : 0), cq = q.confidence - (q.suspicious ? 1 : 0); if ((cp > 0) != (cq > 0)) return cp > 0; return false; });
        return !cands.empty();
    };
    const char* labels[NSNAP] = { "I", "A", "B", "C" };
    const NES::Snapshot* used = nullptr; NES::Snapshot snapS; drv.haveAlt = false;
    // a snapshot that yields only sound effects (or nothing) is not the end: the next one (taken later, e.g. after the
    // title screen) may hold the driver state the request API actually needs. Keep the best snapshot's result.
    std::vector<Candidate> bestSnapCands; NES::Snapshot bestSnapUsed, bestSnapAlt; bool bestHaveAlt = false, haveBest = false;
    // good enough to stop: an observed music request, or observed requests together with plausible tunes
    // good enough to stop: any observed request works from this snapshot (the snapshot accepts the real interface);
    // the later snapshots are tried only when nothing observed could be reproduced (cheap titles stay cheap)
    // stop when an observed request works from this snapshot and some tune was found; with observed requests but no tune
    // the later snapshots are still tried unless the search is expensive (a title without music must not take 20 minutes)
    double lastSearchSec = 0;
    auto hasMusic = [&]() { int obs = 0, music = 0; for (auto& c : cands) { if (c.suspicious) continue; if (c.confidence == 2) obs++; if (c.isMusic && c.confidence >= 1) music++; } return (obs >= 1 && music >= 1) || (obs >= 1 && lastSearchSec > 40); };
    auto observedCount = [&]() { int obs = 0; for (auto& c : cands) if (c.confidence == 2 && !c.suspicious) obs++; return obs; };
    auto snapScore = [&]() { double sc = 0; std::map<std::string, int> per; for (auto& c : cands) { if (c.confidence == 2) sc += 3; else if (c.confidence == 1 && !c.suspicious) { const Recipe& r = c.recipe; char k[48]; if (r.type == RT_SEQ) snprintf(k, sizeof k, "S%04X", r.steps.empty() ? 0 : r.steps[0].routine); else if (r.type == RT_JSR) snprintf(k, sizeof k, "J%04X", r.routine); else snprintf(k, sizeof k, "M%04X:%04X", r.mailbox, r.type == RT_MAILBOX_JSR ? r.routine : 0); if (per[k]++ < 12) sc += c.isMusic ? 1.0 : 0.5; } } return sc; };
    double bestSnapScore = -1;
    for (int si = 0; si < NSNAP; si++) {
        if (!snaps[si]) continue;
        if (opt.cancel && *opt.cancel) break;
        auto t0 = std::chrono::steady_clock::now();
        bool okOne = searchOne(*snaps[si], labels[si], 0);
        lastSearchSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!okOne) continue;
        used = snaps[si];
        // the snapshot was already playing music: every candidate carries that music underneath. Try to find a
        // request that silences the driver, take a snapshot after it (S) and search again from silence; candidates
        // found from S replace the ones from the original snapshot, music requests that only work from the original
        // (e.g. because the stop request pauses the music engine) are kept from the original.
        if (lastBaselineAudible && !(opt.cancel && *opt.cancel) && buildSilencedSnapshot(*snaps[si], lastRecipes, snapS)) {
            std::vector<Candidate> keep = cands;
            NES::Snapshot snapX = *used;   // copy: searchOne(S) may be rejected and we need the original afterwards
            bool okS = searchOne(snapS, "S", 1);
            if (okS) {
                std::vector<Candidate> merged = cands; int fromX = 0;
                std::set<std::string> rs; std::set<uint64_t> hs; for (auto& c : merged) { rs.insert(c.recipe.describe()); hs.insert(c.hashLong); hs.insert(c.hashTail); }
                // the base tune itself, shifted by a frame or two (a "request" that merely re-enters PLAY or pokes an
                // unrelated byte), must not be kept as a tune of its own
                NES nb = base; Recipe none; none.type = RT_MAILBOX; none.mailbox = 0x7FF; none.value = snapX.ram[0x7FF]; none.initCfg = drv.playCfg;
                HarnessResult blX = runRecipe(nb, snapX, none, 300);
                auto shiftedBaseline = [&](const Candidate& c) {
                    if (!blX.ok) return false; HarnessResult hr = runRecipe(nb, snapX, c.recipe, 300); if (!hr.ok) return false;
                    double bestR = 0;
                    for (int sh = -4; sh <= 4; sh++) { int same = 0, cmp = 0, nz = 0;
                        for (int f = 120; f < 300; f++) { int g = f + sh; if (g < 0 || g >= (int)blX.frameHash.size() || f >= (int)hr.frameHash.size()) continue; cmp++; if (hr.frameHash[f] == blX.frameHash[g]) same++; if (hr.frameHash[f]) nz++; }
                        if (cmp > 100 && nz > cmp / 2) bestR = std::max(bestR, (double)same / cmp); }
                    if (getenv("NSFRIP_DEBUG")) log("    shift test " + c.recipe.describe() + ": best match " + std::to_string(bestR));
                    return bestR >= 0.8; };
                // a candidate that does sound from S was judged there (as a duplicate or an effect): only requests
                // that are silent from S can be "music the stop request made unreachable"
                auto soundsFromS = [&](const Candidate& c) { NES n2 = base; HarnessResult p = runRecipe(n2, snapS, c.recipe, 150); return p.ok && p.audibleFrames >= 2; };
                int shifted = 0, judged = 0;
                for (auto& c : keep) {
                    if (!c.isMusic || c.overlay) continue;
                    // the stop request did not disable the driver (a probe sounds from S), so a real music request
                    // works from S too; only requests actually observed in the game are worth keeping from X
                    if (c.confidence < 2 || c.suspicious) continue;
                    if (rs.count(c.recipe.describe()) || hs.count(c.hashLong) || hs.count(c.hashTail)) continue;
                    if (shiftedBaseline(c)) { shifted++; continue; }
                    if (soundsFromS(c)) { judged++; continue; }
                    merged.push_back(c); fromX++;
                }
                if (shifted || judged) log("  " + std::to_string(shifted) + " candidates of the original snapshot were the base tune itself (shifted), " + std::to_string(judged) + " sound from S as well (already judged there): dropped");
                cands.swap(merged);
                drv.snapAlt = snapS; drv.haveAlt = true;
                log("  silenced snapshot S: " + std::to_string(cands.size() - fromX) + " candidates from S, " + std::to_string(fromX) + " music candidates kept from the original snapshot");
            } else { cands = keep; log("  silenced snapshot S yielded nothing; keeping the original"); }
            drv.snapFirst = snapX; used = &drv.snapFirst;
        }
        if (hasMusic()) { haveBest = false; break; }
        // rank snapshots by observed requests first (they prove the snapshot accepts the real interface), then score
        { double sc = observedCount() * 1000 + snapScore();
          if (!haveBest || sc > bestSnapScore) { bestSnapCands = cands; bestSnapUsed = *used; bestSnapAlt = drv.snapAlt; bestHaveAlt = drv.haveAlt; haveBest = true; bestSnapScore = sc; } }
        { bool more = false; for (int k = si + 1; k < NSNAP; k++) if (snaps[k]) more = true; if (more) log("  snapshot " + std::string(labels[si]) + " yielded no observed music request (score " + std::to_string((int)snapScore()) + "); trying the next snapshot"); }
        used = nullptr; cands.clear(); drv.haveAlt = false;
    }
    if (haveBest) { cands = bestSnapCands; drv.snapFirst = bestSnapUsed; drv.snapAlt = bestSnapAlt; drv.haveAlt = bestHaveAlt; used = &drv.snapFirst; }
    if (used && used != &drv.snapFirst) drv.snapFirst = *used; // make the used snapshot the canonical one
    activeSnap = &drv.snapFirst;
    // ---- bank sweep: games whose driver (or its data) sits in a switchable bank keep different tunes in different
    // banks (one driver copy per 32K bank, song tables per level bank...). Re-run the request forms that produced
    // tunes with every other bank mapped in, and keep the new tunes with their own PLAY bank configuration.
    if (used && !(opt.cancel && *opt.cancel)) {
        int unitSlots = 0; // number of 4K slots (from slot 0) that form the switchable unit
        switch (base.mapper) { case 7: case 66: case 34: case 11: unitSlots = 8; break; case 1: case 2: case 30: case 16: case 67: case 28: unitSlots = 4; break; case 4: case 19: case 69: case 22: case 23: case 25: case 33: case 85: case 75: case 80: unitSlots = 2; break; default: unitSlots = 0; }
        int nBanks = unitSlots ? base.prgPages4k / unitSlots : 0;
        std::vector<Candidate> keep = cands;
        bool anyMusic = false; for (auto& c : keep) if (c.isMusic && c.confidence >= 1) anyMusic = true;
        // only worth it when the driver actually fetches from the switchable unit while playing the tunes found so far
        bool unitUsed = false;
        if (unitSlots && nBanks > 1 && anyMusic) {
            NES tn = base; std::fill(tn.romTouch.begin(), tn.romTouch.end(), 0); int tested = 0;
            struct MW : TraceHooks { bool hit = false; void onMapperWrite(uint16_t, uint8_t, uint16_t, bool prgAffecting) override { if (prgAffecting) hit = true; } } mw;
            tn.hooks = &mw; tn.traceOn = true;
            for (auto& c : keep) { if (!c.isMusic || c.confidence < 1 || tested >= 6) continue; tested++; runRecipe(tn, snapOf(c), c.recipe, 300, nullptr, true); }
            tn.hooks = nullptr; tn.traceOn = false; bool selfSwitch = mw.hit;
            // the driver itself must live in the switchable unit (one driver+data copy per bank); a fixed-bank driver that merely
            // reads data from the switched area would only play garbage from other banks
            uint16_t pr = drv.playRoutines.empty() ? 0 : drv.playRoutines[0]; if (pr >= 0x8000 && (pr - 0x8000) / 0x1000 < unitSlots) unitUsed = true;
            if (selfSwitch) { unitUsed = false; log("  bank sweep skipped: the driver switches banks itself"); }
            else if (!unitUsed) log("  bank sweep skipped: the driver is not in the switchable bank");
        }
        if (unitSlots && nBanks > 1 && anyMusic && unitUsed) {
            // request templates: one per (type, routine/mailbox, register) of the music candidates found so far
            // (a request form counts when it produced a plausible tune, or when the game was seen using it: the table
            // heuristic may have demoted every value of a real song variable in this bank's driver copy)
            struct Tpl { Recipe r; int snapId; }; std::vector<Tpl> tpls; std::set<std::string> seenKey, goodKey;
            auto keyOf = [](const Recipe& r) { char k[64]; if (r.type == RT_JSR) snprintf(k, sizeof k, "J%04X:%d", r.routine, r.argReg); else snprintf(k, sizeof k, "M%04X:%04X", r.mailbox, r.type == RT_MAILBOX_JSR ? r.routine : 0); return std::string(k); };
            for (auto& c : keep) { if (c.recipe.type == RT_SEQ) continue; if ((c.isMusic && c.confidence >= 1) || c.confidence == 2) goodKey.insert(keyOf(c.recipe)); }
            for (auto& c : keep) {
                if (c.recipe.type == RT_SEQ) continue;
                std::string k = keyOf(c.recipe); if (!goodKey.count(k) || seenKey.count(k)) continue; seenKey.insert(k); tpls.push_back({ c.recipe, c.snapId });
            }
            std::set<uint64_t> hs; for (auto& c : keep) { hs.insert(c.hashLong); hs.insert(c.hashTail); }
            int curBank = drv.playCfg[0] / unitSlots; int added = 0;
            std::vector<Candidate> extra;
            // how the real driver idles from this snapshot (registers it touches, write rate): other banks must behave alike
            std::set<uint16_t> idleAddrs; int idleWrites = 0;
            { NES t0 = base; Recipe none; none.type = RT_MAILBOX; none.mailbox = 0x7FF; none.value = drv.snapFirst.ram[0x7FF]; none.initCfg = drv.playCfg; std::vector<SoundWrite> lg; HarnessResult h0 = runRecipe(t0, drv.snapFirst, none, 120, &lg); idleWrites = h0.writes; for (auto& w : lg) idleAddrs.insert(w.addr); }
            for (int b = 0; b < nBanks && !(opt.cancel && *opt.cancel); b++) {
                if (b == curBank) continue;
                BankCfg cfg = drv.playCfg; for (int i = 0; i < unitSlots; i++) cfg[i] = (uint8_t)(b * unitSlots + i);
                prog("バンク " + std::to_string(b) + "/" + std::to_string(nBanks) + " で再探索...");
                { // sanity: with this bank mapped the driver must idle quietly (a bank without the driver crashes or spews writes)
                    NES tb = base; Recipe none; none.type = RT_MAILBOX; none.mailbox = 0x7FF; none.value = drv.snapFirst.ram[0x7FF]; none.initCfg = cfg; none.playCfg = cfg;
                    std::vector<SoundWrite> lg; HarnessResult hb = runRecipe(tb, drv.snapFirst, none, 120, &lg);
                    bool alike = hb.ok && hb.writes <= std::max(4 * 120, idleWrites * 2);
                    for (auto& w : lg) if (!idleAddrs.count(w.addr) && w.addr != 0x4015 && w.addr != 0x4017) { alike = false; break; }
                    if (!alike) { log("  bank " + std::to_string(b) + ": no usable driver copy (idle run ok=" + std::to_string((int)hb.ok) + " writes=" + std::to_string(hb.writes) + ")"); continue; }
                }
                for (int sid = 0; sid < 2; sid++) {
                    const NES::Snapshot& sn = sid == 0 ? drv.snapFirst : drv.snapAlt; if (sid == 1 && !drv.haveAlt) break;
                    std::vector<Recipe> rs;
                    for (auto& t : tpls) {
                        if (t.snapId != sid) continue;
                        for (int v = 0; v < 256; v++) {
                            Recipe r = t.r; r.playCfg = cfg; r.initCfg = cfg;   // the request routine (if any) lives in the same driver copy
                            if (r.type == RT_JSR) { int reg = r.argReg == 3 ? 0 : r.argReg; if (reg == 0) r.a = v; else if (reg == 1) r.x = v; else r.y = v; if (r.argReg == 3) r.a = r.x = r.y = v; }
                            else r.value = v;
                            rs.push_back(r);
                        }
                    }
                    if (rs.empty()) continue;
                    recipesOverride = &rs; sweepCfg = cfg;
                    bool ok = searchOne(sn, ("bank " + std::to_string(b)).c_str(), sid);
                    recipesOverride = nullptr;
                    if (!ok) continue;
                    for (auto& c : cands) { if (!c.isMusic || c.confidence < 1) continue; if (hs.count(c.hashLong) || hs.count(c.hashTail)) continue; hs.insert(c.hashLong); hs.insert(c.hashTail); extra.push_back(c); added++; }
                }
            }
            cands = keep; for (auto& c : extra) cands.push_back(c);
            if (added) log("  bank sweep: " + std::to_string(added) + " additional tunes found in other banks");
        } else cands = keep;
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Candidate& p, const Candidate& q) { if (p.isMusic != q.isMusic) return p.isMusic; int cp = p.confidence - (p.suspicious ? 1 : 0), cq = q.confidence - (q.suspicious ? 1 : 0); if ((cp > 0) != (cq > 0)) return cp > 0; return false; });
    int mi = 0, si = 0;
    for (size_t i = 0; i < cands.size(); i++) {
        Candidate& c = cands[i]; c.index = (int)i; char b[64];
        if (c.isMusic) snprintf(b, sizeof b, "Music %02d", ++mi); else snprintf(b, sizeof b, "SFX %02d", ++si);
        c.name = b;
    }
    log("candidates: " + std::to_string(cands.size()));
    return !cands.empty();
}
