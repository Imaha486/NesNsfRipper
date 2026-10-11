// nsfgen.cpp - NSF file generation (stub code, RAM snapshot restore, bank packing, mapper shims) + NSF player
#include "ripper.h"
#include <cmath>
#include "synth.h"
#include <algorithm>
#include <cstdio>
#include <sstream>

// ------------------------------------------------------------------ mini 6502 assembler
struct Asm {
    std::vector<uint8_t> c; uint16_t base;
    std::map<std::string, uint16_t> lab;
    std::vector<std::pair<size_t, std::string>> fixAbs;   // 16-bit absolute fixups
    std::vector<std::pair<size_t, std::string>> fixRel;   // branch fixups
    int uniq = 0;
    explicit Asm(uint16_t b) : base(b) {}
    uint16_t pc() const { return (uint16_t)(base + c.size()); }
    void b(uint8_t v) { c.push_back(v); }
    void w(uint16_t v) { b(v & 0xFF); b(v >> 8); }
    void op(uint8_t o) { b(o); }
    void imm(uint8_t o, uint8_t v) { b(o); b(v); }
    void zp(uint8_t o, uint8_t a) { b(o); b(a); }
    void abs(uint8_t o, uint16_t a) { b(o); w(a); }
    void absL(uint8_t o, const std::string& l, int off = 0) { b(o); fixAbs.push_back({ c.size(), l + "+" + std::to_string(off) }); w(0); }
    void label(const std::string& l) { lab[l] = pc(); }
    std::string tmp() { return "_t" + std::to_string(uniq++); }
    void bra(uint8_t o, const std::string& l) { b(o); fixRel.push_back({ c.size(), l }); b(0); }
    void jcc(uint8_t o, const std::string& l) { b(o ^ 0x20); b(3); b(0x4C); fixAbs.push_back({ c.size(), l + "+0" }); w(0); } // long conditional branch
    void data(const std::vector<uint8_t>& d) { c.insert(c.end(), d.begin(), d.end()); }
    bool resolve(std::string* err) {
        for (auto& f : fixAbs) {
            size_t plus = f.second.find('+'); std::string n = f.second.substr(0, plus); int off = atoi(f.second.c_str() + plus + 1);
            if (!lab.count(n)) { *err = "undefined label " + n; return false; }
            uint16_t v = lab[n] + off; c[f.first] = v & 0xFF; c[f.first + 1] = v >> 8;
        }
        for (auto& f : fixRel) {
            if (!lab.count(f.second)) { *err = "undefined label " + f.second; return false; }
            int rel = (int)lab[f.second] - (int)(base + f.first + 1);
            if (rel < -128 || rel > 127) { *err = "branch out of range to " + f.second; return false; }
            c[f.first] = (uint8_t)rel;
        }
        return true;
    }
};
// opcodes
enum { LDA_IMM = 0xA9, LDA_ABS = 0xAD, LDA_ABSX = 0xBD, LDA_ZP = 0xA5, LDA_INDY = 0xB1, STA_ABS = 0x8D, STA_ABSX = 0x9D, STA_ABSY = 0x99, STA_ZP = 0x85,
       LDX_IMM = 0xA2, LDX_ABS = 0xAE, LDY_IMM = 0xA0, LDY_ABS = 0xAC, STX_ABS = 0x8E, STY_ABS = 0x8C, TAX = 0xAA, TAY = 0xA8, TXA = 0x8A, TYA = 0x98, TSX = 0xBA,
       PHA = 0x48, PLA = 0x68, PHP = 0x08, PLP = 0x28, RTS = 0x60, JSR = 0x20, JMP_ABS = 0x4C, JMP_IND = 0x6C, DEX = 0xCA, DEY = 0x88, INX = 0xE8, INY = 0xC8,
       BPL = 0x10, BMI = 0x30, BNE = 0xD0, BEQ = 0xF0, BCC = 0x90, BCS = 0xB0, AND_IMM = 0x29, ORA_IMM = 0x09, ORA_ABS = 0x0D, AND_ABS = 0x2D, CMP_IMM = 0xC9, CPX_IMM = 0xE0,
       ASL_A = 0x0A, LSR_A = 0x4A, INC_ABS = 0xEE, NOP = 0xEA, SEC = 0x38, CLC = 0x18, ROL_A = 0x2A };

namespace {
struct PatchSite { int romPage; uint16_t off; uint16_t ea; uint8_t opcode; int reg; bool ok; uint16_t pc; bool play = false; };

struct BuildHooks : TraceHooks {
    NES* nes; std::map<uint32_t, PatchSite>* sites; std::vector<std::string>* warn; std::set<uint16_t> bad;
    std::map<uint32_t, PatchSite>* ppuSites;
    void onPpuIo(uint16_t addr) override {
        if (addr >= 0x4000 || (addr & 7) != 2) return;
        uint16_t ip = nes->pc - 3; uint8_t op = nes->peek(ip); uint16_t operand = nes->peek(ip + 1) | (nes->peek(ip + 2) << 8);
        if ((op == 0xAD || op == 0x2C || op == 0xAE || op == 0xAC) && operand == addr) { int page = nes->romPageAt(ip); (*ppuSites)[(page << 16) | (ip & 0xFFF)] = PatchSite{ page, (uint16_t)(ip & 0xFFF), addr, op, 0, true, ip }; }
    }
    void onMapperWrite(uint16_t addr, uint8_t val, uint16_t pc, bool prgAff) override {
        if (!prgAff) return;
        // instruction just executed ends at pc; try 3-byte absolute store
        uint16_t ip = pc - 3; uint8_t op = nes->peek(ip); uint16_t operand = nes->peek(ip + 1) | (nes->peek(ip + 2) << 8);
        bool ok = (op == STA_ABS || op == STX_ABS || op == STY_ABS) && operand == addr;
        bool valueOnly = nes->mapper == 2 || nes->mapper == 7 || nes->mapper == 66 || nes->mapper == 30;   // register address irrelevant: indexed stores are fine too
        if (!ok && valueOnly && (op == STA_ABSX || op == STA_ABSY) && operand >= 0x8000) { ok = true; }
        int page = nes->romPageAt(ip); uint32_t key = (page << 16) | (ip & 0xFFF);
        if (!ok) {
            if (!bad.count(pc)) { bad.insert(pc); char b[160]; snprintf(b, sizeof b, "パッチ不可のマッパー書き込み: PC=$%04X -> $%04X (命令 $%02X)", pc, addr, nes->peek(pc - 2)); warn->push_back(b); }
            return;
        }
        PatchSite ps{ page, (uint16_t)(ip & 0xFFF), addr, op, (op == STA_ABS || op == STA_ABSX || op == STA_ABSY) ? 0 : op == STX_ABS ? 1 : 2, true, ip };
        ps.play = (nes->curMark & MARK_PLAY) != 0;
        auto it = sites->find(key); if (it != sites->end()) { it->second.play = it->second.play || ps.play; } else (*sites)[key] = ps;
    }
    void onSoundWrite(uint16_t addr, uint8_t val) override {
        if (addr == 0x4015 && (val & 0x10)) { // DMC sample range -> mark pages/slots
            uint32_t start = nes->dmcAddr, len = nes->dmcLen; // dmcAddr/dmcLen already updated by apuWrite? (hook called before update)
            start = 0xC000 | (nes->apuRegs[0x12] << 6); len = (nes->apuRegs[0x13] << 4) | 1;
            for (uint32_t a = start; a < start + len && a < 0x10000; a += 0x100) { nes->romTouch[nes->romPageAt((uint16_t)a)] |= MARK_PLAY; nes->slotTouch |= 1 << ((a >> 12) & 7); nes->slotTouchPlay |= 1 << ((a >> 12) & 7); }
            uint32_t e = std::min<uint32_t>(0xFFFF, start + len); nes->romTouch[nes->romPageAt((uint16_t)e)] |= MARK_PLAY; nes->slotTouch |= 1 << ((e >> 12) & 7); nes->slotTouchPlay |= 1 << ((e >> 12) & 7);
        }
    }
};
}

