#ifndef DISPLAYID2_H
#define DISPLAYID2_H

/**
 * Private header for the low-level DisplayID v2 API.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libdisplay-info/displayid2.h>

#include "cta.h"
#include "log.h"

/**
 * The maximum number of data blocks in a DisplayID v2 section.
 *
 * A DisplayID v2 section has a maximum payload size of 251 bytes (256 bytes
 * maximum size, 5 bytes header), and each data block has a minimum size of
 * 3 bytes.
 */
#define DISPLAYID2_MAX_DATA_BLOCKS 83

/**
 * The maximum number of CTA data blocks in a DisplayID v2 CTA-861 data block.
 *
 * A DisplayID v2 section has a maximum payload size of 251 bytes (256 bytes
 * maximum size, 5 bytes header), a DisplayID v2 data block has a maximum
 * payload size of 248 bytes (3 bytes header), and each CTA data block takes at
 * least 1 byte.
 */
#define DISPLAYID2_CTA861_MAX_DATA_BLOCKS 248

struct di_displayid2 {
	int revision;
	enum di_displayid2_product_primary_use_case product_primary_use_case;

	struct di_displayid2_data_block *data_blocks[DISPLAYID2_MAX_DATA_BLOCKS + 1];
	size_t data_blocks_len;

	struct di_logger *logger;
};

struct di_displayid2_data_block {
	enum di_displayid2_data_block_tag tag;

	/* Used for DI_DISPLAYID2_DATA_BLOCK_CTA861 */
	struct {
		/* NULL-terminated */
		struct di_cta_data_block *data_blocks[DISPLAYID2_CTA861_MAX_DATA_BLOCKS + 1];
		size_t data_blocks_len;
	} cta861;
};

bool
_di_displayid2_parse(struct di_displayid2 *displayid2, const uint8_t *data,
		     size_t size, struct di_logger *logger);

void
_di_displayid2_finish(struct di_displayid2 *displayid2);

#endif
