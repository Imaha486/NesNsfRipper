// cli.cpp - command line front end (analysis, NSF output, WAV rendering for verification)
#include "ripper.h"
#include <cmath>
#include "synth.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <chrono>

static std::vector<uint8_t> readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {}); }
static void writeFile(const std::string& p, const std::vector<uint8_t>& d) { std::ofstream f(p, std::ios::binary); f.write((const char*)d.data(), d.size()); }
static void writeWav(const std::string& p, const std::vector<int16_t>& s) {
    std::vector<uint8_t> d; auto w32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) d.push_back((v >> (8 * i)) & 0xFF); }; auto w16 = [&](uint16_t v) { d.push_back(v & 0xFF); d.push_back(v >> 8); };
    d.insert(d.end(), { 'R','I','F','F' }); w32(36 + (uint32_t)s.size() * 2); d.insert(d.end(), { 'W','A','V','E','f','m','t',' ' }); w32(16); w16(1); w16(1); w32(44100); w32(88200); w16(2); w16(16);
    d.insert(d.end(), { 'd','a','t','a' }); w32((uint32_t)s.size() * 2); for (int16_t v : s) w16((uint16_t)v);
    writeFile(p, d);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: nsfrip_cli rom.nes outbase [--wav seconds] [--frames N] [--threads N] [--long N] [--nosfx] [--pal]\n"); return 1; }
    std::string rom = argv[1], out = argv[2]; double wavSec = 0; int frames = 2400, threads = 4, longF = 3600; bool sfx = true, pal = false;
    for (int i = 3; i < argc; i++) { std::string a = argv[i]; if (a == "--wav" && i + 1 < argc) wavSec = atof(argv[++i]); else if (a == "--frames" && i + 1 < argc) frames = atoi(argv[++i]); else if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]); else if (a == "--long" && i + 1 < argc) longF = atoi(argv[++i]); else if (a == "--nosfx") sfx = false; else if (a == "--pal") pal = true; }
    auto t0 = std::chrono::steady_clock::now();
    Ripper rip(readFile(rom));
    if (!rip.ok()) { fprintf(stderr, "load failed: %s\n", rip.error.c_str()); return 1; }
    rip.opt.traceFrames = frames; rip.opt.threads = threads; rip.opt.longFrames = longF; rip.opt.includeSfx = sfx; rip.opt.pal = pal;
    rip.opt.progress = [](const std::string& s) { fprintf(stderr, "[%s]\n", s.c_str()); };
    printf("ROM: %s mapper %d (%s) PRG %zuK CHR %zuK\n", rom.c_str(), rip.base.mapper, rip.base.mapperName().c_str(), rip.base.prg.size() / 1024, rip.base.chr.size() / 1024);
    bool okA = rip.analyze();
    printf("%s", rip.drv.log.c_str()); size_t logLen = rip.drv.log.size();
    if (!okA) { printf("ANALYZE FAILED: %s\n", rip.error.c_str()); return 2; }
    for (int i = 3; i < argc; i++) if (std::string(argv[i]) == "--dump" && i + 2 < argc) {
        unsigned ad = strtoul(argv[i + 1], 0, 16), len = strtoul(argv[i + 2], 0, 16);
        for (int k = 0; k < 2; k++) { const NES::Snapshot& sn = k ? rip.drv.snapLater : rip.drv.snapFirst; printf("snap%c:", k ? 'B' : 'A'); for (unsigned a = ad; a < ad + len && a < 0x800; a++) printf(" %02X", sn.ram[a]); printf("\n"); }
        return 0;
    }
    for (int i = 3; i < argc; i++) if (std::string(argv[i]) == "--recipe" && i + 1 < argc) {
        unsigned ad, a, x, y; Recipe r; bool okr = false;
        if (sscanf(argv[i + 1], "M:%x:%x", &ad, &a) == 2) { r.type = RT_MAILBOX; r.mailbox = ad; r.value = a; r.initCfg = rip.drv.playCfg; okr = true; }
        else if (sscanf(argv[i + 1], "%x:%x:%x:%x", &ad, &a, &x, &y) == 4) { r.type = RT_JSR; r.routine = ad; r.a = a; r.x = x; r.y = y; r.initCfg = rip.drv.initInfo.count(ad) ? rip.drv.initInfo[ad].topCfg() : rip.drv.playCfg; okr = true; }
        if (okr) {
            NES n = rip.base; std::vector<SoundWrite> lg; auto hr = rip.runRecipe(n, rip.drv.snapFirst, r, 120, &lg);
            printf("probe %s: ok=%d writes=%d ch=%02X cfg=%s\n", r.describe().c_str(), hr.ok, hr.writes, hr.channels, cfgStr(r.initCfg).c_str());
            for (size_t k = 0; k < lg.size() && k < 40; k++) printf("   cyc=%u $%04X=%02X\n", lg[k].cycle, lg[k].addr, lg[k].val);
        }
        return 0;
    }
    bool okS = rip.runAlternatives();
    printf("%s", rip.drv.log.c_str() + logLen);
    if (!okS) { printf("SEARCH FAILED: no candidates\n"); return 3; }
    std::vector<int> sel; bool all = false; for (int i = 3; i < argc; i++) if (std::string(argv[i]) == "--all") all = true;
    for (auto& c : rip.cands) {
        bool take = (c.isMusic || sfx) && (all || (c.confidence > 0 && !c.suspicious));
        if (take) sel.push_back(c.index);
        printf("  %c %-9s conf=%d%s %-56s writes=%6d dur=%5d loop=%d+%d ch=%02X\n", take ? '*' : ' ', c.name.c_str(), c.confidence, c.suspicious ? "!" : " ", c.recipe.describe().c_str(), c.writes, c.durationFrames, c.loopStart, c.loopLen, c.channels & 0x3F); printf("      %s\n", c.note.c_str());
    }
    if (sel.empty()) { printf("no confident candidates\n"); return 3; }
    NsfBuild nb = rip.buildNsf(sel, rip.romTitle.empty() ? "NSF rip" : rip.romTitle, "", "");
    for (int tries = 0; tries < 4 && !nb.ok && nb.maxTracksHint > 0 && (int)sel.size() > nb.maxTracksHint; tries++) { int n = std::max(1, nb.maxTracksHint - 4); printf("%s\n-> 先頭 %d 本に絞って再生成\n", nb.report.c_str(), n); sel.resize(n); nb = rip.buildNsf(sel, rip.romTitle.empty() ? "NSF rip" : rip.romTitle, "", ""); }
    printf("%s", nb.report.c_str());
    if (!nb.ok) { printf("BUILD FAILED\n"); return 4; }
    writeFile(out + ".nsf", nb.file);
    int okCount = 0;
    for (size_t t = 0; t < sel.size(); t++) {
        std::string msg; bool ok = rip.verifyNsf(nb, (int)t, sel[t], 600, &msg);
        rip.cands[sel[t]].verified = ok; if (ok) okCount++;
        printf("  verify track %2zu (%s): %s\n", t + 1, rip.cands[sel[t]].name.c_str(), msg.c_str());
    }
    printf("VERIFIED %d/%zu tracks, nsf %zu bytes, %d banks\n", okCount, sel.size(), nb.file.size(), nb.banks);
    if (wavSec > 0) {
        for (size_t t = 0; t < sel.size() && t < 3; t++) {
            std::vector<int16_t> s = Ripper::renderNsf(nb.file, (int)t, wavSec);
            char n[32]; snprintf(n, sizeof n, "_t%02zu.wav", t + 1); writeWav(out + n, s);
            double rms = 0; int16_t pk = 0; for (int16_t v : s) { rms += (double)v * v; pk = std::max<int16_t>(pk, (int16_t)abs(v)); } rms = sqrt(rms / std::max<size_t>(1, s.size()));
            printf("  wav track %zu: %zu samples rms=%.0f peak=%d\n", t + 1, s.size(), rms, pk);
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    printf("time %.1fs\n", std::chrono::duration<double>(t1 - t0).count());
    return 0;
}
