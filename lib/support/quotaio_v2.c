/*
 * Implementation of new quotafile format
 *
 * Jan Kara <jack@suse.cz> - sponsored by SuSE CR
 */

#include "config.h"
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "quotaio_v2.h"
#include "dqblk_v2.h"
#include "quotaio.h"
#include "quotaio_tree.h"

static int v2_check_file(struct quota_handle *h, int type, int fmt);
static int v2_init_io(struct quota_handle *h);
static int v2_new_io(struct quota_handle *h);
static int v2_write_info(struct quota_handle *h);
static struct dquot *v2_read_dquot(struct quota_handle *h, qid_t id);
static int v2_commit_dquot(struct dquot *dquot);
static int v2_scan_dquots(struct quota_handle *h,
			  int (*process_dquot) (struct dquot *dquot,
						void *data),
			  void *data);
static int v2_report(struct quota_handle *h, int verbose);

struct quotafile_ops quotafile_ops_2 = {
	.check_file	= v2_check_file,
	.init_io 	= v2_init_io,
	.new_io 	= v2_new_io,
	.write_info	= v2_write_info,
	.read_dquot	= v2_read_dquot,
	.commit_dquot	= v2_commit_dquot,
	.scan_dquots	= v2_scan_dquots,
	.report		= v2_report,
};

/*
 * Copy dquot from disk to memory
 */
static void v2r0_disk2memdqblk(struct dquot *dquot, void *dp)
{
	struct util_dqblk *m = &dquot->dq_dqb;
	struct v2r0_disk_dqblk *d = dp, empty;

	dquot->dq_id = ext2fs_le32_to_cpu(d->dqb_id);
	m->dqb_ihardlimit = ext2fs_le32_to_cpu(d->dqb_ihardlimit);
	m->dqb_isoftlimit = ext2fs_le32_to_cpu(d->dqb_isoftlimit);
	m->dqb_bhardlimit = ext2fs_le32_to_cpu(d->dqb_bhardlimit);
	m->dqb_bsoftlimit = ext2fs_le32_to_cpu(d->dqb_bsoftlimit);
	m->dqb_curinodes = ext2fs_le32_to_cpu(d->dqb_curinodes);
	m->dqb_curspace = ext2fs_le64_to_cpu(d->dqb_curspace);
	m->dqb_itime = ext2fs_le64_to_cpu(d->dqb_itime);
	m->dqb_btime = ext2fs_le64_to_cpu(d->dqb_btime);

	memset(&empty, 0, sizeof(struct v2r0_disk_dqblk));
	empty.dqb_itime = ext2fs_cpu_to_le64(1);
	if (!memcmp(&empty, dp, sizeof(struct v2r0_disk_dqblk)))
		m->dqb_itime = 0;
}

/*
 * Copy dquot from memory to disk
 */
static void v2r0_mem2diskdqblk(void *dp, struct dquot *dquot)
{
	struct util_dqblk *m = &dquot->dq_dqb;
	struct v2r0_disk_dqblk *d = dp;

	d->dqb_ihardlimit = ext2fs_cpu_to_le32(m->dqb_ihardlimit);
	d->dqb_isoftlimit = ext2fs_cpu_to_le32(m->dqb_isoftlimit);
	d->dqb_bhardlimit = ext2fs_cpu_to_le32(m->dqb_bhardlimit);
	d->dqb_bsoftlimit = ext2fs_cpu_to_le32(m->dqb_bsoftlimit);
	d->dqb_curinodes = ext2fs_cpu_to_le32(m->dqb_curinodes);
	d->dqb_curspace = ext2fs_cpu_to_le64(m->dqb_curspace);
	d->dqb_itime = ext2fs_cpu_to_le64(m->dqb_itime);
	d->dqb_btime = ext2fs_cpu_to_le64(m->dqb_btime);
	d->dqb_id = ext2fs_cpu_to_le32(dquot->dq_id);
	if (qtree_entry_unused(&dquot->dq_h->qh_info.u.v2_mdqi.dqi_qtree, dp))
		d->dqb_itime = ext2fs_cpu_to_le64(1);
}

static int v2r0_is_id(void *dp, struct dquot *dquot)
{
	struct v2r0_disk_dqblk *d = dp;
	struct qtree_mem_dqinfo *info =
			&dquot->dq_h->qh_info.u.v2_mdqi.dqi_qtree;

	if (qtree_entry_unused(info, dp))
		return 0;
	return ext2fs_le32_to_cpu(d->dqb_id) == dquot->dq_id;
}