static inline int prgPagesTotal(const NES& n) { return n.prgPages4k; }
static void emitSetBanks(Asm& A, int slot4k, int count4k, int prgPages, int stubSlot) {
    // in: A = bank number in units of count4k pages. out: $5FF8+slot.. set via PG2NSF table
    int banks = std::max(1, prgPages / count4k);
    A.imm(AND_IMM, (uint8_t)(banks - 1));
    for (int c = count4k; c > 1; c >>= 1) A.op(ASL_A);
    A.op(TAX);
    for (int i = 0; i < count4k; i++) {
        if (slot4k + i != stubSlot) { A.absL(LDA_ABSX, "PG2NSF"); A.abs(STA_ABS, 0x5FF8 + slot4k + i); }
        if (i + 1 < count4k) A.op(INX);
    }
}

NsfBuild Ripper::buildNsf(const std::vector<int>& sel, const std::string& title, const std::string& artist, const std::string& copyright) {
    NsfBuild nb;
    if (sel.empty() || drv.playRoutines.empty()) { nb.report = "no tracks"; return nb; }
    const NES::Snapshot& snap = drv.snapFirst;
    std::ostringstream rep;
    // ---- 1. touch collection + patch sites per track
    NES nes = base; nes.markOn = false;
    std::fill(nes.romTouch.begin(), nes.romTouch.end(), 0); nes.slotTouch = 0; nes.slotTouchPlay = 0; nes.slotTouchInit = 0;
    std::fill(nes.ramRd.begin(), nes.ramRd.end(), 0); std::fill(nes.ramWr.begin(), nes.ramWr.end(), 0);
    std::map<uint32_t, PatchSite> sites, ppuSites; std::vector<std::string> warn;
    BuildHooks bh; bh.nes = &nes; bh.sites = &sites; bh.warn = &warn; bh.ppuSites = &ppuSites;
    nes.hooks = &bh; nes.traceOn = true;
    std::vector<bool> trackOk(sel.size(), true);
    for (size_t i = 0; i < sel.size(); i++) {
        Candidate& c = cands[sel[i]];
        int frames = opt.longFrames; if (c.durationFrames) frames = std::min(frames, c.durationFrames + 120); else if (c.loopLen) frames = std::min(frames, c.loopStart + c.loopLen * 2 + 60);
        HarnessResult hr = runRecipe(nes, snapOf(c), c.recipe, frames, nullptr, true);
        if (!hr.ok) { trackOk[i] = false; warn.push_back(c.name + ": ハーネス実行失敗"); }
    }
    nes.traceOn = false; nes.hooks = nullptr;
    // ---- 1b. per-track INIT effects (replayed in the NSF instead of executing game code): RAM delta + sound register writes
    struct TrackFx { std::vector<std::pair<uint16_t, uint8_t>> delta; std::vector<SoundWrite> writes; MapperState m; bool ok = true; };
    std::vector<TrackFx> fx(sel.size());
    {
        NES n = base;
        for (size_t i = 0; i < sel.size(); i++) {
            Candidate& c = cands[sel[i]]; Recipe& r = c.recipe;
            n.loadSnapshot(snapOf(c)); applyCfg(n, r.initCfg); n.logSound = true; n.soundLog.clear(); n.logBaseCycle = (uint32_t)n.cycles;
            std::set<uint16_t> forced;
            for (const Step& st : r.allSteps()) {
                if (st.kind == 2) { if (st.mailbox < 0x800) n.ram[st.mailbox] = st.value; else if (st.mailbox >= 0x6000 && st.mailbox < 0x8000) n.wram[st.mailbox & 0x1FFF] = st.value; forced.insert(st.mailbox); }
                else if (!n.callSubroutine(st.routine, st.a, st.x, st.y, 400000, false, drv.playSp)) { fx[i].ok = false; break; }
            }
            n.logSound = false;
            fx[i].writes = n.soundLog; fx[i].m = n.m;
            for (int a = 0; a < 0x800; a++) { if (a >= 0x1F8 && a < 0x200) continue; bool rd = ((nes.ramRd[a] | drv.driverRead[a]) & MARK_PLAY) || forced.count((uint16_t)a); if (rd && n.ram[a] != snap.ram[a]) fx[i].delta.push_back({ (uint16_t)a, n.ram[a] }); }
            for (int a = 0x6000; a < 0x8000; a++) { bool rd = ((nes.ramRd[a] | drv.driverRead[a]) & MARK_PLAY) || forced.count((uint16_t)a); if (rd && n.wram[a & 0x1FFF] != snap.wram[a & 0x1FFF]) fx[i].delta.push_back({ (uint16_t)a, n.wram[a & 0x1FFF] }); }
            if (!fx[i].ok) warn.push_back(c.name + ": INIT実行失敗");
        }
    }
    // ---- 2. pages & slots (only PLAY matters now: INIT is replayed, not executed)
    std::vector<int> pages; for (int p = 0; p < base.prgPages4k; p++) if (nes.romTouch[p] & MARK_PLAY) pages.push_back(p);
    uint8_t usedSlots = nes.slotTouchPlay;
    int stubSlot = -1; for (int s = 7; s >= 0; s--) if (!(usedSlots & (1 << s))) { stubSlot = s; break; }
    bool ramMode = stubSlot < 0;           // no free slot: PLAY runs through a RAM trampoline
    if (ramMode) stubSlot = 7;
    uint16_t B = 0x8000 + stubSlot * 0x1000;
    std::vector<int> pg2nsf(base.prgPages4k, 0);
    for (size_t i = 0; i < pages.size(); i++) pg2nsf[pages[i]] = (int)i + 1;
    if (pages.size() + 1 > 255) { nb.report = "バンク数が多すぎます"; return nb; }
    rep << "ROM pages used by PLAY: " << pages.size() << " (4KB each), slots used: ";
    for (int s = 0; s < 8; s++) rep << ((usedSlots >> s) & 1); rep << ", stub slot " << stubSlot << " (" << hex16(B) << ")" << (ramMode ? " [RAM trampoline mode]" : "") << "\n";
    int slot2 = -1; for (int s2 = 7; s2 >= 0; s2--) if (s2 != stubSlot && !(usedSlots & (1 << s2))) { slot2 = s2; break; } if (slot2 < 0) slot2 = stubSlot == 0 ? 1 : stubSlot - 1;
    uint16_t B2 = 0x8000 + slot2 * 0x1000;
    // ---- 3. RAM block & restore set
    std::vector<uint8_t> drvUsed(0x10000, 0);
    for (int a = 0; a < 0x10000; a++) drvUsed[a] = ((nes.ramRd[a] | nes.ramWr[a] | drv.driverRead[a] | drv.driverWrite[a]) & MARK_PLAY) != 0;   // only the PLAY phase runs inside the NSF
    for (int i : sel) for (const Step& st : cands[i].recipe.allSteps()) if (st.kind == 2) drvUsed[st.mailbox] = 1;
    for (auto& kv : drv.refresh) drvUsed[kv.first] = 1;
    const int BLOCK = ramMode ? 128 : 64;
    int block = -1;
    auto findBlock = [&](int lo, int hi) { for (int a = hi - BLOCK + 1; a >= lo; a--) { bool ok = true; for (int i = 0; i < BLOCK; i++) if (drvUsed[a + i]) { ok = false; break; } if (ok) return a; } return -1; };
    block = findBlock(0x200, 0x7FF); if (block < 0) block = findBlock(0x100, 0x1BF); if (block < 0) block = findBlock(0x02, 0xFF);
    if (block < 0) { nb.report = "スタブ用の空きRAMが見つかりません (ドライバがRAMをほぼ全て使用)"; return nb; }
    uint16_t REC = block, CODE = block + 32, SHIM = block + 56, PLAYT = block + 64;
    int zptr = -1; for (int a = 0xFE; a >= 2; a--) if (!drvUsed[a] && !drvUsed[a + 1] && !(a >= block && a < block + BLOCK) && !(a + 1 >= block && a + 1 < block + BLOCK)) { zptr = a; break; }
    bool borrowZp = zptr < 0; if (borrowZp) zptr = 0x00;
    rep << "RAM block " << hex16(REC) << "-" << hex16(REC + BLOCK - 1) << ", ZP ptr " << hex16(zptr) << (borrowZp ? " (borrowed)" : "") << "\n";
    // base restore set: bytes read by INIT/PLAY in harness (any value) or by PLAY in the game trace
    std::vector<std::pair<uint16_t, uint8_t>> rest;
    auto snapByte = [&](int a) -> uint8_t { return a < 0x800 ? snap.ram[a] : snap.wram[a & 0x1FFF]; };
    for (int a = 0; a < 0x800; a++) {
        if (a >= block && a < block + BLOCK) continue;
        if (a >= 0x1F8 && a < 0x200) continue;   // never touch the player's stack top (some drivers keep variables low in the stack page)
        bool rd = (nes.ramRd[a] | drv.driverRead[a]) & MARK_PLAY;
        if (rd) rest.push_back({ (uint16_t)a, snapByte(a) });
    }
    for (int a = 0x6000; a < 0x8000; a++) { bool rd = (nes.ramRd[a] | drv.driverRead[a]) & MARK_PLAY; if (rd) rest.push_back({ (uint16_t)a, snapByte(a) }); }
    struct Run { uint16_t addr; std::vector<uint8_t> d; };
    auto makeRuns = [&](const std::vector<std::pair<uint16_t, uint8_t>>& bytes, int maxGap, size_t maxLen, bool fillFromSnap) {
        std::vector<Run> rs;
        for (size_t i = 0; i < bytes.size(); i++) {
            if (!rs.empty() && bytes[i].first >= rs.back().addr && bytes[i].first - (rs.back().addr + rs.back().d.size()) <= (size_t)maxGap && rs.back().d.size() + (bytes[i].first - (rs.back().addr + rs.back().d.size())) < maxLen) {
                while (rs.back().addr + rs.back().d.size() < bytes[i].first) { int a = (int)(rs.back().addr + rs.back().d.size()); rs.back().d.push_back(fillFromSnap ? snapByte(a) : 0); }
                rs.back().d.push_back(bytes[i].second);
            } else rs.push_back({ bytes[i].first, { bytes[i].second } });
        }
        return rs;
    };
    std::vector<Run> runs = makeRuns(rest, 3, 120, true);
    size_t restBytes = 0; for (auto& r : runs) restBytes += r.d.size();
    rep << "RAM restore: " << rest.size() << " bytes in " << runs.size() << " runs\n";
    // ---- 4. patches (PLAY phase only)
    std::vector<PatchSite> plist; for (auto& kv : sites) if (kv.second.play) plist.push_back(kv.second);
    if (ramMode && !plist.empty()) { warn.push_back("空きスロットが無いためマッパー書き込みのパッチ(" + std::to_string(plist.size()) + "箇所)を適用できません。曲中のバンク切替は再現されません"); plist.clear(); }
    rep << "mapper write patches: " << plist.size() << "\n";
    // ---- 5. per-track blobs: [nRuns][runs: lo hi len data...][nWrites lo hi][writes: lo hi val]
    int ntracks = (int)sel.size();
    std::vector<std::vector<uint8_t>> blobs(ntracks);
    for (int t = 0; t < ntracks; t++) {
        std::vector<std::pair<uint16_t, uint8_t>> d = fx[t].delta;
        if (!plist.empty()) { // shim state after INIT (mapper registers) is part of the state
            std::vector<uint8_t> si;
            switch (base.mapper) {
            case 4: si = { fx[t].m.cmd, fx[t].m.regs[6], fx[t].m.regs[7] }; break;
            case 1: si = { fx[t].m.shift, fx[t].m.shiftCount, fx[t].m.regs[0], fx[t].m.regs[1], fx[t].m.regs[2], fx[t].m.regs[3] }; break;
            case 69: si = { fx[t].m.cmd }; break;
            default: break;
            }
            for (size_t k = 0; k < si.size(); k++) d.push_back({ (uint16_t)(SHIM + k), si[k] });
        }
        std::sort(d.begin(), d.end());
        std::vector<Run> rs = makeRuns(d, 0, 255, false);
        std::vector<uint8_t>& b = blobs[t];
        if (rs.size() > 255) rs.resize(255);
        b.push_back((uint8_t)rs.size());
        for (auto& r : rs) { b.push_back(r.addr & 0xFF); b.push_back(r.addr >> 8); b.push_back((uint8_t)r.d.size()); b.insert(b.end(), r.d.begin(), r.d.end()); }
        std::vector<SoundWrite>& w = fx[t].writes; size_t nw = std::min<size_t>(w.size(), 65535);
        b.push_back((uint8_t)(nw & 0xFF)); b.push_back((uint8_t)(nw >> 8));
        for (size_t k = 0; k < nw; k++) { uint16_t a = w[k].addr == N163_READ_MARK ? 0xFE00 : w[k].addr; b.push_back(a & 0xFF); b.push_back(a >> 8); b.push_back(w[k].val); }   // $FE00 = "read the N163 data port"
    }
    // ---- 6. assemble stub
    std::vector<BankCfg> cfgs; auto cfgIdx = [&](const BankCfg& c) { for (size_t i = 0; i < cfgs.size(); i++) if (cfgs[i] == c) return (int)i; cfgs.push_back(c); return (int)cfgs.size() - 1; };
    std::vector<std::vector<uint8_t>> recs(ntracks);
    for (int t = 0; t < ntracks; t++) {
        BankCfg pc{};
        const BankCfg& tpc = cands[sel[t]].recipe.hasPlayCfg() ? cands[sel[t]].recipe.playCfg : drv.playCfg;
        for (int i = 0; i < 8; i++) pc[i] = (uint8_t)pg2nsf[tpc[i] % prgPagesTotal(base)];
        if (!ramMode) pc[stubSlot] = 0;
        std::vector<uint8_t> rec(8, 0);
        rec[0] = (uint8_t)cfgIdx(pc); // [1] = blob bank (0 = stub), [2..3] = blob address (filled at assembly)
        recs[t] = rec;
    }
    if (cfgs.size() > 32) { nb.report = "バンク構成の種類が多すぎます"; return nb; }
    // data allocator: stub budget first, then 4KB data banks mapped at slot2
    size_t stubBudget = 4096 - 700 - ntracks * 10 - prgPagesTotal(base) - cfgs.size() * 8 - (plist.size() * 70); if ((int)stubBudget > 2600) stubBudget = 2600;
    std::vector<int> runBank(runs.size(), 0); std::vector<uint16_t> runOff(runs.size(), 0);
    std::vector<int> blobBank(ntracks, 0); std::vector<uint16_t> blobOff(ntracks, 0);
    int dataBanks = 0;
    { size_t used = 0; size_t cur = 0; int bank = 0;
      auto place = [&](size_t sz, int& outBank, uint16_t& outOff) {
          if (bank == 0 && used + sz <= stubBudget) { used += sz; outBank = 0; return; }
          if (bank == 0) { bank = 1; cur = 0; }
          if (cur + sz > 4096) { bank++; cur = 0; }
          outBank = bank; outOff = (uint16_t)cur; cur += sz; };
      for (size_t i = 0; i < runs.size(); i++) place(runs[i].d.size(), runBank[i], runOff[i]);
      for (int t = 0; t < ntracks; t++) { if (blobs[t].size() > 4096) { nb.report = cands[sel[t]].name + ": INIT の書き込みが多すぎます"; return nb; } place(blobs[t].size(), blobBank[t], blobOff[t]); }
      dataBanks = bank; }
    if (pages.size() + 1 + dataBanks > 255) { nb.report = "バンク数が多すぎます"; return nb; }
    Asm A(B);
    A.absL(JMP_ABS, "INIT"); A.absL(JMP_ABS, "PLAY");
    // ---- INIT
    A.label("INIT");
    A.op(TAX);
    A.absL(LDA_ABSX, "RECLO"); A.abs(STA_ABS, REC + 16); A.absL(LDA_ABSX, "RECHI"); A.abs(STA_ABS, REC + 17);   // record address
    if (borrowZp) { A.zp(LDA_ZP, zptr); A.op(PHA); A.zp(LDA_ZP, zptr + 1); A.op(PHA); }
    A.abs(LDA_ABS, REC + 16); A.zp(STA_ZP, zptr); A.abs(LDA_ABS, REC + 17); A.zp(STA_ZP, zptr + 1);
    A.imm(LDY_IMM, 7); A.label("L1"); A.zp(LDA_INDY, zptr); A.abs(STA_ABSY, REC + 16); A.op(DEY); A.bra(BPL, "L1");
    // expand play cfg: REC+0..7 = CFGTAB[rec[0]]
    { std::string l = A.tmp();
      A.abs(LDA_ABS, REC + 16); A.op(ASL_A); A.op(ASL_A); A.op(ASL_A); A.op(TAX); A.imm(LDY_IMM, 0);
      A.label(l); A.absL(LDA_ABSX, "CFGTAB"); A.abs(STA_ABSY, REC); A.op(INX); A.op(INY); A.imm(0xC0 /*CPY#*/, 8); A.bra(BNE, l); }
    // base RAM restore
    int curDataBank = 0;
    for (size_t i = 0; i < runs.size(); i++) {
        std::string l = A.tmp();
        if (runBank[i] == 0) { std::string dl = "RUN" + std::to_string(i); A.imm(LDX_IMM, (uint8_t)(runs[i].d.size() - 1)); A.label(l); A.absL(LDA_ABSX, dl); A.abs(STA_ABSX, runs[i].addr); A.op(DEX); A.bra(BPL, l); }
        else {
            if (runBank[i] != curDataBank) { curDataBank = runBank[i]; A.imm(LDA_IMM, (uint8_t)(pages.size() + runBank[i])); A.abs(STA_ABS, 0x5FF8 + slot2); }
            A.imm(LDX_IMM, (uint8_t)(runs[i].d.size() - 1)); A.label(l); A.abs(LDA_ABSX, B2 + runOff[i]); A.abs(STA_ABSX, runs[i].addr); A.op(DEX); A.bra(BPL, l);
        }
    }
    if (dataBanks) rep << "data banks: " << dataBanks << " (via slot " << slot2 << ")\n";
    if (ramMode) {
        // copy PLAY trampoline into RAM
        std::vector<uint8_t> tr = { LDX_IMM, 7, LDA_ABSX, (uint8_t)(REC & 0xFF), (uint8_t)(REC >> 8), STA_ABSX, 0xF8, 0x5F, DEX, BPL, (uint8_t)(-9 & 0xFF) };
        for (auto& kv : drv.refresh) { tr.insert(tr.end(), { LDA_IMM, kv.second, STA_ABS, (uint8_t)(kv.first & 0xFF), (uint8_t)(kv.first >> 8) }); }
        if (drv.playSp < 0xF0) tr.insert(tr.end(), { TSX, STX_ABS, (uint8_t)((CODE + 23) & 0xFF), (uint8_t)((CODE + 23) >> 8), LDX_IMM, drv.playSp, 0x9A });
        { std::vector<DriverInfo::PlayCall> calls = drv.playCalls; if (calls.empty()) calls.push_back({ 0, 0, 0 });
          for (uint16_t pr : drv.playRoutines) for (auto& pcl : calls) {
            tr.insert(tr.end(), { LDA_IMM, pcl.a, LDX_IMM, pcl.x, LDY_IMM, pcl.y });
            if (drv.playKind == 0) { tr.push_back(JSR); tr.push_back(pr & 0xFF); tr.push_back(pr >> 8); }
            else { uint16_t ret = PLAYT + (uint16_t)tr.size() + 10; tr.insert(tr.end(), { LDA_IMM, (uint8_t)(ret >> 8), PHA, LDA_IMM, (uint8_t)(ret & 0xFF), PHA, PHP, JMP_ABS, (uint8_t)(pr & 0xFF), (uint8_t)(pr >> 8) }); }
          } }
        if (drv.playSp < 0xF0) tr.insert(tr.end(), { LDX_ABS, (uint8_t)((CODE + 23) & 0xFF), (uint8_t)((CODE + 23) >> 8), 0x9A });
        tr.push_back(RTS);
        if (tr.size() > 64) { nb.report = "PLAYトランポリンが大きすぎます"; return nb; }
        std::string l = A.tmp();
        A.imm(LDX_IMM, (uint8_t)(tr.size() - 1)); A.label(l); A.absL(LDA_ABSX, "PLAYTRD"); A.abs(STA_ABSX, PLAYT); A.op(DEX); A.bra(BPL, l);
        nb.ramPlayTramp = tr;
    }
    if (base.expansion & EXP_N163) { // restore N163 internal RAM (wave tables / channel registers) from the snapshot
        std::string l = A.tmp();
        A.imm(LDA_IMM, 0x80); A.abs(STA_ABS, 0xF800); A.imm(LDX_IMM, 0); A.label(l); A.absL(LDA_ABSX, "N163TAB"); A.abs(STA_ABS, 0x4800); A.op(INX); A.imm(CPX_IMM, 0x80); A.bra(BNE, l);
    }
    if (drv.playKind == 2) { A.imm(LDA_IMM, 0x00); A.abs(STA_ABS, 0x4017); } // enable APU frame IRQ flag for IRQ-driven drivers
    if (!ppuSites.empty()) { A.imm(LDA_IMM, 0xC0); A.abs(STA_ABS, SHIM + 7); }   // fake PPUSTATUS (vblank + sprite0) for patched $2002 reads
    // ---- replay the track's INIT effects: blob at REC+18..19 (bank in REC+17... stored as rec[1..3])
    A.abs(LDA_ABS, REC + 17); { std::string ns = A.tmp(); A.bra(BEQ, ns); A.abs(STA_ABS, 0x5FF8 + slot2); A.label(ns); }
    A.abs(LDA_ABS, REC + 18); A.zp(STA_ZP, zptr); A.abs(LDA_ABS, REC + 19); A.zp(STA_ZP, zptr + 1);
    // helper: normalize pointer (ptr += Y; Y = 0)
    auto emitNorm = [&]() { std::string nc = A.tmp(); A.op(TYA); A.op(CLC); A.imm(0x65 /*ADC zp*/, (uint8_t)zptr); A.zp(STA_ZP, zptr); A.bra(BCC, nc); A.imm(0xE6 /*INC zp*/, (uint8_t)(zptr + 1)); A.label(nc); A.imm(LDY_IMM, 0); };
    A.imm(LDY_IMM, 0); A.zp(LDA_INDY, zptr); A.abs(STA_ABS, REC + 26); A.op(INY);   // nRuns
    A.label("RUNLOOP");
    A.abs(LDA_ABS, REC + 26); A.jcc(BEQ, "RUNSDONE");
    A.imm(LDA_IMM, STA_ABSX); A.abs(STA_ABS, CODE); A.zp(LDA_INDY, zptr); A.abs(STA_ABS, CODE + 1); A.op(INY); A.zp(LDA_INDY, zptr); A.abs(STA_ABS, CODE + 2); A.op(INY);
    A.imm(LDA_IMM, RTS); A.abs(STA_ABS, CODE + 3);
    A.zp(LDA_INDY, zptr); A.abs(STA_ABS, REC + 27); A.op(INY);                      // len
    emitNorm();
    A.imm(LDX_IMM, 0); { std::string cl = A.tmp(); A.label(cl); A.zp(LDA_INDY, zptr); A.abs(JSR, CODE); A.op(INY); A.op(INX); A.abs(0xEC /*CPX abs*/, REC + 27); A.bra(BNE, cl); }
    emitNorm();
    A.abs(0xCE /*DEC abs*/, REC + 26); A.absL(JMP_ABS, "RUNLOOP");
    A.label("RUNSDONE");
    A.zp(LDA_INDY, zptr); A.abs(STA_ABS, REC + 26); A.op(INY); A.zp(LDA_INDY, zptr); A.abs(STA_ABS, REC + 27); A.op(INY);   // nWrites
    emitNorm();
    A.label("WLOOP");
    A.abs(LDA_ABS, REC + 26); A.abs(ORA_ABS, REC + 27); A.jcc(BEQ, "WDONE");
    A.imm(LDA_IMM, STA_ABS); A.abs(STA_ABS, CODE); A.zp(LDA_INDY, zptr); A.abs(STA_ABS, CODE + 1); A.op(INY); A.zp(LDA_INDY, zptr); A.abs(STA_ABS, CODE + 2); A.op(INY);
    { // entry $FE00 = read the N163 data port (advances its auto-increment address): LDA $4800 instead of STA
        std::string nr = A.tmp(); A.imm(CMP_IMM, 0xFE); A.bra(BNE, nr); A.imm(LDA_IMM, LDA_ABS); A.abs(STA_ABS, CODE); A.imm(LDA_IMM, 0x48); A.abs(STA_ABS, CODE + 2); A.label(nr);
    }
    A.imm(LDA_IMM, RTS); A.abs(STA_ABS, CODE + 3); A.zp(LDA_INDY, zptr); A.op(INY); A.abs(JSR, CODE);
    emitNorm();
    { std::string nb2 = A.tmp(); A.abs(LDA_ABS, REC + 26); A.bra(BNE, nb2); A.abs(0xCE, REC + 27); A.label(nb2); A.abs(0xCE, REC + 26); }
    A.absL(JMP_ABS, "WLOOP");
    A.label("WDONE");
    if (borrowZp) { A.op(PLA); A.zp(STA_ZP, zptr + 1); A.op(PLA); A.zp(STA_ZP, zptr); }
    A.abs(LDA_ABS, REC + slot2); A.abs(STA_ABS, 0x5FF8 + slot2);   // back to the game's page at slot2 (play cfg)
    A.op(RTS);
    // PLAY (ROM version; in RAM mode the header points at the RAM trampoline instead)
    A.label("PLAY");
    A.imm(LDX_IMM, 7); A.label("L3"); A.abs(LDA_ABSX, REC); A.abs(STA_ABSX, 0x5FF8); A.op(DEX); A.bra(BPL, "L3");
    for (auto& kv : drv.refresh) { A.imm(LDA_IMM, kv.second); A.abs(STA_ABS, kv.first); }
    uint16_t SAVES = CODE + 23;
    bool relocSp = drv.playSp < 0xF0;
    if (relocSp) { A.op(TSX); A.abs(STX_ABS, SAVES); A.imm(LDX_IMM, drv.playSp); A.op(0x9A /*TXS*/); }
    {
        std::vector<DriverInfo::PlayCall> calls = drv.playCalls; if (calls.empty()) calls.push_back({ 0, 0, 0 });
        for (uint16_t pr : drv.playRoutines) for (auto& pcl : calls) {
            A.imm(LDA_IMM, pcl.a); A.imm(LDX_IMM, pcl.x); A.imm(LDY_IMM, pcl.y);
            if (drv.playKind == 0) A.abs(JSR, pr);
            else { // LDA #>ret ; PHA ; LDA #<ret ; PHA ; PHP ; JMP handler ; ret:   (handler returns with RTI) -- A is reloaded afterwards
                   uint16_t retAddr = A.pc() + 10; A.imm(LDA_IMM, retAddr >> 8); A.op(PHA); A.imm(LDA_IMM, retAddr & 0xFF); A.op(PHA); A.op(PHP); A.abs(JMP_ABS, pr); }
        }
    }
    if (relocSp) { A.abs(LDX_ABS, SAVES); A.op(0x9A /*TXS*/); }
    A.op(RTS);
    // shim handlers
    int prgPages = base.prgPages4k;
    for (size_t k = 0; k < plist.size(); k++) {
        PatchSite& ps = plist[k];
        A.label("SHIM" + std::to_string(k));
        A.op(PHP); A.op(PHA); A.op(TXA); A.op(PHA); A.op(TYA); A.op(PHA);
        A.op(TSX);
        A.abs(LDA_ABSX, ps.reg == 0 ? 0x0103 : ps.reg == 1 ? 0x0102 : 0x0101); // value written
        uint16_t ea = ps.ea;
        switch (base.mapper) {
        case 2: case 30: emitSetBanks(A, 0, 4, prgPages, stubSlot); break;
        case 7: case 66:
            if (base.mapper == 66) { A.op(LSR_A); A.op(LSR_A); A.op(LSR_A); A.op(LSR_A); }
            emitSetBanks(A, 0, 8, prgPages, stubSlot); break;
        case 16: if ((ea & 0x0F) == 8) emitSetBanks(A, 0, 4, prgPages, stubSlot); break;
        case 19: { int sl = (ea & 0xF800) == 0xE000 ? 0 : (ea & 0xF800) == 0xE800 ? 1 : (ea & 0xF800) == 0xF000 ? 2 : -1; if (sl >= 0) emitSetBanks(A, sl * 2, 2, prgPages, stubSlot); break; }
        case 22: case 75: case 85: {
            int sl = -1;
            if (base.mapper == 22) sl = (ea & 0xF000) == 0x8000 ? 0 : (ea & 0xF000) == 0xA000 ? 1 : -1;
            else if (base.mapper == 75) sl = (ea & 0xF000) == 0x8000 ? 0 : (ea & 0xF000) == 0xA000 ? 1 : (ea & 0xF000) == 0xC000 ? 2 : -1;
            else { uint16_t a = (ea & 0xF000) | ((ea & 0x18) ? 0x10 : 0); sl = a == 0x8000 ? 0 : a == 0x8010 ? 1 : a == 0x9000 ? 2 : -1; }
            if (sl >= 0) emitSetBanks(A, sl * 2, 2, prgPages, stubSlot); break;
        }
        case 33: { int sl = (ea & 0xE003) == 0x8000 ? 0 : (ea & 0xE003) == 0x8001 ? 1 : -1; if (sl >= 0) emitSetBanks(A, sl * 2, 2, prgPages, stubSlot); break; }
        case 67: if ((ea & 0xF800) == 0xF800) emitSetBanks(A, 0, 4, prgPages, stubSlot); break;
        case 80: { int sl = ea >= 0x7EFE ? 2 : ea >= 0x7EFC ? 1 : 0; emitSetBanks(A, sl * 2, 2, prgPages, stubSlot); break; }
        case 69: {
            if (ea < 0xA000) { A.imm(AND_IMM, 0x0F); A.abs(STA_ABS, SHIM); }
            else {
                std::string d = A.tmp(), s1 = A.tmp(), s2 = A.tmp();
                A.op(TAY); A.abs(LDX_ABS, SHIM); A.imm(CPX_IMM, 9); A.jcc(BCC, d); A.imm(CPX_IMM, 12); A.jcc(BCS, d);
                A.op(TYA); A.imm(CPX_IMM, 10); A.jcc(BEQ, s1); A.imm(CPX_IMM, 11); A.jcc(BEQ, s2);
                emitSetBanks(A, 0, 2, prgPages, stubSlot); A.absL(JMP_ABS, d);
                A.label(s1); emitSetBanks(A, 2, 2, prgPages, stubSlot); A.absL(JMP_ABS, d);
                A.label(s2); emitSetBanks(A, 4, 2, prgPages, stubSlot);
                A.label(d);
            }
            break;
        }
        case 4: {
            std::string d = A.tmp(), apply = "MMC3APPLY";
            if (!(ea & 1)) { A.abs(STA_ABS, SHIM); A.absL(JSR, apply); }
            else {
                std::string r6 = A.tmp(), r7 = A.tmp();
                A.op(TAY); A.abs(LDA_ABS, SHIM); A.imm(AND_IMM, 7); A.imm(CMP_IMM, 6); A.jcc(BEQ, r6); A.imm(CMP_IMM, 7); A.jcc(BEQ, r7); A.absL(JMP_ABS, d);
                A.label(r6); A.op(TYA); A.abs(STA_ABS, SHIM + 1); A.absL(JSR, apply); A.absL(JMP_ABS, d);
                A.label(r7); A.op(TYA); A.abs(STA_ABS, SHIM + 2); A.absL(JSR, apply);
            }
            A.label(d);
            break;
        }
        case 1: { A.absL(JSR, "MMC1W" + std::to_string((ea >> 13) & 3)); break; }
        default: break;
        }
        A.op(PLA); A.op(TAY); A.op(PLA); A.op(TAX); A.op(PLA); A.op(PLP); A.op(RTS);
    }
    if (!plist.empty() && base.mapper == 4) {
        A.label("MMC3APPLY");
        std::string swap = A.tmp(), d = A.tmp();
        A.abs(LDA_ABS, SHIM); A.imm(AND_IMM, 0x40); A.jcc(BNE, swap);
        A.abs(LDA_ABS, SHIM + 1); emitSetBanks(A, 0, 2, prgPages, stubSlot); A.imm(LDA_IMM, (uint8_t)(prgPages / 2 - 2)); emitSetBanks(A, 4, 2, prgPages, stubSlot); A.absL(JMP_ABS, d);
        A.label(swap); A.imm(LDA_IMM, (uint8_t)(prgPages / 2 - 2)); emitSetBanks(A, 0, 2, prgPages, stubSlot); A.abs(LDA_ABS, SHIM + 1); emitSetBanks(A, 4, 2, prgPages, stubSlot);
        A.label(d); A.abs(LDA_ABS, SHIM + 2); emitSetBanks(A, 2, 2, prgPages, stubSlot);
        A.imm(LDA_IMM, (uint8_t)(prgPages / 2 - 1)); emitSetBanks(A, 6, 2, prgPages, stubSlot);
        A.op(RTS);
    }
    if (!plist.empty() && base.mapper == 1) {
        // SHIM: +0 shift +1 count +2..+5 regs0..3 +6 hi(16k units)
        int pages16 = prgPages / 4; bool surom = pages16 > 16;
        for (int r = 0; r < 4; r++) {
            std::string nores = A.tmp(), sh = A.tmp(), s0 = A.tmp(), done = A.tmp();
            A.label("MMC1W" + std::to_string(r));
            A.op(TAY); A.imm(AND_IMM, 0x80); A.jcc(BEQ, nores);
            A.imm(LDA_IMM, 0); A.abs(STA_ABS, SHIM); A.abs(STA_ABS, SHIM + 1); A.abs(LDA_ABS, SHIM + 2); A.imm(ORA_IMM, 0x0C); A.abs(STA_ABS, SHIM + 2); A.absL(JMP_ABS, "MMC1RECOMP");
            A.label(nores);
            A.op(TYA); A.imm(AND_IMM, 1); A.abs(LDX_ABS, SHIM + 1); A.bra(BEQ, s0); A.label(sh); A.op(ASL_A); A.op(DEX); A.bra(BNE, sh); A.label(s0);
            A.abs(ORA_ABS, SHIM); A.abs(STA_ABS, SHIM); A.abs(INC_ABS, SHIM + 1); A.abs(LDA_ABS, SHIM + 1); A.imm(CMP_IMM, 5); A.jcc(BNE, done);
            A.abs(LDA_ABS, SHIM); A.abs(STA_ABS, SHIM + 2 + r); A.imm(LDA_IMM, 0); A.abs(STA_ABS, SHIM); A.abs(STA_ABS, SHIM + 1); A.absL(JMP_ABS, "MMC1RECOMP");
            A.label(done); A.op(RTS);
        }
        A.label("MMC1RECOMP");
        std::string m2 = A.tmp(), m3 = A.tmp(), d = A.tmp(), nohi = A.tmp();
        A.imm(LDA_IMM, 0); A.abs(STA_ABS, SHIM + 6);
        if (surom) { A.abs(LDA_ABS, SHIM + 3); A.imm(AND_IMM, 0x10); A.jcc(BEQ, nohi); A.imm(LDA_IMM, 16); A.abs(STA_ABS, SHIM + 6); A.label(nohi); }
        A.abs(LDA_ABS, SHIM + 2); A.op(LSR_A); A.op(LSR_A); A.imm(AND_IMM, 3); A.imm(CMP_IMM, 2); A.jcc(BEQ, m2); A.imm(CMP_IMM, 3); A.jcc(BEQ, m3);
        A.abs(LDA_ABS, SHIM + 5); A.imm(AND_IMM, 0x0E); A.abs(ORA_ABS, SHIM + 6); A.op(LSR_A); emitSetBanks(A, 0, 8, prgPages, stubSlot); A.absL(JMP_ABS, d);
        A.label(m2); A.abs(LDA_ABS, SHIM + 6); emitSetBanks(A, 0, 4, prgPages, stubSlot); A.abs(LDA_ABS, SHIM + 5); A.imm(AND_IMM, 0x0F); A.abs(ORA_ABS, SHIM + 6); emitSetBanks(A, 4, 4, prgPages, stubSlot); A.absL(JMP_ABS, d);
        A.label(m3); A.abs(LDA_ABS, SHIM + 5); A.imm(AND_IMM, 0x0F); A.abs(ORA_ABS, SHIM + 6); emitSetBanks(A, 0, 4, prgPages, stubSlot); A.imm(LDA_IMM, (uint8_t)(surom ? 15 : pages16 - 1)); A.abs(ORA_ABS, SHIM + 6); emitSetBanks(A, 4, 4, prgPages, stubSlot);
        A.label(d); A.op(RTS);
    }
    // tables
    A.label("PG2NSF"); { std::vector<uint8_t> t(prgPages); for (int i = 0; i < prgPages; i++) t[i] = (uint8_t)pg2nsf[i]; A.data(t); }
    A.label("CFGTAB"); for (auto& c : cfgs) A.data(std::vector<uint8_t>(c.begin(), c.end()));
    if (base.expansion & EXP_N163) { A.label("N163TAB"); A.data(std::vector<uint8_t>(snap.n163Ram, snap.n163Ram + 128)); }
    if (ramMode) { A.label("PLAYTRD"); A.data(nb.ramPlayTramp); }
    std::vector<uint8_t> recLo(ntracks), recHi(ntracks);
    A.label("RECLO"); size_t recLoPos = A.c.size(); A.data(recLo);
    A.label("RECHI"); size_t recHiPos = A.c.size(); A.data(recHi);
    std::vector<size_t> recPos(ntracks);
    for (int t = 0; t < ntracks; t++) { uint16_t addr = A.pc(); A.c[recLoPos + t] = addr & 0xFF; A.c[recHiPos + t] = addr >> 8; recPos[t] = A.c.size(); A.data(recs[t]); }
    for (size_t i = 0; i < runs.size(); i++) if (runBank[i] == 0) { A.label("RUN" + std::to_string(i)); A.data(runs[i].d); }
    for (int t = 0; t < ntracks; t++) {
        uint16_t addr; uint8_t bank = (uint8_t)blobBank[t];
        if (bank == 0) { addr = A.pc(); A.data(blobs[t]); } else { addr = B2 + blobOff[t]; bank = (uint8_t)(pages.size() + blobBank[t]); }
        A.c[recPos[t] + 1] = bank; A.c[recPos[t] + 2] = addr & 0xFF; A.c[recPos[t] + 3] = addr >> 8;
    }
    std::string aerr;
    if (!A.resolve(&aerr)) { nb.report = "stub assembly failed: " + aerr; return nb; }
    if (A.c.size() > 4096) {
        size_t perTrack = 0; for (int t = 0; t < ntracks; t++) perTrack += 10 + (blobBank[t] == 0 ? blobs[t].size() : 0); perTrack = std::max<size_t>(1, perTrack / std::max(1, ntracks));
        size_t fixed = A.c.size() - perTrack * ntracks; nb.maxTracksHint = fixed < 4096 ? (int)((4096 - fixed) / perTrack) : 0;
        nb.report = "スタブバンクが4KBを超えました (" + std::to_string(A.c.size()) + " bytes: RAM復元 " + std::to_string(restBytes) + " bytes, トラック " + std::to_string(ntracks) + "本)。出力トラック数を " + std::to_string(nb.maxTracksHint) + " 本以下に減らしてください"; return nb; }
    rep << "stub size: " << A.c.size() << " bytes (tracks " << ntracks << ", cfgs " << cfgs.size() << ")\n";
    // ---- 6. build image
    std::vector<uint8_t> img(4096 * (pages.size() + 1 + dataBanks), 0);
    memcpy(img.data(), A.c.data(), A.c.size());
    for (size_t i = 0; i < pages.size(); i++) memcpy(&img[4096 * (i + 1)], &base.prg[(size_t)pages[i] * 4096], 4096);
    for (size_t i = 0; i < runs.size(); i++) if (runBank[i]) memcpy(&img[4096 * (pages.size() + runBank[i]) + runOff[i]], runs[i].d.data(), runs[i].d.size());
    for (int t = 0; t < ntracks; t++) if (blobBank[t]) memcpy(&img[4096 * (pages.size() + blobBank[t]) + blobOff[t]], blobs[t].data(), blobs[t].size());
    if (pages.size() + 1 + dataBanks > 255) { nb.report = "バンク数が多すぎます"; return nb; }
    // $2002 read sites -> read the fake status byte in RAM instead
    for (auto& kv : ppuSites) {
        PatchSite& ps = kv.second; int idx = pg2nsf[ps.romPage]; if (idx == 0) continue;
        uint8_t* p = &img[4096 * idx + ps.off]; p[1] = (SHIM + 7) & 0xFF; p[2] = (SHIM + 7) >> 8;
        char b[96]; snprintf(b, sizeof b, "  ppu patch PC=$%04X: op %02X $2002 -> $%04X\n", ps.pc, ps.opcode, SHIM + 7); rep << b;
    }
    // apply patches
    for (size_t k = 0; k < plist.size(); k++) {
        PatchSite& ps = plist[k]; int idx = pg2nsf[ps.romPage]; if (idx == 0) { warn.push_back("patch target page not in image"); continue; }
        uint8_t* p = &img[4096 * idx + ps.off];
        uint16_t h = A.lab["SHIM" + std::to_string(k)];
        p[0] = JSR; p[1] = h & 0xFF; p[2] = h >> 8;
        char b[120]; snprintf(b, sizeof b, "  patch PC=$%04X: %s $%04X -> JSR $%04X\n", ps.pc, ps.opcode == STX_ABS ? "STX" : ps.opcode == STY_ABS ? "STY" : ps.opcode == STA_ABS ? "STA" : "STA,idx", ps.ea, h); rep << b;
    }
    // ---- 7. header
    std::vector<uint8_t> f(128, 0);
    memcpy(f.data(), "NESM\x1a", 5); f[5] = 1; f[6] = (uint8_t)ntracks; f[7] = 1;
    uint16_t playAddr = ramMode ? PLAYT : (uint16_t)(B + 3);
    f[8] = 0x00; f[9] = 0x80; f[10] = B & 0xFF; f[11] = B >> 8; f[12] = playAddr & 0xFF; f[13] = playAddr >> 8;
    auto putStr = [&](int off, const std::string& s) { for (int i = 0; i < 31 && i < (int)s.size(); i++) f[off + i] = (uint8_t)s[i]; };
    putStr(0x0E, title); putStr(0x2E, artist); putStr(0x4E, copyright);
    int speed = (int)(16639.0 / playCallsPerFrame());
    if (drv.playKind == 2 && speed == 16639) speed = 16668; // slightly longer than the APU frame IRQ period so the flag is always set
    f[0x6E] = speed & 0xFF; f[0x6F] = speed >> 8;
    for (int i = 0; i < 8; i++) f[0x70 + i] = (i == stubSlot) ? 0 : (uint8_t)pg2nsf[drv.playCfg[i] % prgPages];
    if (ramMode) { for (int i = 0; i < 8; i++) f[0x70 + i] = recs[0].empty() ? 0 : cfgs[recs[0][0]][i]; f[0x70 + stubSlot] = 0; }
    { int pspeed = (int)(19997.0 / playCallsPerFrame()); f[0x78] = pspeed & 0xFF; f[0x79] = pspeed >> 8; }
    f[0x7A] = opt.pal ? 0x01 : 0x00;   // bit0: PAL (50 Hz) / NTSC (60 Hz); the driver's own update rate is kept in both speed fields
    f[0x7B] = (uint8_t)base.expansion;
    f.insert(f.end(), img.begin(), img.end());
    nb.file = f; nb.ok = true; nb.stubSlot = stubSlot; nb.banks = (int)pages.size() + 1 + dataBanks; nb.warnings = warn;
    for (auto& w : warn) rep << "WARN: " << w << "\n";
    nb.report = rep.str();
    return nb;
}

