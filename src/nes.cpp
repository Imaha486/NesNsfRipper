// nes.cpp - 6502 CPU, memory map, timing-only PPU, mappers
#include "nes.h"
#include <algorithm>
#include <cstdio>

static const uint8_t cycTable[256] = {
 7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
 6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
 6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
 6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
 2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4, 2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
 2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4, 2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
 2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
 2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7 };

NES::NES() {
    memset(ram, 0, sizeof ram); memset(wram, 0, sizeof wram); memset(vram, 0, sizeof vram);
    memset(pal, 0, sizeof pal); memset(oam, 0, sizeof oam); memset(chrRam, 0, sizeof chrRam);
    memset(n163Ram, 0, sizeof n163Ram); memset(apuRegs, 0, sizeof apuRegs);
    memset(&m, 0, sizeof m); memset(nsfBank, 0, sizeof nsfBank);
    ramRd.assign(0x10000, 0); ramWr.assign(0x10000, 0);
    pc = 0; a = x = y = 0; sp = 0xFD; p = 0x24;
}

bool NES::loadRom(const std::vector<uint8_t>& f, std::string* err) {
    if (f.size() < 16 || memcmp(f.data(), "NES\x1a", 4)) { if (err) *err = "iNES header not found"; return false; }
    int prgCount = f[4], chrCount = f[5];
    bool nes2 = (f[7] & 0x0C) == 0x08;
    mapper = (f[6] >> 4) | (f[7] & 0xF0);
    if (nes2) { mapper |= (f[8] & 0x0F) << 8; prgCount |= (f[9] & 0x0F) << 8; chrCount |= (f[9] & 0xF0) << 4; }
    battery = f[6] & 2; fourScreen = f[6] & 8; hdrMirror = f[6] & 1;
    size_t off = 16 + ((f[6] & 4) ? 512 : 0);
    size_t prgSize = (size_t)prgCount * 16384, chrSize = (size_t)chrCount * 8192;
    if (off + prgSize > f.size()) { if (err) *err = "file truncated (PRG)"; return false; }
    prg.assign(f.begin() + off, f.begin() + off + prgSize);
    // pad PRG to power of two multiple of 4K (min 32K)
    size_t p2 = 32768; while (p2 < prg.size()) p2 <<= 1;
    if (prg.size() < p2) { // mirror
        std::vector<uint8_t> t(p2); for (size_t i = 0; i < p2; i++) t[i] = prg[i % prg.size()]; prg.swap(t);
    }
    prgPages4k = (int)(prg.size() / 4096);
    if (chrSize && off + prgSize + chrSize <= f.size()) { chr.assign(f.begin() + off + prgSize, f.begin() + off + prgSize + chrSize); chrIsRam = false; }
    else { chr.clear(); chrIsRam = true; }
    if (!chr.empty()) { size_t c2 = 8192; while (c2 < chr.size()) c2 <<= 1; if (chr.size() < c2) { std::vector<uint8_t> t(c2); for (size_t i = 0; i < c2; i++) t[i] = chr[i % chr.size()]; chr.swap(t); } }
    expansion = 0;
    if (mapper == 19) expansion |= EXP_N163;
    if (mapper == 69) expansion |= EXP_5B;
    if (mapper == 85) expansion |= EXP_VRC7;
    if (mapper == 24 || mapper == 26) expansion |= EXP_VRC6;
    if (mapper == 5) expansion |= EXP_MMC5;
    romTouch.assign(prgPages4k, 0); romExec.assign(prg.size(), 0); romData.assign(prg.size(), 0);
    nsfMode = false;
    reset();
    return true;
}

void NES::setupNsf(const std::vector<uint8_t>& data, uint16_t load, const uint8_t* banks, int exp) {
    nsfMode = true; mapper = 0x100; expansion = exp; chr.clear(); chrIsRam = true;
    // build 4K banks: padding = load & 0xFFF
    size_t pad = load & 0xFFF;
    std::vector<uint8_t> img(pad, 0); img.insert(img.end(), data.begin(), data.end());
    while (img.size() % 4096) img.push_back(0);
    prg = img; prgPages4k = (int)(prg.size() / 4096);
    bool bs = false; for (int i = 0; i < 8; i++) if (banks[i]) bs = true;
    for (int i = 0; i < 8; i++) nsfBank[i] = bs ? banks[i] : (uint8_t)i;
    if (!bs) { // non-bankswitched: image placed at load address
        std::vector<uint8_t> img2(32768, 0);
        for (size_t i = 0; i < data.size() && load + i < 0x10000; i++) img2[(load - 0x8000) + i] = data[i];
        prg = img2; prgPages4k = 8;
    }
    romTouch.assign(prgPages4k, 0);
    reset();
}

void NES::powerOnRam() { memset(ram, 0, sizeof ram); memset(wram, 0, sizeof wram); }

void NES::reset() {
    memset(&m, 0, sizeof m);
    memset(apuRegs, 0, sizeof apuRegs); apuFrameIrqInhibit = false; apuFrameMode5 = false; apuFrameCycle = 0; apuFrameStep = 0;
    ppuCtrl = ppuMask = ppuStatus = 0; oamAddr = 0; vramAddr = vramTemp = 0; addrLatch = false; inVblank = false;
    nmiPending = false; nmiDelayed = false; nmiDelayCount = 0; nmiLine = false; irqLine = 0; jammed = false; frameCount = 0; cycles = 0;
    padShift[0] = padShift[1] = 0; padStrobe = false;
    m.mirror = hdrMirror;
    mapperReset();
    sp = 0xFD; p = 0x24; a = x = y = 0;
    if (!nsfMode) pc = read(0xFFFC) | (read(0xFFFD) << 8);
}

// ---------------------------------------------------------------- mapping helpers
void NES::setPrg8k(int slot8k, int bank8k) {
    int pages = prgPages4k; int b = ((bank8k * 2) % pages + pages) % pages;
    m.prgMap[slot8k * 2] = (uint8_t)b; m.prgMap[slot8k * 2 + 1] = (uint8_t)((b + 1) % pages);
}
void NES::setPrg16k(int slot16k, int bank16k) { setPrg8k(slot16k * 2, bank16k * 2); setPrg8k(slot16k * 2 + 1, bank16k * 2 + 1); }
void NES::setPrg32k(int bank32k) { setPrg16k(0, bank32k * 2); setPrg16k(1, bank32k * 2 + 1); }
void NES::setChr1k(int slot, int bank) {
    int pages = chrIsRam ? 8 : (int)(chr.size() / 1024); if (pages <= 0) pages = 8;
    m.chrMap[slot & 7] = (uint8_t)(((bank % pages) + pages) % pages);
}

std::string NES::mapperName() const {
    switch (mapper) {
    case 0: return "NROM"; case 1: return "MMC1"; case 2: return "UxROM"; case 30: return "UNROM 512"; case 28: return "Action 53"; case 3: return "CNROM"; case 4: return "MMC3";
    case 7: return "AxROM"; case 16: return "Bandai FCG"; case 19: return "Namco 163"; case 22: return "VRC2a"; case 23: return "VRC2b/VRC4";
    case 25: return "VRC4"; case 33: return "Taito TC0190"; case 66: return "GxROM"; case 67: return "Sunsoft-3"; case 69: return "Sunsoft FME-7/5B";
    case 75: return "VRC1"; case 80: return "Taito X1-005"; case 85: return "VRC7"; case 0x100: return "NSF"; default: return "mapper " + std::to_string(mapper);
    }
}

