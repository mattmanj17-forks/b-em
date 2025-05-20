/*B-em v2.2 by Tom Walker
  ROM handling*/

#include <ctype.h>
#include "b-em.h"
#include "6502.h"
#include "config.h"
#include "mem.h"
#include "model.h"

/* Layout of the 64K of host RAM pointed to by the 'ram' pointer:
 *
 * 0x0000 -> 0x7fff  Normal RAM, as per BBC B.
 * 0x8000 -> 0xafff  Spare 12K from shadow mode
 * 0xb000 -> 0xffff  20K Shadow screen memory.
 *
 * How the "Spare 12K" is allocated depends on the model:
 *
 * B+:
 *     0x8000 -> 0xafff - short sideways bank.
 *
 * Master:
 *     0x8000 -> 0x8fff - 4K high VDU workspace (mapped at 0x8000).
 *     0x9000 -> 0xafff - 8K filing system RAM  (mapped at 0xc000).
 *
 * Integra:
 *     0x8000 -> 0x83ff - 1K bank at 0x8000.
 *     0x8000 -> 0x8fff - 4K bank at 0x8000 (superset of above).
 *     0x9000 -> 0xafff - 8K bank at 0x9000.
 */

uint8_t *ram, *rom, *os;
uint8_t ram_fe30, ram_fe34;

rom_slot_t rom_slots[ROM_NSLOT];

ALLEGRO_PATH *os_dir, *rom_dir;

static const char slotkeys[16][6] = {
    "rom00", "rom01", "rom02", "rom03",
    "rom04", "rom05", "rom06", "rom07",
    "rom08", "rom09", "rom10", "rom11",
    "rom12", "rom13", "rom14", "rom15"
};

void mem_init()
{
    log_debug("mem: mem_init");
    size_t size = RAM_SIZE + ROM_SIZE + ROM_NSLOT * ROM_SIZE;
    uint8_t *ptr = malloc(size);
    if (ptr) {
        memset(ptr, 0xff, size);
        ram = ptr;
        os  = ptr + RAM_SIZE;
        rom = ptr + RAM_SIZE + ROM_SIZE;
        os_dir  = al_create_path_for_directory("roms/os");
        rom_dir = al_create_path_for_directory("roms/general");
    }
    else {
        log_fatal("mem: unable to allocate memory for BBC Micro: %m");
        exit(1);
    }
}

static void rom_free(int slot)
{
    if (rom_slots[slot].alloc) {
        if (rom_slots[slot].name)
            free(rom_slots[slot].name);
        if (rom_slots[slot].path)
            free(rom_slots[slot].path);
    }
}

void mem_close() {
    for (int slot = 0; slot < ROM_NSLOT; slot++)
        rom_free(slot);
    free(ram);
    if (os_dir)
        al_destroy_path(os_dir);
    if (rom_dir)
        al_destroy_path(rom_dir);
}

static void dump_mem(void *start, size_t size, const char *which, const char *file) {
    FILE *f;

    if ((f = fopen(file, "wb"))) {
        fwrite(start, size, 1, f);
        fclose(f);
    } else
        log_error("mem: unable to open %s dump file %s: %s", which, file, strerror(errno));
}

void mem_dump(void) {
    dump_mem(ram, 64*1024, "RAM", "ram.dmp");
    dump_mem(rom, ROM_NSLOT*ROM_SIZE, "ROM", "rom.dmp");
}

static void load_os_rom(const char *sect) {
    const char *osname, *cpath;
    FILE *f;
    ALLEGRO_PATH *path;

    osname = get_config_string(sect, "os", models[curmodel].os);
    if ((path = find_dat_file(os_dir, osname, ".rom"))) {
        cpath = al_path_cstr(path, ALLEGRO_NATIVE_PATH_SEP);
        if ((f = fopen(cpath, "rb"))) {
            if (fread(os, ROM_SIZE, 1, f) == 1) {
                fclose(f);
                log_debug("mem: OS %s loaded from %s", osname, cpath);
                al_destroy_path(path);
                return;
            }
            else
                log_fatal("mem: unable to load OS %s, read error/truncated file on %s", osname, cpath);
        }
        else
            log_fatal("mem: unable to load OS %s, unable to open %s: %s", osname, cpath, strerror(errno));
        al_destroy_path(path);
    } else
        log_fatal("mem: unable to find OS %s", osname);
    exit(1);
}