static struct qtree_fmt_operations v2r0_fmt_ops = {
	.mem2disk_dqblk = v2r0_mem2diskdqblk,
	.disk2mem_dqblk = v2r0_disk2memdqblk,
	.is_id = v2r0_is_id,
};

/*
 * Copy dquot from disk to memory
 */
static void v2r1_disk2memdqblk(struct dquot *dquot, void *dp)
{
	struct util_dqblk *m = &dquot->dq_dqb;
	struct v2r1_disk_dqblk *d = dp, empty;

	dquot->dq_id = ext2fs_le32_to_cpu(d->dqb_id);
	m->dqb_ihardlimit = ext2fs_le64_to_cpu(d->dqb_ihardlimit);
	m->dqb_isoftlimit = ext2fs_le64_to_cpu(d->dqb_isoftlimit);
	m->dqb_bhardlimit = ext2fs_le64_to_cpu(d->dqb_bhardlimit);
	m->dqb_bsoftlimit = ext2fs_le64_to_cpu(d->dqb_bsoftlimit);
	m->dqb_curinodes = ext2fs_le64_to_cpu(d->dqb_curinodes);
	m->dqb_curspace = ext2fs_le64_to_cpu(d->dqb_curspace);
	m->dqb_itime = ext2fs_le64_to_cpu(d->dqb_itime);
	m->dqb_btime = ext2fs_le64_to_cpu(d->dqb_btime);

	memset(&empty, 0, sizeof(struct v2r1_disk_dqblk));
	empty.dqb_itime = ext2fs_cpu_to_le64(1);
	if (!memcmp(&empty, dp, sizeof(struct v2r1_disk_dqblk)))
		m->dqb_itime = 0;
}

/*
 * Copy dquot from memory to disk
 */
static void v2r1_mem2diskdqblk(void *dp, struct dquot *dquot)
{
	struct util_dqblk *m = &dquot->dq_dqb;
	struct v2r1_disk_dqblk *d = dp;

	d->dqb_ihardlimit = ext2fs_cpu_to_le64(m->dqb_ihardlimit);
	d->dqb_isoftlimit = ext2fs_cpu_to_le64(m->dqb_isoftlimit);
	d->dqb_bhardlimit = ext2fs_cpu_to_le64(m->dqb_bhardlimit);
	d->dqb_bsoftlimit = ext2fs_cpu_to_le64(m->dqb_bsoftlimit);
	d->dqb_curinodes = ext2fs_cpu_to_le64(m->dqb_curinodes);
	d->dqb_curspace = ext2fs_cpu_to_le64(m->dqb_curspace);
	d->dqb_itime = ext2fs_cpu_to_le64(m->dqb_itime);
	d->dqb_btime = ext2fs_cpu_to_le64(m->dqb_btime);
	d->dqb_id = ext2fs_cpu_to_le32(dquot->dq_id);
	if (qtree_entry_unused(&dquot->dq_h->qh_info.u.v2_mdqi.dqi_qtree, dp))
		d->dqb_itime = ext2fs_cpu_to_le64(1);
}

static int v2r1_is_id(void *dp, struct dquot *dquot)
{
	struct v2r1_disk_dqblk *d = dp;
	struct qtree_mem_dqinfo *info =
			&dquot->dq_h->qh_info.u.v2_mdqi.dqi_qtree;

	if (qtree_entry_unused(info, dp))
		return 0;
	return ext2fs_le32_to_cpu(d->dqb_id) == dquot->dq_id;
}

static struct qtree_fmt_operations v2r1_fmt_ops = {
	.mem2disk_dqblk = v2r1_mem2diskdqblk,
	.disk2mem_dqblk = v2r1_disk2memdqblk,
	.is_id = v2r1_is_id,
};

/*
 * Copy dqinfo from disk to memory
 */
static inline void v2_disk2memdqinfo(struct util_dqinfo *m,
				     struct v2_disk_dqinfo *d)
{
	m->dqi_bgrace = ext2fs_le32_to_cpu(d->dqi_bgrace);
	m->dqi_igrace = ext2fs_le32_to_cpu(d->dqi_igrace);
	m->u.v2_mdqi.dqi_flags = ext2fs_le32_to_cpu(d->dqi_flags) & V2_DQF_MASK;
	m->u.v2_mdqi.dqi_qtree.dqi_blocks = ext2fs_le32_to_cpu(d->dqi_blocks);
	m->u.v2_mdqi.dqi_qtree.dqi_free_blk =
		ext2fs_le32_to_cpu(d->dqi_free_blk);
	m->u.v2_mdqi.dqi_qtree.dqi_free_entry =
				ext2fs_le32_to_cpu(d->dqi_free_entry);
}

