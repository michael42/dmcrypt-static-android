/*
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LVM_METAD_H
#define _LVM_METAD_H

struct volume_group;
struct cmd_context;
struct dm_config_tree;

#ifdef LVMETAD_SUPPORT
/*
 * Initialise the communication with lvmetad. Normally called by
 * lvmcache_init. Sets up a global handle for our process.
 */
void lvmetad_init(void);

/*
 * Override the use of lvmetad for retrieving scan results and metadata.
 */
void lvmetad_set_active(int);

/*
 * Check whether lvmetad is active (where active means both that it is running
 * and that we have a working connection with it).
 */
int lvmetad_active(void);

/*
 * Send a new version of VG metadata to lvmetad. This is normally called after
 * vg_write but before vg_commit. After vg_commit, lvmetad_vg_commit is called
 * to seal the transaction. The result of lvmetad_vg_update is that the new
 * metadata is stored tentatively in lvmetad, but it is not used until
 * lvmetad_vg_commit. The request is validated immediately and lvmetad_vg_commit
 * only constitutes a pointer update.
 */
int lvmetad_vg_update(struct volume_group *vg);

/*
 * Inform lvmetad that a VG has been removed. This is not entirely safe, but is
 * only needed during vgremove, which does not wipe PV labels and therefore
 * cannot mark the PVs as gone.
 */
int lvmetad_vg_remove(struct volume_group *vg);

/*
 * Notify lvmetad that a PV has been found. It is not an error if the PV is
 * already marked as present in lvmetad. If a non-NULL vg pointer is supplied,
 * it is taken to represent the metadata read from the MDA(s) present on that
 * PV. It *is* an error if: the VG is already known to lvmetad, the sequence
 * number on the cached and on the discovered PV match but the metadata content
 * does not.
 */
int lvmetad_pv_found(struct id pvid, struct device *device,
		     const struct format_type *fmt, uint64_t label_sector,
		     struct volume_group *vg);

/*
 * Inform the daemon that the device no longer exists.
 */
int lvmetad_pv_gone(dev_t devno, const char *pv_name);
int lvmetad_pv_gone_by_dev(struct device *dev);

/*
 * Request a list of all PVs available to lvmetad. If requested, this will also
 * read labels off all the PVs to populate lvmcache.
 */
int lvmetad_pv_list_to_lvmcache(struct cmd_context *cmd);

/*
 * Lookup an individual PV.
 * If found is not NULL, it is set according to whether or not the PV is found,
 * otherwise if the PV is not found an error is returned.
 */
int lvmetad_pv_lookup(struct cmd_context *cmd, struct id pvid, int *found);
int lvmetad_pv_lookup_by_dev(struct cmd_context *cmd, struct device *dev, int *found);

/*
 * Request a list of all VGs available to lvmetad and use it to fill in
 * lvmcache..
 */
int lvmetad_vg_list_to_lvmcache(struct cmd_context *cmd);

/*
 * Find a VG by its ID or its name in the lvmetad cache. Gives NULL if the VG is
 * not found.
 */
struct volume_group *lvmetad_vg_lookup(struct cmd_context *cmd,
				       const char *vgname, const char *vgid);

/*
 * Scan a single device and update lvmetad with the result(s).
 */
int pvscan_lvmetad_single(struct cmd_context *cmd, struct device *dev);

#  else		/* LVMETAD_SUPPORT */

#    define lvmetad_init()	do { } while (0)
#    define lvmetad_set_active(a)	do { } while (0)
#    define lvmetad_active()	(0)
#    define lvmetad_vg_update(vg)	(1)
#    define lvmetad_vg_remove(vg)	(1)
#    define lvmetad_pv_found(pvid, device, fmt, label_sector, vg)	(1)
#    define lvmetad_pv_gone(devno, pv_name)	(1)
#    define lvmetad_pv_gone_by_dev(dev)	(1)
#    define lvmetad_pv_list_to_lvmcache(cmd)	(1)
#    define lvmetad_pv_lookup(cmd, pvid, found)	(0)
#    define lvmetad_pv_lookup_by_dev(cmd, dev, found)	(0)
#    define lvmetad_vg_list_to_lvmcache(cmd)	(1)
#    define lvmetad_vg_lookup(cmd, vgname, vgid)	(NULL)
#    define pvscan_lvmetad_single(cmd, dev)	(0)

#  endif	/* LVMETAD_SUPPORT */

#endif