void NES::mapperReset() {
    int last16 = prgPages4k / 4 - 1;
    for (int i = 0; i < 8; i++) m.chrMap[i] = i;
    m.wramBank = 0;
    switch (mapper) {
    case 0x100: break;
    case 1: m.regs[0] = 0x0C; m.shift = 0; m.shiftCount = 0; setPrg16k(0, 0); setPrg16k(1, last16); break;
    case 2: case 30: case 16: case 67: case 33: case 22: case 23: case 25: case 75: case 85: case 19:
        setPrg16k(0, 0); setPrg16k(1, last16); break;
    case 28: m.regs[2] = 0x3C; m.regs[3] = 0x3F; m.regs[1] = 0; setPrg16k(0, 0); setPrg16k(1, last16); break;
    case 4: setPrg8k(0, 0); setPrg8k(1, 1); setPrg8k(2, -2); setPrg8k(3, -1); break;
    case 69: setPrg8k(0, 0); setPrg8k(1, 1); setPrg8k(2, 2); setPrg8k(3, -1); break;
    case 80: setPrg8k(0, 0); setPrg8k(1, 1); setPrg8k(2, -2); setPrg8k(3, -1); break;
    default: setPrg32k(0); break; // 0,3,7,66 ...
    }
}

bool NES::isSoundReg(uint16_t addr) const {
    if (addr >= 0x4000 && addr <= 0x4013) return true;
    if (addr == 0x4015 || addr == 0x4017) return true;
    if ((expansion & EXP_N163) && ((addr & 0xF800) == 0x4800 || (addr & 0xF800) == 0xF800)) return true;
    if ((expansion & EXP_5B) && ((addr & 0xE000) == 0xC000 || (addr & 0xE000) == 0xE000)) return true;
    if ((expansion & EXP_VRC7) && ((addr & 0xF030) == 0x9010 || (addr & 0xF030) == 0x9030)) return true;
    if ((expansion & EXP_VRC6) && (addr >= 0x9000 && addr <= 0xB002 && (addr & 0xF000) != 0xC000)) return true;
    if ((expansion & EXP_MMC5) && addr >= 0x5000 && addr <= 0x5015) return true;
    return false;
}

// does a write to addr possibly change PRG mapping?
bool NES::isPrgMapperReg(uint16_t addr, uint8_t v) const {
    switch (mapper) {
    case 0: case 3: return false;
    case 1: return addr >= 0x8000; // serial: any
    case 2: case 7: case 66: case 30: return addr >= 0x8000;
    case 28: return addr >= 0x8000 && (m.cmd == 0x01 || m.cmd == 0x80 || m.cmd == 0x81);
    case 4: return addr >= 0x8000 && addr <= 0x9FFF;
    case 16: return (addr & 0x000F) == 8 && (addr >= 0x6000);
    case 19: return addr >= 0xE000 && addr < 0xF800;
    case 22: return addr >= 0x8000 && addr < 0xC000 && ((addr & 0xF000) == 0x8000 || (addr & 0xF000) == 0xA000);
    case 23: case 25: return (addr >= 0x8000 && addr < 0xB000);
    case 33: return addr >= 0x8000 && addr <= 0x8001;
    case 67: return (addr & 0xF800) == 0xF800;
    case 69: return addr >= 0x8000 && addr < 0xC000; // cmd/param
    case 75: return addr >= 0x8000 && addr < 0xD000 && (addr & 0xF000) != 0x9000;
    case 80: return addr >= 0x7EFA && addr <= 0x7EFF;
    case 85: return addr >= 0x8000 && addr < 0xA000 && (addr & 0x0030) == 0;
    default: return addr >= 0x8000;
    }
}

uint8_t* NES::prgPtr(uint16_t addr) {
    int page = nsfMode ? nsfBank[(addr >> 12) & 7] : m.prgMap[(addr >> 12) & 7];
    if (page >= prgPages4k) page %= prgPages4k;
    return &prg[(size_t)page * 4096 + (addr & 0xFFF)];
}
int NES::romPageAt(uint16_t addr) const {
    int page = nsfMode ? nsfBank[(addr >> 12) & 7] : m.prgMap[(addr >> 12) & 7];
    return page % prgPages4k;
}

uint16_t NES::ntMirror(uint16_t addr) const {
    addr &= 0x0FFF;
    if (fourScreen) return addr;
    int nt = addr >> 10, off = addr & 0x3FF;
    switch (m.mirror) {
    case 0: nt = (nt >> 1) & 1; break; // horizontal mirroring
    case 1: nt &= 1; break;            // vertical mirroring
    case 2: nt = 0; break;
    case 3: nt = 1; break;
    }
    return (nt << 10) | off;
}

uint8_t NES::ppuRead(uint16_t addr) {
    addr &= 0x3FFF;
    if (addr < 0x2000) {
        if (chrIsRam || chr.empty()) return chrRam[addr & 0x1FFF];
        return chr[((size_t)m.chrMap[addr >> 10] << 10) | (addr & 0x3FF)];
    }
    if (addr < 0x3F00) return vram[ntMirror(addr)];
    return pal[addr & 0x1F];
}
void NES::ppuWrite(uint16_t addr, uint8_t v) {
    addr &= 0x3FFF;
    if (addr < 0x2000) { if (chrIsRam || chr.empty()) chrRam[addr & 0x1FFF] = v; return; }
    if (addr < 0x3F00) { vram[ntMirror(addr)] = v; return; }
    pal[addr & 0x1F] = v;
}

// ---------------------------------------------------------------- CPU bus
uint8_t NES::peek(uint16_t addr) const {
    if (addr < 0x2000) return ram[addr & 0x7FF];
    if (addr >= 0x8000) { int page = nsfMode ? nsfBank[(addr >> 12) & 7] : m.prgMap[(addr >> 12) & 7]; return prg[((size_t)(page % prgPages4k) << 12) | (addr & 0xFFF)]; }
    if (addr >= 0x6000) return wram[addr & 0x1FFF];
    return 0;
}

uint8_t NES::fetch(uint16_t addr) {
    if (addr >= 0x8000) {
        if (markOn) { int pg = romPageAt(addr); romTouch[pg] |= curMark; slotTouch |= 1 << ((addr >> 12) & 7); if (curMark & MARK_PLAY) slotTouchPlay |= 1 << ((addr >> 12) & 7); else slotTouchInit |= 1 << ((addr >> 12) & 7); if (!romExec.empty()) romExec[((size_t)pg << 12) | (addr & 0xFFF)] |= curMark; }
        return *prgPtr(addr);
    }
    return read(addr);
}

uint8_t NES::read(uint16_t addr) {
    if (addr < 0x2000) { if (markOn) { ramRd[addr & 0x7FF] |= curMark; } return ram[addr & 0x7FF]; }
    if (addr >= 0x8000) {
        if (markOn) { int pg = romPageAt(addr); romTouch[pg] |= curMark; slotTouch |= 1 << ((addr >> 12) & 7); if (curMark & MARK_PLAY) slotTouchPlay |= 1 << ((addr >> 12) & 7); else slotTouchInit |= 1 << ((addr >> 12) & 7); if (!romData.empty()) romData[((size_t)pg << 12) | (addr & 0xFFF)] |= curMark; if (readLog && readLog->size() < 16384) readLog->push_back(((uint32_t)pg << 12) | (addr & 0xFFF)); }
        return *prgPtr(addr);
    }
    if (addr >= 0x6000) {
        if (mapper == 80 && addr >= 0x7EF0 && addr < 0x7F00) return 0;
        if (mapper == 16 && addr < 0x8000) return 0; // FCG regs (no RAM) - ok for our titles
        if (markOn) ramRd[addr] |= curMark;
        return wram[addr & 0x1FFF];
    }
    if (addr < 0x4000) {
        if (traceOn && hooks) hooks->onPpuIo(addr);
        switch (addr & 7) {
        case 2: { if (harnessMode) return 0xC0; uint8_t v = ppuStatus | (openBus & 0x1F); ppuStatus &= ~0x80; addrLatch = false; return v; }
        case 4: return oam[oamAddr];
        case 7: {
            uint8_t v;
            if ((vramAddr & 0x3FFF) >= 0x3F00) { v = ppuRead(vramAddr); readBuf = ppuRead(vramAddr - 0x1000); }
            else { v = readBuf; readBuf = ppuRead(vramAddr); }
            vramAddr += (ppuCtrl & 4) ? 32 : 1; return v;
        }
        default: return openBus;
        }
    }
    if (addr == 0x4016 || addr == 0x4017) {
        int i = addr & 1;
        uint8_t v = padStrobe ? (pad[i] & 1) : (padShift[i] & 1);
        if (!padStrobe) padShift[i] = (padShift[i] >> 1) | 0x80;
        return 0x40 | v;
    }
    if (addr == 0x4015) {
        uint8_t v = 0; if (irqLine & 2) v |= 0x40; irqLine &= ~2; return v;
    }
    if (addr >= 0x4018) {
        bool handled = false; uint8_t v = mapperReadLow(addr, handled);
        if (handled) return v;
    }
    return openBus;
}

