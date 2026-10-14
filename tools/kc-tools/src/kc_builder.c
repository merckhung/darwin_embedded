#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include "linker.h"
#include "plist.h"
#include "prelink.h"

static uint8_t *read_file(const char *path, size_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);

  uint8_t *buf = malloc((size_t)sz + 1);
  buf[sz] = 0;

  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (got != (size_t)sz) {
    free(buf);
    return NULL;
  }

  *out_size = (size_t)sz;
  return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t size) {
  FILE *f = fopen(path, "wb");
  if (!f) {
    perror(path);
    return -1;
  }

  int r = (fwrite(data, 1, size, f) == size) ? 0 : -1;

  fclose(f);
  return r;
}

typedef struct {
  char id[256];
  char bundle_path[512];
  char version[64];
  char exe_name[128];
  char *info_plist_xml;
  size_t info_plist_len;
  uint8_t *binary;
  size_t binary_size;
} KextBundle;

static int load_kext(const char *kext_dir, const char *prefix, KextBundle *out) {
  memset(out, 0, sizeof(*out));

  char path[700];
  snprintf(path, sizeof(path), "%s/Contents/Info.plist", kext_dir);
  out->info_plist_xml = (char *)read_file(path, &out->info_plist_len);

  if (!out->info_plist_xml) {
    snprintf(path, sizeof(path), "%s/Info.plist", kext_dir);
    out->info_plist_xml = (char *)read_file(path, &out->info_plist_len);
  }

  if (!out->info_plist_xml) {
    fprintf(stderr, "load_kext: no Info.plist in %s\n", kext_dir);
    return -1;
  }

  PlistNode *plist = plist_parse(out->info_plist_xml, out->info_plist_len);
  if (plist) {
    const char *bid = plist_dict_get_str(plist, "CFBundleIdentifier");
    if (bid)
      strncpy(out->id, bid, sizeof(out->id) - 1);

    const char *bver = plist_dict_get_str(plist, "CFBundleVersion");
    if (bver)
      strncpy(out->version, bver, sizeof(out->version) - 1);

    const char *bexe = plist_dict_get_str(plist, "CFBundleExecutable");
    if (bexe)
      strncpy(out->exe_name, bexe, sizeof(out->exe_name) - 1);

    plist_free(plist);
  }

  if (!out->exe_name[0]) {
    const char *base = strrchr(kext_dir, '/');
    base = base ? base + 1 : kext_dir;
    strncpy(out->exe_name, base, sizeof(out->exe_name) - 1);
    char *dot = strrchr(out->exe_name, '.');
    if (dot)
    *dot = '\0';
  }

  const char *bname = strrchr(kext_dir, '/');
  bname = bname ? bname + 1 : kext_dir;
  snprintf(out->bundle_path, sizeof(out->bundle_path), "%s/%s", prefix, bname);

  snprintf(path, sizeof(path), "%s/Contents/MacOS/%s", kext_dir, out->exe_name);
  out->binary = read_file(path, &out->binary_size);
  if (!out->binary) {
    snprintf(path, sizeof(path), "%s/%s", kext_dir, out->exe_name);
    out->binary = read_file(path, &out->binary_size);
  }
  if (!out->binary) {
    fprintf(stderr, "load_kext: no binary for %s in %s\n", out->exe_name, kext_dir);
    return -1;
  }

  fprintf(stderr, "loaded kext: %s v%s (%zu bytes)\n",
          out->id, out->version, out->binary_size);
  return 0;
}

static void usage(const char *prog) {
  fprintf(stderr,
    "Usage: %s -kernel <file> -kext <dir.kext> [...] -o <output>\n"
    "       [-codeless <dir.kext>] [-prefix /System/Library/Extensions]\n"
    "       [-kext-vmbase <hex>] [-kc-base <hex>]\n"
    "Build an x86_64 or ARM64 MH_FILESET collection for embedded Darwin.\n",
    prog);
}

#define MAX_KEXTS 64

