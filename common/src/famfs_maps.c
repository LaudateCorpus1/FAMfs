/*
 * Copyright (c) 2019, HPE
 *
 * Written by: Oleg Neverovitch, Dmitry Ivanov
 */

#include <uuid/uuid.h>

#include "famfs_env.h"
#include "famfs_error.h"
#include "f_pool.h"
#include "f_layout.h"
#include "famfs_maps.h"
#include "famfs_lf_connect.h"
#include "famfs_configurator.h"


F_POOL_t *pool = NULL;
static F_META_IFACE_t *meta_iface = NULL;


F_POOL_t *f_get_pool(void) {
    return pool;
}

/* Return 0 if the slab map entry checksum (CRC-4) is correct */
unsigned char f_crc4_sm_chk(F_SLAB_ENTRY_t *se)
{
    unsigned char c;
    size_t i;
    int crc = 0;

    for (i = 0; i < offsetof(F_SLAB_ENTRY_t, rc); i++) {
	c = ((unsigned char *)se)[i];
	crc = f_crc4_fast_table[crc | c];
    }
    /* Skip recovery count (32bits) */
    for (i += sizeof(se->rc); i < sizeof(F_SLAB_ENTRY_t); i++) {
	c = ((unsigned char *)se)[i];
	crc = f_crc4_fast_table[crc | c];
    }
    return (unsigned char)(crc >> 8);
}


/* Layout name (moniker) parser */
int f_layout_parse_name(struct f_layout_info_ *info)
{
	size_t chunk_size;
	int data, parity, mirrors;
	int ret = -1;

	if (info->name)
		ret = f_parse_moniker(info->name,
				&data, &parity, &mirrors, &chunk_size);
	if (ret == 0 && mirrors == 0) {
		info->data_chunks = data;
		info->chunks = data + parity;
		info->chunk_sz = chunk_size;
		info->slab_stripes = pool->info.extent_sz / chunk_size;

		if (!IN_RANGE(info->chunks, 1, info->devnum))
			ret = -1;
	}
	return ret;
}

void f_set_meta_iface(F_META_IFACE_t *iface)
{
	if (meta_iface == NULL)
		meta_iface = iface;
}

static int create_persistent_map(F_MAP_INFO_t *mapinfo, int intl, char *db_name)
{
	if (!meta_iface || !meta_iface->create_map_fn)
		return -1;

	mapinfo->ro = 0U; /* create_map_fn() could set map read-only flag */
	return meta_iface->create_map_fn(mapinfo, intl, db_name);
}

int f_create_persistent_sm(int layout_id, int intl, F_MAP_INFO_t *mapinfo)
{
	char name[6];
	unsigned int map_id = LO_TO_SM_ID(layout_id);

	snprintf(name, sizeof(name), "sm_%1u", map_id);
	mapinfo->map_id = map_id;
	return create_persistent_map(mapinfo, intl, name);
}

int f_create_persistent_cv(int layout_id, int intl, F_MAP_INFO_t *mapinfo)
{
	char name[6];
	unsigned int map_id = LO_TO_CV_ID(layout_id);

	snprintf(name, sizeof(name), "cv_%1u", map_id);
	mapinfo->map_id = map_id;
	return create_persistent_map(mapinfo, intl, name);
}

ssize_t f_db_bget(unsigned long *buf, int map_id, uint64_t *keys, size_t size,
    uint64_t *off_p)
{
	ssize_t ret;

	keys[0] = *off_p;
	ret = meta_iface->bget_fn(buf, map_id, size, keys);
	if (ret > 0)
		*off_p = keys[ret-1] + 1U;
	return ret;
}

int f_db_bput(unsigned long *buf, int map_id, void **keys, size_t size,
    size_t value_len)
{
	return meta_iface->bput_fn(buf, map_id, size, keys, value_len);
}

int f_db_bdel(int map_id, void **keys, size_t size)
{
	return meta_iface->bdel_fn(map_id, size, keys);
}

static F_IONODE_INFO_t *get_ionode_info(F_POOL_t *p, const char *hostname)
{
    unsigned int count;
    int idx;
    char **nodelist;

    if (p == NULL)
	return NULL;

    /* List of IO nodes */
    count = p->ionode_count;
    nodelist = p->ionodelist;
    assert( nodelist );

    /* Find foreign hostname if given, otherwise local hostname or IP */
    if (hostname)
	idx = f_find_node(nodelist, (int)count, hostname);
    else
	idx = find_my_node(nodelist, (int)count, NULL);

    if (idx < 0)
	return NULL;
    return &p->ionodes[idx];
}

/* Set ionode_id and HasMDS, IsIOnode flags in pool struct */
static void set_my_ionode_info(F_POOL_t *p)
{
    F_IONODE_INFO_t *info;

    assert (p && p->ionodes);
    info = get_ionode_info(p, NULL);
    if (info) {
	SetNodeIsIOnode(&p->mynode);
	if (IOnodeForceHelper(info))
	    SetNodeForceHelper(&p->mynode);
	if (info->mds)
	    SetNodeHasMDS(&p->mynode);
	else
	    ClearNodeHasMDS(&p->mynode);
	p->mynode.ionode_id = (uint32_t)(info - p->ionodes);
	assert( p->mynode.ionode_id < p->ionode_count );
    } else {
	ClearNodeIsIOnode(&p->mynode);
	ClearNodeHasMDS(&p->mynode);
    }
}