void NES::write(uint16_t addr, uint8_t v) {
    openBus = v;
    if (addr < 0x2000) { ram[addr & 0x7FF] = v; if (markOn) ramWr[addr & 0x7FF] |= curMark; if (traceOn && hooks) hooks->onRamWrite(addr & 0x7FF, v, pc); return; }
    if (addr >= 0x8000) {
        if (isSoundReg(addr)) {
            if (logSound) soundLog.push_back({ (uint32_t)(cycles - logBaseCycle), addr, v });
            if (traceOn && hooks) hooks->onSoundWrite(addr, v);
        }
        if (nsfMode) {
            if (expansion & EXP_N163) { if ((addr & 0xF800) == 0xF800) m.n163Addr = v; }
            return;
        }
        bool prgAff = isPrgMapperReg(addr, v);
        if (traceOn && hooks) hooks->onMapperWrite(addr, v, pc, prgAff);
        mapperWrite(addr, v); return;
    }
    if (addr >= 0x6000) {
        if (mapper == 80 && addr >= 0x7EF0 && addr < 0x7F00) { if (traceOn && hooks) hooks->onMapperWrite(addr, v, pc, isPrgMapperReg(addr, v)); mapperWriteLow(addr, v); return; }
        if (mapper == 16) { if (traceOn && hooks) hooks->onMapperWrite(addr, v, pc, isPrgMapperReg(addr, v)); mapperWriteLow(addr, v); return; }
        wram[addr & 0x1FFF] = v; if (markOn) ramWr[addr] |= curMark; if (traceOn && hooks) hooks->onRamWrite(addr, v, pc); return;
    }
    if (addr < 0x4000) {
        if (traceOn && hooks) hooks->onPpuIo(addr);
        switch (addr & 7) {
        case 0: { bool was = ppuCtrl & 0x80; ppuCtrl = v; if (!was && (v & 0x80) && (ppuStatus & 0x80)) nmiDelayed = true; break; } // NMI edge: taken after the next instruction
        case 1: ppuMask = v; break;
        case 3: oamAddr = v; break;
        case 4: oam[oamAddr++] = v; break;
        case 5: break;
        case 6: if (!addrLatch) vramTemp = (vramTemp & 0x00FF) | ((v & 0x3F) << 8); else { vramTemp = (vramTemp & 0xFF00) | v; vramAddr = vramTemp; } addrLatch = !addrLatch; break;
        case 7: ppuWrite(vramAddr, v); vramAddr += (ppuCtrl & 4) ? 32 : 1; break;
        }
        return;
    }
    if (addr < 0x4018) {
        if (addr == 0x4014) { uint16_t base = v << 8; for (int i = 0; i < 256; i++) oam[(oamAddr + i) & 0xFF] = read(base + i); cycles += 513; if (traceOn && hooks) hooks->onPpuIo(addr); return; }
        if (addr == 0x4016) { padStrobe = v & 1; if (padStrobe) { padShift[0] = pad[0]; padShift[1] = pad[1]; } return; }
        apuWrite(addr, v); return;
    }
    // $4018-$5FFF
    if (nsfMode) {
        if (addr >= 0x5FF8) { nsfBank[addr & 7] = v; return; }
        if ((expansion & EXP_N163) && (addr & 0xF800) == 0x4800) {
            if (logSound) soundLog.push_back({ (uint32_t)(cycles - logBaseCycle), addr, v });
            n163Ram[m.n163Addr & 0x7F] = v; if (m.n163Addr & 0x80) m.n163Addr = 0x80 | ((m.n163Addr + 1) & 0x7F);
        }
        if ((expansion & EXP_MMC5) && isSoundReg(addr)) { if (logSound) soundLog.push_back({ (uint32_t)(cycles - logBaseCycle), addr, v }); }
        return;
    }
    if (isSoundReg(addr)) {
        if (logSound) soundLog.push_back({ (uint32_t)(cycles - logBaseCycle), addr, v });
        if (traceOn && hooks) hooks->onSoundWrite(addr, v);
    }
    mapperWriteLow(addr, v);
}

void NES::apuWrite(uint16_t addr, uint8_t v) {
    if (logSound && isSoundReg(addr)) soundLog.push_back({ (uint32_t)(cycles - logBaseCycle), addr, v });
    if (traceOn && hooks && isSoundReg(addr)) hooks->onSoundWrite(addr, v);
    apuRegs[addr & 0x1F] = v;
    if (addr == 0x4012) dmcAddr = 0xC000 | (v << 6);
    if (addr == 0x4013) dmcLen = (v << 4) | 1;
    if (addr == 0x4015) { if (!(v & 0x10)) irqLine &= ~4; }
    if (addr == 0x4017) { apuFrameMode5 = v & 0x80; apuFrameIrqInhibit = v & 0x40; if (apuFrameIrqInhibit) irqLine &= ~2; apuFrameCycle = 0; apuFrameStep = 0; }
}

void NES::apuFrameClock(int cyc) {
    apuFrameCycle += cyc;
    if (apuFrameCycle >= 7457) {
        apuFrameCycle -= 7457; apuFrameStep++;
        if (!apuFrameMode5 && apuFrameStep >= 4) { apuFrameStep = 0; if (!apuFrameIrqInhibit) irqLine |= 2; }
        if (apuFrameMode5 && apuFrameStep >= 5) apuFrameStep = 0;
    }
}

// ---------------------------------------------------------------- mappers
uint8_t NES::mapperReadLow(uint16_t addr, bool& handled) {
    handled = false;
    if ((mapper == 19 || (nsfMode && (expansion & EXP_N163))) && (addr & 0xF800) == 0x4800) {
        handled = true; uint8_t v = n163Ram[m.n163Addr & 0x7F];
        if (logSound) soundLog.push_back({ (uint32_t)(cycles - logBaseCycle), N163_READ_MARK, 0 });
        if (m.n163Addr & 0x80) m.n163Addr = 0x80 | ((m.n163Addr + 1) & 0x7F); return v;
    }
    if (mapper == 19 && (addr & 0xF800) == 0x5000) { handled = true; irqLine &= ~1; return m.irqCounter & 0xFF; }
    if (mapper == 19 && (addr & 0xF800) == 0x5800) { handled = true; irqLine &= ~1; return (m.irqCounter >> 8) & 0xFF; }
    return 0;
}

void NES::mapperWriteLow(uint16_t addr, uint8_t v) {
    switch (mapper) {
    case 19:
        if ((addr & 0xF800) == 0x4800) { n163Ram[m.n163Addr & 0x7F] = v; if (m.n163Addr & 0x80) m.n163Addr = 0x80 | ((m.n163Addr + 1) & 0x7F); }
        else if ((addr & 0xF800) == 0x5000) { m.irqCounter = (m.irqCounter & 0xFF00) | v; irqLine &= ~1; }
        else if ((addr & 0xF800) == 0x5800) { m.irqCounter = (m.irqCounter & 0x00FF) | (v << 8); irqLine &= ~1; }
        break;
    case 16:
        if (addr >= 0x6000 && addr < 0x8000) mapperWrite(0x8000 | (addr & 0x0F), v);
        break;
    case 28: if (addr >= 0x5000 && addr < 0x6000) m.cmd = v & 0x81; break;
    case 80:
        if (addr >= 0x7EF0 && addr < 0x7F00) {
            switch (addr) {
            case 0x7EF0: setChr1k(0, (v & 0xFE)); setChr1k(1, (v & 0xFE) + 1); break;
            case 0x7EF1: setChr1k(2, (v & 0xFE)); setChr1k(3, (v & 0xFE) + 1); break;
            case 0x7EF2: setChr1k(4, v); break; case 0x7EF3: setChr1k(5, v); break; case 0x7EF4: setChr1k(6, v); break; case 0x7EF5: setChr1k(7, v); break;
            case 0x7EF6: m.mirror = (v & 1) ? 1 : 0; break;
            case 0x7EFA: case 0x7EFB: setPrg8k(0, v); break;
            case 0x7EFC: case 0x7EFD: setPrg8k(1, v); break;
            case 0x7EFE: case 0x7EFF: setPrg8k(2, v); break;
            }
        }
        break;
    default: break;
    }
}

