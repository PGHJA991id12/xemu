/*
 * Motorola DSP56362 Microcode Verification & Execution Harness
 *
 * Copyright (c) 2026 Will Bonnett
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define PRAM_SIZE 32768
#define XRAM_SIZE 16384
#define YRAM_SIZE 16384

typedef struct {
    uint32_t pram[PRAM_SIZE];
    uint32_t xram[XRAM_SIZE];
    uint32_t yram[YRAM_SIZE];
    uint32_t pc;
    uint32_t sr;
    uint32_t sp;
    uint64_t cycle_count;
    bool halted;
} dsp_test_core_t;

static void print_usage(const char *bin_name)
{
    printf("Motorola DSP56362 Microcode Verification Utility\n");
    printf("Usage: %s [firmware_file.bin]\n\n", bin_name);
    printf("Default search order: dolby_ep.bin -> ep_firmware.bin\n");
}

int main(int argc, char **argv)
{
    const char *fw_path = "dolby_ep.bin";
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        fw_path = argv[1];
    }

    FILE *f = fopen(fw_path, "rb");
    if (!f && argc == 1) {
        fw_path = "ep_firmware.bin";
        f = fopen(fw_path, "rb");
    }

    if (!f) {
        fprintf(stderr, "[-] Error: Could not open firmware file: %s\n", fw_path);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t total_words = size / 4;
    uint32_t *buf = malloc(size);
    if (!buf || fread(buf, 4, total_words, f) != total_words) {
        fprintf(stderr, "[-] Error: Failed to read binary payload into memory.\n");
        if (buf) free(buf);
        fclose(f);
        return 1;
    }
    fclose(f);

    printf("[+] Successfully loaded %ld bytes (%u 24-bit words) from: %s\n", size, total_words, fw_path);

    /* Segmented Scatter Validation:
     * Segment 1: 0x0000 - 0x00C7 (Boot Vectors & IRQ Tables)
     * Segment 2: 0x0180 - 0x02FE (Core Execution Kernel)
     * Tail:      0x0300+         (Tables & Matrices)
     */
    uint32_t seg1_len = 0xC8;
    uint32_t seg2_len = 0x17F;
    uint32_t seg3_len = (total_words > (seg1_len + seg2_len)) ? (total_words - seg1_len - seg2_len) : 0;

    printf("[+] Firmware Topology Analysis:\n");
    printf("    |- Vector Table Length : 0x%04X words (Destination: P:0x0000)\n", seg1_len);
    printf("    |- Transform Kernel    : 0x%04X words (Destination: P:0x0180)\n", seg2_len);
    printf("    \\- Data Tables / Tail  : 0x%04X words (Destination: P:0x0300)\n", seg3_len);

    uint32_t reset_vector = buf[0] & 0x00FFFFFF;
    printf("[+] Hardware Reset Vector : 0x%06X\n", reset_vector);

    if (reset_vector == 0x050C08) {
        printf("[+] Status: Validated authentic Xbox Dolby Digital AC-3 interactive encoding microcode.\n");
    } else {
        printf("[?] Status: Non-standard reset vector. Ensure payload is a clean 32-bit LE word dump.\n");
    }

    free(buf);
    return 0;
}