static int find_pd_index_in_ag(F_AG_t *ags, uint32_t pool_ags,
    uint32_t ag_devs, uint16_t index, F_POOL_DEV_t *devlist)
{
    F_AG_t *ag = ags;
    F_POOL_DEV_t (*ag_devlist)[ag_devs] = (F_POOL_DEV_t(*)[ag_devs])devlist;
    unsigned int u;

    for (u = 0; u < pool_ags; u++, ag++) {
	uint16_t *gpdi = ag->gpdi;
	unsigned int uu;

	assert( ag->pdis <= ag_devs );
	/* TODO: Check for duplicates in AG */
	for (uu = 0; uu < ag->pdis; uu++, gpdi++)
	    if (*gpdi == index)
		return (int)(&ag_devlist[u][uu] - devlist);
    }
    return -1;
}

static uint16_t find_pdev(struct f_pool_ *p, unsigned int index) {
    F_AG_t *ag;
    unsigned int u;

    ag = p->ags;
    for (u = 0; u < p->pool_ags; u++, ag++) {
	F_POOL_DEV_t *pdev = ((F_POOL_DEV_t (*)[p->ag_devs]) p->devlist)[u];
	unsigned int uu;

	for (uu = 0; uu < ag->pdis; uu++, pdev++)
	    if (pdev->pool_index == index)
		return p->ag_devs*u + uu;
    }
    return F_PDI_NONE;
}

struct f_pool_dev_ *f_find_pdev(unsigned int media_id) {
    struct f_pool_ *p = pool;

    return (media_id <= p->info.pdev_max_idx)?
	(&p->devlist[ p->info.media_ids[media_id] ]) : NULL;
}

static int cmp_pdev_by_index(const void *a, const void *b)
{
    uint16_t ia = ((F_POOL_DEV_t *)a)->pool_index;
    uint16_t ib = ((F_POOL_DEV_t *)b)->pool_index;

    return ((int)ia - (int)ib); /* ascending order */
}

/* Sort layout devices in ascending order: AG first, then pool index */
static int cmp_pdi(const void *a, const void *b)
{
    F_POOLDEV_INDEX_t *pdia = (F_POOLDEV_INDEX_t *)a;
    F_POOLDEV_INDEX_t *pdib = (F_POOLDEV_INDEX_t *)b;
    unsigned int ia = pdia->idx_ag * (F_PDI_NONE+1) + pdia->idx_dev;
    unsigned int ib = pdib->idx_ag * (F_PDI_NONE+1) + pdib->idx_dev;
    if (ia > ib)
	return 1;
    else if (ia == ib)
	return 0;
    return -1;
}

static int dev_ionode(F_POOL_t *p, struct f_dev_ *dev)
{
    F_IONODE_INFO_t *ionode = p->ionodes;
    char *topo = dev->f.zfm.topo;
    uint32_t i;

    for (i = 0; i < p->ionode_count; i++, ionode++) {
	if (!strncmp(topo, ionode->zfm.topo, strlen(ionode->zfm.topo)))
	    return (int)i;
    }
    return -1;
}

static int clone_pool_dev(F_POOL_DEV_t *clone, F_POOL_DEV_t *pdev)
{
    FAM_DEV_t *fam, *cfam;

    uuid_copy(clone->uuid, pdev->uuid);
    clone->size = pdev->size;
    clone->extent_sz = pdev->extent_sz;
    clone->extent_start = pdev->extent_start;
    clone->extent_count = pdev->extent_count;
    clone->pool_index = pdev->pool_index;
    clone->idx_ag = pdev->idx_ag;
    clone->idx_dev = pdev->idx_dev;

    fam = &pdev->dev->f;
    assert( fam );
    clone->dev = (F_DEV_t *) calloc(sizeof(F_DEV_t), 1);
    if (!clone->dev)
	return -ENOMEM;
    cfam = &clone->dev->f;
    cfam->zfm.topo = strdup(fam->zfm.topo);
    if (fam->zfm.znode)
	cfam->zfm.znode = strdup(fam->zfm.znode);
    if (fam->zfm.geo)
	cfam->zfm.geo = strdup(fam->zfm.geo);
    cfam->ionode_idx = fam->ionode_idx;
    cfam->service = fam->service;
    cfam->fam_part = fam->fam_part;
    return 0;
}

static void free_pdev(F_POOL_DEV_t *pdev)
{
    if (pdev->dev) {
	pthread_rwlock_destroy(&pdev->rwlock);
	f_dev_free(&pdev->dev->f);
	free(pdev->dev);
    }
    free(pdev->sha);
}

/* Allocate libfabric devices for FAM emulation on this IO node. */
static int emulated_devs_alloc(F_POOL_t *p)
{
    F_POOL_DEV_t *devlist = NULL;
    F_POOL_DEV_t *pdev, *fdev;
    F_POOL_INFO_t *info = &p->info;
    F_MYNODE_t *mynode = &p->mynode;
    uint16_t ion_idx, *pd_idx;
    unsigned int i, j, k;
    int rc;

    ion_idx = (uint16_t)mynode->ionode_id;
    pd_idx = info->pdev_indexes;
    for (i = j = 0; i < info->dev_count; i++, pd_idx++) {
        pdev = &p->devlist[*pd_idx];
	if (pdev->dev->f.ionode_idx == ion_idx)
	    j++;
    }

    if (j == 0) {
	err("ionode #%u (%s) has no FAMs - please check configuration!",
	    ion_idx, mynode->hostname);
	return 0;
    }
    devlist = fdev = (F_POOL_DEV_t*) calloc(j, sizeof(F_POOL_DEV_t));

    pd_idx = info->pdev_indexes;
    for (i = k = 0; i < info->dev_count; i++, pd_idx++) {
        pdev = &p->devlist[*pd_idx];
	if (pdev->dev->f.ionode_idx != ion_idx)
	    continue;
	if ((rc = clone_pool_dev(fdev, pdev)))
	    goto _err;
	fdev->dev->f.zfm.url = strdup(mynode->hostname);
	fdev->pool = p;
	fdev++; k++;
    }

    mynode->emul_devs = j;
    mynode->emul_devlist = devlist;
    return 0;

_err:
    fdev = devlist;
    for (i = 0; i < k; i++, fdev++)
	 free_pdev(fdev);
    free(devlist);
    return rc;
}

