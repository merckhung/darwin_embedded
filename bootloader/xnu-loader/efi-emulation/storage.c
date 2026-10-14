/* FAT32 boot volume over legacy IDE (ATA PIO on the first legacy channel/drive
 * holding a partition table): the fallback when the boot protocol supplied no modules. */
#include "efi_emulation.h"
#include "serial.h"
#include <efilib.h>

#if defined(__aarch64__) || defined(__riscv)
/* No firmware disk on the arm Linux-image path: modules are the only volume */
void efiemu_bios_disk_set(UINT32 drive, EfiEmuBiosRead read) {
  (void)drive;
  (void)read;
}
EFI_STATUS efiemu_disk_init(void) { return EFI_NOT_FOUND; }
EFI_HANDLE efiemu_disk_handle(void) { return NULL; }
EFI_STATUS efiemu_disk_protocol(EFI_GUID *guid, VOID **out) {
  (void)guid;
  (void)out;
  return EFI_UNSUPPORTED;
}
#else

#define FAT_LBA 2048U
#define ATA_DATA ata_base
#define ATA_STATUS (UINT16)(ata_base + 7)
#define ATA_DRQ 0x08
#define ATA_ERR 0x01

typedef struct __attribute__((packed)) {
  UINT8 jump[3], oem[8];
  UINT16 bytes_sector;
  UINT8 sectors_cluster;
  UINT16 reserved;
  UINT8 fats;
  UINT16 root_entries, total16;
  UINT8 media;
  UINT16 fat16, sectors_track, heads;
  UINT32 hidden, total32, fat32;
  UINT16 flags, version;
  UINT32 root_cluster;
} FatBpb;

typedef struct {
  EFI_FILE_PROTOCOL proto;
  UINT32 cluster;
  UINT32 size;
  UINT32 position;
  BOOLEAN directory;
  BOOLEAN allocated;
} LegacyFile;

static FatBpb bpb;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs;
static EFI_BLOCK_IO_PROTOCOL block_io;
static EFI_BLOCK_IO_MEDIA block_media;
static LegacyFile root;
static LegacyFile files[12];
static EFI_HANDLE volume_handle = (EFI_HANDLE)(UINTN)0x46415432;
static EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID file_info_guid = EFI_FILE_INFO_ID;
static BOOLEAN storage_ready;
/* First sector of the FAT32 boot volume: GPT images use the conventional 2048,
 * MBR images take it from the partition entry. */
static UINT32 fat_lba = FAT_LBA;
/* The boot disk need not be the primary master (USB emulation, slave, secondary). */
static UINT16 ata_base = 0x1f0;
static UINT8 ata_drive = 0xe0;

static BOOLEAN guid_equal_local(const EFI_GUID *a, const EFI_GUID *b) {
  const UINT64 *x = (const UINT64 *)a, *y = (const UINT64 *)b;
  return x[0] == y[0] && x[1] == y[1];
}

static UINT8 in8(UINT16 p) {
  UINT8 v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
  return v;
}

static void out8(UINT16 p, UINT8 v) {
  __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}

static EFI_STATUS ata_read(UINT32 lba, UINT8 *dst) {
  for (UINTN spin = 0; spin < 1000000; ++spin)
    if (!(in8(ATA_STATUS) & 0x80))
      break;
  out8(ata_base + 6, ata_drive | ((lba >> 24) & 0x0f));
  /* ~400 ns for the drive select to settle: four alternate-status reads. */
  for (UINTN i = 0; i < 4; ++i)
    (void)in8(ata_base + 0x206);
  if (in8(ATA_STATUS) == 0xff)
    return EFI_NOT_FOUND;               /* floating bus: no device here */
  out8(ata_base + 2, 1);
  out8(ata_base + 3, lba);
  out8(ata_base + 4, lba >> 8);
  out8(ata_base + 5, lba >> 16);
  out8(ATA_STATUS, 0x20);
  UINT8 status = 0;
  for (UINTN spin = 0; spin < 1000000; ++spin) {
    status = in8(ATA_STATUS);
    if (status & ATA_ERR)
      return EFI_DEVICE_ERROR;
    if (!(status & 0x80) && (status & ATA_DRQ))
      break;
  }
  if (!(status & ATA_DRQ))
    return EFI_TIMEOUT;
  UINTN words = 256;
  __asm__ volatile("cld; rep insw"
                   : "+D"(dst), "+c"(words)
                   : "d"((UINT16)ATA_DATA)
                   : "memory");
  return EFI_SUCCESS;
}