/*
 * Copy dqinfo from memory to disk
 */
static inline void v2_mem2diskdqinfo(struct v2_disk_dqinfo *d,
				     struct util_dqinfo *m)
{
	d->dqi_bgrace = ext2fs_cpu_to_le32(m->dqi_bgrace);
	d->dqi_igrace = ext2fs_cpu_to_le32(m->dqi_igrace);
	d->dqi_flags = ext2fs_cpu_to_le32(m->u.v2_mdqi.dqi_flags & V2_DQF_MASK);
	d->dqi_blocks = ext2fs_cpu_to_le32(m->u.v2_mdqi.dqi_qtree.dqi_blocks);
	d->dqi_free_blk =
		ext2fs_cpu_to_le32(m->u.v2_mdqi.dqi_qtree.dqi_free_blk);
	d->dqi_free_entry =
		ext2fs_cpu_to_le32(m->u.v2_mdqi.dqi_qtree.dqi_free_entry);
}

static int v2_read_header(struct quota_handle *h, struct v2_disk_dqheader *dqh)
{
	if (h->e2fs_read(&h->qh_qf, 0, dqh, sizeof(struct v2_disk_dqheader)) !=
			sizeof(struct v2_disk_dqheader))
		return 0;

	return 1;
}

/*
 * Check whether given quota file is in our format
 */
static int v2_check_file(struct quota_handle *h, int type, int fmt)
{
	struct v2_disk_dqheader dqh;
	int file_magics[] = INITQMAGICS;
	int be_magic;

	if (fmt != QFMT_VFS_V1)
		return 0;

	if (!v2_read_header(h, &dqh))
		return 0;

	be_magic = ext2fs_be32_to_cpu((__force __be32)dqh.dqh_magic);
	if (be_magic == file_magics[type]) {
		log_err("Your quota file is stored in wrong endianness");
		return 0;
	}
	if (V2_VERSION_R0 != ext2fs_le32_to_cpu(dqh.dqh_version) &&
	    V2_VERSION_R1 != ext2fs_le32_to_cpu(dqh.dqh_version))
		return 0;
	return 1;
}

/*
 * Open quotafile
 */
static int v2_init_io(struct quota_handle *h)
{
	struct v2_disk_dqheader dqh;
	struct v2_disk_dqinfo ddqinfo;
	struct v2_mem_dqinfo *info;
	__u64 filesize;
	int version;

	if (!v2_read_header(h, &dqh))
		return -1;
	version = ext2fs_le32_to_cpu(dqh.dqh_version);

	if (version == V2_VERSION_R0) {
		h->qh_info.u.v2_mdqi.dqi_qtree.dqi_entry_size =
			sizeof(struct v2r0_disk_dqblk);
		h->qh_info.u.v2_mdqi.dqi_qtree.dqi_ops = &v2r0_fmt_ops;
	} else {
		h->qh_info.u.v2_mdqi.dqi_qtree.dqi_entry_size =
			sizeof(struct v2r1_disk_dqblk);
		h->qh_info.u.v2_mdqi.dqi_qtree.dqi_ops = &v2r1_fmt_ops;
	}

	/* Read information about quotafile */
	if (h->e2fs_read(&h->qh_qf, V2_DQINFOOFF, &ddqinfo,
			 sizeof(ddqinfo)) != sizeof(ddqinfo))
		return -1;
	v2_disk2memdqinfo(&h->qh_info, &ddqinfo);

	/* Check to make sure quota file info is sane */
	info = &h->qh_info.u.v2_mdqi;
	if (ext2fs_file_get_lsize(h->qh_qf.e2_file, &filesize))
		return -1;
	if ((filesize > (1U << 31)) ||
	    (info->dqi_qtree.dqi_blocks >
	     (filesize + QT_BLKSIZE - 1) >> QT_BLKSIZE_BITS)) {
		log_err("Quota inode %u corrupted: file size %llu; "
			"dqi_blocks %u", h->qh_qf.ino,
			(unsigned long long) filesize,
			info->dqi_qtree.dqi_blocks);
		return -1;
	}
	if (info->dqi_qtree.dqi_free_blk >= info->dqi_qtree.dqi_blocks) {
		log_err("Quota inode %u corrupted: free_blk %u; dqi_blocks %u",
			h->qh_qf.ino, info->dqi_qtree.dqi_free_blk,
			info->dqi_qtree.dqi_blocks);
		return -1;
	}
	if (info->dqi_qtree.dqi_free_entry >= info->dqi_qtree.dqi_blocks) {
		log_err("Quota inode %u corrupted: free_entry %u; "
			"dqi_blocks %u", h->qh_qf.ino,
			info->dqi_qtree.dqi_free_entry,
			info->dqi_qtree.dqi_blocks);
		return -1;
	}
	return 0;
}