// ------------------------------------------------------------------ NSF player
bool NsfPlayer::load(const std::vector<uint8_t>& nsf) {
    ok = false;
    if (nsf.size() < 128 || memcmp(nsf.data(), "NESM\x1a", 5)) { err = "not an NSF"; return false; }
    songs = nsf[6]; uint16_t loadAddr = nsf[8] | (nsf[9] << 8); init = nsf[10] | (nsf[11] << 8); play = nsf[12] | (nsf[13] << 8);
    pal = (nsf[0x7A] & 1) != 0;
    speed = pal ? (nsf[0x78] | (nsf[0x79] << 8)) : (nsf[0x6E] | (nsf[0x6F] << 8)); if (speed == 0) speed = pal ? 19997 : 16639; expansion = nsf[0x7B];
    std::vector<uint8_t> data(nsf.begin() + 128, nsf.end());
    nes.setupNsf(data, loadAddr, &nsf[0x70], expansion);
    initBanks.assign(nsf.begin() + 0x70, nsf.begin() + 0x78);
    ok = true; return true;
}
bool NsfPlayer::selectTrack(int t) {
    if (!ok) return false;
    memset(nes.ram, 0, sizeof nes.ram); memset(nes.wram, 0, sizeof nes.wram); memset(nes.n163Ram, 0, sizeof nes.n163Ram);
    nes.reset(); nes.expansion = expansion;
    bool bs = false; for (int i = 0; i < 8; i++) if (initBanks[i]) bs = true;
    for (int i = 0; i < 8; i++) nes.nsfBank[i] = bs ? initBanks[i] : (uint8_t)i;
    for (uint16_t a = 0x4000; a <= 0x4013; a++) nes.write(a, 0); nes.write(0x4015, 0x0F); nes.write(0x4017, 0x40);
    nes.logSound = true; nes.soundLog.clear(); nes.logBaseCycle = (uint32_t)nes.cycles;
    pendingInit = true;
    bool ok = nes.callSubroutine(init, (uint8_t)t, 0, 0, 2000000);
    if (!ok && getenv("NSFRIP_DEBUG")) fprintf(stderr, "DBG selectTrack fail: pc=%04X sp=%02X jammed=%d sentinelHit=%d cycles=%llu\n", nes.pc, nes.sp, nes.jammed, nes.sentinelHit, (unsigned long long)nes.cycles);
    return ok;
}
bool NsfPlayer::frame(std::vector<SoundWrite>& writes) {
    if (!pendingInit) { nes.soundLog.clear(); nes.logBaseCycle = (uint32_t)nes.cycles; }
    pendingInit = false;
    nes.apuFrameClock(cyclesPerFrame()); // advance APU frame counter for the idle part of the frame
    bool r = nes.callSubroutine(play, 0, 0, 0, 1000000);
    writes = nes.soundLog; nes.soundLog.clear();
    return r;
}