void NES::mapperWrite(uint16_t addr, uint8_t v) {
    int last16 = prgPages4k / 4 - 1;
    switch (mapper) {
    case 0: break;
    case 3: for (int i = 0; i < 8; i++) setChr1k(i, (v & 3) * 8 + i); break;
    case 1: {
        if (v & 0x80) { m.shift = 0; m.shiftCount = 0; m.regs[0] |= 0x0C; }
        else {
            m.shift |= (v & 1) << m.shiftCount; m.shiftCount++;
            if (m.shiftCount == 5) {
                int r = (addr >> 13) & 3; m.regs[r] = m.shift; m.shift = 0; m.shiftCount = 0;
            }
        }
        // recompute
        {
            uint8_t ctrl = m.regs[0]; int prgBank = m.regs[3] & 0x0F; int pages16 = prgPages4k / 4;
            int hi = 0; if (pages16 > 16) hi = (m.regs[1] & 0x10) ? 16 : 0; // SUROM 512K
            switch ((ctrl >> 2) & 3) {
            case 0: case 1: setPrg32k(((prgBank & 0x0E) | hi) / 2); break;
            case 2: setPrg16k(0, hi); setPrg16k(1, prgBank | hi); break;
            case 3: setPrg16k(0, prgBank | hi); setPrg16k(1, (pages16 > 16 ? 15 : last16) | hi); break;
            }
            if (ctrl & 0x10) { for (int i = 0; i < 4; i++) { setChr1k(i, (m.regs[1] & 0x1F) * 4 + i); setChr1k(4 + i, (m.regs[2] & 0x1F) * 4 + i); } }
            else for (int i = 0; i < 8; i++) setChr1k(i, (m.regs[1] & 0x1E) * 4 + i);
            m.mirror = (ctrl & 3) == 2 ? 1 : (ctrl & 3) == 3 ? 0 : 2 + (ctrl & 1);
        }
        break;
    }
    case 2: case 30: setPrg16k(0, v & 0x1F); if (mapper == 30 && chrIsRam) { /* CHR RAM bank: ignore */ } break;
    case 28: {
        if (addr >= 0x8000) {
            switch (m.cmd) { case 0x00: break; case 0x01: m.regs[1] = v & 0x0F; break; case 0x80: m.regs[2] = v; break; case 0x81: m.regs[3] = v & 0x3F; break; }
            int mode = m.regs[2], inner = m.regs[1], outer = m.regs[3]; int S = (mode >> 4) & 3, P = (mode >> 2) & 3; int mask = (1 << (S + 1)) - 1;
            for (int a14 = 0; a14 < 2; a14++) {
                int bank; if (P == 2 && a14 == 0) bank = 0; else if (P == 3 && a14 == 1) bank = mask; else if (P >= 2) bank = inner & mask; else bank = ((inner << 1) | a14) & mask;
                setPrg16k(a14, ((outer << 1) & ~mask) | bank);
            }
            m.mirror = (mode & 2) ? ((mode & 1) ? 0 : 1) : 2 + (mode & 1);
        }
        break;
    }
    case 7: setPrg32k(v & 7); m.mirror = (v & 0x10) ? 3 : 2; break;
    case 66: setPrg32k((v >> 4) & 3); for (int i = 0; i < 8; i++) setChr1k(i, (v & 3) * 8 + i); break;
    case 4: {
        switch (addr & 0xE001) {
        case 0x8000: m.cmd = v; break;
        case 0x8001: {
            m.regs[m.cmd & 7] = v;
            break;
        }
        case 0xA000: m.mirror = (v & 1) ? 0 : 1; break;
        case 0xA001: break;
        case 0xC000: m.irqLatch = v; break;
        case 0xC001: m.irqCounter = 0; m.irqReload = true; break;
        case 0xE000: m.irqEnable = false; irqLine &= ~1; break;
        case 0xE001: m.irqEnable = true; break;
        }
        bool prgSwap = m.cmd & 0x40, chrInv = m.cmd & 0x80;
        if (prgSwap) { setPrg8k(0, -2); setPrg8k(2, m.regs[6]); } else { setPrg8k(0, m.regs[6]); setPrg8k(2, -2); }
        setPrg8k(1, m.regs[7]); setPrg8k(3, -1);
        int inv = chrInv ? 4 : 0;
        setChr1k(0 ^ inv, m.regs[0] & 0xFE); setChr1k(1 ^ inv, (m.regs[0] & 0xFE) + 1); setChr1k(2 ^ inv, m.regs[1] & 0xFE); setChr1k(3 ^ inv, (m.regs[1] & 0xFE) + 1);
        setChr1k(4 ^ inv, m.regs[2]); setChr1k(5 ^ inv, m.regs[3]); setChr1k(6 ^ inv, m.regs[4]); setChr1k(7 ^ inv, m.regs[5]);
        break;
    }
    case 16: {
        switch (addr & 0x0F) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: setChr1k(addr & 7, v); break;
        case 8: setPrg16k(0, v & 0x0F); break;
        case 9: m.mirror = v & 3; break;
        case 0xA: m.irqEnable = v & 1; irqLine &= ~1; m.irqCounter = m.irqLatch; break;
        case 0xB: m.irqLatch = (m.irqLatch & 0xFF00) | v; break;
        case 0xC: m.irqLatch = (m.irqLatch & 0x00FF) | (v << 8); break;
        }
        break;
    }
    case 19: {
        switch (addr & 0xF800) {
        case 0x8000: case 0x8800: case 0x9000: case 0x9800: case 0xA000: case 0xA800: case 0xB000: case 0xB800:
            setChr1k((addr - 0x8000) >> 11, v); break;
        case 0xE000: setPrg8k(0, v & 0x3F); break;
        case 0xE800: setPrg8k(1, v & 0x3F); break;
        case 0xF000: setPrg8k(2, v & 0x3F); break;
        case 0xF800: m.n163Addr = v; break;
        }
        break;
    }
    case 22: {
        uint16_t a = addr & 0xF003; // VRC2a: A0/A1 swapped
        int lo = ((a & 2) >> 1) | ((a & 1) << 1);
        switch (addr & 0xF000) {
        case 0x8000: setPrg8k(0, v & 0x0F); break;
        case 0xA000: setPrg8k(1, v & 0x0F); break;
        case 0x9000: m.mirror = (v & 1) ? 0 : 1; break;
        default: {
            if (addr >= 0xB000 && addr < 0xF000) {
                int reg = ((addr >> 12) - 0xB) * 2 + (lo >> 1); int nib = lo & 1;
                if (nib) m.regs[8 + reg] = (m.regs[8 + reg] & 0x0F) | ((v & 0x0F) << 4); else m.regs[8 + reg] = (m.regs[8 + reg] & 0xF0) | (v & 0x0F);
                setChr1k(reg, m.regs[8 + reg] >> 1);
            }
        }
        }
        break;
    }
    case 23: case 25: {
        uint16_t a = addr; int lo;
        if (mapper == 23) lo = (a & 3) | ((a >> 2) & 3); else lo = (((a & 1) << 1) | ((a & 2) >> 1)) | (((a >> 2) & 1) << 1) | (((a >> 3) & 1));
        lo &= 3;
        switch (addr & 0xF000) {
        case 0x8000: m.regs[0] = v & 0x1F; break;
        case 0xA000: m.regs[1] = v & 0x1F; break;
        case 0x9000: if (lo & 2) m.regs[2] = v & 2; else m.mirror = (v & 1) ? 0 : 1; break;
        case 0xF000: if (lo == 0) m.irqLatch = (m.irqLatch & 0xF0) | (v & 0x0F); else if (lo == 1) m.irqLatch = (m.irqLatch & 0x0F) | ((v & 0x0F) << 4);
                   else if (lo == 2) { m.regs[3] = v; m.irqEnable = v & 2; irqLine &= ~1; if (v & 2) { m.irqCounter = m.irqLatch; m.irqPrescaler = 341; } }
                   else { m.irqEnable = m.regs[3] & 1; irqLine &= ~1; }
                   break;
        default:
            if (addr >= 0xB000 && addr < 0xF000) {
                int reg = ((addr >> 12) - 0xB) * 2 + (lo >> 1); int nib = lo & 1;
                if (nib) m.regs[8 + reg] = (m.regs[8 + reg] & 0x0F) | ((v & 0x1F) << 4); else m.regs[8 + reg] = (m.regs[8 + reg] & 0x1F0) | (v & 0x0F);
                setChr1k(reg, m.regs[8 + reg]);
            }
        }
        if (m.regs[2] & 2) { setPrg8k(0, -2); setPrg8k(2, m.regs[0]); } else { setPrg8k(0, m.regs[0]); setPrg8k(2, -2); }
        setPrg8k(1, m.regs[1]); setPrg8k(3, -1);
        break;
    }
    case 33: {
        switch (addr & 0xE003) {
        case 0x8000: setPrg8k(0, v & 0x3F); m.mirror = (v & 0x40) ? 1 : 0; break;
        case 0x8001: setPrg8k(1, v & 0x3F); break;
        case 0x8002: setChr1k(0, v * 2); setChr1k(1, v * 2 + 1); break;
        case 0x8003: setChr1k(2, v * 2); setChr1k(3, v * 2 + 1); break;
        case 0xA000: setChr1k(4, v); break; case 0xA001: setChr1k(5, v); break; case 0xA002: setChr1k(6, v); break; case 0xA003: setChr1k(7, v); break;
        }
        break;
    }
    case 67: {
        switch (addr & 0xF800) {
        case 0x8800: setChr1k(0, v * 2); setChr1k(1, v * 2 + 1); break;
        case 0x9800: setChr1k(2, v * 2); setChr1k(3, v * 2 + 1); break;
        case 0xA800: setChr1k(4, v * 2); setChr1k(5, v * 2 + 1); break;
        case 0xB800: setChr1k(6, v * 2); setChr1k(7, v * 2 + 1); break;
        case 0xC800: if (!m.irqReload) m.irqCounter = (m.irqCounter & 0x00FF) | (v << 8); else m.irqCounter = (m.irqCounter & 0xFF00) | v; m.irqReload = !m.irqReload; break;
        case 0xD800: m.irqEnable = v & 0x10; m.irqReload = false; irqLine &= ~1; break;
        case 0xE800: m.mirror = v & 3; break;
        case 0xF800: setPrg16k(0, v & 0x0F); break;
        }
        break;
    }
    case 69: {
        if (addr < 0xA000) m.cmd = v & 0x0F;
        else if (addr < 0xC000) {
            int c = m.cmd;
            if (c < 8) setChr1k(c, v);
            else if (c == 8) { m.regs[8] = v; m.wramBank = v & 0x3F; }
            else if (c <= 0xB) setPrg8k(c - 9, v & 0x3F);
            else if (c == 0xC) m.mirror = v & 3;
            else if (c == 0xD) { m.irqEnable = v & 1; m.irqEnableAfter = v & 0x80; irqLine &= ~1; }
            else if (c == 0xE) m.irqCounter = (m.irqCounter & 0xFF00) | v;
            else m.irqCounter = (m.irqCounter & 0x00FF) | (v << 8);
        }
        else if (addr < 0xE000) m.s5bAddr = v & 0x0F;
        break;
    }
    case 75: {
        switch (addr & 0xF000) {
        case 0x8000: setPrg8k(0, v & 0x0F); break;
        case 0xA000: setPrg8k(1, v & 0x0F); break;
        case 0xC000: setPrg8k(2, v & 0x0F); break;
        case 0x9000: m.mirror = (v & 1) ? 0 : 1; m.regs[0] = v; for (int i = 0; i < 4; i++) { setChr1k(i, ((m.regs[1] & 0x0F) | ((v & 2) << 3)) * 4 + i); setChr1k(4 + i, ((m.regs[2] & 0x0F) | ((v & 4) << 2)) * 4 + i); } break;
        case 0xE000: m.regs[1] = v; for (int i = 0; i < 4; i++) setChr1k(i, ((v & 0x0F) | ((m.regs[0] & 2) << 3)) * 4 + i); break;
        case 0xF000: m.regs[2] = v; for (int i = 0; i < 4; i++) setChr1k(4 + i, ((v & 0x0F) | ((m.regs[0] & 4) << 2)) * 4 + i); break;
        }
        break;
    }
    case 85: {
        uint16_t a = (addr & 0xF000) | ((addr & 0x18) ? 0x10 : 0);
        switch (a) {
        case 0x8000: setPrg8k(0, v & 0x3F); break;
        case 0x8010: setPrg8k(1, v & 0x3F); break;
        case 0x9000: setPrg8k(2, v & 0x3F); break;
        case 0x9010: m.vrc7Addr = v; break;
        case 0x9030: break; // audio data
        case 0xA000: setChr1k(0, v); break; case 0xA010: setChr1k(1, v); break; case 0xB000: setChr1k(2, v); break; case 0xB010: setChr1k(3, v); break;
        case 0xC000: setChr1k(4, v); break; case 0xC010: setChr1k(5, v); break; case 0xD000: setChr1k(6, v); break; case 0xD010: setChr1k(7, v); break;
        case 0xE000: m.mirror = (v & 1) ? 0 : 1; break;
        case 0xE010: m.irqLatch = v; break;
        case 0xF000: m.regs[3] = v; m.irqEnable = v & 2; irqLine &= ~1; if (v & 2) { m.irqCounter = m.irqLatch; m.irqPrescaler = 341; } break;
        case 0xF010: m.irqEnable = m.regs[3] & 1; irqLine &= ~1; break;
        }
        break;
    }
    default: setPrg32k(v); break;
    }
}