const uint8_t *mem_romdetail(int slot) {
    uint8_t *base = rom + (slot * ROM_SIZE);
    uint8_t rtype, *copyr;

    rtype = base[6];
    if (rtype & 0xc0) {
        copyr = base + base[7];
        if (copyr[0] == 0 && copyr[1] == '(' && copyr[2] == 'C' && copyr[3] == ')') {
            return base + 8;
        }
    }
    return NULL;
}

void mem_loadrom(int slot, const char *name, const char *path, uint8_t use_name) {
    FILE *f;

    if ((f = fopen(path, "rb"))) {
        if (fread(rom + (slot * ROM_SIZE), ROM_SIZE, 1, f) == 1 || feof(f)) {
            fclose(f);
            log_debug("mem: ROM slot %02d loaded with %s from %s", slot, name, path);
            rom_slots[slot].use_name = use_name;
            rom_slots[slot].alloc = 1;
            rom_slots[slot].name = strdup(name);
            rom_slots[slot].path = strdup(path);
        }
        else
            log_warn("mem: unable to load ROM slot %02d with %s: %s", slot, name, strerror(errno));
    }
    else
        log_warn("mem: unable to load ROM slot %02d with %s, uanble to open %s: %s", slot, name, path, strerror(errno));
}

static void cfg_load_rom(int slot, const char *sect) {
    const char *key, *name, *file;
    ALLEGRO_PATH *path;

    key = slotkeys[slot];
    name = al_get_config_value(bem_cfg, sect, key);
    if (name != NULL && *name != '\0') {
        if (is_relative_filename(name)) {
            if ((path = find_dat_file(rom_dir, name, ".rom"))) {
                file = al_path_cstr(path, ALLEGRO_NATIVE_PATH_SEP);
                mem_loadrom(slot, name, file, 1);
                al_destroy_path(path);
            }
            else
                log_warn("mem: unable to load ROM slot %02d with %s, ROM file not found", slot, name);
        } else {
            if ((file = strrchr(name, '/')))
                file++;
            else
                file = name;
            mem_loadrom(slot, file, name, 0);
        }
    }
}

static bool mem_load_batback(int slot)
{
    bool worked = false;
    char name[16];
    snprintf(name, sizeof(name), "model%02dram%02d", curmodel, slot);
    ALLEGRO_PATH *path = find_cfg_file(name, ".bin");
    if (path) {
        const char *cpath = al_path_cstr(path, ALLEGRO_NATIVE_PATH_SEP);
        FILE *fp = fopen(cpath, "rb");
        if (fp) {
            if (fread(rom + (slot * ROM_SIZE), ROM_SIZE, 1, fp) == 1) {
                log_debug("mem: battery backed slot %d loaded from %s", slot, cpath);
                worked = true;
            }
            else
                log_error("mem: unable to restore battery backed slot %d from file %s: %s", slot, cpath, strerror(errno));
            fclose(fp);
        }
        al_destroy_path(path);
    }
    return worked;
}

