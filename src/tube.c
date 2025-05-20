/*B-em v2.2 by Tom Walker
  Tube ULA emulation*/

#include <stdio.h>
#include "b-em.h"
#include "6502.h"
#include "model.h"
#include "tube.h"

#include "NS32016/32016.h"
#include "NS32016/mem32016.h"
#include "6502tube.h"
#include "65816.h"
#include "6809tube.h"
#include "arm.h"
#include "x86_tube.h"
#include "z80.h"
#include "pdp11/pdp11.h"
#include "musahi/m68k.h"
#include "sprow.h"

double tube_multiplier = 1.0;
int tube_speed_num = 0;
int tubecycles = 0;

uint8_t (*tube_readmem)(uint32_t addr);
void (*tube_writemem)(uint32_t addr, uint8_t byte);
void (*tube_exec)(void);
void (*tube_proc_savestate)(ZFILE *zfp);
void (*tube_proc_loadstate)(ZFILE *zfp);

/*
 * The number of tube cycles to run for each core 6502 processor cycle
 * is calculated by mutliplying the multiplier in this table with the
 * one in the processor-specific tube entry and dividing by four.
 */

tube_speed_t tube_speeds[NUM_TUBE_SPEEDS] =
{
    { "100%",   1 },
    { "200%",   2 },
    { "400%",   4 },
    { "800%",   8 },
    { "1600%", 16 },
    { "3200%", 32 },
    { "6400%", 64 }
};

enum tube_flow {
    TUBE_SPACE_AVAIL = 0x40,
    TUBE_DATA_AVAIL  = 0x80,
    TUBE_BOTH_AVAIL  = 0xc0
};

int tube_irq=0;
tubetype tube_type=TUBEX86;
tube_ula tubeula;

static void tube_reset_most(void)
{
    tubeula.ph1count = tubeula.ph1head = tubeula.ph1tail = 0;
    tubeula.ph3pos = 1;
    tubeula.hstat[0] = tubeula.hstat[1] = tubeula.hstat[3] = TUBE_SPACE_AVAIL;
    tubeula.pstat[0] = TUBE_SPACE_AVAIL;
    tubeula.pstat[1] = tubeula.pstat[3] = 0x7f;
    tubeula.pstat[2] = 0x3f;
    tubeula.hstat[2] = TUBE_BOTH_AVAIL;
}

void tube_reset(void)
{
    tube_reset_most();
    tubeula.r1stat = 0;
}

void tube_updateints()
{
    int new_irq = 0;

    interrupt &= ~8;

    if ((tubeula.r1stat & TUBE_STAT_Q) && (tubeula.hstat[3] & TUBE_DATA_AVAIL))
        interrupt |= 8;

    if (((tubeula.r1stat & TUBE_STAT_I) && (tubeula.pstat[0] & TUBE_DATA_AVAIL)) || ((tubeula.r1stat & TUBE_STAT_J) && (tubeula.pstat[3] & TUBE_DATA_AVAIL))) {
        new_irq |= 1;
        if (!(tube_irq & 1)) {
            log_debug("tube: parasite IRQ asserted");
            if (tube_type == TUBEPDP11) {
                if (((m_pdp11->PS >> 5) & 7) < 6)
                    pdp11_interrupt(0x84, 6);
            }
            else if (tube_type == TUBE68000)
                m68k_set_virq(2, 1);
            else if (tube_type == TUBESPROW)
                sprow_interrupt(1);
        }
    }
    else if (tube_irq & 1) {
        log_debug("tube: parasite IRQ de-asserted");
        if (tube_type == TUBE68000)
            m68k_set_virq(2, 0);
    }

    if (tubeula.r1stat & TUBE_STAT_M && (tubeula.pstat[2] & TUBE_DATA_AVAIL)) {
        new_irq |= 2;
        if (!(tube_irq & 2)) {
            log_debug("tube: parasite NMI asserted");
            if (tube_type == TUBEPDP11)
                pdp11_interrupt(0x80, 7);
            else if (tube_type == TUBESPROW)
                sprow_interrupt(2);
            else if (tube_type == TUBE68000)
                m68k_set_virq(5, 1);
        }
    }
    else if (tube_irq & 2)
    {
        log_debug("tube: parasite NMI de-asserted");
        if (tube_type == TUBE68000)
            m68k_set_virq(5, 0);
    }

    if (new_irq != tube_irq && tube_type == TUBE6809)
        tube_6809_int(new_irq);

    tube_irq = new_irq;
}