void NES::mapperScanline() {
    if (mapper == 4) {
        if (m.irqCounter == 0 || m.irqReload) { m.irqCounter = (uint16_t)m.irqLatch; m.irqReload = false; }
        else m.irqCounter--;
        if (m.irqCounter == 0 && m.irqEnable) irqLine |= 1;
    }
}

void NES::mapperClock(int cyc) {
    switch (mapper) {
    case 16:
        if (m.irqEnable) { int c = m.irqCounter - cyc; if (c <= 0) { irqLine |= 1; m.irqCounter = (uint16_t)(c & 0xFFFF); } else m.irqCounter = (uint16_t)c; }
        break;
    case 19:
        if (m.irqCounter & 0x8000) { int c = (m.irqCounter & 0x7FFF) + cyc; if (c >= 0x7FFF) { irqLine |= 1; c = 0x7FFF; } m.irqCounter = 0x8000 | c; }
        break;
    case 23: case 25: case 85:
        if (m.irqEnable) {
            auto tick = [&]() { if (m.irqCounter == 0xFF) { m.irqCounter = (uint16_t)m.irqLatch; irqLine |= 1; } else m.irqCounter++; };
            if (m.regs[3] & 4) { for (int i = 0; i < cyc; i++) tick(); }
            else { m.irqPrescaler -= cyc * 3; while (m.irqPrescaler <= 0) { m.irqPrescaler += 341; tick(); } }
        }
        break;
    case 67:
        if (m.irqEnable) { int c = m.irqCounter - cyc; if (c < 0) { irqLine |= 1; m.irqEnable = false; m.irqCounter = 0xFFFF; } else m.irqCounter = (uint16_t)c; }
        break;
    case 69:
        if (m.irqEnableAfter) { int c = m.irqCounter - cyc; if (c < 0 && m.irqEnable) irqLine |= 1; m.irqCounter = (uint16_t)(c & 0xFFFF); }
        break;
    default: break;
    }
}