static void free_pool(F_POOL_t *p)
{
    if (p) {
	F_POOL_DEV_t *pdev;
	unsigned int i;

	free_lf_params(&p->lfs_params);

	pdev = p->devlist;
	if (pdev) {
	    for (i = 0; i < p->pool_devs; i++, pdev++)
		free_pdev(pdev);
	    free(p->devlist);
	}
	f_domain_close(&p->mynode.domain);
	free(p->info.pdev_indexes);
	free(p->info.media_ids);

	if (p->ags) {
	    for (i = 0; i < p->pool_ags; i++)
		free(p->ags[i].gpdi);
	    free(p->ags);
	}

	if (p->ionodes) {
	    F_IONODE_INFO_t *ioi = p->ionodes;

	    for (i = 0; i < p->ionode_count; i++, ioi++) {
		free(ioi->hostname);
		free(ioi->zfm.url);
		free(ioi->zfm.znode);
		free(ioi->zfm.topo);
		free(ioi->zfm.geo);
	    }
	    free(p->ionodes);
	}
	free(p->ionode_fams);

	if (p->ionode_comm) {
	    MPI_Barrier(p->ionode_comm);
	    MPI_Comm_free(&p->ionode_comm);
	}
	if (p->mynode.emul_devlist) {
	    pdev = p->mynode.emul_devlist;
	    for (i = 0; i < p->mynode.emul_devs; i++, pdev++)
		free_pdev(pdev);
	    free(p->mynode.emul_devlist);
	}
	f_domain_close(&p->mynode.emul_domain);

	if (p->lf_info) {
	    LF_INFO_t *lf_info = p->lf_info;

	    free(lf_info->fabric);
	    free(lf_info->domain);
	    free(lf_info->service);
	    free(lf_info->provider);
	    free(lf_info);
	}
	free(p->mynode.hostname);

	f_dict_free(p->dict);
	pthread_spin_destroy(&p->dict_lock);
	pthread_rwlock_destroy(&p->lock);
	free(p);
    }
}

