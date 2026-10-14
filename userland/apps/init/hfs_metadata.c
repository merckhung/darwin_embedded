/* Host-only metadata editing through the imported HFS catalog implementation. */
#include <hfs/hfsplus.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char endianness;

static unsigned number(const char *value, unsigned base)
{
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, base);
    if (errno || !*value || *end || parsed > UINT_MAX) {
        fprintf(stderr, "invalid HFS metadata number\n");
        exit(1);
    }
    return (unsigned)parsed;
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 6) {
        fprintf(stderr, "usage: hfs_metadata image path [uid gid mode]\n");
        return 1;
    }
    unsigned short word = 1;
    endianness = *(unsigned char *)&word ? IS_LITTLE_ENDIAN : IS_BIG_ENDIAN;
    io_func *io = openFlatFile(argv[1]);
    if (!io) return 1;
    Volume *volume = openVolume(io);
    if (!volume) { CLOSE(io); return 1; }
    int result = 1;
    if (argc == 6 && (!chownFile(argv[2], number(argv[3], 10), number(argv[4], 10), volume) ||
                      !chmodFile(argv[2], number(argv[5], 8), volume)))
        goto done;
    HFSPlusCatalogRecord *record = getRecordFromPath3(argv[2], volume, NULL, NULL, TRUE, FALSE, kHFSRootFolderID);
    if (!record) goto done;
    HFSPlusBSDInfo *info = record->recordType == kHFSPlusFolderRecord ?
        &((HFSPlusCatalogFolder *)record)->permissions : record->recordType == kHFSPlusFileRecord ?
        &((HFSPlusCatalogFile *)record)->permissions : NULL;
    if (info) {
        printf("{\"uid\":%u,\"gid\":%u,\"mode\":%u}\n", info->ownerID, info->groupID, info->fileMode);
        result = 0;
    }
    free(record);
done:
    closeVolume(volume);
    CLOSE(io);
    return result;
}
