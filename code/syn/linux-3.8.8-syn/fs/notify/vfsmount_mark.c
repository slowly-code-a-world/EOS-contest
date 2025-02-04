/*
 *  Copyright (C) 2008 Red Hat, Inc., Eric Paris <eparis@redhat.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

#include <linux/atomic.h>

#include <linux/fsnotify_backend.h>
#include "fsnotify.h"
#include "../mount.h"

void fsnotify_clear_marks_by_mount(struct vfsmount *mnt)
{
	struct fsnotify_mark *mark, *lmark;
	struct hlist_node *pos, *n;
	struct mount *m = real_mount(mnt);
	volatile struct qnode node;
	enum release_mode mode;

	LIST_HEAD(free_list);

	mode = acquire_mixed_lock(&mnt->mnt_root->d_lock, &node);
	hlist_for_each_entry_safe(mark, pos, n, &m->mnt_fsnotify_marks, m.m_list) {
		list_add(&mark->m.free_m_list, &free_list);
		hlist_del_init_rcu(&mark->m.m_list);
		fsnotify_get_mark(mark);
	}
	release_mixed_lock(&mnt->mnt_root->d_lock, &node, mode);

	list_for_each_entry_safe(mark, lmark, &free_list, m.free_m_list) {
		struct fsnotify_group *group;

		spin_lock(&mark->lock);
		fsnotify_get_group(mark->group);
		group = mark->group;
		spin_unlock(&mark->lock);

		fsnotify_destroy_mark(mark, group);
		fsnotify_put_mark(mark);
		fsnotify_put_group(group);
	}
}

void fsnotify_clear_vfsmount_marks_by_group(struct fsnotify_group *group)
{
	fsnotify_clear_marks_by_group_flags(group, FSNOTIFY_MARK_FLAG_VFSMOUNT);
}

/*
 * Recalculate the mask of events relevant to a given vfsmount locked.
 */
static void fsnotify_recalc_vfsmount_mask_locked(struct vfsmount *mnt)
{
	struct mount *m = real_mount(mnt);
	struct fsnotify_mark *mark;
	struct hlist_node *pos;
	__u32 new_mask = 0;


	hlist_for_each_entry(mark, pos, &m->mnt_fsnotify_marks, m.m_list)
		new_mask |= mark->mask;
	m->mnt_fsnotify_mask = new_mask;
}

/*
 * Recalculate the mnt->mnt_fsnotify_mask, or the mask of all FS_* event types
 * any notifier is interested in hearing for this mount point
 */
void fsnotify_recalc_vfsmount_mask(struct vfsmount *mnt)
{
	volatile struct qnode node;
	enum release_mode mode;

	mode = acquire_mixed_lock(&mnt->mnt_root->d_lock, &node);
	fsnotify_recalc_vfsmount_mask_locked(mnt);
	release_mixed_lock(&mnt->mnt_root->d_lock, &node, mode);
}

void fsnotify_destroy_vfsmount_mark(struct fsnotify_mark *mark)
{
	struct vfsmount *mnt = mark->m.mnt;
	volatile struct qnode node;
	enum release_mode mode;

	BUG_ON(!mutex_is_locked(&mark->group->mark_mutex));
	assert_spin_locked(&mark->lock);

	mode = acquire_mixed_lock(&mnt->mnt_root->d_lock, &node);

	hlist_del_init_rcu(&mark->m.m_list);
	mark->m.mnt = NULL;

	fsnotify_recalc_vfsmount_mask_locked(mnt);

	release_mixed_lock(&mnt->mnt_root->d_lock, &node, mode);
}

static struct fsnotify_mark *fsnotify_find_vfsmount_mark_locked(struct fsnotify_group *group,
								struct vfsmount *mnt)
{
	struct mount *m = real_mount(mnt);
	struct fsnotify_mark *mark;
	struct hlist_node *pos;


	hlist_for_each_entry(mark, pos, &m->mnt_fsnotify_marks, m.m_list) {
		if (mark->group == group) {
			fsnotify_get_mark(mark);
			return mark;
		}
	}
	return NULL;
}

/*
 * given a group and vfsmount, find the mark associated with that combination.
 * if found take a reference to that mark and return it, else return NULL
 */
struct fsnotify_mark *fsnotify_find_vfsmount_mark(struct fsnotify_group *group,
						  struct vfsmount *mnt)
{
	struct fsnotify_mark *mark;
	volatile struct qnode node;
	enum release_mode mode;

	mode = acquire_mixed_lock(&mnt->mnt_root->d_lock, &node);
	mark = fsnotify_find_vfsmount_mark_locked(group, mnt);
	release_mixed_lock(&mnt->mnt_root->d_lock, &node, mode);

	return mark;
}

/*
 * Attach an initialized mark to a given group and vfsmount.
 * These marks may be used for the fsnotify backend to determine which
 * event types should be delivered to which groups.
 */
int fsnotify_add_vfsmount_mark(struct fsnotify_mark *mark,
			       struct fsnotify_group *group, struct vfsmount *mnt,
			       int allow_dups)
{
	struct mount *m = real_mount(mnt);
	struct fsnotify_mark *lmark;
	struct hlist_node *node, *last = NULL;
	int ret = 0;
	volatile struct qnode xnode;
	enum release_mode xmode;

	mark->flags |= FSNOTIFY_MARK_FLAG_VFSMOUNT;

	BUG_ON(!mutex_is_locked(&group->mark_mutex));
	assert_spin_locked(&mark->lock);

	xmode = acquire_mixed_lock(&mnt->mnt_root->d_lock, &xnode);

	mark->m.mnt = mnt;

	/* is mark the first mark? */
	if (hlist_empty(&m->mnt_fsnotify_marks)) {
		hlist_add_head_rcu(&mark->m.m_list, &m->mnt_fsnotify_marks);
		goto out;
	}

	/* should mark be in the middle of the current list? */
	hlist_for_each_entry(lmark, node, &m->mnt_fsnotify_marks, m.m_list) {
		last = node;

		if ((lmark->group == group) && !allow_dups) {
			ret = -EEXIST;
			goto out;
		}

		if (mark->group->priority < lmark->group->priority)
			continue;

		if ((mark->group->priority == lmark->group->priority) &&
		    (mark->group < lmark->group))
			continue;

		hlist_add_before_rcu(&mark->m.m_list, &lmark->m.m_list);
		goto out;
	}

	BUG_ON(last == NULL);
	/* mark should be the last entry.  last is the current last entry */
	hlist_add_after_rcu(last, &mark->m.m_list);
out:
	fsnotify_recalc_vfsmount_mask_locked(mnt);
	release_mixed_lock(&mnt->mnt_root->d_lock, &xnode, xmode);

	return ret;
}
