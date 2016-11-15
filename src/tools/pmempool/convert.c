/*
 * Copyright 2014-2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * convert.c -- pmempool convert command source file
 */

#include <stdio.h>
#include <libgen.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <endian.h>
#include "common.h"
#include "set.h"
#include "libpmem.h"
#include "convert.h"

static const char *help_str = "";

/*
 * print_usage -- print application usage short description
 */
static void
print_usage(char *appname)
{
	printf("Usage: %s convert <file>\n", appname);
}

/*
 * print_version -- print version string
 */
static void
print_version(char *appname)
{
	printf("%s %s\n", appname, SRCVERSION);
}

/*
 * pmempool_convert_help -- print help message for convert command
 */
void
pmempool_convert_help(char *appname)
{
	print_usage(appname);
	print_version(appname);
	printf(help_str, appname);
}

typedef int (*convert_func)(void *poolset, void *addr);

/*
 * Collection of pool converting functions. Each array index is used as a
 * source version.
 */
static convert_func version_convert[] = {
	NULL, /* from version 0 to version 1 - does not exist */
	convert_v1_v2, /* from v1 to v2 */
};

/*
 * pmempool_convert_persist -- calls the appropriate persist func for poolset
 */
void
pmempool_convert_persist(void *poolset, const void *addr, size_t len)
{
	pool_set_file_persist(poolset, addr, len);
}

/*
 * pmempool_convert_func -- main function for convert command
 */
int
pmempool_convert_func(char *appname, int argc, char *argv[])
{
	if (argc != 2) {
		print_usage(appname);

		return -1;
	}

	int ret = 0;
	const char *f = argv[1];

	struct pmem_pool_params params;
	if (pmem_pool_parse_params(f, &params, 1)) {
		fprintf(stderr, "Cannot determine type of pool.\n");
		return -1;
	}

	if (params.is_part) {
		fprintf(stderr, "Conversion cannot be performed on "
			"a poolset part.\n");
		return -1;
	}

	if (params.type != PMEM_POOL_TYPE_OBJ) {
		fprintf(stderr, "Conversion is currently supported only for "
				"pmemobj pools.\n");
		return -1;
	}

	struct pool_set_file *psf = pool_set_file_open(f, 0, 1);
	if (psf == NULL) {
		perror(f);
		return -1;
	}

	if (psf->poolset->remote) {
		fprintf(stderr, "Conversion of remotely replicated  pools is "
			"currently not supported. Remove the replica first\n");
		return -1;
	}

	void *addr = pool_set_file_map(psf, 0);
	if (addr == NULL) {
		perror(f);
		ret = -1;
		goto out;
	}

	struct pool_hdr *phdr = addr;
	uint32_t m = le32toh(phdr->major);
	if (m >= COUNT_OF(version_convert) || !version_convert[m]) {
		fprintf(stderr, "There's no conversion method for the pool.\n"
				"Please make sure the pmempool utility "
				"is up-to-date.\n");
		ret = -1;
		goto out;
	}

	printf("This tool will update the pool to the latest available "
		"layout version.\nThis process is NOT fail-safe.\n"
		"Proceed only if the pool has been backed up or\n"
		"the risks are fully understood and acceptable.\n");
	if (ask_Yn('?', "convert the pool '%s' ?", f) != 'y') {
		return 0;
	}

	PMEMobjpool *pop = addr;

	for (unsigned r = 0; r < psf->poolset->nreplicas; ++r) {
		struct pool_replica *rep = psf->poolset->replica[r];
		for (unsigned p = 0; p < rep->nparts; ++p) {
			struct pool_set_part *part = &rep->part[p];
			if (util_map_hdr(part, MAP_SHARED, 0) != 0) {
				fprintf(stderr, "Failed to map headers.\n"
						"Conversion did not start.\n");
				ret = -1;
				goto out;
			}
		}
	}

	if (version_convert[m](psf, pop) != 0) {
		fprintf(stderr, "Failed to convert the pool\n");
	} else {
		/* need to update every header of every part */
		uint32_t target_m = m + 1;
		for (unsigned r = 0; r < psf->poolset->nreplicas; ++r) {
			struct pool_replica *rep = psf->poolset->replica[r];
			for (unsigned p = 0; p < rep->nparts; ++p) {
				struct pool_set_part *part = &rep->part[p];

				struct pool_hdr *hdr = part->hdr;
				hdr->major = htole32(target_m);
				util_checksum(hdr, sizeof(*hdr),
					&hdr->checksum, 1);
				PERSIST_GENERIC_AUTO(hdr,
					sizeof(struct pool_hdr));
			}
		}
	}

	PERSIST_GENERIC_AUTO(pop, psf->size);

out:
	for (unsigned r = 0; r < psf->poolset->nreplicas; ++r) {
		struct pool_replica *rep = psf->poolset->replica[r];
		for (unsigned p = 0; p < rep->nparts; ++p) {
			struct pool_set_part *part = &rep->part[p];
			if (part->hdr != NULL)
				util_unmap_hdr(part);
		}
	}
	pool_set_file_close(psf);

	return ret;
}