int main(int argc, char *argv[]) {
  const char *kernel_path = NULL;
  const char *output_path = NULL;
  const char *prefix = "/System/Library/Extensions";
  const char *kext_paths[MAX_KEXTS];
  int num_kext_paths = 0;
  const char *codeless_paths[MAX_KEXTS];
  int num_codeless_paths = 0;
  uint64_t kext_vmbase = 0;
  uint64_t kc_base = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-kernel") && i + 1 < argc) {
      kernel_path = argv[++i];
    } else if (!strcmp(argv[i], "-kext") && i + 1 < argc) {
      if (num_kext_paths < MAX_KEXTS) kext_paths[num_kext_paths++] = argv[++i];
    } else if (!strcmp(argv[i], "-codeless") && i + 1 < argc) {
      if (num_codeless_paths < MAX_KEXTS) codeless_paths[num_codeless_paths++] = argv[++i];
    } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
      output_path = argv[++i];
    } else if (!strcmp(argv[i], "-prefix") && i + 1 < argc) {
      prefix = argv[++i];
    } else if (!strcmp(argv[i], "-kext-vmbase") && i + 1 < argc) {
      kext_vmbase = strtoull(argv[++i], NULL, 16);
    } else if (!strcmp(argv[i], "-kc-base") && i + 1 < argc) {
      kc_base = strtoull(argv[++i], NULL, 16);
    } else {
      fprintf(stderr, "unknown argument: %s\n", argv[i]);
      usage(argv[0]);
      return 1;
    }
  }
  if (!kernel_path || !output_path) { usage(argv[0]); return 1; }

  size_t kernel_size;
  uint8_t *kernel_data = read_file(kernel_path, &kernel_size);
  if (!kernel_data)
  return 1;
  fprintf(stderr, "kernel: %zu bytes\n", kernel_size);

  SymTable ksyms = {0};
  if (symtab_build_from_macho(&ksyms, kernel_data, kernel_size) != 0) {
    fprintf(stderr, "Failed to build kernel symbol table\n");
    return 1;
  }

  KextBundle kbundles[MAX_KEXTS];
  int nkexts = 0;
  for (int i = 0; i < num_kext_paths; i++) {
    if (load_kext(kext_paths[i], prefix, &kbundles[nkexts]) != 0)
      return 1;
    nkexts++;
  }

  if (num_kext_paths > 0 && nkexts == 0) {
    fprintf(stderr, "No kexts loaded\n");
    return 1;
  }

  // Compute kext placement base from the actual kernel VM layout.
  // -kext-vmbase overrides (useful for testing), otherwise derive from kernel.
  uint64_t kvm_end = macho_kernel_vm_end(kernel_data, kernel_size);
  kvm_end = (kvm_end + 0xFFFF) & ~(uint64_t)0xFFFF; // align to 64 KB

  uint64_t vmcursor = kext_vmbase ? kext_vmbase : kvm_end;

  /* arm64 must place the kexts *below* the kernel's __TEXT. arm_vm_init sizes
   * the gap it protects as (segTEXTB - (segPRELINKTEXTB + segSizePRELINKTEXT)),
   * which underflows to a ~garbage length if the collection payload sits above
   * the kernel. i386_init has no equivalent rule, so x86 keeps appending. */
  if (!kext_vmbase && kc_base) {
    /* Directly above the collection header page: the payload has to stay below
     * the kernel's __TEXT (arm_vm_init sizes the gap it protects as
     * segTEXTB - prelink_end) and packing it against the collection base keeps
     * the staged span small. */
    vmcursor = kc_base + 0x4000;
    fprintf(stderr, "kext placement: above collection header 0x%llx\n",
            (unsigned long long)kc_base);
    /* The payload must end below the kernel's __TEXT, or it overwrites the
     * kernel once loaded (seen as a silent hang after MMU enable). */
    if (macho_uses_arm_kc_layout(kernel_data, kernel_size)) {
      uint64_t ktext = macho_seg_vmaddr(kernel_data, kernel_size, "__TEXT");
      uint64_t total = 0;
      for (int i = 0; i < nkexts; i++)
        total += ALIGN_UP_64(macho_image_vm_extent(kbundles[i].binary,
                                                   kbundles[i].binary_size), 0x1000);
      if (ktext && vmcursor + total > ktext) {
        fprintf(stderr, "kext placement: payload 0x%llx bytes from 0x%llx runs "
                "over kernel __TEXT 0x%llx; lower -kc-base (and ARM64_KC_BASE)\n",
                (unsigned long long)total, (unsigned long long)vmcursor,
                (unsigned long long)ktext);
        return 1;
      }
    }
  } else if (!kext_vmbase && macho_uses_arm_kc_layout(kernel_data, kernel_size)) {
    uint64_t ktext = macho_seg_vmaddr(kernel_data, kernel_size, "__TEXT");
    uint64_t total = 0;
    for (int i = 0; i < nkexts; i++)
      total += ALIGN_UP_64(macho_image_vm_extent(kbundles[i].binary,
                                                 kbundles[i].binary_size), 0x1000);
    /* Room for the prelink info/XML that prelink_assemble places after the
     * kexts, and slack so a rounding difference cannot push into __TEXT. */
    total = ALIGN_UP_64(total + (total / 4) + 0x100000, 0x10000);
    if (ktext > total) {
      vmcursor = (ktext - total) & ~(uint64_t)0xFFFF;
      fprintf(stderr, "kext placement: below kernel __TEXT 0x%llx, "
              "reserving 0x%llx\n", (unsigned long long)ktext,
              (unsigned long long)total);
    } else {
      fprintf(stderr, "kext placement: WARNING kernel __TEXT 0x%llx leaves no "
              "room for 0x%llx of kexts; falling back above the kernel\n",
              (unsigned long long)ktext, (unsigned long long)total);
    }
  }
  fprintf(stderr, "kext placement start: 0x%llx\n", (unsigned long long)vmcursor);

  LinkedKext linked[MAX_KEXTS];
  int nlinked = 0;

  // Link kexts in order. After each link, merge its exported symbols into
  // ksyms so subsequent kexts can resolve inter-kext dependencies.
  for (int i = 0; i < nkexts; i++) {
    KextBundle *kb = &kbundles[i];
    vmcursor = (vmcursor + 0xFFF) & ~(uint64_t)0xFFF;

    LinkedKext *lk = &linked[nlinked];
    memset(lk, 0, sizeof(*lk));
    strncpy(lk->bundle_id,   kb->id,         sizeof(lk->bundle_id) - 1);
    strncpy(lk->bundle_path, kb->bundle_path, sizeof(lk->bundle_path) - 1);
    strncpy(lk->version,     kb->version,     sizeof(lk->version) - 1);
    lk->info_plist_xml = kb->info_plist_xml;
    lk->info_plist_len = kb->info_plist_len;

    if (link_kext(kb->binary, kb->binary_size, &ksyms, vmcursor, lk) != 0) {
      fprintf(stderr, "link_kext failed for required extension %s\n", kb->id);
      return 1;
    }
    patch_kmod_info(lk);
    symtab_merge_from_linked(&ksyms, lk);
    // Space by what the kext occupies once mapped, not by its file size: a
    // segment with vmsize > filesize (__bss, __LINKEDIT padding) runs past the
    // end of the file image, so linked_size lets this kext's last segment
    // overlap the next kext's header.
    uint64_t occupied = lk->vm_extent ? lk->vm_extent : lk->linked_size;
    vmcursor += (occupied + 0xFFF) & ~(uint64_t)0xFFF;
    nlinked++;
  }

  // A kernel-only KC (nlinked == 0) is allowed for bring-up validation.
  if (nkexts > 0 && nlinked == 0) {
    fprintf(stderr, "No kexts linked\n");
    return 1;
  }

  // Codeless kexts (com.apple.kpi.* KPI pseudo-kexts, ApplePlatformFamily, ...):
  // Info.plist only, no executable. They must appear in _PrelinkInfoDictionary
  // so OSKext::resolveDependencies() can satisfy real kexts' OSBundleLibraries
  // (e.g. AppleI386GenericPlatform needs com.apple.kpi.iokit); without them the
  // real kext fails to load on demand and its IOKit personality never matches.
  for (int i = 0; i < num_codeless_paths; i++) {
    KextBundle kb;
    // load_kext requires a binary; for codeless kexts we only need the plist.
    memset(&kb, 0, sizeof(kb));
    char path[700];
    snprintf(path, sizeof(path), "%s/Contents/Info.plist", codeless_paths[i]);
    kb.info_plist_xml = (char *)read_file(path, &kb.info_plist_len);
    if (!kb.info_plist_xml) {
      snprintf(path, sizeof(path), "%s/Info.plist", codeless_paths[i]);
      kb.info_plist_xml = (char *)read_file(path, &kb.info_plist_len);
    }
    if (!kb.info_plist_xml) {
      fprintf(stderr, "codeless: no Info.plist in %s\n", codeless_paths[i]);
      return 1;
    }
    PlistNode *pl = plist_parse(kb.info_plist_xml, kb.info_plist_len);
    const char *bid = pl ? plist_dict_get_str(pl, "CFBundleIdentifier") : NULL;
    const char *bver = pl ? plist_dict_get_str(pl, "CFBundleVersion") : NULL;

    LinkedKext *lk = &linked[nlinked];
    memset(lk, 0, sizeof(*lk));
    lk->codeless = 1;
    if (bid) strncpy(lk->bundle_id, bid, sizeof(lk->bundle_id) - 1);
    if (bver) strncpy(lk->version, bver, sizeof(lk->version) - 1);
    const char *bname = strrchr(codeless_paths[i], '/');
    bname = bname ? bname + 1 : codeless_paths[i];
    snprintf(lk->bundle_path, sizeof(lk->bundle_path), "%s/%s", prefix, bname);
    lk->info_plist_xml = kb.info_plist_xml;
    lk->info_plist_len = kb.info_plist_len;
    if (pl) plist_free(pl);
    fprintf(stderr, "codeless kext: %s v%s\n", lk->bundle_id, lk->version);
    nlinked++;
  }

  uint8_t *out_data = NULL;
  size_t out_size = 0;
  if (fileset_assemble(kernel_data, kernel_size, linked, (size_t)nlinked, kc_base,
                       &out_data, &out_size) != 0) {
    fprintf(stderr, "fileset_assemble failed\n");
    return 1;
  }

  if (write_file(output_path, out_data, out_size) != 0)
    return 1;
  fprintf(stderr, "wrote %s (%zu bytes)\n", output_path, out_size);

  free(out_data);
  free(kernel_data);
  symtab_free(&ksyms);
  return 0;
}
