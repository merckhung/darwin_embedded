#ifndef DEVTREE_H
#define DEVTREE_H

#include "common.h"
#include "app.h"
#include "lowmem.h"

#define DT_MAX_NAME 32
#define DT_FIXED_SIZE (EFI_PAGE_SIZE)

typedef struct DeviceTreeNode {
  UINT32 nProperties;
  UINT32 nChildren;
  struct DeviceTreeNodeProperty **properties;
  struct DeviceTreeNode **children;
} DeviceTreeNode;

typedef struct DeviceTreeNodeProperty {
  CHAR8 name[DT_MAX_NAME];
  UINT32 length;
  VOID *value;
} DeviceTreeNodeProperty;

DeviceTreeNode *dt_create_node(AppContext *ctx);
DeviceTreeNodeProperty *dt_create_property(AppContext *ctx, const CHAR8 *name, VOID *data, UINT32 len);

VOID dt_add_property(AppContext *ctx, DeviceTreeNode *node, DeviceTreeNodeProperty *prop);
VOID dt_add_child(AppContext *ctx, DeviceTreeNode *parent, DeviceTreeNode *child);

UINT32 dt_flatten_node(DeviceTreeNode *node, UINT8 *buf);

EFI_STATUS dt_build(
    AppContext *ctx,
    VOID **out_blob,
    UINT32 *out_size,
    LowMemBuffer *out_buf,
    const CHAR8 *boot_args,
    UINT64 rt_table_phys);   /* physical addr of EFI_RUNTIME_SERVICES copy in conventional memory */

#if defined(__riscv) || defined(__aarch64__)
// src/devtree-fdt.c: the fdt's nodes in the kernel's format, cells as native words
EFI_STATUS dt_import_fdt(AppContext *ctx, DeviceTreeNode *root, DeviceTreeNode *chosen);
BOOLEAN dt_riscv_fdt_is_qemu(AppContext *ctx);
UINT32 dt_flat_size(DeviceTreeNode *node);
#endif

#endif