static int cfg_load_pool(unifycr_cfg_t *c)
{
    F_POOL_t *p;
    F_POOL_INFO_t *pool_info;
    F_AG_t *ag;
    F_POOL_DEV_t *pdev;
    LF_INFO_t *lf_info;
    const char *s;
    int rc, count;
    unsigned int u, uu, ag_maxlen;
    long l;
    bool b;

    p = (F_POOL_t *) calloc(sizeof(F_POOL_t), 1);
    if (!p)
	return -ENOMEM;

    if (pthread_rwlock_init(&p->lock, NULL)) {
	free(p);
	return -ENOMEM;
    }
    if (pthread_spin_init(&p->dict_lock, PTHREAD_PROCESS_PRIVATE)) {
	free(p);
	return -ENOMEM;
    }
    INIT_LIST_HEAD(&p->layouts);
    pool_info = &p->info;

    /* Pool */
    if (configurator_int_val(c->log_verbosity, &l)) goto _noarg;
    p->verbose = (int)l;
    if (f_uuid_parse(c->devices_uuid, p->uuid)) {
	assert( uuid_parse(FAMFS_PDEVS_UUID_DEF, p->uuid) == 0);
    }
    /* my node name */
    p->mynode.hostname = f_get_myhostname();
    if (!p->mynode.hostname) goto _nomem;

    /* Generic device section: 'devices' */
    lf_info = (LF_INFO_t *) calloc(sizeof(LF_INFO_t), 1);
    if (!lf_info) goto _nomem;
    if (configurator_int_val(c->devices_extent_size, &l)) goto _noarg;
    pool_info->extent_sz = (unsigned long)l;
    if (configurator_int_val(c->devices_offset, &l)) goto _noarg;
    pool_info->data_offset = (unsigned long)l;
    if (configurator_bool_val(c->devices_multidomain, &b)) goto _noarg;
    if (b)
	lf_info->opts.multi_domains = 1;
    if (configurator_bool_val(c->devices_emulated, &b)) goto _noarg;
    if (b)
	SetPoolFAMEmul(p);
    else
	lf_info->opts.true_fam = 1;
    if (configurator_int_val(c->devices_size, &l)) goto _noarg;
    pool_info->size_def = (size_t)l;
    if (configurator_int_val(c->devices_pk, &l)) goto _noarg;
    pool_info->pkey_def = (uint64_t)l;
    if (c->devices_fabric)
	lf_info->fabric = strdup(c->devices_fabric);
    if (c->devices_domain)
	lf_info->domain = strdup(c->devices_domain);
    lf_info->service = strdup(c->devices_port);
    lf_info->provider = strdup(c->devices_provider);
    if (!strcmp(lf_info->provider, "zhpe"))
	lf_info->opts.zhpe_support = 1;
    if (configurator_bool_val(c->devices_use_cq, &b)) goto _noarg;
    if (b)
	lf_info->opts.use_cq = 1;
    if (configurator_int_val(c->devices_timeout, &l)) goto _noarg;
    lf_info->io_timeout_ms = (uint64_t)l;
    s = c->devices_memreg;
    lf_info->mrreg.scalable = strcasecmp(s, LF_MR_MODEL_SCALABLE)? 0:1;
    lf_info->mrreg.basic = strncasecmp(s, LF_MR_MODEL_BASIC,
				       strlen(LF_MR_MODEL_BASIC))? 0:1;
    lf_info->mrreg.local = strcasecmp(s + lf_info->mrreg.basic*(strlen(LF_MR_MODEL_BASIC)+1),
				LF_MR_MODEL_LOCAL)? 0:1;
    if (!lf_info->mrreg.scalable && !lf_info->mrreg.basic && !lf_info->mrreg.local)
	goto _badstr;
    if (lf_info->mrreg.basic) {
	/* basic registration is equivalent to FI_MR_VIRT_ADDR|FI_MR_ALLOCATED|FI_MR_PROV_KEY */
	lf_info->mrreg.basic = 0;
	lf_info->mrreg.allocated = 1;
	if (!strcmp(lf_info->provider, "verbs")) {
	    lf_info->mrreg.prov_key = 1;
	    lf_info->mrreg.virt_addr = 1;
	} else {
	    /* "zhpe" or "sockets" */
	    lf_info->mrreg.prov_key = 0;
	    lf_info->mrreg.virt_addr = 0;
	}
    }
    s = c->devices_progress;
    lf_info->progress.progress_manual = strncasecmp(s, "manual", 3)? 0:1;
    if (!lf_info->progress.progress_manual) {
	lf_info->progress.progress_auto = strcasecmp(s, "auto")? 0:1;
	if (!lf_info->progress.progress_auto && strcasecmp(s, "default"))
	    goto _badstr;
    }
    p->lf_info = lf_info;

    /* IO nodes */
    count = configurator_get_sec_size(c, "ionode");
    if (!IN_RANGE(count, 1, F_IONODES_MAX)) goto _noarg;
    p->ionode_count = (uint32_t)count;
    p->ionode_fams = (FAM_MAP_t *) calloc(sizeof(FAM_MAP_t), 1);
    p->ionode_fams->ionode_cnt = p->ionode_count;
    if (!p->ionode_fams) goto _nomem;
    p->ionodes = (F_IONODE_INFO_t*) calloc(sizeof(F_IONODE_INFO_t),
					   p->ionode_count);
    if (!p->ionodes) goto _nomem;
    p->ionodelist = (char**) calloc(sizeof(char*), p->ionode_count);
    if (!p->ionodelist) goto _nomem;
    for (u = 0; u < p->ionode_count; u++) {
	F_IONODE_INFO_t *ioi = &p->ionodes[u];

	if (configurator_int_val(c->ionode_id[u][0], &l)) goto _syntax;
	ioi->conf_id = (uint32_t)l;
	f_uuid_parse(c->ionode_uuid[u][0], ioi->uuid);
	if (configurator_int_val(c->ionode_mds[u][0], &l)) goto _syntax;
	ioi->mds = (uint32_t)l;
	if (configurator_bool_val(c->ionode_force_helper[u][0], &b)) goto _syntax;
	if (b)
	    SetIOnodeForceHelper(ioi);
	if (c->ionode_host[u][0])
	    ioi->hostname = strdup(c->ionode_host[u][0]);
	else if (u == 0)
	    ioi->hostname = strdup(p->mynode.hostname);
	else
	    goto _noarg;
	p->ionodelist[u] = strdup(ioi->hostname);
	if (!c->ionode_topo[u][0]) goto _noarg;
	ioi->zfm.topo = strdup(c->ionode_topo[u][0]);
	if (c->ionode_z_node[u][0])
	    ioi->zfm.znode = strdup(c->ionode_z_node[u][0]);
	if (c->ionode_geo[u][0])
	    ioi->zfm.geo = strdup(c->ionode_geo[u][0]);

    }
    set_my_ionode_info(p);

    /* Device count */
    count = configurator_get_sec_size(c, "device");
    if (!IN_RANGE(count, 1, F_DEVICES_MAX)) goto _noarg;
    pool_info->dev_count = (unsigned int)count;

    /* Allocation groups */
    count = configurator_get_sec_size(c, "ag");
    p->ags = (F_AG_t *) calloc(sizeof(F_AG_t), count);
    if (!p->ags) goto _nomem;
    if (!IN_RANGE(count, 1, F_DEVICES_MAX)) goto _noarg;
    p->pool_ags = (uint32_t)count;
    ag = p->ags;
    ag_maxlen = 0;
    for (u = 0; u < p->pool_ags; u++, ag++) {
	/* AG id */
	if (configurator_int_val(c->ag_id[u][0], &l)) goto _syntax;
	ag->gid = (uint32_t)l;
	f_uuid_parse(c->ag_uuid[u][0], ag->uuid);
	/* List of devices in the group */
	count = u;
	assert( configurator_get_sizes(c, "ag", "devices", &count) );
	if (!IN_RANGE((unsigned)count, 1, pool_info->dev_count))
	    goto _noarg;
	ag->pdis = (uint32_t)count;
	if (ag->pdis > ag_maxlen)
	    ag_maxlen = ag->pdis; /* maximum number of group devices */
	ag->gpdi = (uint16_t *) calloc(sizeof(uint16_t), ag->pdis);
	for (uu = 0; uu < ag->pdis; uu++) {
	    if (configurator_int_val(c->ag_devices[u][uu], &l))
		ag->gpdi[uu] = F_PDI_NONE;
	    else
		ag->gpdi[uu] = (uint16_t)l;
	}
    }
    p->ag_devs = ag_maxlen;

    /* Devices */
    pool_info->pdev_max_idx = 0;
    p->pool_devs = ag_maxlen * p->pool_ags;
    p->devlist = (F_POOL_DEV_t*) calloc(sizeof(F_POOL_DEV_t), p->pool_devs);
    if (!p->devlist) goto _nomem;
    pdev = p->devlist;
    for (u = 0; u < p->pool_devs; u++, pdev++)
	pdev->pool_index = F_PDI_NONE;
    for (u = 0; u < pool_info->dev_count; u++) {
	FAM_DEV_t *fam;
	int pd_index, idx;
	uint16_t pool_index;

	/* device media_id */
	if (configurator_int_val(c->device_id[u][0], &l)) goto _noarg;
	pool_index = (uint32_t)l;

	/* Find device in AG by media_id (pool_index) and map it to devlist */
	pd_index = find_pd_index_in_ag(p->ags, p->pool_ags, p->ag_devs,
				       pool_index, p->devlist);
	if (!IN_RANGE(pd_index, 0, (int)p->pool_devs-1)) goto _syntax;
	pdev = &p->devlist[pd_index];
	pdev->pool_index = pool_index;

	f_uuid_parse(c->device_uuid[u][0], pdev->uuid);
	if (configurator_int_val(c->device_size[u][0], &l)) goto _syntax;
	pdev->size = (size_t)l;
	if (pdev->size == 0)
	    pdev->size = p->info.size_def;

	/* TODO: Parse extent_sz, offset */
	pdev->extent_start = p->info.data_offset/p->info.extent_sz;
	pdev->extent_sz = p->info.extent_sz;
	pdev->extent_count = pdev->size/p->info.extent_sz;

	pdev->sha = (F_PDEV_SHA_t *) calloc(sizeof(F_PDEV_SHA_t), 1);
	if (!pdev->sha) goto _nomem;

	if (configurator_bool_val(c->device_failed[u][0], &b)) goto _noarg;
	if (b)
	    SetDevFailed(pdev->sha);
	/* Allocate FAMFS device in devlist */
	pdev->dev = (F_DEV_t *) calloc(sizeof(F_DEV_t), 1);
	if (!pdev->dev) goto _nomem;
	fam = &pdev->dev->f;
	if (!PoolFAMEmul(p) && c->device_url[u][0])
	    fam->zfm.url = strdup(c->device_url[u][0]);
	if (!c->device_topo[u][0]) goto _noarg;
	fam->zfm.topo = strdup(c->device_topo[u][0]);
	if (c->device_z_node[u][0])
	    fam->zfm.znode = strdup(c->device_z_node[u][0]);
	if (c->device_geo[u][0])
	    fam->zfm.geo = strdup(c->device_geo[u][0]);
	/* match device IO node by toplogy */
	if ((idx = dev_ionode(p, pdev->dev)) < 0) goto _syntax;
	    fam->ionode_idx = (uint16_t)idx;

	if (configurator_int_val(c->device_pk[u][0], &l))
	    fam->pkey = pool_info->pkey_def;
	else
	    fam->pkey = (uint64_t)l;
	if (pool_info->pdev_max_idx < pool_index)
	    pool_info->pdev_max_idx = pool_index;

	if (pthread_rwlock_init(&pdev->rwlock, NULL)) goto _nomem;
	pdev->pool = p;

	/* TODO: probe devices, sha.extent_bmap */
    }
    /* Sort pool devices array by pool_index for every AG */
    for (u = 0; u < p->pool_ags; u++) {
	pdev = ((F_POOL_DEV_t (*)[p->ag_devs]) p->devlist)[u];
	qsort(pdev, p->ag_devs, sizeof(F_POOL_DEV_t), cmp_pdev_by_index);

	p->ags[u].ionode_idx = pdev->dev->f.ionode_idx;
	/* check the presence and enumerate devices in AG */
	for (uu = 0; uu < p->ags[u].pdis; uu++, pdev++) {
	    if (pdev->pool_index == F_PDI_NONE) {
		/* non-existing device in AG */
		fprintf (stderr, " AG id:%u - device not in configuration!\n",
		    pdev->idx_ag);
		goto _syntax;
	    }
	    if (p->ags[u].ionode_idx != pdev->dev->f.ionode_idx) {
		/* mix of devices from different IO nodes in AG */
		fprintf (stderr, " AG id:%u - device id:%u does not belong to"
				 " IO node id:%u but %u\n",
		    pdev->idx_ag, pdev->pool_index,
		    p->ionodes[p->ags[u].ionode_idx].conf_id,
		    pdev->dev->f.ionode_idx);
		goto _syntax;
	    }
	    /* set pool device indexes in array for back-reference */
	    pdev->idx_ag = u;	/* 1st index */
	    pdev->idx_dev = uu;	/* 2nd index */
	}
    }

    /* Populate pool devlist lookup helper arrays */

    /* Array of indexes in devlist for devlist stripped of F_PDI_NONE devices */
    pool_info->pdev_indexes = (uint16_t *) calloc(pool_info->dev_count,
						  sizeof(uint16_t));
    if (!pool_info->pdev_indexes) goto _nomem;
    pdev = p->devlist;
    for (u = uu = 0; u < pool_info->dev_count; u++, uu++, pdev++) {
	    while (uu < p->pool_devs && pdev->pool_index == F_PDI_NONE) {
		pdev++;
		uu++;
	    }
	    assert( uu < p->pool_devs );
	    pool_info->pdev_indexes[u] = uu;
    }
    /* Array of indexes in devlist for each media_id from zero to 'pdev_max_idx' */
    pool_info->media_ids = (uint16_t *) calloc(pool_info->pdev_max_idx+1,
					       sizeof(uint16_t));
    if (!pool_info->media_ids) goto _nomem;
    for (u = 0; u <= pool_info->pdev_max_idx; u++)
	pool_info->media_ids[u] = find_pdev(p, u);

    /* Allocate fabric devices for FAM emulation */
    if (PoolFAMEmul(p) && emulated_devs_alloc(p))
	goto _nomem;

    /* Layout count */
    count = configurator_get_sec_size(c, "layout");
    if (!IN_RANGE(count, 1, F_LAYOUTS_MAX)) goto _noarg;
    pool_info->layouts_count = count;

    pool = p;
    return 0;

_nomem:
    rc = -ENOMEM;
    goto _err;
_badstr:
    rc = -3; /* invalid string value */
    goto _err;
_syntax:
    rc = -2; /* semantic error in configuration */
    goto _err;
_noarg:
    rc = -1; /* no mandatory parameter specified */
_err:
    free_pool(p);
    return rc;
}

