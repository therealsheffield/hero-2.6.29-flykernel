/*
 * Copyright (C) 2005-2009 Junjiro R. Okajima
 *
 * This program, aufs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 * inode operations (del entry)
 */

#include "aufs.h"

/*
 * decide if a new whiteout for @dentry is necessary or not.
 * when it is necessary, prepare the parent dir for the upper branch whose
 * branch index is @bcpup for creation. the actual creation of the whiteout will
 * be done by caller.
 * return value:
 * 0: wh is unnecessary
 * plus: wh is necessary
 * minus: error
 */
int au_wr_dir_need_wh(struct dentry *dentry, int isdir, aufs_bindex_t *bcpup)
{
	int need_wh, err;
	aufs_bindex_t bstart;
	struct super_block *sb;

	sb = dentry->d_sb;
	bstart = au_dbstart(dentry);
	if (*bcpup < 0) {
		*bcpup = bstart;
		if (au_test_ro(sb, bstart, dentry->d_inode)) {
			err = AuWbrCopyup(au_sbi(sb), dentry);
			*bcpup = err;
			if (unlikely(err < 0))
				goto out;
		}
	} else
		AuDebugOn(bstart < *bcpup
			  || au_test_ro(sb, *bcpup, dentry->d_inode));
	AuDbg("bcpup %d, bstart %d\n", *bcpup, bstart);

	if (*bcpup != bstart) {
		err = au_cpup_dirs(dentry, *bcpup);
		if (unlikely(err))
			goto out;
		need_wh = 1;
	} else {
		aufs_bindex_t old_bend, new_bend, bdiropq = -1;

		old_bend = au_dbend(dentry);
		if (isdir) {
			bdiropq = au_dbdiropq(dentry);
			au_set_dbdiropq(dentry, -1);
		}
		need_wh = au_lkup_dentry(dentry, bstart + 1, /*type*/0,
					 /*nd*/NULL);
		err = need_wh;
		if (isdir)
			au_set_dbdiropq(dentry, bdiropq);
		if (unlikely(err < 0))
			goto out;
		new_bend = au_dbend(dentry);
		if (!need_wh && old_bend != new_bend) {
			au_set_h_dptr(dentry, new_bend, NULL);
			au_set_dbend(dentry, old_bend);
		}
	}
	AuDbg("need_wh %d\n", need_wh);
	err = need_wh;

 out:
	return err;
}

/*
 * simple tests for the del-entry operations.
 * following the checks in vfs, plus the parent-child relationship.
 */
int au_may_del(struct dentry *dentry, aufs_bindex_t bindex,
	       struct dentry *h_parent, int isdir)
{
	int err;
	umode_t h_mode;
	struct dentry *h_dentry, *h_latest;
	struct inode *h_inode;

	h_dentry = au_h_dptr(dentry, bindex);
	h_inode = h_dentry->d_inode;
	if (dentry->d_inode) {
		err = -ENOENT;
		if (unlikely(!h_inode || !h_inode->i_nlink))
			goto out;

		h_mode = h_inode->i_mode;
		if (!isdir) {
			err = -EISDIR;
			if (unlikely(S_ISDIR(h_mode)))
				goto out;
		} else if (unlikely(!S_ISDIR(h_mode))) {
			err = -ENOTDIR;
			goto out;
		}
	} else {
		/* rename(2) case */
		err = -EIO;
		if (unlikely(h_inode))
			goto out;
	}

	err = -ENOENT;
	/* expected parent dir is locked */
	if (unlikely(h_parent != h_dentry->d_parent))
		goto out;
	err = 0;

	/*
	 * rmdir a dir may break the consistency on some filesystem.
	 * let's try heavy test.
	 */
	err = -EACCES;
	if (unlikely(au_test_h_perm(h_parent->d_inode, MAY_EXEC | MAY_WRITE)))
		goto out;

	h_latest = au_sio_lkup_one(&dentry->d_name, h_parent,
				   au_sbr(dentry->d_sb, bindex));
	err = -EIO;
	if (IS_ERR(h_latest))
		goto out;
	if (h_latest == h_dentry)
		err = 0;
	dput(h_latest);

 out:
	return err;
}

/*
 * decide the branch where we operate for @dentry. the branch index will be set
 * @rbcpup. after diciding it, 'pin' it and store the timestamps of the parent
 * dir for reverting.
 * when a new whiteout is necessary, create it.
 */
static struct dentry*
lock_hdir_create_wh(struct dentry *dentry, int isdir, aufs_bindex_t *rbcpup,
		    struct au_dtime *dt, struct au_pin *pin)
{
	struct dentry *wh_dentry;
	struct super_block *sb;
	struct path h_path;
	int err, need_wh;
	unsigned int udba;
	aufs_bindex_t bcpup;