static inline uint64_t fnv(uint64_t h, uint32_t v) { h ^= v; h *= 1099511628211ULL; return h; }

bool Ripper::verifyNsf(const NsfBuild& b, int trackIdx, int candIdx, int frames, std::string* msg) {
    if (!b.ok) return false;
    NsfPlayer pl; if (!pl.load(b.file)) { if (msg) *msg = pl.err; return false; }
    if (!pl.selectTrack(trackIdx)) {
        if (msg) *msg = "INIT timeout";
        if (getenv("NSFRIP_DEBUG")) { // re-run INIT step by step and dump the tail of the PC trace
            NsfPlayer p2; p2.load(b.file); NES& n = p2.nes; memset(n.ram, 0, sizeof n.ram); n.reset(); n.expansion = p2.expansion; for (int i = 0; i < 8; i++) n.nsfBank[i] = p2.initBanks[i];
            n.sp = 0xFF; n.push(0xFF); n.push(0xFD); n.pc = p2.init; n.a = (uint8_t)trackIdx; n.x = n.y = 0; n.p = 0x24; n.harnessMode = true;
            std::vector<uint16_t> pcs; for (int i = 0; i < 400000 && n.pc != 0xFFFE; i++) { pcs.push_back(n.pc); n.step(); }
            fprintf(stderr, "DBG INIT trace (%zu steps), first 60:", pcs.size()); for (size_t i = 0; i < pcs.size() && i < 60; i++) fprintf(stderr, " %04X", pcs[i]);
            fprintf(stderr, "\n  last 40:"); for (size_t i = pcs.size() > 40 ? pcs.size() - 40 : 0; i < pcs.size(); i++) fprintf(stderr, " %04X", pcs[i]); fprintf(stderr, "\n");
        }
        return false;
    }
    if (getenv("NSFRIP_DEBUG")) {
        NES hn = base; runRecipe(hn, snapOf(cands[candIdx]), cands[candIdx].recipe, 0);
        int nd = 0; for (int a = 0; a < 0x800; a++) if (hn.ram[a] != pl.nes.ram[a] && (a < 0x100 || a >= 0x200)) { if (nd++ < 24) fprintf(stderr, "DBG RAM diff %04X: harness=%02X nsf=%02X drvRead=%d\n", a, hn.ram[a], pl.nes.ram[a], (drv.driverRead[a] & MARK_PLAY) ? 1 : 0); }
        fprintf(stderr, "DBG RAM diffs in driver-read set: %d; nsf banks:", nd); for (int i = 0; i < 8; i++) fprintf(stderr, " %d", pl.nes.nsfBank[i]); fprintf(stderr, " play=%04X bytes:", pl.play); for (int i = 0; i < 24; i++) fprintf(stderr, " %02X", pl.nes.ram[(pl.play + i) & 0x7FF]); fprintf(stderr, "\n");

    }
    uint64_t total = 1469598103934665603ULL; int w = 0;
    std::vector<SoundWrite> ws;
    std::vector<uint64_t> fh; std::vector<std::vector<SoundWrite>> nsfWrites;
    bool dbg = getenv("NSFRIP_DEBUG") != nullptr;
    if (dbg) { // lockstep PC comparison of the first PLAY call
        NsfPlayer p4; p4.load(b.file); p4.selectTrack(trackIdx); NES& n4 = p4.nes;
        NES h4 = base; runRecipe(h4, snapOf(cands[candIdx]), cands[candIdx].recipe, 0);
        applyCfg(h4, cands[candIdx].recipe.hasPlayCfg() ? cands[candIdx].recipe.playCfg : drv.playCfg); for (auto& kv : drv.refresh) if (kv.first < 0x800) h4.ram[kv.first] = kv.second;
        uint16_t pr = drv.playRoutines[0];
        // harness: direct call; nsf: run the PLAY stub until it reaches pr
        h4.sp = 0xFF; h4.push(0xFF); h4.push(0xFD); h4.pc = pr; h4.a = h4.x = h4.y = 0; h4.p = 0x24; h4.harnessMode = true;
        n4.sp = 0xFF; n4.push(0xFF); n4.push(0xFD); n4.pc = p4.play; n4.a = (uint8_t)0; n4.x = n4.y = 0; n4.p = 0x24; n4.harnessMode = true;
        for (int i = 0; i < 2000 && n4.pc != pr; i++) n4.step();
        int div = -1; for (int i = 0; i < 20000; i++) { if (h4.pc != n4.pc || h4.a != n4.a || h4.x != n4.x || h4.y != n4.y) { div = i; fprintf(stderr, "DBG lockstep divergence at step %d: harness pc=%04X A=%02X X=%02X Y=%02X P=%02X | nsf pc=%04X A=%02X X=%02X Y=%02X P=%02X\n", i, h4.pc, h4.a, h4.x, h4.y, h4.p, n4.pc, n4.a, n4.x, n4.y, n4.p); break; } if (h4.pc == 0xFFFE) break; uint16_t ppc = h4.pc; h4.step(); n4.step(); if (div < 0 && i < 3) fprintf(stderr, "DBG step %d pc=%04X\n", i, ppc); }
        if (div < 0) fprintf(stderr, "DBG lockstep: no divergence in first PLAY call\n");
    }
    if (dbg) { // compare ROM read sequence of the first PLAY frame
        NsfPlayer p3; p3.load(b.file); p3.selectTrack(trackIdx); std::vector<uint32_t> rl; p3.nes.markOn = true; p3.nes.readLog = &rl; std::vector<SoundWrite> w3; p3.frame(w3); p3.nes.readLog = nullptr; p3.nes.markOn = false;
        fprintf(stderr, "DBG NSF frame0 reads (%zu):", rl.size()); for (size_t i = 0; i < rl.size() && i < 40; i++) fprintf(stderr, " %05X", rl[i]); fprintf(stderr, "\n");
        NES h3 = base; std::vector<uint32_t> hl; runRecipe(h3, snapOf(cands[candIdx]), cands[candIdx].recipe, 1, nullptr, true, &hl);
        fprintf(stderr, "DBG HAR init+frame0 reads (%zu):", hl.size()); for (size_t i = hl.size() > 40 ? hl.size() - 40 : 0; i < hl.size(); i++) fprintf(stderr, " %05X", hl[i]); fprintf(stderr, "\n");
    }
    for (int f = 0; f < frames; f++) {
        if (!pl.frame(ws)) { if (msg) *msg = "PLAY timeout at frame " + std::to_string(f); return false; }
        uint64_t h = 1469598103934665603ULL;
        for (auto& x : ws) { if (x.addr == 0x4017 || (x.addr & 0xF800) == 0x4800 || (x.addr & 0xF800) == 0xF800) continue; h = fnv(h, ((uint32_t)x.addr << 8) | x.val); w++; }
        fh.push_back(h); total = fnv(total, (uint32_t)h) ^ (h >> 32);
        if (dbg) nsfWrites.push_back(ws);
    }
    NES nes = base; std::vector<SoundWrite> hlog;
    HarnessResult hr = runRecipe(nes, snapOf(cands[candIdx]), cands[candIdx].recipe, frames, dbg ? &hlog : nullptr);
    if (!hr.ok) { if (msg) *msg = "harness failed"; return false; }
    if (hr.total != total) {
        int first = -1; for (size_t i = 0; i < fh.size() && i < hr.frameHash.size(); i++) if (fh[i] != hr.frameHash[i]) { first = (int)i; break; }
        if (dbg && first >= 0) {
            fprintf(stderr, "DBG verify mismatch frame %d\n  NSF:", first); for (auto& x : nsfWrites[first]) fprintf(stderr, " %04X=%02X", x.addr, x.val);
            int cpf = (int)(29780.5 / playCallsPerFrame());
            fprintf(stderr, "\n  HAR:"); for (size_t i = 0; i < hlog.size(); i++) { int fr = (int)(hlog[i].cycle / cpf); if (fr == first) fprintf(stderr, " %04X=%02X", hlog[i].addr, hlog[i].val); } fprintf(stderr, "\n");
        }
        if (msg) *msg = "mismatch (first differing frame " + std::to_string(first) + ", nsf writes " + std::to_string(w) + " vs harness " + std::to_string(hr.writes) + ")";
        return false;
    }
    if (msg) *msg = "OK (" + std::to_string(w) + " writes, " + std::to_string(frames) + " frames)";
    return true;
}

std::vector<int16_t> Ripper::renderNsf(const std::vector<uint8_t>& nsf, int track, double seconds) {
    std::vector<int16_t> out;
    NsfPlayer pl; if (!pl.load(nsf)) return out;
    if (!pl.selectTrack(track)) return out;
    Synth syn; syn.setClock(pl.cpuClock()); syn.reset(pl.expansion);
    NES* np = &pl.nes; syn.setMemReader([np](uint16_t a) { return np->peek(a); });
    syn.write(0x4015, 0x0F); syn.write(0x4017, 0x40);
    int cpf = pl.cyclesPerFrame(); int frames = (int)(seconds * pl.cpuClock() / cpf);
    std::vector<SoundWrite> ws;
    for (int f = 0; f < frames; f++) {
        if (!pl.frame(ws)) break;
        uint32_t t = 0;
        for (auto& w : ws) { uint32_t at = std::min<uint32_t>(w.cycle, cpf - 1); if (at > t) { syn.run(at - t, out); t = at; } syn.write(w.addr, w.val); }
        if ((int)t < cpf) syn.run(cpf - t, out);
    }
    return out;
}