static void free_layout(F_LAYOUT_t *lo)
{
    free(lo->info.name);
    if (lo->devlist) {
	F_POOLDEV_INDEX_t *pdi = lo->devlist;
	unsigned int u;

	for (u = 0; u < lo->devlist_sz; u++, pdi++)
	    free(pdi->sha);
	free(lo->devlist);
    }

    if (lo->lp) {
	pthread_spin_destroy(&lo->lp->alloc_lock);
	pthread_rwlock_destroy(&lo->lp->claimdec_lock);
	free(lo->lp);
    }

    f_dict_free(lo->dict);
    pthread_spin_destroy(&lo->dict_lock);
    pthread_rwlock_destroy(&lo->lock);
    free(lo);
}

/* Create layout structure from configurator at index */
static int cfg_load_layout(unifycr_cfg_t *c, int idx)
{
    F_LAYOUT_t *lo;
    F_LAYOUT_INFO_t *info;
    F_LO_PART_t *lp;
    F_POOL_t *p = pool;
    F_POOL_DEV_t *pdev;
    F_POOLDEV_INDEX_t *pdi;
    unsigned int u, uu;
    int rc, count;
    long l;
    bool all_pdevs;

    assert( p && IN_RANGE(idx, 0, (int)p->info.layouts_count-1) );
    lo = (F_LAYOUT_t *) calloc(sizeof(F_LAYOUT_t), 1);
    if (!lo) return -ENOMEM;
    if (pthread_rwlock_init(&lo->lock, NULL)) goto _nomem;
    if (pthread_spin_init(&lo->dict_lock, PTHREAD_PROCESS_PRIVATE)) goto _nomem;
    INIT_LIST_HEAD(&lo->list);
    /* Parse this layout's configurator ID and name */
    info = &lo->info;
    if (configurator_int_val(c->layout_id[idx][0], &l)) goto _noarg;
    info->conf_id = (unsigned int)l;

    /* Partition */
    lo->lp = (F_LO_PART_t *) calloc(sizeof(F_LO_PART_t), 1);
    if (!lo->lp) goto _nomem;
    lp = lo->lp;
    lp->layout = lo;
    if (pthread_rwlock_init(&lp->claimdec_lock, NULL)) goto _nomem;
    if (pthread_spin_init(&lp->alloc_lock, PTHREAD_PROCESS_PRIVATE))
	goto _nomem;
    INIT_LIST_HEAD(&lp->alloc_buckets);
    INIT_LIST_HEAD(&lp->claimdecq);

    /* Devices */
    count = idx; /* layout section index */
    assert( configurator_get_sizes(c, "layout", "devices", &count) );
    all_pdevs = (count <= 0);
    if (all_pdevs) {
	/* no layout devices list given - default to pool devices */
	count = p->info.dev_count;
    } else {
	if (!IN_RANGE((unsigned)count, 1, p->info.dev_count))
	    goto _noarg;
    }
    lo->devlist_sz = info->devnum = (unsigned)count;
    info->name = strdup(c->layout_name[idx][0]);
    if (f_layout_parse_name(info)) goto _noarg;
    lo->devlist = (F_POOLDEV_INDEX_t *) calloc(sizeof(F_POOLDEV_INDEX_t),
	lo->devlist_sz);
    if (!lo->devlist) goto _nomem;
    pdi = lo->devlist;
    for (u = uu = 0; u < info->devnum; u++, pdi++) {
	/* find pool device by id */
	if (all_pdevs) {
	    pdev = &pool->devlist[uu];
	    while (pdev->pool_index == F_PDI_NONE &&
		   ++uu < p->pool_devs) pdev++;
	    assert( uu < p->pool_devs ); /* less than devnum */
	} else {
	    if (configurator_int_val(c->layout_devices[idx][u], &l))
		pdev = NULL;
	    else
		pdev = f_find_pdev((unsigned int)l);
	}
	/* copy pdev */
	if (pdev == NULL) {
	    pdi->pool_index = F_PDI_NONE;
	} else {
	    pdi->pool_index = pdev->pool_index;
	    pdi->idx_ag = pdev->idx_ag;
	    pdi->idx_dev = pdev->idx_dev;
	    /* Allocate the device index shareable atomics */
	    pdi->sha = (F_PDI_SHA_t *) calloc(sizeof(F_PDI_SHA_t), 1);
	    if (!pdi->sha) goto _nomem;
	}
    }
    /* Sort by AG first, then by pool index */
    qsort(lo->devlist, lo->devlist_sz, sizeof(F_POOLDEV_INDEX_t), cmp_pdi);

    lo->pool = p;
    /* Add to layouts list */
    list_add(&lo->list, &p->layouts);
    return 0;

_nomem:
    rc = -ENOMEM;
    goto _err;
_noarg:
    rc = -1; /* no mandatory parameter specified */
_err:
    free_layout(lo);
    return rc;
}