	need_wh = au_wr_dir_need_wh(dentry, isdir, rbcpup);
	wh_dentry = ERR_PTR(need_wh);
	if (unlikely(need_wh < 0))
		goto out;

	sb = dentry->d_sb;
	udba = au_opt_udba(sb);
	bcpup = *rbcpup;
	err = au_pin(pin, dentry, bcpup, udba,
		     AuPin_DI_LOCKED | AuPin_MNT_WRITE);
	wh_dentry = ERR_PTR(err);
	if (unlikely(err))
		goto out;

	h_path.dentry = au_pinned_h_parent(pin);
	if (udba != AuOpt_UDBA_NONE
	    && au_dbstart(dentry) == bcpup) {
		err = au_may_del(dentry, bcpup, h_path.dentry, isdir);
		wh_dentry = ERR_PTR(err);
		if (unlikely(err))
			goto out_unpin;
	}

	h_path.mnt = au_sbr_mnt(sb, bcpup);
	au_dtime_store(dt, au_pinned_parent(pin), &h_path);
	wh_dentry = NULL;
	if (!need_wh)
		goto out; /* success, no need to create whiteout */

	wh_dentry = au_wh_create(dentry, bcpup, h_path.dentry);
	if (!IS_ERR(wh_dentry))
		goto out; /* success */
	/* returns with the parent is locked and wh_dentry is dget-ed */

 out_unpin:
	au_unpin(pin);
 out:
	return wh_dentry;
}

/*
 * when removing a dir, rename it to a unique temporary whiteout-ed name first
 * in order to be revertible and save time for removing many child whiteouts
 * under the dir.
 * returns 1 when there are too many child whiteout and caller should remove
 * them asynchronously. returns 0 when the number of children is enough small to
 * remove now or the branch fs is a remote fs.
 * otherwise return an error.
 */
static int renwh_and_rmdir(struct dentry *dentry, aufs_bindex_t bindex,
			   struct au_nhash *whlist, struct inode *dir)
{
	int rmdir_later, err, dirwh;
	struct dentry *h_dentry;
	struct super_block *sb;

	sb = dentry->d_sb;
	SiMustAnyLock(sb);
	h_dentry = au_h_dptr(dentry, bindex);
	err = au_whtmp_ren(h_dentry, au_sbr(sb, bindex));
	if (unlikely(err))
		goto out;

	/* stop monitoring */
	au_hin_free(au_hi(dentry->d_inode, bindex));

	if (!au_test_fs_remote(h_dentry->d_sb)) {
		dirwh = au_sbi(sb)->si_dirwh;
		rmdir_later = (dirwh <= 1);
		if (!rmdir_later)
			rmdir_later = au_nhash_test_longer_wh(whlist, bindex,
							      dirwh);
		if (rmdir_later)
			return rmdir_later;
	}

	err = au_whtmp_rmdir(dir, bindex, h_dentry, whlist);
	if (unlikely(err)) {
		AuIOErr("rmdir %.*s, b%d failed, %d. ignored\n",
			AuDLNPair(h_dentry), bindex, err);
		err = 0;
	}

 out:
	return err;
}

/*
 * final procedure for deleting a entry.
 * maintain dentry and iattr.
 */
static void epilog(struct inode *dir, struct dentry *dentry,
		   aufs_bindex_t bindex)
{
	struct inode *inode;

	inode = dentry->d_inode;
	d_drop(dentry);
	inode->i_ctime = dir->i_ctime;

	if (atomic_read(&dentry->d_count) == 1) {
		au_set_h_dptr(dentry, au_dbstart(dentry), NULL);
		au_update_dbstart(dentry);
	}
	if (au_ibstart(dir) == bindex)
		au_cpup_attr_timesizes(dir);
	dir->i_version++;
}

/*
 * when an error happened, remove the created whiteout and revert everything.
 */
static int do_revert(int err, struct inode *dir, aufs_bindex_t bwh,
		     struct dentry *wh_dentry, struct dentry *dentry,
		     struct au_dtime *dt)
{
	int rerr;
	struct path h_path = {
		.dentry	= wh_dentry,
		.mnt	= au_sbr_mnt(dir->i_sb, bwh)
	};

	rerr = au_wh_unlink_dentry(au_h_iptr(dir, bwh), &h_path, dentry);
	if (!rerr) {
		au_set_dbwh(dentry, bwh);
		au_dtime_revert(dt);
		return 0;
	}

	AuIOErr("%.*s reverting whiteout failed(%d, %d)\n",
		AuDLNPair(dentry), err, rerr);
	return -EIO;
}

/* ---------------------------------------------------------------------- */

