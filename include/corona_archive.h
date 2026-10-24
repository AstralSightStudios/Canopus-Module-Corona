#ifndef CORONA_ARCHIVE_H
#define CORONA_ARCHIVE_H
#include <stdint.h>
struct canopus_context_v1;
struct rh_archive;
/* The parent of staging_path must exist. staging_path itself must be absent:
 * extraction never reuses a directory containing untrusted or stale files.
 * Destruction closes resources; the caller owns cleanup of partial staging.
 * ZIP path components are limited to CANOPUS_FS_NAME_MAX (63 UTF-8 bytes):
 * longer names return LIMIT before their entry is created, because the SDK's
 * directory enumerator cannot represent them without truncation. Expanded
 * native paths are additionally limited to 255 UTF-8 bytes.
 * All calls run on the serialized module owner. No manifest schema or registry
 * mutation occurs here. Only public FS/MEM services are used. */
int rh_archive_create(const struct canopus_context_v1 *ctx,
                      const char *archive_path, uint32_t archive_bytes,
                      const char *staging_path, struct rh_archive **out);
/* 0: bounded work remains; 1: all entries, CRCs and the unique root manifest
 * have passed. Negative protocol codes: IO=-5, PACK=-7, LIMIT=-8,
 * UNSUPPORTED=-11. Errors latch until destruction. */
int rh_archive_step(struct rh_archive *a);
void rh_archive_destroy(struct rh_archive *a);
/* Available only after step returned 1. Copies the absolute manifest path. */
int rh_archive_manifest(struct rh_archive *a, char out[256]);
#endif
