/*
 * MCPX DSP emulator - C interpreter backend
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2020-2025 Matt Borgerson
 *
 * Adapted from Hatari DSP M56001 emulation
 * (C) 2001-2008 ARAnyM developer team
 * Adaption to Hatari (C) 2008 by Thomas Huth
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "qemu/osdep.h"
#include "dsp_internal.h"
#include "interp/dsp_cpu.h"
#include "debug.h"

static dsp_core_t *c_core(DSPState *dsp)
{
    return (dsp_core_t *)dsp->backend;
}

static uint32_t c_dma_mem_read(void *opaque, int space, uint32_t addr)
{
    return dsp56k_read_memory((dsp_core_t *)opaque, space, addr);
}

static void c_dma_mem_write(void *opaque, int space, uint32_t addr,
                            uint32_t value)
{
    dsp56k_write_memory((dsp_core_t *)opaque, space, addr, value);
}

static uint32_t c_read_peripheral(dsp_core_t *core, uint32_t address)
{
    return read_peripheral((DSPState *)core->opaque, address);
}

static void c_write_peripheral(dsp_core_t *core, uint32_t address,
                               uint32_t value)
{
    write_peripheral((DSPState *)core->opaque, address, value);
}

static void dsp_c_reset(DSPState *dsp)
{
    dsp56k_reset_cpu(c_core(dsp));
    dsp->save_cycles = 0;
}

static void dsp_c_step(DSPState *dsp)
{
    dsp_core_t *core = c_core(dsp);
    dsp56k_execute_instruction(core);
    core->cycle_count += core->instr_cycle;
}

static void dsp_c_run(DSPState *dsp, int cycles)
{
    dsp_core_t *core = c_core(dsp);

    dsp->save_cycles += cycles;

    if (dsp->save_cycles <= 0)
        return;

    while (dsp->save_cycles > 0) {
        dsp56k_execute_instruction(core);
        dsp->save_cycles -= core->instr_cycle;
        core->cycle_count += core->instr_cycle;

        if (core->is_idle) {
            break;
        }
    }
}

static void dsp_c_bootstrap_ep_firmware(dsp_core_t *core)
{
    const char *candidates[] = {
        "dolby_ep.bin",
        "tools/dolby_ep.bin",
        "../tools/dolby_ep.bin",
        "tools/halo2_dolby.bin",
        NULL
    };

    FILE *f = NULL;
    const char *found_path = NULL;
    for (int i = 0; candidates[i] != NULL; i++) {
        f = fopen(candidates[i], "rb");
        if (f) {
            found_path = candidates[i];
            break;
        }
    }

    if (!f) {
        fprintf(stderr, "[EP NOTICE] Discrete 5.1 Dolby microcode (dolby_ep.bin) not found.\n"
                        "[EP NOTICE] Encoding Processor will remain idle; audio running in standard fallback mode.\n");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t total_words = size / 4;
    uint32_t *buf = g_malloc(size);
    if (fread(buf, 4, total_words, f) != total_words) {
        fprintf(stderr, "[EP ERROR] Failed to read microcode payload from %s\n", found_path);
        g_free(buf);
        fclose(f);
        return;
    }
    fclose(f);

    /* Segmented Dolby Scatter-Loader */
    uint32_t seg1_len = 0xC8;
    uint32_t seg2_len = 0x17F;
    uint32_t seg3_len = (total_words > (seg1_len + seg2_len)) ? (total_words - seg1_len - seg2_len) : 0;

    for (uint32_t i = 0; i < seg1_len && i < total_words; i++) {
        core->pram[0x0000 + i] = buf[i] & 0x00FFFFFF;
    }
    for (uint32_t i = 0; i < seg2_len && (seg1_len + i) < total_words; i++) {
        core->pram[0x0180 + i] = buf[seg1_len + i] & 0x00FFFFFF;
    }
    for (uint32_t i = 0; i < seg3_len && (seg1_len + seg2_len + i) < total_words; i++) {
        if ((0x0300 + i) >= DSP_PRAM_SIZE) break;
        core->pram[0x0300 + i] = buf[seg1_len + seg2_len + i] & 0x00FFFFFF;
    }

    g_free(buf);
    fprintf(stderr, "[EP BOOTSTRAP] Loaded %u words from %s | Reset Vector P:0 = 0x%06X\n",
            total_words, found_path, core->pram[0]);
}

