// synth.cpp - 2A03 APU + N163 + 5B + VRC7 synthesis (test-playback quality)
#include "synth.h"
#include "nes.h"
#define _USE_MATH_DEFINES
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstring>
#include <algorithm>

static const uint8_t lengthTable[32] = { 10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14, 12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
static const uint8_t dutyTable[4][8] = { {0,1,0,0,0,0,0,0},{0,1,1,0,0,0,0,0},{0,1,1,1,1,0,0,0},{1,0,0,1,1,1,1,1} };
static const uint16_t noisePeriods[16] = { 4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068 };
static const uint16_t dmcRates[16] = { 428,380,340,320,286,254,226,214,190,160,142,128,106,84,72,54 };
static const uint8_t triSeq[32] = { 15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };

// VRC7 fixed instrument set (nesdev wiki, 2019 verified)
static const uint8_t vrc7Inst[16][8] = {
 {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},{0x03,0x21,0x05,0x06,0xE8,0x81,0x42,0x27},{0x13,0x41,0x14,0x0D,0xD8,0xF6,0x23,0x12},{0x11,0x11,0x08,0x08,0xFA,0xB2,0x20,0x12},
 {0x31,0x61,0x0C,0x07,0xA8,0x64,0x61,0x27},{0x32,0x21,0x1E,0x06,0xE1,0x76,0x01,0x28},{0x02,0x01,0x06,0x00,0xA3,0xE2,0xF4,0xF4},{0x21,0x61,0x1D,0x07,0x82,0x81,0x11,0x07},
 {0x23,0x21,0x22,0x17,0xA2,0x72,0x01,0x17},{0x35,0x11,0x25,0x00,0x40,0x73,0x72,0x01},{0xB5,0x01,0x0F,0x0F,0xA8,0xA5,0x51,0x02},{0x17,0xC1,0x24,0x07,0xF8,0xF8,0x22,0x12},
 {0x71,0x23,0x11,0x06,0x65,0x74,0x18,0x16},{0x01,0x02,0xD3,0x05,0xC9,0x95,0x03,0x02},{0x61,0x63,0x0C,0x00,0x94,0xC0,0x33,0xF6},{0x21,0x72,0x0D,0x00,0xC1,0xD5,0x56,0x06} };
static const double multTable[16] = { 0.5,1,2,3,4,5,6,7,8,9,10,10,12,12,15,15 };
static const double kslTable[16] = { 0,9,12,13.875,15,16.125,16.875,17.625,18,18.75,19.125,19.5,19.875,20.25,20.625,21 };
static const double OPLL_RATE = 3579545.0 / 72.0;

Synth::Synth() { cycPerSample = CPU_CLOCK / SAMPLE_RATE; reset(0); }

void Synth::reset(int exp) {
    expansion = exp; act = 0;
    memset(pl, 0, sizeof pl); memset(&tri, 0, sizeof tri); memset(&noi, 0, sizeof noi); memset(&dmc, 0, sizeof dmc);
    noi.lfsr = 1; noi.period = 4; dmc.period = 428; dmc.out = 0; status4015 = 0;
    frameCyc = 0; frameStep = 0; mode5 = false; irqInhibit = false;
    sampleAcc = 0; sampleSum = 0; accCount = 0;
    memset(n163Ram, 0, sizeof n163Ram); n163Addr = 0; n163Cyc = 0; n163Ch = 7; memset(n163Out, 0, sizeof n163Out);
    memset(s5bRegs, 0, sizeof s5bRegs); s5bRegs[7] = 0x3F; s5bAddr = 0; memset(s5bTone, 0, sizeof s5bTone); s5bLfsr = 1; s5bEnvStep = 0; s5bEnvHold = false; s5bEnvOut = 0;
    memset(vch, 0, sizeof vch); memset(vrc7Regs, 0, sizeof vrc7Regs); vrc7Addr = 0; vrc7Cyc = 0; lfoAm = lfoVib = 0; vrc7Last = 0;
    for (int c = 0; c < 6; c++) for (int o = 0; o < 2; o++) { vch[c].op[o].state = 0; vch[c].op[o].att = 48; }
}

// ------------------------------------------------------------------ writes
void Synth::write(uint16_t addr, uint8_t v) {
    if (addr >= 0x4000 && addr <= 0x4017) {
        switch (addr) {
        case 0x4000: case 0x4004: { Pulse& p = pl[(addr >> 2) & 1]; p.duty = v >> 6; p.lenHalt = (v >> 5) & 1; p.constVol = (v >> 4) & 1; p.vol = v & 15; break; }
        case 0x4001: case 0x4005: { Pulse& p = pl[(addr >> 2) & 1]; p.sweepEn = v >> 7; p.sweepPeriod = (v >> 4) & 7; p.sweepNeg = (v >> 3) & 1; p.sweepShift = v & 7; p.sweepReload = 1; break; }
        case 0x4002: case 0x4006: { Pulse& p = pl[(addr >> 2) & 1]; p.period = (p.period & 0x700) | v; break; }
        case 0x4003: case 0x4007: { Pulse& p = pl[(addr >> 2) & 1]; p.period = (p.period & 0xFF) | ((v & 7) << 8); if (p.enabled) p.len = lengthTable[v >> 3]; p.seq = 0; p.envStart = 1; break; }
        case 0x4008: tri.ctrl = v >> 7; tri.lenHalt = v >> 7; tri.linearReload = v & 0x7F; break;
        case 0x400A: tri.period = (tri.period & 0x700) | v; break;
        case 0x400B: tri.period = (tri.period & 0xFF) | ((v & 7) << 8); if (tri.enabled) tri.len = lengthTable[v >> 3]; tri.linReloadFlag = 1; break;
        case 0x400C: noi.lenHalt = (v >> 5) & 1; noi.constVol = (v >> 4) & 1; noi.vol = v & 15; break;
        case 0x400E: noi.mode = v >> 7; noi.period = noisePeriods[v & 15]; break;
        case 0x400F: if (noi.enabled) noi.len = lengthTable[v >> 3]; noi.envStart = 1; break;
        case 0x4010: dmc.irq = v >> 7; dmc.loop = (v >> 6) & 1; dmc.rateIdx = v & 15; dmc.period = dmcRates[v & 15]; break;
        case 0x4011: dmc.out = v & 0x7F; break;
        case 0x4012: dmc.sampleAddr = 0xC000 | (v << 6); break;
        case 0x4013: dmc.sampleLen = (v << 4) | 1; break;
        case 0x4015:
            status4015 = v;
            pl[0].enabled = v & 1; pl[1].enabled = v & 2; tri.enabled = v & 4; noi.enabled = v & 8; dmc.enabled = v & 16;
            if (!pl[0].enabled) pl[0].len = 0; if (!pl[1].enabled) pl[1].len = 0; if (!tri.enabled) tri.len = 0; if (!noi.enabled) noi.len = 0;
            if (!dmc.enabled) dmc.bytesLeft = 0; else if (dmc.bytesLeft == 0) { dmc.curAddr = dmc.sampleAddr; dmc.bytesLeft = dmc.sampleLen; }
            break;
        case 0x4017: mode5 = v & 0x80; irqInhibit = v & 0x40; frameCyc = 0; frameStep = 0; if (mode5) { clockQuarter(); clockHalf(); } break;
        }
        return;
    }
    if (expansion & 16) { // N163
        if ((addr & 0xF800) == 0xF800) { n163Addr = v; return; }
        if (addr == N163_READ_MARK) { if (n163Addr & 0x80) n163Addr = 0x80 | ((n163Addr + 1) & 0x7F); return; }
        if ((addr & 0xF800) == 0x4800) { n163Ram[n163Addr & 0x7F] = v; if (n163Addr & 0x80) n163Addr = 0x80 | ((n163Addr + 1) & 0x7F); return; }
    }
    if (expansion & 32) { // 5B
        if ((addr & 0xE000) == 0xC000) { s5bAddr = v & 0x0F; return; }
        if ((addr & 0xE000) == 0xE000) { s5bRegs[s5bAddr] = v; if (s5bAddr == 13) { s5bEnvStep = 0; s5bEnvHold = false; s5bEnvCnt = 0; s5bEnvAlt = false; } return; }
    }
    if (expansion & 2) { // VRC7
        if ((addr & 0xF030) == 0x9010) { vrc7Addr = v & 0x3F; return; }
        if ((addr & 0xF030) == 0x9030) {
            vrc7Regs[vrc7Addr] = v;
            int r = vrc7Addr;
            if (r < 8) { for (int c = 0; c < 6; c++) if (vch[c].inst == 0) vrc7UpdateChannel(c); }
            else if (r >= 0x10 && r <= 0x15) { int c = r - 0x10; vch[c].fnum = (vch[c].fnum & 0x100) | v; vrc7UpdateChannel(c); }
            else if (r >= 0x20 && r <= 0x25) { int c = r - 0x20; vch[c].fnum = (vch[c].fnum & 0xFF) | ((v & 1) << 8); vch[c].block = (v >> 1) & 7; vch[c].sus = (v >> 5) & 1; bool k = (v >> 4) & 1; if (k != vch[c].key) vrc7Key(c, k); vch[c].key = k; vrc7UpdateChannel(c); }
            else if (r >= 0x30 && r <= 0x35) { int c = r - 0x30; vch[c].inst = v >> 4; vch[c].vol = v & 15; vrc7UpdateChannel(c); }
            return;
        }
        if ((addr & 0xF030) == 0xE000 && (v & 0x40)) { for (int c = 0; c < 6; c++) { vch[c].key = false; vch[c].op[0].att = vch[c].op[1].att = 48; vch[c].op[0].state = vch[c].op[1].state = 0; } }
    }
}

// ------------------------------------------------------------------ 2A03
void Synth::clockPulse(Pulse& p) {
    if (p.envStart) { p.envStart = 0; p.envVol = 15; p.envDiv = p.vol; }
    else if (p.envDiv == 0) { p.envDiv = p.vol; if (p.envVol) p.envVol--; else if (p.lenHalt) p.envVol = 15; }
    else p.envDiv--;
}
void Synth::clockSweep(Pulse& p, int ch) {
    if (p.sweepDiv == 0 && p.sweepEn && p.sweepShift) {
        int delta = p.period >> p.sweepShift; int target = p.sweepNeg ? p.period - delta - (ch == 0 ? 1 : 0) : p.period + delta;
        if (target >= 0 && target <= 0x7FF && p.period >= 8) p.period = (uint16_t)target;
    }
    if (p.sweepDiv == 0 || p.sweepReload) { p.sweepDiv = p.sweepPeriod; p.sweepReload = 0; }
    else p.sweepDiv--;
}
void Synth::clockQuarter() {
    clockPulse(pl[0]); clockPulse(pl[1]);
    // noise envelope
    if (noi.envStart) { noi.envStart = 0; noi.envVol = 15; noi.envDiv = noi.vol; }
    else if (noi.envDiv == 0) { noi.envDiv = noi.vol; if (noi.envVol) noi.envVol--; else if (noi.lenHalt) noi.envVol = 15; }
    else noi.envDiv--;
    // triangle linear
    if (tri.linReloadFlag) tri.linear = tri.linearReload; else if (tri.linear) tri.linear--;
    if (!tri.ctrl) tri.linReloadFlag = 0;
}
void Synth::clockHalf() {
    for (int i = 0; i < 2; i++) { if (pl[i].len && !pl[i].lenHalt) pl[i].len--; clockSweep(pl[i], i); }
    if (tri.len && !tri.lenHalt) tri.len--;
    if (noi.len && !noi.lenHalt) noi.len--;
}

double Synth::apuOut() {
    int p1 = 0, p2 = 0, t = 0, n = 0, d = dmc.out;
    for (int i = 0; i < 2; i++) {
        Pulse& p = pl[i];
        int delta = p.period >> p.sweepShift; int target = p.sweepNeg ? p.period - delta : p.period + delta;
        bool mute = p.period < 8 || (p.sweepEn && !p.sweepNeg && target > 0x7FF) || p.len == 0;
        int vol = p.constVol ? p.vol : p.envVol;
        int o = mute ? 0 : (dutyTable[p.duty][p.seq] ? vol : 0);
        if (i == 0) p1 = o; else p2 = o;
        if (o && (chMask & (1 << i))) act |= 1 << i; if (!(chMask & (1 << i))) { if (i == 0) p1 = 0; else p2 = 0; }
    }
    if (tri.len && tri.linear && tri.period >= 2) { t = triSeq[tri.seq]; act |= 4; }
    if (!(chMask & 4)) t = 0;
    if (noi.len && !(noi.lfsr & 1)) { n = noi.constVol ? noi.vol : noi.envVol; if (n) act |= 8; }
    if (!(chMask & 8)) n = 0;
    if (!(chMask & 16)) d = 0; else if (dmc.bytesLeft || dmc.bitsLeft) act |= 16;
    double pulseOut = (p1 + p2) ? 95.88 / (8128.0 / (p1 + p2) + 100) : 0;
    double tnd = (t || n || d) ? 159.79 / (1.0 / (t / 8227.0 + n / 12241.0 + d / 22638.0) + 100) : 0;
    return pulseOut + tnd; // 0..~1
}

// ------------------------------------------------------------------ N163
void Synth::n163Tick(int cyc) {
    n163Cyc += cyc;
    while (n163Cyc >= 15) {
        n163Cyc -= 15;
        int nch = ((n163Ram[0x7F] >> 4) & 7) + 1;
        int ch = n163Ch; // 7 .. 8-nch
        if (ch < 8 - nch) ch = 7;
        uint8_t* r = &n163Ram[0x40 + ch * 8];
        uint32_t freq = r[0] | (r[2] << 8) | ((r[4] & 3) << 16);
        uint32_t phase = r[1] | (r[3] << 8) | (r[5] << 16);
        uint32_t len = 256 - (r[4] & 0xFC); if (len == 0) len = 256;
        phase = (phase + freq) % (len << 16);
        r[1] = phase & 0xFF; r[3] = (phase >> 8) & 0xFF; r[5] = (phase >> 16) & 0xFF;
        uint32_t idx = ((phase >> 16) + r[6]) & 0xFF;
        int s = (n163Ram[idx >> 1] >> ((idx & 1) * 4)) & 15;
        int vol = r[7] & 15;
        n163Out[ch] = (s - 8) * vol / (8.0 * 15.0);
        if (vol && s != 8) act |= 32;
        n163Ch = ch - 1; if (n163Ch < 8 - nch) n163Ch = 7;
    }
}

// ------------------------------------------------------------------ 5B
static const double ymVolTable[32] = { 0.0,0.0,0.00465,0.00772,0.01095,0.01395,0.0191,0.02301,0.03198,0.0377,0.05318,0.0634,0.08871,0.1055,0.1458,0.1731,0.2347,0.2778,0.3717,0.4387,0.5812,0.6826,0.8905,1.0, 1,1,1,1,1,1,1,1 };
void Synth::s5bTick(int cyc) {
    s5bCyc += cyc;
    while (s5bCyc >= 16) {
        s5bCyc -= 16;
        for (int c = 0; c < 3; c++) {
            uint16_t per = s5bRegs[c * 2] | ((s5bRegs[c * 2 + 1] & 15) << 8); if (per == 0) per = 1;
            if (++s5bTone[c].cnt >= per) { s5bTone[c].cnt = 0; s5bTone[c].out ^= 1; }
        }
        uint16_t nper = s5bRegs[6] & 31; if (nper == 0) nper = 1;
        if (++s5bNoiseCnt >= (uint32_t)nper * 2) { s5bNoiseCnt = 0; uint32_t bit = ((s5bLfsr) ^ (s5bLfsr >> 3)) & 1; s5bLfsr = (s5bLfsr >> 1) | (bit << 16); }
        uint32_t eper = (s5bRegs[11] | (s5bRegs[12] << 8)); if (eper == 0) eper = 1;
        if (++s5bEnvCnt >= eper) {
            s5bEnvCnt = 0;
            if (!s5bEnvHold) {
                s5bEnvStep++;
                if (s5bEnvStep >= 32) {
                    uint8_t sh = s5bRegs[13];
                    if (!(sh & 8)) { s5bEnvHold = true; s5bEnvOut = 0; }
                    else if (sh & 1) { s5bEnvHold = true; s5bEnvOut = (((sh & 4) != 0) != ((sh & 2) != 0)) ? 31 : 0; }
                    else { s5bEnvStep = 0; if (sh & 2) s5bEnvAlt = !s5bEnvAlt; }
                }
            }
        }
    }
}
double Synth::s5bOut() {
    uint8_t sh = s5bRegs[13];
    int env;
    if (s5bEnvHold) env = s5bEnvOut;
    else { bool attack = ((sh & 4) != 0) != s5bEnvAlt; env = attack ? s5bEnvStep : 31 - s5bEnvStep; }
    double out = 0;
    for (int c = 0; c < 3; c++) {
        bool toneOff = (s5bRegs[7] >> c) & 1, noiseOff = (s5bRegs[7] >> (c + 3)) & 1;
        int on = (toneOff || s5bTone[c].out) && (noiseOff || (s5bLfsr & 1));
        int vol = (s5bRegs[8 + c] & 16) ? env : (s5bRegs[8 + c] & 15) * 2 + 1;
        if (!(chMask & 32)) on = 0;
        if (on && vol) { out += ymVolTable[vol]; if (vol > 1) act |= 32; }
    }
    return out / 3.0;
}

// ------------------------------------------------------------------ VRC7
static double rateTime48(int rate) { // seconds for 48dB sweep at given effective rate (4..63)
    if (rate < 4) return 1e9;
    if (rate >= 60) return 0.0005;
    static const double mul[4] = { 1.0, 0.8, 0.6667, 0.5714 };
    return 20.16 * pow(2.0, -((rate >> 2) - 1)) * mul[rate & 3];
}
void Synth::vrc7UpdateChannel(int c) {
    Ch& ch = vch[c];
    const uint8_t* pt = ch.inst == 0 ? vrc7Regs : vrc7Inst[ch.inst];
    for (int o = 0; o < 2; o++) {
        Op& op = ch.op[o];
        uint8_t b0 = pt[o]; op.am = b0 & 0x80; op.vib = b0 & 0x40; op.egType = b0 & 0x20; op.ksr = b0 & 0x10; op.mult = b0 & 15;
        op.ksl = pt[2 + o] >> 6;
        op.tl = o == 0 ? (pt[2] & 0x3F) : 0;
        op.waveHalf = o == 0 ? (pt[3] & 8) : (pt[3] & 0x10);
        op.ar = pt[4 + o] >> 4; op.dr = pt[4 + o] & 15; op.sl = pt[6 + o] >> 4; op.rr = pt[6 + o] & 15;
        int rks = ch.block * 2 + (ch.fnum >> 8); if (!op.ksr) rks >>= 2; op.rks = rks;
        op.dphase = ch.fnum * pow(2.0, ch.block - 1) / 262144.0 * multTable[op.mult];
    }
    ch.fb = pt[3] & 7;
}
void Synth::vrc7Key(int c, bool on) {
    for (int o = 0; o < 2; o++) {
        Op& op = vch[c].op[o];
        if (on) { op.state = 1; op.attackPhase = 0; op.phase = 0; if (op.ar == 15) { op.state = 2; op.att = 0; } }
        else if (op.state && op.state != 4) { op.state = 4; }
    }
}
double Synth::vrc7Sample() {
    lfoAm += 3.7 / OPLL_RATE; if (lfoAm >= 1) lfoAm -= 1;
    lfoVib += 6.4 / OPLL_RATE; if (lfoVib >= 1) lfoVib -= 1;
    double amDb = (1 - cos(lfoAm * 2 * M_PI)) * 0.5 * 4.8; // 0..4.8 dB
    double vibFactor = sin(lfoVib * 2 * M_PI) * 0.007;
    double total = 0;
    for (int c = 0; c < 6; c++) {
        Ch& ch = vch[c];
        double outs[2];
        for (int o = 0; o < 2; o++) {
            Op& op = ch.op[o];
            // envelope
            double att = 48;
            switch (op.state) {
            case 0: att = 48; break;
            case 1: { int rate = std::min(63, op.ar * 4 + (int)op.rks); double t = rateTime48(rate) / 11.6; op.attackPhase += 1.0 / (t * OPLL_RATE);
                      if (op.attackPhase >= 1 || op.ar == 0 && false) { op.attackPhase = 1; op.state = 2; op.att = 0; }
                      att = 48 * (1 - log(1 + op.attackPhase * 127) / log(128.0)); op.att = att; break; }
            case 2: { int rate = op.dr ? std::min(63, op.dr * 4 + (int)op.rks) : 0; if (rate) op.att += 48.0 / (rateTime48(rate) * OPLL_RATE);
                      double sl = op.sl * 3.0; if (op.att >= sl) { op.att = sl; op.state = 3; } att = op.att; break; }
            case 3: { if (!op.egType) { int rate = op.rr ? std::min(63, op.rr * 4 + (int)op.rks) : 0; if (rate) op.att += 48.0 / (rateTime48(rate) * OPLL_RATE); } att = op.att; if (att >= 48) { att = 48; op.state = 0; } break; }
            case 4: { int R = ch.sus ? 5 : (op.egType ? op.rr : 7); int rate = R ? std::min(63, R * 4 + (int)op.rks) : 0; if (rate) op.att += 48.0 / (rateTime48(rate) * OPLL_RATE); att = op.att; if (att >= 48) { att = 48; op.state = 0; } break; }
            }
            if (op.state == 0) { outs[o] = 0; op.prevOut = op.out; op.out = 0; continue; }
            // level
            double tlDb = o == 0 ? op.tl * 0.75 : ch.vol * 3.0;
            double kslDb = 0; if (op.ksl) { double k = kslTable[ch.fnum >> 5] - 3.0 * (7 - ch.block); if (k > 0) kslDb = k / (1 << (3 - op.ksl)); }
            double amd = op.am ? amDb : 0;
            double totalDb = att + tlDb + kslDb + amd;
            double lin = totalDb >= 48 ? 0 : pow(10.0, -totalDb / 20.0);
            // phase
            double dp = op.dphase * (op.vib ? (1 + vibFactor) : 1);
            op.phase += dp; if (op.phase >= 1) op.phase -= floor(op.phase);
            double ph = op.phase;
            if (o == 0) { if (ch.fb) ph += (op.prevOut + op.out) * pow(2.0, ch.fb - 7); }
            else ph += outs[0] * 4.0;
            double s = sin(ph * 2 * M_PI);
            if (op.waveHalf && s < 0) s = 0;
            double v = s * lin;
            op.prevOut = op.out; op.out = v; outs[o] = v;
        }
        if (chMask & 32) { total += outs[1]; if (fabs(outs[1]) > 0.001) act |= 32; }
    }
    return total / 6.0;
}
void Synth::vrc7Tick(int cyc) {
    vrc7Cyc += cyc;
    while (vrc7Cyc >= 36) { vrc7Cyc -= 36; vrc7Last = vrc7Sample(); }
}

// ------------------------------------------------------------------ main run
void Synth::run(int cpuCycles, std::vector<int16_t>& out) {
    for (int i = 0; i < cpuCycles; i++) {
        // frame sequencer
        frameCyc++;
        if (frameCyc >= 7457) {
            frameCyc -= 7457; frameStep++;
            if (!mode5) { clockQuarter(); if (frameStep == 2 || frameStep == 4) clockHalf(); if (frameStep >= 4) frameStep = 0; }
            else { if (frameStep != 4) clockQuarter(); if (frameStep == 2 || frameStep == 5) clockHalf(); if (frameStep >= 5) frameStep = 0; }
        }
        // timers
        for (int k = 0; k < 2; k++) { Pulse& p = pl[k]; if (p.timer == 0) { p.timer = (p.period + 1) * 2; p.seq = (p.seq + 1) & 7; } else p.timer--; }
        if (tri.timer == 0) { tri.timer = tri.period + 1; if (tri.len && tri.linear) tri.seq = (tri.seq + 1) & 31; } else tri.timer--;
        if (noi.timer == 0) { noi.timer = noi.period; uint16_t fb = (noi.lfsr ^ (noi.lfsr >> (noi.mode ? 6 : 1))) & 1; noi.lfsr = (noi.lfsr >> 1) | (fb << 14); } else noi.timer--;
        // dmc
        if (dmc.timer == 0) {
            dmc.timer = dmc.period;
            if (!dmc.silence) { if (dmc.shift & 1) { if (dmc.out <= 125) dmc.out += 2; } else { if (dmc.out >= 2) dmc.out -= 2; } }
            dmc.shift >>= 1;
            if (dmc.bitsLeft == 0 || --dmc.bitsLeft == 0) {
                dmc.bitsLeft = 8;
                if (dmc.bufFull) { dmc.silence = false; dmc.shift = dmc.buf; dmc.bufFull = false; } else dmc.silence = true;
            }
            if (!dmc.bufFull && dmc.bytesLeft) {
                dmc.buf = memRead ? memRead(dmc.curAddr) : 0; dmc.bufFull = true;
                dmc.curAddr = (dmc.curAddr == 0xFFFF) ? 0x8000 : dmc.curAddr + 1;
                if (--dmc.bytesLeft == 0 && dmc.loop) { dmc.curAddr = dmc.sampleAddr; dmc.bytesLeft = dmc.sampleLen; }
            }
        } else dmc.timer--;
        double o = apuOut();
        if (expansion & 16) { n163Tick(1); int nch = ((n163Ram[0x7F] >> 4) & 7) + 1; double s = 0; for (int c = 8 - nch; c < 8; c++) s += n163Out[c]; if (chMask & 32) o += s / nch * 0.6; }
        if (expansion & 32) { s5bTick(1); o += s5bOut() * 0.5; }
        if (expansion & 2) { vrc7Tick(1); o += vrc7Last * 0.9; }
        sampleSum += o; sampleAcc += 1;
        if (sampleAcc >= cycPerSample) {
            double v = sampleSum / sampleAcc; sampleAcc -= cycPerSample; sampleSum = v * sampleAcc; // carry fraction
            // DC blocking (AC coupling) + gain
            double y = v - dcPrevIn + 0.996 * dcPrevOut; dcPrevIn = v; dcPrevOut = y;
            int s = (int)(y * 40000.0); if (s > 32767) s = 32767; if (s < -32768) s = -32768;
            out.push_back((int16_t)s);
        }
    }
}
