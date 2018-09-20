/*
 * Copyright (c) 2017-2018, HPE
 *
 * Written by: Oleg Neverovitch, Dmitry Ivanov
 */

#include <malloc.h>

#include "fam_stripe.h"
#include "lf_client.h"


/*
 * Map stripe chunk's in stripe for given extent number.
 * Return node number, i.e. the chunk position in the stripe.
 **/
static void map_stripe_chunks(N_STRIPE_t *stripe, unsigned int extent)
{
    N_CHUNK_t *chunk;
    unsigned int partition, part_extents, e;
    int chunk_n, chunks;
    int p, parities;

    parities = stripe->p;
    chunks = stripe->d + parities;
    part_extents = stripe->srv_extents;
    partition = extent_to_part(extent, part_extents);

    /* Map each chunk in the stripe for this extent */
    chunk = stripe->chunks;
    for (chunk_n = 0; chunk_n < chunks; chunk_n++, chunk++) {
	chunk->node = chunk_n;
	/* p = (chunk_n - extent) mod chunks */
	p = (chunk_n - extent) % chunks;
	p = (p < 0)? (p + chunks) : p;
	if (p < parities) {
		chunk->parity = p;
		chunk->data = -1;
	} else {
		chunk->data = p - parities;
		ASSERT(chunk->data >= 0 && chunk->data < (chunks - parities));
		chunk->parity = -1;
	}
        chunk->lf_client_idx = to_lf_client_id(chunk_n, stripe->part_count, partition);
	chunk->r_event = 0;
	chunk->w_event = 0;
    }

    e = extent - partition * part_extents;
    ASSERT(e >= 0);
    stripe->extent_in_part = e;
    stripe->extent = extent;
    stripe->partition = partition;
}

/*
 * Map I/O node log chunk to FAM chunk;
 * Ensure the stripe is mapped to the extent;
 * Return the chunk index in the stripe and
 * the pointer to the corresponding N_CHUNK_t structure.
 **/
N_CHUNK_t *get_fam_chunk(uint64_t ionode_chunk_id, struct n_stripe_ *stripe, int *index)
{
    N_CHUNK_t *chunk;
    unsigned int i, data, size;
    unsigned int extent, part_extents;
    unsigned int stripe_n, stripe_chunk_id;
    uint64_t fam_chunk;

    data = stripe->d;
    size = data + stripe->p;
    part_extents = stripe->srv_extents;

    /* Convert I/O node log physical chunk to FAM logical chunk */
    fam_chunk = ionode_chunk_id * size + (unsigned int)stripe->node_id;

    stripe_n = fam_chunk / data;
    stripe_chunk_id = fam_chunk - ((uint64_t)stripe_n * data);
    extent = stripe_n / stripe->extent_stipes;

    /* Is this stripe mapped? */
    if (stripe->extent != extent)
	map_stripe_chunks(stripe, extent);

    /* find chunk index by D# */
    chunk = stripe->chunks;
    for (i = 0; i < size; i++, chunk++) {
	if (chunk->data == stripe_chunk_id)
	    break;
    }
    if (i == size)
	return NULL; /* ASSERT */

    if (index)
	*index = (int)i;
    return chunk;
}