/* Linaer map: devices to ionodes */
static FAM_MAP_t *map_fams_to_ionodes(F_POOL_t *p, int count)
{
    FAM_MAP_t *m;
    F_POOL_DEV_t *pdev;
    unsigned long long *ids;
    unsigned int pd;
    int i, j, k, d, n, nn;

    m = (FAM_MAP_t *) calloc(1, sizeof(FAM_MAP_t));
    if (!m) goto _free;
    m->ionode_cnt = p->ionode_count;
    m->total_fam_cnt = count;
    m->node_fams = (int *) calloc(m->ionode_cnt, sizeof(int));
    m->fam_ids = (unsigned long long**) calloc(m->ionode_cnt, sizeof(*m->fam_ids));
    if (!m->node_fams || !m->fam_ids) goto _free;

    nn = m->total_fam_cnt / m->ionode_cnt;
    k = m->total_fam_cnt % m->ionode_cnt;
    pd = 0;
    pdev = p->devlist;
    for (i = d = 0; i < m->ionode_cnt; i++) {
	n = (i < k)? (nn+1):nn; /* number of FAMs on ionode i */
	m->node_fams[i] = n;
	ids = (unsigned long long*) calloc(n, sizeof(unsigned long long));
	for (j = 0; j < n; j++, d++, pd++, pdev++) {
	    while (pd < p->pool_devs && pdev->pool_index == F_PDI_NONE) {
		pdev++;
		pd++;
	    }
	    assert( pd < p->pool_devs );
	    ids[j] = pdev->pool_index;
	}
	m->fam_ids[i] = ids;
    }
    return m;

_free:
    free_fam_map(&m);
    return NULL;
}

