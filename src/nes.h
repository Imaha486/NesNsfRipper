// nes.h - compact NES core for sound-driver tracing / NSF playback
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <cstring>

struct SoundWrite { uint32_t cycle; uint16_t addr; uint8_t val; };
// pseudo "write" logged when the N163 data port is READ: the read advances the auto-increment address, so a synth or
// player replaying the log must advance too (drivers use dummy reads to skip the phase bytes)
enum : uint16_t { N163_READ_MARK = 0x4FFE };

enum : uint8_t { MARK_OUT = 1, MARK_PLAY = 2, MARK_INIT = 4 };

// Expansion chip flags (NSF header bit layout)
enum { EXP_VRC6 = 1, EXP_VRC7 = 2, EXP_FDS = 4, EXP_MMC5 = 8, EXP_N163 = 16, EXP_5B = 32 };

struct TraceHooks {
    virtual ~TraceHooks() {}
    virtual void onJsr(uint16_t target, uint16_t from, uint8_t sp) {}
    virtual void onRts(uint8_t spBefore) {}
    virtual void onInterrupt(int kind, uint16_t vector, uint8_t sp) {} // kind 1=NMI 2=IRQ
    virtual void onRti(uint8_t spBefore) {}
    virtual void onSoundWrite(uint16_t addr, uint8_t val) {}
    virtual void onRamWrite(uint16_t addr, uint8_t val, uint16_t pc) {}
    virtual void onPpuIo(uint16_t addr) {}
    virtual void onMapperWrite(uint16_t addr, uint8_t val, uint16_t pc, bool prgAffecting) {}
};

struct MapperState {
    // generic
    uint8_t prgMap[8];      // 4K page index per 4K slot ($8000..$F000)
    uint8_t chrMap[8];      // 1K page index per 1K slot
    uint8_t regs[32];       // mapper specific registers
    uint8_t cmd;            // MMC3/5B command
    uint8_t shift, shiftCount; // MMC1
    uint16_t irqCounter, irqLatch; bool irqEnable, irqReload, irqPending, irqEnableAfter;
    int32_t irqCycleCounter; int irqPrescaler;
    uint8_t mirror;
    uint8_t wramBank;
    uint8_t n163Addr; // N163 sound address port
    uint8_t s5bAddr;  // 5B audio register select
    uint8_t vrc7Addr;
};

class NES {
public:
    // ROM
    std::vector<uint8_t> prg, chr;
    int mapper = 0; bool battery = false; bool fourScreen = false; uint8_t hdrMirror = 0;
    int prgPages4k = 0; // prg.size()/4096
    bool chrIsRam = false;
    int expansion = 0;   // EXP_* bits of chips present (sound write tracking)

    // NSF mode
    bool nsfMode = false;
    uint8_t nsfBank[8];

    // memory
    uint8_t ram[0x800];
    uint8_t wram[0x2000];
    uint8_t vram[0x1000];
    uint8_t pal[32];
    uint8_t oam[256];
    uint8_t chrRam[0x2000];
    uint8_t n163Ram[128];
    uint8_t openBus = 0;

    // CPU
    uint16_t pc; uint8_t a, x, y, sp, p;
    uint64_t cycles = 0;
    bool nmiPending = false, nmiLine = false, nmiDelayed = false; int nmiDelayCount = 0; uint8_t irqLine = 0; // bits: 1 mapper 2 apu-frame 4 dmc
    bool jammed = false;
    uint64_t illegalOps = 0;

    // PPU (timing-only)
    uint8_t ppuCtrl = 0, ppuMask = 0, ppuStatus = 0, oamAddr = 0;
    uint16_t vramAddr = 0, vramTemp = 0; bool addrLatch = false; uint8_t readBuf = 0;
    int scanline = 0; // current scanline during runFrame
    bool inVblank = false;
    uint64_t frameCount = 0;

    // Controllers
    uint8_t pad[2] = {0, 0}; uint8_t padShift[2] = {0, 0}; bool padStrobe = false;

    // APU (register state for frame IRQ & DMC range tracking)
    uint8_t apuRegs[0x18];
    bool apuFrameIrqInhibit = true; bool apuFrameMode5 = false; int apuFrameCycle = 0; int apuFrameStep = 0;
    uint16_t dmcAddr = 0xC000, dmcLen = 0;