/*
 * Initialize new quotafile
 */
static int v2_new_io(struct quota_handle *h)
{
	int file_magics[] = INITQMAGICS;
	struct v2_disk_dqheader ddqheader;
	struct v2_disk_dqinfo ddqinfo;

	if (h->qh_fmt != QFMT_VFS_V1)
		return -1;

	/* Write basic quota header */
	ddqheader.dqh_magic = ext2fs_cpu_to_le32(file_magics[h->qh_type]);
	ddqheader.dqh_version = ext2fs_cpu_to_le32(V2_VERSION_R1);
	if (h->e2fs_write(&h->qh_qf, 0, &ddqheader, sizeof(ddqheader)) !=
			sizeof(ddqheader))
		return -1;

	/* Write information about quotafile */
	h->qh_info.dqi_bgrace = MAX_DQ_TIME;
	h->qh_info.dqi_igrace = MAX_IQ_TIME;
	h->qh_info.u.v2_mdqi.dqi_flags = 0;
	h->qh_info.u.v2_mdqi.dqi_qtree.dqi_blocks = QT_TREEOFF + 1;
	h->qh_info.u.v2_mdqi.dqi_qtree.dqi_free_blk = 0;
	h->qh_info.u.v2_mdqi.dqi_qtree.dqi_free_entry = 0;
	h->qh_info.u.v2_mdqi.dqi_qtree.dqi_entry_size =
				sizeof(struct v2r1_disk_dqblk);
	h->qh_info.u.v2_mdqi.dqi_qtree.dqi_ops = &v2r1_fmt_ops;
	v2_mem2diskdqinfo(&ddqinfo, &h->qh_info);
	if (h->e2fs_write(&h->qh_qf, V2_DQINFOOFF, &ddqinfo,
			  sizeof(ddqinfo)) !=
	    sizeof(ddqinfo))
		return -1;

	return 0;
}

/*
 * Write information (grace times to file)
 */
static int v2_write_info(struct quota_handle *h)
{
	struct v2_disk_dqinfo ddqinfo;

	v2_mem2diskdqinfo(&ddqinfo, &h->qh_info);
	if (h->e2fs_write(&h->qh_qf, V2_DQINFOOFF, &ddqinfo, sizeof(ddqinfo)) !=
			sizeof(ddqinfo))
		return -1;

	return 0;
}

/*
 * Read dquot from disk
 */
static struct dquot *v2_read_dquot(struct quota_handle *h, qid_t id)
{
	return qtree_read_dquot(h, id);
}

/*
 * Commit changes of dquot to disk - it might also mean deleting it when quota
 * became fake one and user has no blocks.
 * User can process use 'errno' to detect errstr.
 */
static int v2_commit_dquot(struct dquot *dquot)
{
	struct util_dqblk *b = &dquot->dq_dqb;

	if (!b->dqb_curspace && !b->dqb_curinodes && !b->dqb_bsoftlimit &&
	    !b->dqb_isoftlimit && !b->dqb_bhardlimit && !b->dqb_ihardlimit)
		qtree_delete_dquot(dquot);
	else
		qtree_write_dquot(dquot);
	return 0;
}

static int v2_scan_dquots(struct quota_handle *h,
			  int (*process_dquot) (struct dquot *, void *),
			  void *data)
{
	return qtree_scan_dquots(h, process_dquot, data);
}

/* Report information about quotafile.
 * TODO: Not used right now, but we should be able to use this when we add
 * support to debugfs to read quota files.
 */
static int v2_report(struct quota_handle *h EXT2FS_ATTR((unused)),
		     int verbose EXT2FS_ATTR((unused)))
{
	log_err("Not Implemented.");
	return -1;
}