static void dsp_c_bootstrap(DSPState *dsp)
{
    dsp_core_t *core = c_core(dsp);

    if (dsp->is_gp) {
        // scratch memory is dma'd in to pram by the bootrom
        dsp->dma.scratch_rw(dsp->dma.rw_opaque, (uint8_t *)core->pram, 0, 0x800 * 4,
                            false);
        for (int i = 0; i < 0x800; i++) {
            if (core->pram[i] & 0xff000000) {
                DPRINTF("Bootstrap %04x: %08x\n", i, core->pram[i]);
                core->pram[i] &= 0x00ffffff;
            }
        }
    } else {
        /* Bootstrap Encoding Processor with genuine Dolby firmware */
        dsp_c_bootstrap_ep_firmware(core);
    }
    memset(core->pram_opcache, 0, sizeof(core->pram_opcache));
}

static uint32_t dsp_c_read_memory(DSPState *dsp, char space, uint32_t address)
{
    int space_id;

    switch (space) {
    case 'X':
        space_id = DSP_SPACE_X;
        break;
    case 'Y':
        space_id = DSP_SPACE_Y;
        break;
    case 'P':
        space_id = DSP_SPACE_P;
        break;
    default:
        assert(!"Invalid dsp space when reading from memory");
        return 0;
    }

    return dsp56k_read_memory(c_core(dsp), space_id, address);
}

static void dsp_c_write_memory(DSPState *dsp, char space, uint32_t address,
                               uint32_t value)
{
    int space_id;

    switch (space) {
    case 'X':
        space_id = DSP_SPACE_X;
        break;
    case 'Y':
        space_id = DSP_SPACE_Y;
        break;
    case 'P':
        space_id = DSP_SPACE_P;
        break;
    default:
        assert(!"Invalid dsp space when writing to memory");
        return;
    }

    dsp56k_write_memory(c_core(dsp), space_id, address, value);
}

static bool dsp_c_get_halt_requested(DSPState *dsp)
{
    return c_core(dsp)->is_idle || c_core(dsp)->halt_requested;
}

static void dsp_c_set_halt_requested(DSPState *dsp, bool halt)
{
    c_core(dsp)->is_idle = halt;
    c_core(dsp)->halt_requested = halt;
}

static uint32_t dsp_c_get_cycle_count(DSPState *dsp)
{
    return c_core(dsp)->cycle_count;
}

static void dsp_c_set_cycle_count(DSPState *dsp, uint32_t count)
{
    c_core(dsp)->cycle_count = count;
}

static void dsp_c_invalidate_opcache(DSPState *dsp)
{
    memset(c_core(dsp)->pram_opcache, 0, sizeof(c_core(dsp)->pram_opcache));
}

static uint32_t dsp_c_get_pc(DSPState *dsp)
{
    return c_core(dsp)->pc;
}

/*
 * Sync: C interpreter -> DspCoreState (before VM save / debug)
 */
static void dsp_c_sync_to_vm(DSPState *dsp)
{
    dsp_core_t *core = c_core(dsp);
    DspCoreState *vm = &dsp->core;

    vm->pc = core->pc;
    vm->cycle_count = core->cycle_count;
    vm->instr_cycle = core->instr_cycle;
    vm->halt_requested = core->is_idle;
    vm->is_gp = core->is_gp;
    vm->loop_rep = core->loop_rep;
    vm->pc_on_rep = core->pc_on_rep;
    vm->cur_inst = core->cur_inst;
    vm->cur_inst_len = core->cur_inst_len;

    memcpy(vm->registers, core->registers, sizeof(vm->registers));
    memcpy(vm->stack, core->stack, sizeof(vm->stack));
    memcpy(vm->xram, core->xram, sizeof(vm->xram));
    memcpy(vm->yram, core->yram, sizeof(vm->yram));
    memcpy(vm->pram, core->pram, sizeof(vm->pram));
    memcpy(vm->mixbuffer, core->mixbuffer, sizeof(vm->mixbuffer));
    memcpy(vm->periph, core->periph, sizeof(vm->periph));

    vm->interrupt_state = core->interrupt_state;
    vm->interrupt_instr_fetch = core->interrupt_instr_fetch;
    vm->interrupt_save_pc = core->interrupt_save_pc;
    vm->interrupt_counter = core->interrupt_counter;
    vm->interrupt_ipl_to_raise = core->interrupt_ipl_to_raise;
    vm->interrupt_pipeline_count = core->interrupt_pipeline_count;
    memcpy(vm->interrupt_ipl, core->interrupt_ipl, sizeof(core->interrupt_ipl));
    memcpy(vm->interrupt_is_pending, core->interrupt_is_pending,
           sizeof(core->interrupt_is_pending));
}

