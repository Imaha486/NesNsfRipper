// synth.h - audio synthesis for 2A03 APU + N163 + Sunsoft 5B + VRC7
#pragma once
#include <cstdint>
#include <vector>
#include <functional>

class Synth {
public:
    static constexpr int SAMPLE_RATE = 44100;
    static constexpr double CPU_CLOCK = 1789772.7;
    void setClock(double hz) { cycPerSample = hz / SAMPLE_RATE; }

    Synth();
    void reset(int expansionFlags);
    void setMemReader(std::function<uint8_t(uint16_t)> r) { memRead = r; }
    void write(uint16_t addr, uint8_t v);          // register write at current time
    // Run `cpuCycles` cycles, appending 16-bit mono samples to out
    void run(int cpuCycles, std::vector<int16_t>& out);
    // channel activity flags (for display): bit0-4 2A03 ch, bit5 expansion
    uint32_t activity() const { return act; }
    void setChannelMask(uint32_t mask) { chMask = mask; }

public:
    std::function<uint8_t(uint16_t)> memRead;
    int expansion = 0;
    uint32_t act = 0; uint32_t chMask = 0xFFFFFFFF;
    double sampleAcc = 0; double sampleSum = 0; double cycPerSample; int accCount = 0; double dcPrevIn = 0, dcPrevOut = 0;
    // ---- 2A03
    struct Pulse { uint8_t duty, lenHalt, constVol, vol; uint8_t sweepEn, sweepPeriod, sweepNeg, sweepShift, sweepDiv, sweepReload; uint16_t period, timer; uint8_t seq, len, envDiv, envVol, envStart; bool enabled; };
    Pulse pl[2];
    struct { uint8_t ctrl, lenHalt; uint8_t linear, linearReload, linReloadFlag; uint16_t period, timer; uint8_t seq, len; bool enabled; } tri;
    struct { uint8_t lenHalt, constVol, vol, mode; uint16_t period, timer, lfsr; uint8_t len, envDiv, envVol, envStart; bool enabled; } noi;
    struct { uint8_t irq, loop, rateIdx; uint16_t timer, period; uint8_t out; uint16_t sampleAddr, sampleLen, curAddr, bytesLeft; uint8_t shift, bitsLeft; bool silence, bufFull; uint8_t buf; bool enabled; } dmc;
    uint8_t status4015 = 0;
    int frameCyc = 0; int frameStep = 0; bool mode5 = false; bool irqInhibit = false; int frameResetDelay = 0;
    void clockQuarter(); void clockHalf();
    void clockPulse(Pulse& p); void clockSweep(Pulse& p, int ch);
    int apuTick(); // returns raw mixed output (float scaled int)
    double apuOut();
    // ---- N163
    uint8_t n163Ram[128]; uint8_t n163Addr = 0; int n163Cyc = 0; int n163Ch = 7; double n163Out[8]; bool n163Silence = false;
    void n163Tick(int cyc);
    // ---- 5B
    uint8_t s5bRegs[16]; uint8_t s5bAddr = 0; int s5bCyc = 0;
    struct { uint16_t cnt; uint8_t out; } s5bTone[3];
    uint32_t s5bNoiseCnt = 0, s5bLfsr = 1; uint32_t s5bEnvCnt = 0; int s5bEnvStep = 0; bool s5bEnvHold = false; int s5bEnvOut = 0; bool s5bEnvAlt = false;
    void s5bTick(int cyc); double s5bOut();
    // ---- VRC7
    struct Op { double phase, dphase; int state; double att; double attackPhase; double prevOut, out; double fb; int ar, dr, sl, rr, mult, ksl, tl; bool am, vib, egType, ksr, waveHalf; double rks; };
    struct Ch { Op op[2]; int fnum, block, inst, vol; bool key, sus; int fb; };
    Ch vch[6]; uint8_t vrc7Regs[0x40]; uint8_t vrc7Addr = 0; int vrc7Cyc = 0; double lfoAm = 0, lfoVib = 0; double vrc7OutLP = 0;
    void vrc7UpdateChannel(int ch); void vrc7Key(int ch, bool on); double vrc7Sample(); void vrc7Tick(int cyc);
    double expOut; // leftover
    double vrc7Last = 0;
};