/* BIOS path (USB and anything else only the BIOS can reach): two 64-sector
 * read-ahead windows, so FAT lookups and file data do not evict each other. */
static EfiEmuBiosRead bios_read;
static UINT32 bios_drive;
typedef struct {
  UINT32 lba, count, age;
  UINT8 data[EFIEMU_BIOS_SECTORS * 512];
} BiosWindow;
static BiosWindow bios_windows[2];
static UINT32 bios_clock;
static BOOLEAN use_bios;

void efiemu_bios_disk_set(UINT32 drive, EfiEmuBiosRead read) {
  bios_drive = drive;
  bios_read = read;
}

static EFI_STATUS bios_sector(UINT32 lba, UINT8 *dst) {
  BiosWindow *w = &bios_windows[0];
  for (UINTN i = 0; i < 2; ++i) {
    BiosWindow *c = &bios_windows[i];
    if (c->count && lba >= c->lba && lba - c->lba < c->count) {
      c->age = ++bios_clock;
      CopyMem(dst, c->data + (lba - c->lba) * 512, 512);
      return EFI_SUCCESS;
    }
    if (c->age < w->age)
      w = c;
  }
  UINT32 count = EFIEMU_BIOS_SECTORS;
  if (block_media.LastBlock && lba <= block_media.LastBlock &&
      block_media.LastBlock - lba + 1 < count)
    count = (UINT32)(block_media.LastBlock - lba + 1);
  w->count = 0;
  /* A read past the end of the disk fails as a whole; retry one sector. */
  if (bios_read(lba, count) != 0 && (count == 1 || bios_read(lba, count = 1) != 0))
    return EFI_DEVICE_ERROR;
  CopyMem(w->data, (VOID *)EFIEMU_BIOS_BOUNCE, count * 512);
  w->lba = lba;
  w->count = count;
  w->age = ++bios_clock;
  CopyMem(dst, w->data, 512);
  return EFI_SUCCESS;
}

static EFI_STATUS disk_read(UINT32 lba, UINT8 *dst) {
  return use_bios ? bios_sector(lba, dst) : ata_read(lba, dst);
}