uint8_t tube_host_read(uint16_t addr)
{
        uint8_t temp = 0;
        if (!tube_exec) return 0xFE;
        switch (addr & 7)
        {
            case 0: /*Reg 1 Stat*/
                temp = (tubeula.hstat[0] & TUBE_BOTH_AVAIL) | tubeula.r1stat;
                break;
            case 1: /*Register 1*/
                if (tubeula.ph1count > 0) {              
                    temp = tubeula.ph1[tubeula.ph1head];
                    if (--tubeula.ph1count == 0)
                        tubeula.hstat[0] &= ~TUBE_DATA_AVAIL;
                    if (++tubeula.ph1head == TUBE_PH1_SIZE)
                        tubeula.ph1head = 0;
                    tubeula.pstat[0] |= TUBE_SPACE_AVAIL;
                    log_debug("tube: host read R%c=%02X from FIFO", '1', temp);
                }
                else {
                    temp = tubeula.phl;
                    log_debug("tube: host read R%c=%02X from latch", '1', temp);
                }
                break;
            case 2: /*Register 2 Stat*/
                temp = tubeula.hstat[1];
                break;
            case 3: /*Register 2*/
                temp = tubeula.ph2;
                log_debug("tube: host read R%c=%02X", '2', temp);
                tubeula.hstat[1] &= ~TUBE_DATA_AVAIL;
                tubeula.pstat[1] |=  TUBE_SPACE_AVAIL;
                break;
            case 4: /*Register 3 Stat*/
                temp = tubeula.hstat[2];
                break;
            case 5: /*Register 3*/
                if (tubeula.ph3pos > 0) {
                    temp = tubeula.ph3[0];
                    tubeula.ph3[0] = tubeula.ph3[1];
                    tubeula.ph3pos--;
                    if (!tubeula.ph3pos) {
                        tubeula.hstat[2] &= ~TUBE_DATA_AVAIL;
                        tubeula.pstat[2] |=  TUBE_BOTH_AVAIL;
                    }
                    log_debug("tube: host read R%c=%02X from FIFO", '3', temp);
                }
                else {
                    temp = tubeula.phl;
                    log_debug("tube: host read R%c=%02X from latch", '3', temp);
                }
                break;
            case 6: /*Register 4 Stat*/
                temp = tubeula.hstat[3];
                break;
            case 7: /*Register 4*/
                temp = tubeula.ph4;
                log_debug("tube: host read R%c=%02X", '4', temp);
                tubeula.hstat[3] &= ~TUBE_DATA_AVAIL;
                tubeula.pstat[3] |=  TUBE_SPACE_AVAIL;
                break;
        }
        tube_updateints();
        return temp;
}

void tube_host_write(uint16_t addr, uint8_t val)
{
        if (!tube_exec) return;
        tubeula.hpl = val;

        switch (addr & 7)
        {
            case 0: /*Register 1 stat*/
                if (val & TUBE_STAT_S) {
                    if (val & TUBE_STAT_T && !(tubeula.r1stat & TUBE_STAT_T))
                        tube_reset_most();
                    tubeula.r1stat |= val & 0x3F;
                }
                else {
                    if (!(val & TUBE_STAT_P) && tubeula.r1stat & TUBE_STAT_P && curtube != -1)
                        tubes[curtube].cpu->reset();
                    tubeula.r1stat &= ~(val&0x3F);

                }
                log_debug("tube: host write S1=%02X->%02X", val, tubeula.r1stat);
                tubeula.hstat[0] = (tubeula.hstat[0] & TUBE_BOTH_AVAIL) | (val & 0x3F);
                break;
            case 1: /*Register 1*/
                log_debug("tube: host write R%c=%02X", '1', val);
                tubeula.hp1 = val;
                tubeula.pstat[0] |=  TUBE_DATA_AVAIL;
                tubeula.hstat[0] &= ~TUBE_SPACE_AVAIL;
                break;
            case 3: /*Register 2*/
                log_debug("tube: host write R%c=%02X", '2', val);
                tubeula.hp2 = val;
                tubeula.pstat[1] |=  TUBE_DATA_AVAIL;
                tubeula.hstat[1] &= ~TUBE_SPACE_AVAIL;
                break;
            case 5: /*Register 3*/
                if (tubeula.hp3pos < 2) {
                    tubeula.hp3[tubeula.hp3pos++] = val;
                    log_debug("tube: host write R3=%02X (sucessful)", val);
                    if (tubeula.hp3pos >= ((tubeula.r1stat & TUBE_STAT_V) ? 2 : 1)) {
                        tubeula.pstat[2] |=  TUBE_DATA_AVAIL; /* data available to parasite */
                        tubeula.hstat[2] &= ~TUBE_SPACE_AVAIL; /* no space for host */
                    }
                }
                else
                    log_debug("tube: host write R3=%02X (overrun)", val);
                break;
            case 7: /*Register 4*/
                log_debug("tube: host write R%c=%02X", '4', val);
                tubeula.hp4 = val;
                tubeula.pstat[3] |=  TUBE_DATA_AVAIL;
                tubeula.hstat[3] &= ~TUBE_SPACE_AVAIL;
                break;
        }
        tube_updateints();
}