static int cfg_alloc_params(F_POOL_t *p, N_PARAMS_t **params_p)
{
    N_PARAMS_t *params;
    F_LAYOUT_INFO_t *lo_info;
    F_POOL_INFO_t *info = &p->info;
    LF_INFO_t *lf_info = p->lf_info;
    int rc;
    bool fam_emul = PoolFAMEmul(p);

    params = (N_PARAMS_t *) calloc(sizeof(N_PARAMS_t), 1);
    if (!params) goto _nomem;

    /* default layout */
    lo_info = f_get_layout_info(0);
    params->nchunks = lo_info->chunks;
    params->parities = lo_info->chunks - lo_info->data_chunks;
    params->chunk_sz = lo_info->chunk_sz;
    params->extent_sz = info->extent_sz;
    params->srv_extents = info->size_def / info->extent_sz;
    params->vmem_sz = info->extent_sz * params->srv_extents;
    params->node_servers = 1;
    /* part_mreg:1 - emulate multiple FAMs on each node as separate "partitions" */
    params->opts.zhpe_support = lf_info->opts.zhpe_support;
    params->opts.true_fam = lf_info->opts.true_fam;
    params->opts.part_mreg = (fam_emul)?1:0;
    params->opts.use_cq = lf_info->opts.use_cq;
    params->opts.multi_domains = lf_info->opts.multi_domains;
    params->io_timeout_ms = lf_info->io_timeout_ms;
    memcpy(&params->lf_mr_flags, &lf_info->mrreg, sizeof(LF_MR_MODE_t));
    memcpy(&params->lf_progress_flags, &lf_info->progress, sizeof(LF_PRG_MODE_t));
    if (lf_info->fabric)
	params->lf_fabric = strdup(lf_info->fabric);
    if (lf_info->domain)
	params->lf_domain = strdup(lf_info->domain);
    params->prov_name = strdup(lf_info->provider);
    if (sscanf(lf_info->service, "%5d", &params->lf_port) != 1)
	goto _badval;
    params->nodelist = p->ionodelist;
    params->node_id = p->mynode.ionode_id;
    params->fam_cnt = (fam_emul)?info->dev_count:p->ionode_count;
    params->fam_map = map_fams_to_ionodes(p, params->fam_cnt);

    params->verbose = p->verbose;
    //params->mpi_comm = MPI_COMM_NULL;
    params->w_thread_cnt = 1;
    if (!params->lf_mr_flags.scalable) {
        size_t len = params->fam_cnt * params->node_servers * sizeof(uint64_t);
        params->mr_prov_keys = (uint64_t *)malloc(len);
        params->mr_virt_addrs = (uint64_t *)malloc(len);
    }

    *params_p = params;
    return 0;

_nomem:
    rc = -ENOMEM;
    goto _err;
_badval:
    rc = -4; /* invalid value */
_err:
    free_lf_params(&params);
    return rc;
}

static int cfg_load(unifycr_cfg_t *c)
{
    int rc;
    unsigned int i;

    if ((rc = cfg_load_pool(c)))
	return rc;

    for (i = 0; i < pool->info.layouts_count; i++)
	if ((rc = cfg_load_layout(c, i)))
	    return rc;

    /* Allocate legacy N_PARAMS_t */
    rc = cfg_alloc_params(pool, &pool->lfs_params);
    return rc;
}

/* Set layout info from configurator once */
int f_set_layouts_info(unifycr_cfg_t *cfg)
{
    if (cfg && pool == NULL) {
	f_crc4_init_table();
	return cfg_load(cfg);
    }
    return -1;
}

/* Get layout 'layout_id' info */
F_LAYOUT_t *f_get_layout(int layout_id) {
    struct list_head *l;
    F_LAYOUT_t *lo;

    if (pool && pool) {
	list_for_each(l, &pool->layouts) {
	    lo = container_of(l, struct f_layout_, list);
	    if ((int)lo->info.conf_id == layout_id)
		return lo;
	}
    }
    return NULL;
}

/* Get layout 'layout_id' info */
F_LAYOUT_INFO_t *f_get_layout_info(int layout_id) {
    F_LAYOUT_t *lo = f_get_layout(layout_id);

    return lo?&lo->info:NULL;
}

F_LAYOUT_t *f_get_layout_by_name(const char *moniker) {
    struct list_head *l;
    F_LAYOUT_t *lo;

    if (pool && moniker) {
	list_for_each(l, &pool->layouts) {
	    lo = container_of(l, struct f_layout_, list);
	    if (!strcmp(lo->info.name, moniker))
		return lo;
	}
    }
    return NULL;
}