int aufs_unlink(struct inode *dir, struct dentry *dentry)
{
	int err;
	aufs_bindex_t bwh, bindex, bstart;
	struct au_dtime dt;
	struct au_pin pin;
	struct path h_path;
	struct inode *inode, *h_dir;
	struct dentry *parent, *wh_dentry;

	IMustLock(dir);
	inode = dentry->d_inode;
	if (unlikely(!inode))
		return -ENOENT; /* possible? */
	IMustLock(inode);

	aufs_read_lock(dentry, AuLock_DW);
	parent = dentry->d_parent; /* dir inode is locked */
	di_write_lock_parent(parent);

	bstart = au_dbstart(dentry);
	bwh = au_dbwh(dentry);
	bindex = -1;
	wh_dentry = lock_hdir_create_wh(dentry, /*isdir*/0, &bindex, &dt, &pin);
	err = PTR_ERR(wh_dentry);
	if (IS_ERR(wh_dentry))
		goto out;

	h_path.mnt = au_sbr_mnt(dentry->d_sb, bstart);
	h_path.dentry = au_h_dptr(dentry, bstart);
	dget(h_path.dentry);
	if (bindex == bstart) {
		h_dir = au_pinned_h_dir(&pin);
		err = vfsub_unlink(h_dir, &h_path, /*force*/0);
	} else {
		/* dir inode is locked */
		h_dir = wh_dentry->d_parent->d_inode;
		IMustLock(h_dir);
		err = 0;
	}

	if (!err) {
		drop_nlink(inode);
		epilog(dir, dentry, bindex);

		/* update target timestamps */
		if (bindex == bstart) {
			vfsub_update_h_iattr(&h_path, /*did*/NULL); /*ignore*/
			inode->i_ctime = h_path.dentry->d_inode->i_ctime;
		} else
			/* todo: this timestamp may be reverted later */
			inode->i_ctime = h_dir->i_ctime;
		goto out_unlock; /* success */
	}

	/* revert */
	if (wh_dentry) {
		int rerr;

		rerr = do_revert(err, dir, bwh, wh_dentry, dentry, &dt);
		if (rerr)
			err = rerr;
	}

 out_unlock:
	au_unpin(&pin);
	dput(wh_dentry);
	dput(h_path.dentry);
 out:
	di_write_unlock(parent);
	aufs_read_unlock(dentry, AuLock_DW);
	return err;
}

int aufs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int err, rmdir_later;
	aufs_bindex_t bwh, bindex, bstart;
	struct au_dtime dt;
	struct au_pin pin;
	struct inode *inode;
	struct dentry *parent, *wh_dentry, *h_dentry;
	struct au_whtmp_rmdir *args;

	IMustLock(dir);
	inode = dentry->d_inode;
	err = -ENOENT; /* possible? */
	if (unlikely(!inode))
		goto out;
	IMustLock(inode);

	aufs_read_lock(dentry, AuLock_DW | AuLock_FLUSH);
	err = -ENOMEM;
	args = au_whtmp_rmdir_alloc(dir->i_sb, GFP_NOFS);
	if (unlikely(!args))
		goto out_unlock;

	parent = dentry->d_parent; /* dir inode is locked */
	di_write_lock_parent(parent);
	err = au_test_empty(dentry, &args->whlist);
	if (unlikely(err))
		goto out_args;

	bstart = au_dbstart(dentry);
	bwh = au_dbwh(dentry);
	bindex = -1;
	wh_dentry = lock_hdir_create_wh(dentry, /*isdir*/1, &bindex, &dt, &pin);
	err = PTR_ERR(wh_dentry);
	if (IS_ERR(wh_dentry))
		goto out_args;

	h_dentry = au_h_dptr(dentry, bstart);
	dget(h_dentry);
	rmdir_later = 0;
	if (bindex == bstart) {
		err = renwh_and_rmdir(dentry, bstart, &args->whlist, dir);
		if (err > 0) {
			rmdir_later = err;
			err = 0;
		}
	} else {
		/* stop monitoring */
		au_hin_free(au_hi(inode, bstart));

		/* dir inode is locked */
		IMustLock(wh_dentry->d_parent->d_inode);
		err = 0;
	}

	if (!err) {
		clear_nlink(inode);
		au_set_dbdiropq(dentry, -1);
		epilog(dir, dentry, bindex);

		if (rmdir_later) {
			au_whtmp_kick_rmdir(dir, bstart, h_dentry, args);
			args = NULL;
		}

		goto out_unpin; /* success */
	}

	/* revert */
	AuLabel(revert);
	if (wh_dentry) {
		int rerr;

		rerr = do_revert(err, dir, bwh, wh_dentry, dentry, &dt);
		if (rerr)
			err = rerr;
	}

 out_unpin:
	au_unpin(&pin);
	dput(wh_dentry);
	dput(h_dentry);
 out_args:
	di_write_unlock(parent);
	if (args)
		au_whtmp_rmdir_free(args);
 out_unlock:
	aufs_read_unlock(dentry, AuLock_DW);
 out:
	return err;
}