static void mem_save_batback(int slot)
{
    char name[16];
    snprintf(name, sizeof(name), "model%02dram%02d", curmodel, slot);
    ALLEGRO_PATH *path = find_cfg_dest(name, ".bin");
    if (path) {
        const char *cpath = al_path_cstr(path, ALLEGRO_NATIVE_PATH_SEP);
        FILE *fp = fopen(cpath, "wb");
        if (fp) {
            uint8_t *base = rom + (slot * ROM_SIZE);
            if (fwrite(base, ROM_SIZE, 1, fp) == 1)
                log_debug("mem: battery backed slot %d saved to %s", slot, cpath);
            else
                log_error("mem: unable to save battery-backed RAM bank %d: failed writing to %s: %s", slot, cpath, strerror(errno));
            fclose(fp);
        }
        else
            log_error("unable to save battery-backed RAM bank %d: unable to open %s for writing: %s", slot, cpath, strerror(errno));
        al_destroy_path(path);
    }
    else
        log_error("unable to save battery-backed RAM bank %d: no suitable destination", slot);
}

void mem_romsetup_os01() {
    const char *sect = models[curmodel].cfgsect;
    char *name, *path;
    int c;

    load_os_rom(sect);
    cfg_load_rom(15, sect);
    memcpy(rom + 14 * ROM_SIZE, rom + 15 * ROM_SIZE, ROM_SIZE);
    memcpy(rom + 12 * ROM_SIZE, rom + 14 * ROM_SIZE, ROM_SIZE * 2);
    memcpy(rom + 8 * ROM_SIZE, rom + 12 * ROM_SIZE, ROM_SIZE * 4);
    memcpy(rom, rom + 8 * ROM_SIZE, ROM_SIZE * 8);
    name = rom_slots[15].name;
    path = rom_slots[15].path;
    for (c = 0; c < 15; c++) {
        rom_slots[c].locked = 1;
        rom_slots[c].swram = 0;
        rom_slots[c].alloc = 0;
        rom_slots[c].name = name;
        rom_slots[c].path = path;
    }
}

void mem_romsetup_std(void) {
    const char *sect = models[curmodel].cfgsect;
    int slot;

    load_os_rom(sect);
    for (slot = 15; slot >= 0; slot--)
        cfg_load_rom(slot, sect);
}

static void fill_swram(void) {
    int slot;

    for (slot = 0; slot < ROM_NSLOT; slot++)
        if (!rom_slots[slot].name)
            rom_slots[slot].swram = 1;
}

void mem_romsetup_swram(void) {

    mem_romsetup_std();
    fill_swram();
}

void mem_romsetup_bp128(void) {
    const char *sect = models[curmodel].cfgsect;
    int slot;

    load_os_rom(sect);
    cfg_load_rom(15, sect);
    cfg_load_rom(14, sect);
    rom_slots[13].swram = 1;
    rom_slots[12].swram = 1;
    for (slot = 11; slot >= 0; slot--)
        cfg_load_rom(slot, sect);
    rom_slots[1].swram = 1;
    rom_slots[0].swram = 1;
}

void mem_romsetup_compact(void)
{
    mem_romsetup_std();
    rom_slots[7].swram = 1;
    rom_slots[6].swram = 1;
    rom_slots[5].swram = 1;
    rom_slots[4].swram = 1;
}

void mem_romsetup_master(void) {
    const char *sect = models[curmodel].cfgsect;
    const char *osname, *cpath;
    FILE *f;
    ALLEGRO_PATH *path;
    int slot;

    osname = get_config_string(sect, "os", models[curmodel].os);
    if ((path = find_dat_file(os_dir, osname, ".rom"))) {
        cpath = al_path_cstr(path, ALLEGRO_NATIVE_PATH_SEP);
        if ((f = fopen(cpath, "rb"))) {
            if (fread(os, ROM_SIZE, 1, f) == 1) {
                if (fread(rom + (9 * ROM_SIZE), 7 * ROM_SIZE, 1, f) == 1) {
                    fclose(f);
                    al_destroy_path(path);
                    for (slot = ROM_NSLOT-1; slot >= 9; slot--) {
                        rom_slots[slot].swram = 0;
                        rom_slots[slot].locked = 1;
                        rom_slots[slot].alloc = 0;
                        rom_slots[slot].name = (char *)models[curmodel].os;
                    }
                    cfg_load_rom(8, sect);
                    rom_slots[7].swram = 1;
                    rom_slots[6].swram = 1;
                    rom_slots[5].swram = 1;
                    rom_slots[4].swram = 1;
                    for (slot = 7; slot >= 0; slot--)
                        cfg_load_rom(slot, sect);
                    return;
                }
            }
            log_fatal("mem: unable to read complete OS ROM %s: %s", osname, strerror(errno));
            fclose(f);
        } else
            log_fatal("mem: unable to load OS %s, unable to open %s: %s", osname, cpath, strerror(errno));
        al_destroy_path(path);
    } else
        log_fatal("mem: unable to find OS %s", osname);
    exit(1);
}