// ---------------------------------------------------------------- snapshot
void NES::saveSnapshot(Snapshot& s) const {
    memcpy(s.ram, ram, sizeof ram); memcpy(s.wram, wram, sizeof wram); memcpy(s.n163Ram, n163Ram, sizeof n163Ram);
    s.pc = pc; s.a = a; s.x = x; s.y = y; s.sp = sp; s.p = p; s.m = m; memcpy(s.nsfBank, nsfBank, 8);
    memcpy(s.apuRegs, apuRegs, sizeof apuRegs); s.dmcAddr = dmcAddr; s.dmcLen = dmcLen; s.ppuCtrl = ppuCtrl; s.ppuMask = ppuMask;
}
void NES::loadSnapshot(const Snapshot& s) {
    memcpy(ram, s.ram, sizeof ram); memcpy(wram, s.wram, sizeof wram); memcpy(n163Ram, s.n163Ram, sizeof n163Ram);
    pc = s.pc; a = s.a; x = s.x; y = s.y; sp = s.sp; p = s.p; m = s.m; memcpy(nsfBank, s.nsfBank, 8);
    memcpy(apuRegs, s.apuRegs, sizeof apuRegs); dmcAddr = s.dmcAddr; dmcLen = s.dmcLen; ppuCtrl = s.ppuCtrl; ppuMask = s.ppuMask;
    irqLine = 0; nmiPending = false; jammed = false;
}

// ---------------------------------------------------------------- CPU
void NES::nmi() {
    if (traceOn && hooks) hooks->onInterrupt(1, 0xFFFA, sp);
    push(pc >> 8); push(pc & 0xFF); push((p & ~0x10) | 0x20); p |= 0x04;
    pc = read(0xFFFA) | (read(0xFFFB) << 8); cycles += 7;
}
void NES::irq() {
    if (traceOn && hooks) hooks->onInterrupt(2, 0xFFFE, sp);
    push(pc >> 8); push(pc & 0xFF); push((p & ~0x10) | 0x20); p |= 0x04;
    pc = read(0xFFFE) | (read(0xFFFF) << 8); cycles += 7;
}

void NES::branch(bool cond, int& cyc) {
    int8_t off = (int8_t)read(pc++);
    if (cond) { uint16_t np = pc + off; cyc += ((np & 0xFF00) != (pc & 0xFF00)) ? 2 : 1; pc = np; }
}
void NES::adc(uint8_t v) {
    unsigned s = a + v + (p & 1);
    p = (p & ~0x41) | (s > 0xFF ? 1 : 0) | ((~(a ^ v) & (a ^ s) & 0x80) ? 0x40 : 0);
    a = (uint8_t)s; setNZ(a);
}
void NES::sbc(uint8_t v) { adc(~v); }
void NES::cmp(uint8_t r, uint8_t v) { unsigned d = r - v; p = (p & ~1) | (r >= v ? 1 : 0); setNZ((uint8_t)d); }

#define ZP   (fetch(pc++))
#define ZPX  ((uint8_t)(fetch(pc++) + x))
#define ZPY  ((uint8_t)(fetch(pc++) + y))
#define ABS  (tmp = fetch(pc) | (fetch(pc + 1) << 8), pc += 2, tmp)
#define ABSX (tmp = fetch(pc) | (fetch(pc + 1) << 8), pc += 2, cyc += (((tmp + x) ^ tmp) & 0x100) ? 1 : 0, (uint16_t)(tmp + x))
#define ABSY (tmp = fetch(pc) | (fetch(pc + 1) << 8), pc += 2, cyc += (((tmp + y) ^ tmp) & 0x100) ? 1 : 0, (uint16_t)(tmp + y))
#define ABSX_W (tmp = fetch(pc) | (fetch(pc + 1) << 8), pc += 2, (uint16_t)(tmp + x))
#define ABSY_W (tmp = fetch(pc) | (fetch(pc + 1) << 8), pc += 2, (uint16_t)(tmp + y))
#define ZPR(a) (markOn ? (ramRd[(uint8_t)(a)] |= curMark, ram[(uint8_t)(a)]) : ram[(uint8_t)(a)])
#define INDX (z = (uint8_t)(fetch(pc++) + x), (uint16_t)(ZPR(z) | (ZPR(z + 1) << 8)))
#define INDY (z = fetch(pc++), tmp = ZPR(z) | (ZPR(z + 1) << 8), cyc += (((tmp + y) ^ tmp) & 0x100) ? 1 : 0, (uint16_t)(tmp + y))
#define INDY_W (z = fetch(pc++), tmp = ZPR(z) | (ZPR(z + 1) << 8), (uint16_t)(tmp + y))