static EFI_STATUS EFIAPI block_reset(EFI_BLOCK_IO_PROTOCOL *self,
                                     BOOLEAN extended_verification) {
  (void)self;
  (void)extended_verification;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI block_read(EFI_BLOCK_IO_PROTOCOL *self,
                                    UINT32 media_id, EFI_LBA lba,
                                    UINTN size, VOID *buffer) {
  if (!self || !buffer || media_id != block_media.MediaId)
    return EFI_INVALID_PARAMETER;
  if ((size % 512) != 0 || lba > block_media.LastBlock ||
      size / 512 > block_media.LastBlock - lba + 1)
    return EFI_BAD_BUFFER_SIZE;

  UINT8 *dst = buffer;
  while (size) {
    EFI_STATUS status = disk_read((UINT32)lba, dst);
    if (EFI_ERROR(status))
      return status;
    ++lba;
    dst += 512;
    size -= 512;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI block_write(EFI_BLOCK_IO_PROTOCOL *self,
                                     UINT32 media_id, EFI_LBA lba,
                                     UINTN size, VOID *buffer) {
  (void)self;
  (void)media_id;
  (void)lba;
  (void)size;
  (void)buffer;
  return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI block_flush(EFI_BLOCK_IO_PROTOCOL *self) {
  (void)self;
  return EFI_SUCCESS;
}

static UINT32 first_data(void) {
  return fat_lba + bpb.reserved + bpb.fats * bpb.fat32;
}

static UINT32 cluster_lba(UINT32 c) {
  return first_data() + (c - 2) * bpb.sectors_cluster;
}

static UINT32 next_cluster(UINT32 c) {
  UINT8 sec[512];
  UINT32 off = c * 4;
  if (EFI_ERROR(disk_read(fat_lba + bpb.reserved + off / 512, sec)))
    return 0x0fffffff;
  return (*(UINT32 *)(sec + off % 512)) & 0x0fffffff;
}

static void short_name(CONST CHAR16 *component, UINTN len, CHAR8 out[11]) {
  for (UINTN i = 0; i < 11; ++i)
    out[i] = ' ';
  /* Stable aliases installed by the image builder for names that need LFNs. */
  if (len == 13) {
    CONST CHAR16 alias[] = L"BOOTARGS.TXT";
    component = alias;
    len = 12;
  }
  UINTN at = 0, ext = 8;
  for (UINTN i = 0; i < len; ++i) {
    CHAR16 c = component[i];
    if (c == '.') {
      at = ext;
      continue;
    }
    if (at >= 11)
      continue;
    if (c >= 'a' && c <= 'z')
      c -= 32;
    out[at++] = (CHAR8)c;
  }
}

static EFI_STATUS find_entry(UINT32 dir_cluster, CONST CHAR16 *name, UINTN len,
                             UINT32 *cluster, UINT32 *size, BOOLEAN *directory) {
  CHAR8 wanted[11];
  short_name(name, len, wanted);
  UINT8 sec[512];
  for (UINT32 c = dir_cluster; c >= 2 && c < 0x0ffffff8; c = next_cluster(c)) {
    for (UINTN s = 0; s < bpb.sectors_cluster; ++s) {
      EFI_STATUS st = disk_read(cluster_lba(c) + s, sec);
      if (EFI_ERROR(st))
        return st;
      for (UINTN off = 0; off < 512; off += 32) {
        UINT8 *e = sec + off;
        if (e[0] == 0)
          return EFI_NOT_FOUND;
        if (e[0] == 0xe5 || e[11] == 0x0f || (e[11] & 8))
          continue;
        BOOLEAN match = TRUE;
        for (UINTN i = 0; i < 11; ++i)
          if ((CHAR8)e[i] != wanted[i])
            match = FALSE;
        if (!match)
          continue;
        *cluster = ((UINT32) * (UINT16 *)(e + 20) << 16) | *(UINT16 *)(e + 26);
        *size = *(UINT32 *)(e + 28);
        *directory = (e[11] & 0x10) != 0;
        return EFI_SUCCESS;
      }
    }
  }
  return EFI_NOT_FOUND;
}

static LegacyFile *new_file(void) {
  for (UINTN i = 0; i < 12; ++i)
    if (!files[i].allocated) {
      files[i].allocated = TRUE;
      return &files[i];
    }
  return NULL;
}

static EFI_STATUS EFIAPI file_close(EFI_FILE_PROTOCOL *p) {
  LegacyFile *f = (LegacyFile *)p;
  if (f != &root)
    f->allocated = FALSE;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_open(EFI_FILE_PROTOCOL *p, EFI_FILE_PROTOCOL **out,
                                   CHAR16 *path, UINT64 mode, UINT64 attrs) {
  (void)p;
  (void)attrs;
  if (!out || mode != EFI_FILE_MODE_READ)
    return EFI_UNSUPPORTED;
  UINT32 c = bpb.root_cluster, size = 0;
  BOOLEAN dir = TRUE;
  CHAR16 *q = path;
  while (*q == '\\' || *q == '/')
    ++q;
  while (*q) {
    CHAR16 *start = q;
    while (*q && *q != '\\' && *q != '/')
      ++q;
    EFI_STATUS s = find_entry(c, start, q - start, &c, &size, &dir);
    if (EFI_ERROR(s))
      return s;
    while (*q == '\\' || *q == '/')
      ++q;
  }
  LegacyFile *f = new_file();
  if (!f)
    return EFI_OUT_OF_RESOURCES;
  f->cluster = c;
  f->size = size;
  f->position = 0;
  f->directory = dir;
  *out = &f->proto;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_read(EFI_FILE_PROTOCOL *p, UINTN *amount, VOID *buffer) {
  LegacyFile *f = (LegacyFile *)p;
  if (!amount || !buffer || f->directory)
    return EFI_UNSUPPORTED;
  UINTN want = *amount;
  if (want > f->size - f->position)
    want = f->size - f->position;
  UINTN done = 0;
  UINT8 sec[512];
  UINT32 c = f->cluster;
  UINT32 skip = f->position / (512 * bpb.sectors_cluster);
  while (skip--)
    c = next_cluster(c);
  UINTN within = f->position % (512 * bpb.sectors_cluster);
  while (done < want && c < 0x0ffffff8) {
    for (UINTN s = within / 512; s < bpb.sectors_cluster && done < want; ++s) {
      EFI_STATUS st = disk_read(cluster_lba(c) + s, sec);
      if (EFI_ERROR(st))
        return st;
      UINTN o = within % 512, n = 512 - o;
      if (n > want - done)
        n = want - done;
      for (UINTN i = 0; i < n; ++i)
        ((UINT8 *)buffer)[done + i] = sec[o + i];
      done += n;
      within = 0;
    }
    c = next_cluster(c);
  }
  f->position += done;
  *amount = done;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_get_info(EFI_FILE_PROTOCOL *p, EFI_GUID *type,
                                       UINTN *amount, VOID *buffer) {
  LegacyFile *f = (LegacyFile *)p;
  if (!guid_equal_local(type, &file_info_guid))
    return EFI_UNSUPPORTED;
  UINTN need = SIZE_OF_EFI_FILE_INFO + 2;
  if (!buffer || *amount < need) {
    *amount = need;
    return EFI_BUFFER_TOO_SMALL;
  }
  EFI_FILE_INFO *i = buffer;
  for (UINTN n = 0; n < need; ++n)
    ((UINT8 *)i)[n] = 0;
  i->Size = need;
  i->FileSize = f->size;
  i->PhysicalSize = f->size;
  i->Attribute = f->directory ? EFI_FILE_DIRECTORY : EFI_FILE_READ_ONLY;
  *amount = need;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,
                                     EFI_FILE_PROTOCOL **out) {
  (void)self;
  root.position = 0;
  *out = &root.proto;
  return EFI_SUCCESS;
}

static void init_proto(LegacyFile *f) {
  f->proto.Revision = EFI_FILE_PROTOCOL_REVISION;
  f->proto.Open = file_open;
  f->proto.Close = file_close;
  f->proto.Read = file_read;
  f->proto.GetInfo = file_get_info;
}

static void put_hex8(UINT8 v) {
  CHAR8 b[4] = { "0123456789abcdef"[v >> 4], "0123456789abcdef"[v & 15], ' ', 0 };
  serial_puts8(b);
}

/* Pick the first legacy IDE position whose disk carries a GPT or MBR signature;
 * log what every position returned so a miss can be diagnosed from serial. */
static EFI_STATUS select_boot_disk(UINT8 *sec) {
  static CONST UINT16 bases[] = { 0x1f0, 0x1f0, 0x170, 0x170 };
  static CONST UINT8 drives[] = { 0xe0, 0xf0, 0xe0, 0xf0 };
  static CONST CHAR8 *names[] = { "primary master", "primary slave",
                                  "secondary master", "secondary slave" };
  UINT8 lba1[512];

  if (bios_read) {
    use_bios = TRUE;
    EFI_STATUS b = disk_read(1, lba1);
    if (!EFI_ERROR(b))
      b = disk_read(0, sec);
    serial_puts8((CONST CHAR8 *)"efi-emulation: disk: BIOS drive ");
    put_hex8((UINT8)bios_drive);
    if (!EFI_ERROR(b) &&
        (CompareMem(lba1, "EFI PART", 8) == 0 || (sec[510] == 0x55 && sec[511] == 0xaa))) {
      serial_puts8((CONST CHAR8 *)"has a partition table\r\n");
      CopyMem(sec, lba1, 512);
      return EFI_SUCCESS;
    }
    serial_puts8((CONST CHAR8 *)(EFI_ERROR(b) ? "read failed\r\n" : "has no GPT or MBR\r\n"));
    use_bios = FALSE;
  }
  for (UINTN i = 0; i < 4; ++i) {
    ata_base = bases[i];
    ata_drive = drives[i];
    EFI_STATUS s1 = ata_read(1, lba1);
    EFI_STATUS s0 = EFI_ERROR(s1) ? s1 : ata_read(0, sec);
    serial_puts8((CONST CHAR8 *)"efi-emulation: disk: ");
    serial_puts8((CONST CHAR8 *)names[i]);
    if (EFI_ERROR(s0)) {
      serial_puts8((CONST CHAR8 *)(s0 == EFI_NOT_FOUND ? ": absent\r\n" : ": read failed\r\n"));
      continue;
    }
    serial_puts8((CONST CHAR8 *)": lba0 ");
    for (UINTN k = 0; k < 8; ++k)
      put_hex8(sec[k]);
    serial_puts8((CONST CHAR8 *)"sig ");
    put_hex8(sec[510]);
    put_hex8(sec[511]);
    serial_puts8((CONST CHAR8 *)"lba1 ");
    for (UINTN k = 0; k < 8; ++k)
      put_hex8(lba1[k]);
    serial_puts8((CONST CHAR8 *)"\r\n");
    if (CompareMem(lba1, "EFI PART", 8) == 0 || (sec[510] == 0x55 && sec[511] == 0xaa)) {
      CopyMem(sec, lba1, 512);
      return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND;
}

EFI_STATUS efiemu_disk_init(void) {
  UINT8 sec[512];
  UINT64 last_block = 0;
  EFI_STATUS s = select_boot_disk(sec);
  if (EFI_ERROR(s)) {
    serial_puts8((CONST CHAR8 *)"efi-emulation: disk: no legacy IDE disk has a GPT or MBR; "
                 "the controller must be in IDE/legacy mode\r\n");
    return s;
  }
  if (CompareMem(sec, "EFI PART", 8) == 0) {
    for (UINTN i = 0; i < 8; ++i)
      last_block |= (UINT64)sec[32 + i] << (i * 8);
  } else {
    /* No GPT: use the MBR partition table at LBA 0 and boot from the first
     * FAT32 or EFI System entry it lists. */
    s = disk_read(0, sec);
    if (EFI_ERROR(s)) {
      serial_puts8((CONST CHAR8 *)"efi-emulation: disk: ATA read of LBA 0 failed\r\n");
      return s;
    }
    if (sec[510] != 0x55 || sec[511] != 0xaa) {
      serial_puts8((CONST CHAR8 *)"efi-emulation: disk: no GPT header and no MBR "
                   "signature\r\n");
      return EFI_UNSUPPORTED;
    }
    UINT32 part_lba = 0;
    for (UINTN i = 0; i < 4; ++i) {
      CONST UINT8 *e = sec + 446 + i * 16;
      UINT32 start = (UINT32)e[8] | ((UINT32)e[9] << 8) |
                     ((UINT32)e[10] << 16) | ((UINT32)e[11] << 24);
      UINT32 count = (UINT32)e[12] | ((UINT32)e[13] << 8) |
                     ((UINT32)e[14] << 16) | ((UINT32)e[15] << 24);

      if ((UINT64)start + count > last_block)
        last_block = (UINT64)start + count;
      if (part_lba == 0 && start != 0 &&
          (e[4] == 0x0b || e[4] == 0x0c || e[4] == 0x0e || e[4] == 0xef))
        part_lba = start;
    }
    /* No partition entry: a bare FAT32 at LBA 2048 (legacy-boot.img). */
    if (part_lba == 0 && !EFI_ERROR(disk_read(2048, sec)) &&
        CompareMem(sec + 82, "FAT32   ", 8) == 0)
      part_lba = 2048;
    if (part_lba == 0) {
      serial_puts8((CONST CHAR8 *)"efi-emulation: disk: MBR has no FAT32 or EFI System "
                   "partition\r\n");
      return EFI_UNSUPPORTED;
    }
    fat_lba = part_lba;
    if (last_block != 0)
      last_block -= 1;
  }

  SetMem(&block_media, sizeof(block_media), 0);
  block_media.MediaId = 1;
  block_media.MediaPresent = TRUE;
  block_media.ReadOnly = TRUE;
  block_media.BlockSize = 512;
  block_media.IoAlign = 1;
  block_media.LastBlock = last_block;

  SetMem(&block_io, sizeof(block_io), 0);
  block_io.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
  block_io.Media = &block_media;
  block_io.Reset = block_reset;
  block_io.ReadBlocks = block_read;
  block_io.WriteBlocks = block_write;
  block_io.FlushBlocks = block_flush;

  s = disk_read(fat_lba, sec);
  if (EFI_ERROR(s)) {
    serial_puts8((CONST CHAR8 *)"efi-emulation: disk: ATA read of the ESP failed\r\n");
    return s;
  }
  for (UINTN i = 0; i < sizeof(bpb); ++i)
    ((UINT8 *)&bpb)[i] = sec[i];
  if (bpb.bytes_sector != 512 || bpb.fat32 == 0) {
    serial_puts8((CONST CHAR8 *)"efi-emulation: disk: ESP is not FAT32 with 512-byte "
                 "sectors\r\n");
    return EFI_UNSUPPORTED;
  }
  fs.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
  fs.OpenVolume = open_volume;
  init_proto(&root);
  root.cluster = bpb.root_cluster;
  root.directory = TRUE;
  root.allocated = TRUE;
  for (UINTN i = 0; i < 12; ++i)
    init_proto(&files[i]);
  storage_ready = TRUE;
  return EFI_SUCCESS;
}

EFI_HANDLE efiemu_disk_handle(void) {
  return volume_handle;
}

EFI_STATUS efiemu_disk_protocol(EFI_GUID *guid, VOID **out) {
  if (!guid || !out)
    return EFI_INVALID_PARAMETER;
  /* Init returns early on unsupported media, leaving these tables zeroed; the
   * loader would otherwise call straight through a null OpenVolume. */
  if (!storage_ready)
    return EFI_UNSUPPORTED;
  if (guid_equal_local(guid, &fs_guid)) {
    *out = &fs;
    return EFI_SUCCESS;
  }
  if (guid_equal_local(guid, &block_io_guid)) {
    *out = &block_io;
    return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}
#endif