void mem_romsetup_weramrom(void) {
    const char *sect = models[curmodel].cfgsect;

    load_os_rom(sect);
    cfg_load_rom(15, sect);
    if (!mem_load_batback(14))
        cfg_load_rom(14, sect);
    for (int slot = 13; slot >= 0; --slot)
        cfg_load_rom(slot, sect);

    rom_slots[14].backed = 1;
    rom_slots[14].swram = 1;
    rom_slots[7].swram = 1;
    rom_slots[6].swram = 1;
    rom_slots[5].swram = 1;
    rom_slots[4].swram = 1;
    rom_slots[3].swram = 1;
    rom_slots[2].swram = 1;
    rom_slots[1].swram = 1;
    rom_slots[0].swram = 1;
    weramrom = true;
}

int mem_findswram(int n) {
    int c;

    for (c = 0; c < ROM_NSLOT; c++)
        if (rom_slots[c].swram)
            if (n-- <= 0)
                return c;
    return -1;
}

static void rom_clearmeta(int slot) {
    rom_free(slot);
    rom_slots[slot].split = 0xc0;
    rom_slots[slot].locked = 0;
    rom_slots[slot].use_name = 0;
    rom_slots[slot].alloc = 0;
    rom_slots[slot].name = NULL;
    rom_slots[slot].path = NULL;
}

void mem_clearrom(int slot) {
    uint8_t *base = rom + (slot * ROM_SIZE);

    memset(base, 0xff, ROM_SIZE);
    rom_clearmeta(slot);
}

void mem_clearroms(void) {
    int slot;

    memset(rom, 0xff, ROM_NSLOT * ROM_SIZE);
    for (slot = 0; slot < ROM_NSLOT; slot++) {
        rom_clearmeta(slot);
        rom_slots[slot].swram = 0;
        rom_slots[slot].backed = 0;
    }
}

void mem_savezlib(ZFILE *zfp)
{
    unsigned char latches[2];

    latches[0] = ram_fe30;
    latches[1] = ram_fe34;
    savestate_zwrite(zfp, latches, 2);
    savestate_zwrite(zfp, ram, RAM_SIZE);
    savestate_zwrite(zfp, rom, ROM_SIZE*ROM_NSLOT);
}

void mem_loadzlib(ZFILE *zfp)
{
    unsigned char latches[2];

    savestate_zread(zfp, latches, 2);
    writemem(0xFE30, latches[0]);
    writemem(0xFE34, latches[1]);
    savestate_zread(zfp, ram, RAM_SIZE);
    savestate_zread(zfp, rom, ROM_SIZE*ROM_NSLOT);
}

void mem_loadstate(FILE *f) {
    writemem(0xFE30, getc(f));
    writemem(0xFE34, getc(f));
    fread(ram, RAM_SIZE, 1, f);
    fread(rom, ROM_SIZE*ROM_NSLOT, 1, f);
}

void mem_save_romcfg(const char *sect) {
    int slot;
    rom_slot_t *slotp;
    const char *value;

    for (slot = ROM_NSLOT-1; slot >= 0; slot--) {
        slotp = rom_slots + slot;
        if (!slotp->locked) {
            value = slotp->use_name ? slotp->name : slotp->path;
            if (value)
                al_set_config_value(bem_cfg, sect, slotkeys[slot], value);
            else
                al_remove_config_key(bem_cfg, sect, slotkeys[slot]);
            if (slotp->backed)
                mem_save_batback(slot);
        }
    }
}

