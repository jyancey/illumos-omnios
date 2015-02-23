/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2015 Joyent, Inc.
 */

/*
 * varpd plugin management
 */

#include <libvarpd_impl.h>
#include <errno.h>
#include <umem.h>
#include <assert.h>
#include <strings.h>
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <bunyan.h>

static varpd_impl_t *varpd_load_handle;
static const char *varpd_load_path;
static mutex_t varpd_load_lock;
static cond_t varpd_load_cv;

int
libvarpd_plugin_comparator(const void *lp, const void *rp)
{
	int ret;
	const varpd_plugin_t *lpp, *rpp;

	lpp = lp;
	rpp = rp;

	ret = strcmp(lpp->vpp_name, rpp->vpp_name);
	if (ret > 0)
		return (1);
	if (ret < 0)
		return (-1);
	return (0);
}

varpd_plugin_register_t *
libvarpd_plugin_alloc(uint_t version, int *errp)
{
	int err;
	varpd_plugin_register_t *vprp;

	if (errp == NULL)
		errp = &err;

	if (version != VARPD_VERSION_ONE) {
		(void) bunyan_warn(varpd_load_handle->vdi_bunyan,
		    "unsupported registration version",
		    BUNYAN_T_STRING, "module_path", varpd_load_path,
		    BUNYAN_T_INT32, "module_version", version,
		    BUNYAN_T_END);
		*errp = EINVAL;
		return (NULL);
	}

	vprp = umem_alloc(sizeof (varpd_plugin_register_t), UMEM_DEFAULT);
	if (vprp == NULL) {
		(void) bunyan_warn(varpd_load_handle->vdi_bunyan,
		    "failed to allocate registration handle",
		    BUNYAN_T_STRING, "module_path", varpd_load_path,
		    BUNYAN_T_END);
		*errp = ENOMEM;
		return (NULL);
	}

	vprp->vpr_version = VARPD_VERSION_ONE;

	return (vprp);
}

void
libvarpd_plugin_free(varpd_plugin_register_t *vprp)
{
	umem_free(vprp, sizeof (varpd_plugin_register_t));
}

int
libvarpd_plugin_register(varpd_plugin_register_t *vprp)
{
	varpd_plugin_t *vpp;
	varpd_plugin_t lookup;

	vpp = umem_alloc(sizeof (varpd_plugin_t), UMEM_DEFAULT);
	if (vpp == NULL) {
		(void) bunyan_warn(varpd_load_handle->vdi_bunyan,
		    "failed to allocate memory for the varpd_plugin_t",
		    BUNYAN_T_STRING, "module_path", varpd_load_path,
		    BUNYAN_T_END);
		return (ENOMEM);
	}

	/* Watch out for an evil plugin */
	if (vprp->vpr_version != VARPD_VERSION_ONE) {
		(void) bunyan_warn(varpd_load_handle->vdi_bunyan,
		    "unsupported registration version",
		    BUNYAN_T_STRING, "module_path", varpd_load_path,
		    BUNYAN_T_INT32, "module_version", vprp->vpr_version,
		    BUNYAN_T_END);
		return (EINVAL);
	}

	(void) mutex_lock(&varpd_load_lock);
	if (varpd_load_handle == NULL)
		libvarpd_panic("varpd_load_handle was unexpectedly null");

	(void) mutex_lock(&varpd_load_handle->vdi_lock);
	lookup.vpp_name = vprp->vpr_name;
	if (avl_find(&varpd_load_handle->vdi_plugins, &lookup, NULL) != NULL) {
		(void) bunyan_warn(varpd_load_handle->vdi_bunyan,
		    "module already exists with requested name",
		    BUNYAN_T_STRING, "module_path", varpd_load_path,
		    BUNYAN_T_STRING, "name", vprp->vpr_name,
		    BUNYAN_T_END);
		(void) mutex_unlock(&varpd_load_handle->vdi_lock);
		(void) mutex_unlock(&varpd_load_lock);
		umem_free(vpp, sizeof (varpd_plugin_t));
		return (EEXIST);
	}
	vpp->vpp_name = strdup(vprp->vpr_name);
	if (vpp->vpp_name == NULL) {
		(void) bunyan_warn(varpd_load_handle->vdi_bunyan,
		    "failed to allocate memory to duplicate name",
		    BUNYAN_T_STRING, "module_path", varpd_load_path,
		    BUNYAN_T_END);
		(void) mutex_unlock(&varpd_load_handle->vdi_lock);
		(void) mutex_unlock(&varpd_load_lock);
		umem_free(vpp, sizeof (varpd_plugin_t));
		return (ENOMEM);
	}

	vpp->vpp_mode = vprp->vpr_mode;
	vpp->vpp_ops = vprp->vpr_ops;
	if (mutex_init(&vpp->vpp_lock, USYNC_THREAD, NULL) != 0)
		libvarpd_panic("failed to create plugin's vpp_lock");
	vpp->vpp_active = 0;
	avl_add(&varpd_load_handle->vdi_plugins, vpp);
	(void) mutex_unlock(&varpd_load_handle->vdi_lock);
	(void) mutex_unlock(&varpd_load_lock);

	return (0);
}