uint8_t tube_parasite_read(uint32_t addr)
{
        uint8_t temp = 0;
        switch (addr & 7)
        {
            case 0: /*Register 1 stat*/
                tube_6502_rom_in = false;
                temp = tubeula.pstat[0] | tubeula.r1stat;
                break;
            case 1: /*Register 1*/
                temp = tubeula.hp1;
                log_debug("tube: parasite read R%c=%02X", '1', temp);
                tubeula.pstat[0] &= ~TUBE_DATA_AVAIL;
                tubeula.hstat[0] |=  TUBE_SPACE_AVAIL;
                break;
            case 2: /*Register 2 stat*/
                temp = tubeula.pstat[1];
                break;
            case 3: /*Register 2*/
                temp = tubeula.hp2;
                log_debug("tube: parasite read R%c=%02X", '2', temp);
                tubeula.pstat[1] &= ~TUBE_DATA_AVAIL;
                tubeula.hstat[1] |=  TUBE_SPACE_AVAIL;
                break;
            case 4: /*Register 3 stat*/
                temp = tubeula.pstat[2];
                break;
            case 5: /*Register 3*/
                if (tubeula.hp3pos > 0) {
                    temp = tubeula.hp3[0];
                    log_debug("tube: parasite read R%c=%02X from FIFO", '3', temp);
                    tubeula.hp3[0] = tubeula.hp3[1];
                    tubeula.hp3pos--;
                    if (!tubeula.hp3pos) {
                        tubeula.hstat[2] |=  TUBE_SPACE_AVAIL;
                        tubeula.pstat[2] &= ~TUBE_DATA_AVAIL;
                    }
                }
                else {
                    temp = tubeula.hpl;
                    log_debug("tube: parasite read R%c=%02X from latch", '3', temp);
                }
                break;
            case 6: /*Register 4 stat*/
                temp = tubeula.pstat[3];
                break;
            case 7: /*Register 4*/
                temp = tubeula.hp4;
                log_debug("tube: parasite read R%c=%02X", '4', temp);
                tubeula.pstat[3] &= ~TUBE_DATA_AVAIL;
                tubeula.hstat[3] |=  TUBE_SPACE_AVAIL;
                break;
        }
        tube_updateints();
        return temp;
}