enum mem_jim_sz mem_jim_size = JIM_NONE;
static uint32_t mem_jim_max = 0;
static uint8_t *mem_jim_data = NULL;
static uint32_t mem_jim_page;

static const uint32_t mem_jim_sizes[6] = {
    0,
    0x1000000,
    0x4000000,
    0x10000000,
    0x1e000000,
    0x3e000000
};

void mem_jim_setsize(enum mem_jim_sz size)
{
    if (size != mem_jim_size) {
        uint32_t nmax = mem_jim_sizes[size];
        log_debug("mem: new jim size %d=%d bytes", size, nmax);
        if (nmax == 0) {
            free(mem_jim_data);
            mem_jim_size = size;
            mem_jim_max = nmax;
            mem_jim_data = NULL;
        }
        else {
            uint8_t *njim = realloc(mem_jim_data, nmax);
            if (njim) {
                mem_jim_size = size;
                mem_jim_max = nmax;
                mem_jim_data = njim;
            }
            else
                log_error("mem: out of memory allocating JIM expansion RAM");
        }
    }
}

uint8_t mem_jim_getsize(void)
{
    uint8_t m16 = mem_jim_max >> 24;
    log_debug("mem: get jim size, max=%08X, returns %d", mem_jim_max, m16);
    return m16;
}

uint8_t mem_jim_read(uint16_t addr)
{
    uint32_t full_addr = mem_jim_page | (addr & 0xff);
    if (full_addr < mem_jim_max)
        return mem_jim_data[full_addr];
    return addr >> 8;
}

void mem_jim_write(uint16_t addr, uint8_t value)
{
    if (addr >= 0xfd00) {
        uint32_t full_addr = mem_jim_page | (addr & 0xff);
        if (full_addr < mem_jim_max)
            mem_jim_data[full_addr] = value;
    }
    else if (addr == 0xfcff)
        mem_jim_page = (mem_jim_page & 0xffff0000) | (value << 8);
    else if (addr == 0xfcfe)
        mem_jim_page = (mem_jim_page & 0xff00ff00) | (value << 16);
    else if (addr == 0xfcfd)
        mem_jim_page = (mem_jim_page & 0x00ffff00) | (value << 24);
}

void mem_jim_savez(ZFILE *zfp)
{
    unsigned char buf[7];
    buf[0] = mem_jim_max & 0xff;
    buf[1] = (mem_jim_max >> 8) & 0xff;
    buf[2] = (mem_jim_max >> 16) & 0xff;
    buf[3] = (mem_jim_max >> 24) & 0xff;
    buf[4] = (mem_jim_page >> 8) & 0xff;
    buf[5] = (mem_jim_page >> 16) & 0xff;
    buf[6] = (mem_jim_page >> 24) & 0xff;
    savestate_zwrite(zfp, buf, sizeof(buf));
    if (mem_jim_max > 0)
        savestate_zwrite(zfp, mem_jim_data, mem_jim_max);
}

extern void mem_jim_loadz(ZFILE *zfp)
{
    unsigned char buf[7];
    savestate_zread(zfp, buf, sizeof(buf));
    size_t nsize = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
    if (mem_jim_data)
        free(mem_jim_data);
    mem_jim_data = NULL;
    mem_jim_max = 0;
    mem_jim_size = JIM_NONE;
    if (nsize > 0) {
        uint8_t *njim = malloc(nsize);
        if (njim) {
            mem_jim_data = njim;
            mem_jim_max = nsize;
            while (mem_jim_size < JIM_INVALID && nsize != mem_jim_sizes[mem_jim_size])
                ++mem_jim_size;
            mem_jim_page = (buf[4] << 8) | (buf[5] << 16) | (buf[6] << 24);
            savestate_zread(zfp, njim, nsize);
        }
        else
            log_warn("mem: out of memory restoring JIM from savefile");
    }
}
