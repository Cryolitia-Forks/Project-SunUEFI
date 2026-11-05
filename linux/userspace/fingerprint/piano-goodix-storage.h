// SPDX-License-Identifier: BSD-3-Clause
#ifndef PIANO_GOODIX_STORAGE_H
#define PIANO_GOODIX_STORAGE_H

struct goodix_storage;

/* Diagnostic FS/GPFS services: empty, explicit Linux directory; no TA identity. */
int goodix_storage_open(const char *directory, struct goodix_storage **storage);
void goodix_storage_close(struct goodix_storage *storage);

#endif