void tube_parasite_write(uint32_t addr, uint8_t val)
{
    tubeula.phl = val;

        switch (addr & 7)
        {
            case 1: /*Register 1*/
                if (tubeula.ph1count < TUBE_PH1_SIZE) {
                    log_debug("tube: parasite write R%c=%02X (sucessful)", '1', val);
                    tubeula.ph1[tubeula.ph1tail++] = val;
                    tubeula.hstat[0] |= TUBE_DATA_AVAIL;
                    if (tubeula.ph1tail == TUBE_PH1_SIZE)
                        tubeula.ph1tail = 0;
                    if (++tubeula.ph1count == TUBE_PH1_SIZE)
                        tubeula.pstat[0] &= ~TUBE_SPACE_AVAIL;
                }
                else
                    log_debug("tube: parasite write R%c=%02X (overrun)", '1', val);
                break;
            case 3: /*Register 2*/
                log_debug("tube: parasite write R%c=%02X", '2', val);
                tubeula.ph2 = val;
                tubeula.hstat[1] |=  TUBE_DATA_AVAIL;
                tubeula.pstat[1] &= ~TUBE_SPACE_AVAIL;
                break;
            case 5: /*Register 3*/
                if (tubeula.ph3pos < 2) {
                    log_debug("tube: parasite write R%c=%02X (sucessful)", '3', val);
                    tubeula.ph3[tubeula.ph3pos++] = val;
                    if (tubeula.ph3pos >= ((tubeula.r1stat & TUBE_STAT_V) ? 2 : 1)) {
                        tubeula.hstat[2] |=  TUBE_DATA_AVAIL; /* data available to host */
                        tubeula.pstat[2] &= ~TUBE_BOTH_AVAIL; /* no space for parasite */
                    }
                }
                else
                    log_debug("tube: parasite write R%c=%02X (overrun)", '3', val);
                break;
            case 7: /*Register 4*/
                log_debug("tube: parasite write R%c=%02X", '4', val);
                tubeula.ph4 = val;
                tubeula.hstat[3] |=  TUBE_DATA_AVAIL;
                tubeula.pstat[3] &= ~TUBE_SPACE_AVAIL;
                break;
        }
        tube_updateints();
}

void tube_updatespeed()
{
    tube_multiplier = (double)(tube_speeds[tube_speed_num].multipler) * tubes[curtube].speed_multiplier / 2.0;
}

bool tube_32016_init(void *rom)
{
        tube_type = TUBE32016;
        n32016_init();
        n32016_reset();
        tube_readmem = read_x8;
        tube_writemem = write_x8;
        tube_exec  = n32016_exec;
        tube_proc_savestate = NULL;
        tube_proc_loadstate = NULL;
        return true;
}

void tube_ula_savestate(FILE *f)
{
    uint8_t bytes[2];
    bytes[0] = 2; // version two.
    bytes[1] = tube_6502_rom_in;
    fwrite(bytes, 2, 1, f);
    fwrite(&tubeula, sizeof tubeula, 1, f);
}

void tube_ula_loadstate(FILE *f)
{
    int byte = getc(f);
    if (byte == 2) {  // version two.
        log_debug("tube: loading new tube ULA structure");
        tube_6502_rom_in = getc(f);
        fread(&tubeula, sizeof tubeula, 1, f);
    }
    else {
        struct {
            uint8_t ph1[TUBE_PH1_SIZE],ph2,ph3[2],ph4;
            uint8_t hp1,hp2,hp3[2],hp4;
            uint8_t hstat[4],pstat[4],r1stat;
            int ph1tail,ph1head,ph1count,ph3pos,hp3pos;
        } old_ula;
        log_debug("tube: loading old tube ULA structure");
        tube_6502_rom_in = byte;
        fread(&old_ula, sizeof(old_ula), 1, f);
        for (int i = 0; i < TUBE_PH1_SIZE; ++i)
            tubeula.ph1[i] = old_ula.ph1[i];
        tubeula.ph2    = old_ula.ph2;
        tubeula.ph3[0] = old_ula.ph3[0];
        tubeula.ph3[1] = old_ula.ph3[1];
        tubeula.ph4    = old_ula.ph4;
        tubeula.hp1    = old_ula.hp1;
        tubeula.hp2    = old_ula.hp2;
        tubeula.hp3[0] = old_ula.hp3[0];
        tubeula.hp3[1] = old_ula.hp3[1];
        tubeula.hp4    = old_ula.hp4;
        for (int i = 0; i < 4; ++i) {
            tubeula.hstat[i] = old_ula.hstat[i];
            tubeula.pstat[i] = old_ula.pstat[i];
        }
        tubeula.r1stat   = old_ula.r1stat;
        tubeula.ph1tail  = old_ula.ph1tail;
        tubeula.ph1head  = old_ula.ph1head;
        tubeula.ph1count = old_ula.ph1count;
        tubeula.ph3pos   = old_ula.ph3pos;
        tubeula.hp3pos   = old_ula.hp3pos;
    }
    tube_updateints();
}