int NES::step() {
    if (jammed) { cycles += 2; return 2; }
    if (nmiPending) { nmiPending = false; nmiDelayed = false; nmiDelayCount = 0; nmi(); return 7; }
    if (nmiDelayed) { nmiDelayed = false; nmiPending = true; }
    // vblank NMI: the two instructions in flight complete first, so a main loop polling $2002 sees the flag before the
    // NMI handler's own $2002 read clears it (on hardware this is a cycle-level race the game was tuned to win)
    if (nmiDelayCount > 0 && --nmiDelayCount == 0) nmiPending = true;
    if (irqLine && !(p & 4) && !harnessMode) { irq(); return 7; }
    uint8_t op = fetch(pc++);
    int cyc = cycTable[op];
    uint16_t tmp, ea; uint8_t z, v;
    switch (op) {
    // --- loads/stores
    case 0xA9: a = fetch(pc++); setNZ(a); break;
    case 0xA5: a = read(ZP); setNZ(a); break;
    case 0xB5: a = read(ZPX); setNZ(a); break;
    case 0xAD: a = read(ABS); setNZ(a); break;
    case 0xBD: a = read(ABSX); setNZ(a); break;
    case 0xB9: a = read(ABSY); setNZ(a); break;
    case 0xA1: a = read(INDX); setNZ(a); break;
    case 0xB1: a = read(INDY); setNZ(a); break;
    case 0xA2: x = fetch(pc++); setNZ(x); break;
    case 0xA6: x = read(ZP); setNZ(x); break;
    case 0xB6: x = read(ZPY); setNZ(x); break;
    case 0xAE: x = read(ABS); setNZ(x); break;
    case 0xBE: x = read(ABSY); setNZ(x); break;
    case 0xA0: y = fetch(pc++); setNZ(y); break;
    case 0xA4: y = read(ZP); setNZ(y); break;
    case 0xB4: y = read(ZPX); setNZ(y); break;
    case 0xAC: y = read(ABS); setNZ(y); break;
    case 0xBC: y = read(ABSX); setNZ(y); break;
    case 0x85: write(ZP, a); break;
    case 0x95: write(ZPX, a); break;
    case 0x8D: write(ABS, a); break;
    case 0x9D: write(ABSX_W, a); break;
    case 0x99: write(ABSY_W, a); break;
    case 0x81: write(INDX, a); break;
    case 0x91: write(INDY_W, a); break;
    case 0x86: write(ZP, x); break;
    case 0x96: write(ZPY, x); break;
    case 0x8E: write(ABS, x); break;
    case 0x84: write(ZP, y); break;
    case 0x94: write(ZPX, y); break;
    case 0x8C: write(ABS, y); break;
    // --- transfers
    case 0xAA: x = a; setNZ(x); break; case 0xA8: y = a; setNZ(y); break; case 0x8A: a = x; setNZ(a); break; case 0x98: a = y; setNZ(a); break;
    case 0xBA: x = sp; setNZ(x); break; case 0x9A: sp = x; break;
    // --- stack
    case 0x48: push(a); break; case 0x68: a = pull(); setNZ(a); break;
    case 0x08: push(p | 0x30); break; case 0x28: p = (pull() | 0x20) & ~0x10; break;
    // --- arithmetic
    case 0x69: adc(fetch(pc++)); break; case 0x65: adc(read(ZP)); break; case 0x75: adc(read(ZPX)); break; case 0x6D: adc(read(ABS)); break;
    case 0x7D: adc(read(ABSX)); break; case 0x79: adc(read(ABSY)); break; case 0x61: adc(read(INDX)); break; case 0x71: adc(read(INDY)); break;
    case 0xE9: case 0xEB: sbc(fetch(pc++)); break; case 0xE5: sbc(read(ZP)); break; case 0xF5: sbc(read(ZPX)); break; case 0xED: sbc(read(ABS)); break;
    case 0xFD: sbc(read(ABSX)); break; case 0xF9: sbc(read(ABSY)); break; case 0xE1: sbc(read(INDX)); break; case 0xF1: sbc(read(INDY)); break;
    case 0xC9: cmp(a, fetch(pc++)); break; case 0xC5: cmp(a, read(ZP)); break; case 0xD5: cmp(a, read(ZPX)); break; case 0xCD: cmp(a, read(ABS)); break;
    case 0xDD: cmp(a, read(ABSX)); break; case 0xD9: cmp(a, read(ABSY)); break; case 0xC1: cmp(a, read(INDX)); break; case 0xD1: cmp(a, read(INDY)); break;
    case 0xE0: cmp(x, fetch(pc++)); break; case 0xE4: cmp(x, read(ZP)); break; case 0xEC: cmp(x, read(ABS)); break;
    case 0xC0: cmp(y, fetch(pc++)); break; case 0xC4: cmp(y, read(ZP)); break; case 0xCC: cmp(y, read(ABS)); break;
    // --- logic
    case 0x29: a &= fetch(pc++); setNZ(a); break; case 0x25: a &= read(ZP); setNZ(a); break; case 0x35: a &= read(ZPX); setNZ(a); break; case 0x2D: a &= read(ABS); setNZ(a); break;
    case 0x3D: a &= read(ABSX); setNZ(a); break; case 0x39: a &= read(ABSY); setNZ(a); break; case 0x21: a &= read(INDX); setNZ(a); break; case 0x31: a &= read(INDY); setNZ(a); break;
    case 0x09: a |= fetch(pc++); setNZ(a); break; case 0x05: a |= read(ZP); setNZ(a); break; case 0x15: a |= read(ZPX); setNZ(a); break; case 0x0D: a |= read(ABS); setNZ(a); break;
    case 0x1D: a |= read(ABSX); setNZ(a); break; case 0x19: a |= read(ABSY); setNZ(a); break; case 0x01: a |= read(INDX); setNZ(a); break; case 0x11: a |= read(INDY); setNZ(a); break;
    case 0x49: a ^= fetch(pc++); setNZ(a); break; case 0x45: a ^= read(ZP); setNZ(a); break; case 0x55: a ^= read(ZPX); setNZ(a); break; case 0x4D: a ^= read(ABS); setNZ(a); break;
    case 0x5D: a ^= read(ABSX); setNZ(a); break; case 0x59: a ^= read(ABSY); setNZ(a); break; case 0x41: a ^= read(INDX); setNZ(a); break; case 0x51: a ^= read(INDY); setNZ(a); break;
    case 0x24: v = read(ZP); p = (p & ~0xC2) | (v & 0xC0) | ((a & v) == 0 ? 2 : 0); break;
    case 0x2C: v = read(ABS); p = (p & ~0xC2) | (v & 0xC0) | ((a & v) == 0 ? 2 : 0); break;
    // --- shifts (RMW)
#define RMW(addrExpr, OPER) { ea = addrExpr; v = read(ea); OPER; write(ea, v); }
#define ASL_ p = (p & ~1) | (v >> 7); v <<= 1; setNZ(v)
#define LSR_ p = (p & ~1) | (v & 1); v >>= 1; setNZ(v)
#define ROL_ { uint8_t c = p & 1; p = (p & ~1) | (v >> 7); v = (v << 1) | c; setNZ(v); }
#define ROR_ { uint8_t c = p & 1; p = (p & ~1) | (v & 1); v = (v >> 1) | (c << 7); setNZ(v); }
    case 0x0A: v = a; ASL_; a = v; break; case 0x06: RMW(ZP, ASL_); break; case 0x16: RMW(ZPX, ASL_); break; case 0x0E: RMW(ABS, ASL_); break; case 0x1E: RMW(ABSX_W, ASL_); break;
    case 0x4A: v = a; LSR_; a = v; break; case 0x46: RMW(ZP, LSR_); break; case 0x56: RMW(ZPX, LSR_); break; case 0x4E: RMW(ABS, LSR_); break; case 0x5E: RMW(ABSX_W, LSR_); break;
    case 0x2A: v = a; ROL_; a = v; break; case 0x26: RMW(ZP, ROL_); break; case 0x36: RMW(ZPX, ROL_); break; case 0x2E: RMW(ABS, ROL_); break; case 0x3E: RMW(ABSX_W, ROL_); break;
    case 0x6A: v = a; ROR_; a = v; break; case 0x66: RMW(ZP, ROR_); break; case 0x76: RMW(ZPX, ROR_); break; case 0x6E: RMW(ABS, ROR_); break; case 0x7E: RMW(ABSX_W, ROR_); break;
    case 0xE6: RMW(ZP, v++; setNZ(v)); break; case 0xF6: RMW(ZPX, v++; setNZ(v)); break; case 0xEE: RMW(ABS, v++; setNZ(v)); break; case 0xFE: RMW(ABSX_W, v++; setNZ(v)); break;
    case 0xC6: RMW(ZP, v--; setNZ(v)); break; case 0xD6: RMW(ZPX, v--; setNZ(v)); break; case 0xCE: RMW(ABS, v--; setNZ(v)); break; case 0xDE: RMW(ABSX_W, v--; setNZ(v)); break;
    case 0xE8: x++; setNZ(x); break; case 0xC8: y++; setNZ(y); break; case 0xCA: x--; setNZ(x); break; case 0x88: y--; setNZ(y); break;
    // --- flags
    case 0x18: p &= ~1; break; case 0x38: p |= 1; break; case 0x58: p &= ~4; break; case 0x78: p |= 4; break;
    case 0xB8: p &= ~0x40; break; case 0xD8: p &= ~8; break; case 0xF8: p |= 8; break;
    // --- branches
    case 0x10: branch(!(p & 0x80), cyc); break; case 0x30: branch(p & 0x80, cyc); break;
    case 0x50: branch(!(p & 0x40), cyc); break; case 0x70: branch(p & 0x40, cyc); break;
    case 0x90: branch(!(p & 1), cyc); break; case 0xB0: branch(p & 1, cyc); break;
    case 0xD0: branch(!(p & 2), cyc); break; case 0xF0: branch(p & 2, cyc); break;
    // --- jumps
    case 0x4C: pc = ABS; break;
    case 0x6C: { ea = ABS; uint16_t hi = (ea & 0xFF00) | ((ea + 1) & 0xFF); pc = read(ea) | (read(hi) << 8); break; }
    case 0x20: { ea = ABS; uint16_t ret = pc - 1; if (traceOn && hooks) hooks->onJsr(ea, pc - 3, sp); push(ret >> 8); push(ret & 0xFF); pc = ea; break; }
    case 0x60: { if (traceOn && hooks) hooks->onRts(sp); uint8_t lo = pull(); uint8_t hi = pull(); pc = ((hi << 8) | lo) + 1; if (pc == sentinel) sentinelHit = true; break; }
    case 0x40: { if (traceOn && hooks) hooks->onRti(sp); p = (pull() | 0x20) & ~0x10; uint8_t lo = pull(); uint8_t hi = pull(); pc = (hi << 8) | lo; if (pc == sentinel) sentinelHit = true; break; }
    case 0x00: { pc++; push(pc >> 8); push(pc & 0xFF); push(p | 0x30); p |= 4; pc = read(0xFFFE) | (read(0xFFFF) << 8); break; }
    case 0xEA: break;
    // --- illegal/unofficial (common ones)
    case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: break;
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: pc++; break;
    case 0x04: case 0x44: case 0x64: pc++; break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: pc++; break;
    case 0x0C: pc += 2; break;
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: pc += 2; break;
    case 0xA7: a = x = read(ZP); setNZ(a); break; case 0xB7: a = x = read(ZPY); setNZ(a); break; case 0xAF: a = x = read(ABS); setNZ(a); break;
    case 0xBF: a = x = read(ABSY); setNZ(a); break; case 0xA3: a = x = read(INDX); setNZ(a); break; case 0xB3: a = x = read(INDY); setNZ(a); break;
    case 0x87: write(ZP, a & x); break; case 0x97: write(ZPY, a & x); break; case 0x8F: write(ABS, a & x); break; case 0x83: write(INDX, a & x); break;
    case 0x0B: case 0x2B: a &= fetch(pc++); setNZ(a); p = (p & ~1) | (a >> 7); break;
    case 0x4B: a &= fetch(pc++); p = (p & ~1) | (a & 1); a >>= 1; setNZ(a); break;
    case 0xCB: v = fetch(pc++); x = (a & x) - v; p = (p & ~1) | ((a & x) >= v ? 1 : 0); setNZ(x); break;
    case 0xC7: RMW(ZP, v--; cmp(a, v)); break; case 0xD7: RMW(ZPX, v--; cmp(a, v)); break; case 0xCF: RMW(ABS, v--; cmp(a, v)); break;
    case 0xDF: RMW(ABSX_W, v--; cmp(a, v)); break; case 0xDB: RMW(ABSY_W, v--; cmp(a, v)); break; case 0xC3: RMW(INDX, v--; cmp(a, v)); break; case 0xD3: RMW(INDY_W, v--; cmp(a, v)); break;
    case 0xE7: RMW(ZP, v++; sbc(v)); break; case 0xF7: RMW(ZPX, v++; sbc(v)); break; case 0xEF: RMW(ABS, v++; sbc(v)); break;
    case 0xFF: RMW(ABSX_W, v++; sbc(v)); break; case 0xFB: RMW(ABSY_W, v++; sbc(v)); break; case 0xE3: RMW(INDX, v++; sbc(v)); break; case 0xF3: RMW(INDY_W, v++; sbc(v)); break;
    case 0x07: RMW(ZP, ASL_; a |= v; setNZ(a)); break; case 0x17: RMW(ZPX, ASL_; a |= v; setNZ(a)); break; case 0x0F: RMW(ABS, ASL_; a |= v; setNZ(a)); break;
    case 0x1F: RMW(ABSX_W, ASL_; a |= v; setNZ(a)); break; case 0x1B: RMW(ABSY_W, ASL_; a |= v; setNZ(a)); break; case 0x03: RMW(INDX, ASL_; a |= v; setNZ(a)); break; case 0x13: RMW(INDY_W, ASL_; a |= v; setNZ(a)); break;
    case 0x27: RMW(ZP, ROL_; a &= v; setNZ(a)); break; case 0x37: RMW(ZPX, ROL_; a &= v; setNZ(a)); break; case 0x2F: RMW(ABS, ROL_; a &= v; setNZ(a)); break;
    case 0x3F: RMW(ABSX_W, ROL_; a &= v; setNZ(a)); break; case 0x3B: RMW(ABSY_W, ROL_; a &= v; setNZ(a)); break; case 0x23: RMW(INDX, ROL_; a &= v; setNZ(a)); break; case 0x33: RMW(INDY_W, ROL_; a &= v; setNZ(a)); break;
    case 0x47: RMW(ZP, LSR_; a ^= v; setNZ(a)); break; case 0x57: RMW(ZPX, LSR_; a ^= v; setNZ(a)); break; case 0x4F: RMW(ABS, LSR_; a ^= v; setNZ(a)); break;
    case 0x5F: RMW(ABSX_W, LSR_; a ^= v; setNZ(a)); break; case 0x5B: RMW(ABSY_W, LSR_; a ^= v; setNZ(a)); break; case 0x43: RMW(INDX, LSR_; a ^= v; setNZ(a)); break; case 0x53: RMW(INDY_W, LSR_; a ^= v; setNZ(a)); break;
    case 0x67: RMW(ZP, ROR_; adc(v)); break; case 0x77: RMW(ZPX, ROR_; adc(v)); break; case 0x6F: RMW(ABS, ROR_; adc(v)); break;
    case 0x7F: RMW(ABSX_W, ROR_; adc(v)); break; case 0x7B: RMW(ABSY_W, ROR_; adc(v)); break; case 0x63: RMW(INDX, ROR_; adc(v)); break; case 0x73: RMW(INDY_W, ROR_; adc(v)); break;
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52: case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
        jammed = true; illegalOps++; break;
    default: illegalOps++; break;
    }
    cycles += cyc;
    return cyc;
}

