#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "bootloader/video/metadata.h"

struct DeviceTreeNode {
    uint32_t nProperties;
    uint32_t nChildren;
};

struct DeviceTreeNodeProperty {
    char name[32];
    uint32_t length;
};

void write_prop(FILE *f, const char *name, const void *data, uint32_t len) {
    struct DeviceTreeNodeProperty p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, name, 31);
    p.length = len;
    fwrite(&p, sizeof(p), 1, f);
    
    if (len > 0) {
        fwrite(data, len, 1, f);
        uint32_t pad = (4 - (len % 4)) % 4;
        if (pad > 0) {
            uint32_t zero = 0;
            fwrite(&zero, pad, 1, f);
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--current") != 0)) {
        fprintf(stderr, "usage: adt_builder [--current]\n");
        return 2;
    }
    long seed_offset = 0, frequency_offset = 0, video_offset = 0, ramdisk_offset = 0;
    FILE *f = fopen("adt.bin", "wb");
    if (!f) {
        perror("fopen adt.bin");
        return 1;
    }
    
    uint32_t cells = 2;
    uint64_t freq = 54000000;
    uint64_t cpu_freq = 1500000000;

    // Root Node
    struct DeviceTreeNode root = { 6, 4 }; // chosen, cpus, arm-io, defaults
    fwrite(&root, sizeof(root), 1, f);
    write_prop(f, "name", "device-tree", 12);
    write_prop(f, "model", "Raspberry Pi 4", 15);
    write_prop(f, "compatible", "bcm2711", 8);
    write_prop(f, "target-type", "BCM2711", 8);
    write_prop(f, "#address-cells", &cells, 4);
    write_prop(f, "#size-cells", &cells, 4);
    
    // Child 1: chosen
    struct DeviceTreeNode chosen = { 7, 1 }; // memory-map is a child
    fwrite(&chosen, sizeof(chosen), 1, f);
    write_prop(f, "name", "chosen", 7);
    char boot_args[161];
    memset(boot_args, ' ', 161);
    const char *orig_args = "-v -s -noprogress serial=3 fastipi=0 debug=0x144 keepsyms=1 rd=md0 pmd0=0x0a000000.0x04000000";
    memcpy(boot_args, orig_args, strlen(orig_args));
    boot_args[160] = 0;
    write_prop(f, "boot-args", boot_args, 161);
    write_prop(f, "stdout-path", "/arm-io/uart0", 14);
    uint64_t dram_base = 0;
    uint64_t dram_size = 2048ULL * 1024 * 1024;
    write_prop(f, "dram-base", &dram_base, 8);
    write_prop(f, "dram-size", &dram_size, 8);
    uint8_t random_seed[256];
    uint32_t seed_size = 256;
    /* Replaced by fresh host entropy on every QEMU launch. */
    memset(random_seed, 0, seed_size);
    seed_offset = ftell(f) + sizeof(struct DeviceTreeNodeProperty);
    write_prop(f, "random-seed", random_seed, seed_size);
    struct native_boot_video video = {0};
    video_offset = ftell(f) + sizeof(struct DeviceTreeNodeProperty);
    write_prop(f, NATIVE_BOOT_VIDEO_PROPERTY, &video, sizeof(video));

    // Child 1.1: memory-map
    struct DeviceTreeNode memory_map = { 2, 0 };
    fwrite(&memory_map, sizeof(memory_map), 1, f);
    write_prop(f, "name", "memory-map", 11);
    uint64_t ramdisk_props[2] = { 0x0A000000ULL, 0x4000000ULL };
    ramdisk_offset = ftell(f) + sizeof(struct DeviceTreeNodeProperty);
    write_prop(f, "RAMDisk", ramdisk_props, 16);
    
    // Child 2: cpus
    struct DeviceTreeNode cpus = { 2, 1 };
    fwrite(&cpus, sizeof(cpus), 1, f);
    write_prop(f, "name", "cpus", 5);
    write_prop(f, "#address-cells", &cells, 4);
    
    // Child 2.1: cpu0
    struct DeviceTreeNode cpu0 = { 6, 0 };
    fwrite(&cpu0, sizeof(cpu0), 1, f);
    write_prop(f, "name", "cpu0", 5);
    write_prop(f, "device_type", "cpu", 4);
    uint32_t reg = 0;
    write_prop(f, "reg", &reg, 4);
    write_prop(f, "state", "running", 8);
    frequency_offset = ftell(f) + sizeof(struct DeviceTreeNodeProperty);
    write_prop(f, "timebase-frequency", &freq, 8);
    write_prop(f, "clock-frequency", &cpu_freq, 8);

    // Child 3: arm-io
    struct DeviceTreeNode armio = { 3, 2 };
    fwrite(&armio, sizeof(armio), 1, f);
    write_prop(f, "name", "arm-io", 7);
    write_prop(f, "device_type", "arm-io", 7);
    uint64_t ranges[3] = {0, 0xfe000000ULL, 0x02000000ULL};
    write_prop(f, "ranges", ranges, 24);

    // Child 3.1: uart0 (PL011)
    struct DeviceTreeNode uart0 = { 4, 0 };
    fwrite(&uart0, sizeof(uart0), 1, f);
    write_prop(f, "name", "uart0", 6);
    uint64_t uart0_reg[2] = {0x00201000ULL, 0x1000ULL};
    write_prop(f, "reg", uart0_reg, 16);
    {
        uint32_t phandle = 1;
        write_prop(f, "compatible", "arm,pl011", 10);
        write_prop(f, "AAPL,phandle", &phandle, sizeof(phandle));

        struct DeviceTreeNode gic = { 3, 0 };
        fwrite(&gic, sizeof(gic), 1, f);
        write_prop(f, "name", "gic", 4);
        write_prop(f, "compatible", "arm,gic-400", 12);
        uint64_t gic_reg[4] = { 0x01841000, 0x1000, 0x01842000, 0x2000 };
        write_prop(f, "reg", gic_reg, sizeof(gic_reg));

        struct DeviceTreeNode defaults = { 2, 0 };
        fwrite(&defaults, sizeof(defaults), 1, f);
        write_prop(f, "name", "defaults", 9);
        write_prop(f, "serial-device", &phandle, sizeof(phandle));
    }
    
    fclose(f);
    {
        FILE *layout = fopen("adt_layout.h", "w");
        if (!layout) { perror("fopen adt_layout.h"); return 1; }
        fprintf(layout, "/* Generated offsets into the serialized device tree. */\n"
                "#define ADT_SEED_OFFSET %ld\n#define ADT_SEED_SIZE 256\n"
                "#define ADT_FREQUENCY_OFFSET %ld\n#define ADT_VIDEO_OFFSET %ld\n#define ADT_RAMDISK_OFFSET %ld\n", seed_offset, frequency_offset, video_offset, ramdisk_offset);
        if (fclose(layout)) return 1;
    }
    return 0;
}