varpd_plugin_t *
libvarpd_plugin_lookup(varpd_impl_t *vip, const char *name)
{
	varpd_plugin_t lookup, *ret;

	lookup.vpp_name = name;
	(void) mutex_lock(&vip->vdi_lock);
	ret = avl_find(&vip->vdi_plugins, &lookup, NULL);
	(void) mutex_unlock(&vip->vdi_lock);

	return (ret);
}

/* ARGSUSED */
static int
libvarpd_plugin_load_cb(varpd_impl_t *vip, const char *path, void *unused)
{
	void *dlp;

	varpd_load_path = path;
	dlp = dlopen(path, RTLD_LOCAL | RTLD_NOW);
	if (dlp == NULL) {
		(void) bunyan_error(vip->vdi_bunyan, "dlopen failed",
		    BUNYAN_T_STRING, "module path", path,
		    BUNYAN_T_END);
	}
	path = NULL;

	return (0);
}

int
libvarpd_plugin_load(varpd_handle_t *vph, const char *path)
{
	int ret = 0;
	varpd_impl_t *vip = (varpd_impl_t *)vph;

	if (vip == NULL || path == NULL)
		return (EINVAL);
	(void) mutex_lock(&varpd_load_lock);
	while (varpd_load_handle != NULL)
		(void) cond_wait(&varpd_load_cv, &varpd_load_lock);
	varpd_load_handle = vip;
	(void) mutex_unlock(&varpd_load_lock);

	ret = libvarpd_dirwalk(vip, path, ".so", libvarpd_plugin_load_cb, NULL);

	(void) mutex_lock(&varpd_load_lock);
	varpd_load_handle = NULL;
	(void) cond_signal(&varpd_load_cv);
	(void) mutex_unlock(&varpd_load_lock);

	return (ret);
}

int
libvarpd_plugin_walk(varpd_handle_t *vph, libvarpd_plugin_walk_f func,
    void *arg)
{
	varpd_impl_t *vip = (varpd_impl_t *)vph;
	varpd_plugin_t *vpp;

	(void) mutex_lock(&vip->vdi_lock);
	for (vpp = avl_first(&vip->vdi_plugins); vpp != NULL;
	    vpp = AVL_NEXT(&vip->vdi_plugins, vpp)) {
		if (func(vph, vpp->vpp_name, arg) != 0) {
			(void) mutex_unlock(&vip->vdi_lock);
			return (1);
		}
	}
	(void) mutex_unlock(&vip->vdi_lock);
	return (0);
}

void
libvarpd_plugin_init(void)
{
	if (mutex_init(&varpd_load_lock, USYNC_THREAD | LOCK_RECURSIVE |
	    LOCK_ERRORCHECK, NULL) != 0)
		libvarpd_panic("failed to create varpd_load_lock");

	if (cond_init(&varpd_load_cv, USYNC_THREAD, NULL) != 0)
		libvarpd_panic("failed to create varpd_load_cv");

	varpd_load_handle = NULL;
}

void
libvarpd_plugin_fini(void)
{
	assert(varpd_load_handle == NULL);
	if (mutex_destroy(&varpd_load_lock) != 0)
		libvarpd_panic("failed to destroy varpd_load_lock");
	if (cond_destroy(&varpd_load_cv) != 0)
		libvarpd_panic("failed to destroy varpd_load_cv");
}

void
libvarpd_plugin_prefork(void)
{
	(void) mutex_lock(&varpd_load_lock);
	while (varpd_load_handle != NULL)
		(void) cond_wait(&varpd_load_cv, &varpd_load_lock);
}

void
libvarpd_plugin_postfork(void)
{
	(void) cond_signal(&varpd_load_cv);
	(void) mutex_unlock(&varpd_load_lock);
}