/*
 * Sync: DspCoreState -> C interpreter (after VM load / engine switch)
 */
static void dsp_c_sync_from_vm(DSPState *dsp)
{
    dsp_core_t *core = c_core(dsp);
    DspCoreState *vm = &dsp->core;

    core->pc = vm->pc;
    core->cycle_count = vm->cycle_count;
    core->instr_cycle = vm->instr_cycle;
    core->is_idle = vm->halt_requested;
    core->is_gp = vm->is_gp;
    core->loop_rep = vm->loop_rep;
    core->pc_on_rep = vm->pc_on_rep;
    core->cur_inst = vm->cur_inst;
    core->cur_inst_len = vm->cur_inst_len;

    memcpy(core->registers, vm->registers, sizeof(vm->registers));
    memcpy(core->stack, vm->stack, sizeof(vm->stack));
    memcpy(core->xram, vm->xram, sizeof(vm->xram));
    memcpy(core->yram, vm->yram, sizeof(vm->yram));
    memcpy(core->pram, vm->pram, sizeof(vm->pram));
    memcpy(core->mixbuffer, vm->mixbuffer, sizeof(vm->mixbuffer));
    memcpy(core->periph, vm->periph, sizeof(vm->periph));

    core->interrupt_state = vm->interrupt_state;
    core->interrupt_instr_fetch = vm->interrupt_instr_fetch;
    core->interrupt_save_pc = vm->interrupt_save_pc;
    core->interrupt_counter = vm->interrupt_counter;
    core->interrupt_ipl_to_raise = vm->interrupt_ipl_to_raise;
    core->interrupt_pipeline_count = vm->interrupt_pipeline_count;
    memcpy(core->interrupt_ipl, vm->interrupt_ipl, sizeof(core->interrupt_ipl));
    memcpy(core->interrupt_is_pending, vm->interrupt_is_pending,
           sizeof(core->interrupt_is_pending));

    memset(core->pram_opcache, 0, sizeof(core->pram_opcache));
}

void dsp_c_init(DSPState *dsp)
{
    dsp_core_t *core = g_new0(dsp_core_t, 1);
    core->opaque = dsp;
    core->is_gp = dsp->is_gp;
    core->read_peripheral = c_read_peripheral;
    core->write_peripheral = c_write_peripheral;

    dsp->backend = core;
    dsp->ops = &c_dsp_ops;

    dsp->dma.mem_opaque = core;
    dsp->dma.mem_read = c_dma_mem_read;
    dsp->dma.mem_write = c_dma_mem_write;

    /* Ensure the interpreter's opcode decoder tables are initialized.
     * dsp56k_reset_cpu populates the static nonparallel_matches[] array
     * on first call.  The runtime state it writes (registers, PC, etc.)
     * will be overwritten by the subsequent sync_from_vm. */
    dsp56k_reset_cpu(core);

    if (dsp->is_gp) {
        memset(core->pram, 0xCA, DSP_PRAM_SIZE * sizeof(uint32_t));
    } else {
        memset(core->pram, 0, DSP_PRAM_SIZE * sizeof(uint32_t));
    }
    memset(core->xram, 0xCA, DSP_XRAM_SIZE * sizeof(uint32_t));
    memset(core->yram, 0xCA, DSP_YRAM_SIZE * sizeof(uint32_t));
    dsp->ops->invalidate_opcache(dsp);
}

static void dsp_c_finalize(DSPState *dsp)
{
    g_free(dsp->backend);
    dsp->backend = NULL;
}

const DSPOps c_dsp_ops = {
    .bootstrap = dsp_c_bootstrap,
    .finalize = dsp_c_finalize,
    .get_cycle_count = dsp_c_get_cycle_count,
    .get_halt_requested = dsp_c_get_halt_requested,
    .get_pc = dsp_c_get_pc,
    .invalidate_opcache = dsp_c_invalidate_opcache,
    .read_memory = dsp_c_read_memory,
    .reset = dsp_c_reset,
    .run = dsp_c_run,
    .set_cycle_count = dsp_c_set_cycle_count,
    .set_halt_requested = dsp_c_set_halt_requested,
    .start_frame = dsp_start_frame_impl,
    .step = dsp_c_step,
    .sync_from_vm = dsp_c_sync_from_vm,
    .sync_to_vm = dsp_c_sync_to_vm,
    .write_memory = dsp_c_write_memory,
};