void NES::runCycles(int64_t n) {
    uint64_t end = cycles + n;
    while (cycles < end) {
        int c = step();
        apuFrameClock(c);
        if (mapper != 4 && mapper != 0 && mapper != 0x100) mapperClock(c);
        if (jammed) { cycles = end; break; }
    }
}

void NES::runFrame() {
    // 262 scanlines * 113.667 cycles. vblank begins at scanline 241.
    static const int lineCycles[3] = { 114, 114, 113 };
    bool rendering = (ppuMask & 0x18) != 0;
    bool s0set = false;
    // the cycle budget is tracked against the frame start (an instruction that overshoots a scanline shortens the next
    // one): the frame stays 29780.67 CPU cycles long instead of accumulating ~1 cycle per scanline
    uint64_t frameStart = cycles, lineEnd = cycles;
    for (int sl = 0; sl < 262; sl++) {
        scanline = sl;
        if (sl == 241) { ppuStatus |= 0x80; inVblank = true; if (ppuCtrl & 0x80) nmiDelayCount = 2; }   // the instruction in flight completes first (a main loop polling $2002 may see the flag before the NMI handler clears it, as on hardware)
        if (sl == 261) { ppuStatus &= ~0xE0; inVblank = false; s0set = false; }
        rendering = (ppuMask & 0x18) != 0;
        if (sl < 240 && rendering && !s0set && oam[0] < 240 && sl >= oam[0] + 1) { ppuStatus |= 0x40; s0set = true; }
        if ((sl < 240 || sl == 261) && rendering) mapperScanline();
        lineEnd += lineCycles[sl % 3] + (sl == 0 && (frameCount & 1) ? 1 : 0);
        if (cycles < lineEnd) runCycles(lineEnd - cycles);
    }
    (void)frameStart;
    frameCount++;
}

bool NES::callSubroutine(uint16_t addr, uint8_t ra, uint8_t rx, uint8_t ry, int64_t maxCycles, bool asInterrupt, uint8_t sp0) {
    // push sentinel return address, run until RTS/RTI to sentinel
    sp = sp0; // stack top used by the player / the game
    if (asInterrupt) { push(sentinel >> 8); push(sentinel & 0xFF); push(0x24); }
    else { uint16_t ret = sentinel - 1; push(ret >> 8); push(ret & 0xFF); }
    pc = addr; a = ra; x = rx; y = ry; p = 0x24; // I set, no interrupts
    sentinelHit = false; jammed = false; nmiPending = false; harnessMode = true;
    uint64_t end = cycles + maxCycles;
    while (!sentinelHit && cycles < end && !jammed) {
        int c = step();
        apuFrameClock(c);
        if (mapper != 4 && mapper != 0 && mapper != 0x100) mapperClock(c);
        if (pc == sentinel) { sentinelHit = true; break; }
    }
    harnessMode = false;
    return sentinelHit;
}