    // Mapper
    MapperState m;

    // tracing
    TraceHooks* hooks = nullptr;
    bool traceOn = false;
    uint8_t curMark = MARK_OUT;
    std::vector<uint8_t> romTouch;        // per 4K ROM page: mark bits
    uint8_t slotTouch = 0, slotTouchPlay = 0, slotTouchInit = 0;
    std::vector<uint8_t> romExec, romData;    // per PRG byte: executed as code / read as data (mark bits)
    std::vector<uint32_t>* readLog = nullptr;  // optional ordered log of ROM data reads (PRG offsets)                // bitmask of 4K slots read
    std::vector<uint8_t> ramRd, ramWr;  // 0x10000 each (heap: keeps NES copies off the stack); marks for RAM/WRAM reads & writes (by cpu addr)
    bool markOn = false;

    // sound write log
    std::vector<SoundWrite> soundLog;
    bool logSound = false;
    uint32_t logBaseCycle = 0;

    // harness
    uint16_t sentinel = 0xFFFE; bool sentinelHit = false;

    NES();
    bool loadRom(const std::vector<uint8_t>& file, std::string* err);
    void setupNsf(const std::vector<uint8_t>& data, uint16_t load, const uint8_t* banks, int expansionFlags);
    void reset();
    void powerOnRam();

    // execution
    void runFrame();                 // one NTSC frame with PPU timing/NMI/IRQ
    int step();                      // execute one instruction, returns cycles
    void runCycles(int64_t n);
    void nmi(); void irq();
    bool callSubroutine(uint16_t addr, uint8_t ra, uint8_t rx, uint8_t ry, int64_t maxCycles, bool asInterrupt = false, uint8_t sp0 = 0xFF); // harness; false on timeout

    // memory
    uint8_t read(uint16_t addr);
    uint8_t fetch(uint16_t addr);  // instruction byte read (marks code instead of data)
    uint8_t peek(uint16_t addr) const; // no side effects
    void write(uint16_t addr, uint8_t v);
    uint8_t* prgPtr(uint16_t addr); // pointer to ROM byte (for addr>=0x8000)
    int romPageAt(uint16_t addr) const;

    // mapper
    void mapperReset();
    void mapperWrite(uint16_t addr, uint8_t v);
    void mapperWriteLow(uint16_t addr, uint8_t v); // $4018-$5FFF and $6000-$7FFF special
    uint8_t mapperReadLow(uint16_t addr, bool& handled);
    void mapperScanline();           // per visible scanline
    void mapperClock(int cyc);       // per cpu cycle(s)
    void setPrg8k(int slot8k, int bank8k);
    void setPrg16k(int slot16k, int bank16k);
    void setPrg32k(int bank32k);
    void setChr1k(int slot, int bank);
    bool isSoundReg(uint16_t addr) const;
    bool isPrgMapperReg(uint16_t addr, uint8_t v) const;
    std::string mapperName() const;

    // snapshot (CPU/RAM/mapper, not trace)
    struct Snapshot {
        uint8_t ram[0x800], wram[0x2000], n163Ram[128];
        uint16_t pc; uint8_t a, x, y, sp, p;
        MapperState m; uint8_t nsfBank[8];
        uint8_t apuRegs[0x18]; uint16_t dmcAddr, dmcLen;
        uint8_t ppuCtrl, ppuMask;
    };
    void apuFrameClock(int cyc);
    bool harnessMode = false;   // no interrupt dispatch (but IRQ flags still visible)
    void saveSnapshot(Snapshot& s) const;
    void loadSnapshot(const Snapshot& s);

private:
    uint8_t ppuRead(uint16_t addr);
    void ppuWrite(uint16_t addr, uint8_t v);
    uint16_t ntMirror(uint16_t addr) const;
public:
    void push(uint8_t v) { ram[0x100 | sp] = v; sp--; }
    uint8_t pull() { sp++; return ram[0x100 | sp]; }
    void apuWrite(uint16_t addr, uint8_t v);

    inline void setNZ(uint8_t v) { p = (p & ~0x82) | (v & 0x80) | (v == 0 ? 2 : 0); }
    void branch(bool cond, int& cyc);
    void adc(uint8_t v); void sbc(uint8_t v); void cmp(uint8_t r, uint8_t v);
};