void f_free_layouts_info(void)
{
    F_LAYOUT_t *lo;
    struct list_head *l, *tmp;

    if (pool) {
	list_for_each_safe(l, tmp, &pool->layouts) {
	    lo = container_of(l, struct f_layout_, list);
	    list_del(l);
	    free_layout(lo);
	}
	free_pool(pool);
	pool = NULL;
    }
}

static void print_lf_info(LF_INFO_t *info) {
    LF_MR_MODE_t m = info->mrreg;
    LF_PRG_MODE_t pgs = info->progress;
    LF_OPTS_t o = info->opts;

    printf("\nlibfabric\n");
    printf("provider:%s fabric:%s domain:%s service:%s timeout:%.3f sec\n",
	info->provider, info->fabric, info->domain, info->service,
	(float)info->io_timeout_ms/1000);
    printf("MR mode: %s%s%s%s%s%s\n",
	m.scalable?"scalable, ":"", m.basic?"basic, ":"",
	m.local?"local, ":"", m.prov_key?"prov_key, ":"",
	m. virt_addr?"virt_addr, ":"", m.allocated?"allocated":"");
    printf("Data/control progress: %s%s\n",
	pgs.progress_manual?"manual ":"", pgs.progress_auto?"progress_auto":"");
    printf("Options: %s%s%s%s%s\n",
	o.zhpe_support?"zhpe_support, ":"", o.true_fam?"true_fam, ":"",
	o.part_mreg?"part_mreg, ":"", o.multi_domains?"multi_domains, ":"",
	o.use_cq?"use_cq":"");
}

void f_print_layouts(void) {
    F_POOL_t *p = pool;
    F_POOL_INFO_t *pool_info;
    F_AG_t *ag;
    F_POOL_DEV_t *pdev;
    F_LAYOUT_t *lo;
    char pr_uuid[F_UUID_BUF_SIZE];
    struct list_head *l, *tmp;
    unsigned int u, uu;

    if (p == NULL)
	return;
    /* Pool */
    pool_info = &p->info;
    printf("Pool has %u devices in %u AGs, %u each\n",
	pool_info->dev_count, p->pool_ags, p->ag_devs);
    ag = p->ags;
    for (u = 0; u < p->pool_ags; u++, ag++) {
	pdev = ((F_POOL_DEV_t (*)[p->ag_devs]) p->devlist)[u];
	printf("  AG#%u id:%u has %u devices at ionode id:%u\n",
	       u, ag->gid, ag->pdis,
	       p->ionodes[ag->ionode_idx].conf_id);
	for (uu = 0; uu < ag->pdis; uu++, pdev++) {
	    F_ZFM_t *zfm;

	    assert( pdev->pool_index != F_PDI_NONE );
	    uuid_unparse(pdev->uuid, pr_uuid);
	    printf ("    [%u,%u] id:%u uuid:%s size:%zu\n",
		pdev->idx_ag, pdev->idx_dev, pdev->pool_index,
		pr_uuid, pdev->size);
	    zfm=&pdev->dev->f.zfm;
	    printf("    znode:%s topo:%s geo:%s at ionode id:%u\n",
		zfm->znode, zfm->topo, zfm->geo,
		p->ionodes[pdev->dev->f.ionode_idx].conf_id);
	}
    }

    /* IO nodes */
    printf("\nConfiguration has %u IO nodes:\n", p->ionode_count);
    for (u = 0; u < p->ionode_count; u++) {
	F_IONODE_INFO_t *info = &p->ionodes[u];

	printf("  #%u%s id:%u %s MDS:%u flags:%s znode:%s topo:%s geo:%s\n",
	    u, (u == p->mynode.ionode_id && NodeIsIOnode(&p->mynode))?"*":"",
	    info->conf_id, info->hostname, info->mds,
	    IOnodeForceHelper(info)?"H":"",
	    info->zfm.znode, info->zfm.topo, info->zfm.geo);
    }

    /* libfabric info */
    print_lf_info(p->lf_info);

    /* Layouts */
    printf("\nPool has %u layout(s).\n", pool_info->layouts_count);
    list_for_each_prev_safe(l, tmp, &p->layouts) {
	F_LAYOUT_INFO_t *info;
	F_POOLDEV_INDEX_t *pdi;

	lo = container_of(l, struct f_layout_, list);
	info = &lo->info;
	printf("\nLayout id:%u moniker %s\n",
	    info->conf_id, info->name);
        printf("  %uD+%uP chunk:%u, stripes:%u per slab, total %u slab(s)\n",
	    info->data_chunks, (info->chunks - info->data_chunks),
	    info->chunk_sz, info->slab_stripes, info->slab_count);
	printf("  This layout has %u device(s), including %u missing.\n",
	    info->devnum, info->misdevnum);
	pdi = lo->devlist;
	for (u = 0; u < lo->devlist_sz; u++, pdi++) {
	    if (pdi->pool_index == F_PDI_NONE)
		continue;
	    printf("  dev#%u pool index:%u [%u,%u] ext used/failed:%u/%u\n",
		u, pdi->pool_index, pdi->idx_ag, pdi->idx_dev,
		pdi->sha->extents_used, pdi->sha->failed_extents);
	}
	printf("  Layout partition:%u\n", lo->lp->part_num);
    }
}

/* Is 'hostname' in IO nodes list? If NULL, check my node name */
int f_host_is_ionode(const char *hostname)
{
    if (hostname)
	return !!get_ionode_info(pool, hostname);

    return pool && NodeIsIOnode(&pool->mynode);
}

/* Is 'hostname' a MD server? If NULL, check my node name */
int f_host_is_mds(const char *hostname)
{
    if (hostname) {
	F_IONODE_INFO_t *info = get_ionode_info(pool, hostname);

	return info && info->mds;
    }
    return pool && NodeHasMDS(&pool->mynode);
}

