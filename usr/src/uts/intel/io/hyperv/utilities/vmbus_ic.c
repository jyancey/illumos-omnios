/*
 * Copyright (c) 2014,2016 Microsoft Corp.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

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
 * Copyright (c) 2017 by Delphix. All rights reserved.
 */

/*
 * A common driver for all hyper-V util services.
 */

#include <sys/sunddi.h>
#include <sys/devops.h>
#include <sys/cmn_err.h>
#include <sys/param.h>
#include <sys/kmem.h>
#include <sys/reboot.h>
#include <sys/systm.h>
#include <sys/debug.h>

#include <sys/hyperv.h>
#include <sys/hyperv_illumos.h>
#include <sys/vmbus.h>
#include "vmbus_icreg.h"
#include "vmbus_icvar.h"

#define	VMBUS_IC_BRSIZE		(4 * PAGE_SIZE)

#define	VMBUS_IC_VERCNT		2
#define	VMBUS_IC_NEGOSZ		\
	offsetof(struct vmbus_icmsg_negotiate, ic_ver[VMBUS_IC_VERCNT])

int
vmbus_ic_negomsg(struct vmbus_ic_softc *sc, void *data, int *dlen0,
    uint32_t fw_ver, uint32_t msg_ver)
{
	struct vmbus_icmsg_negotiate *nego;
	int i, cnt, dlen = *dlen0, error;
	uint32_t sel_fw_ver = 0, sel_msg_ver = 0;
	boolean_t has_fw_ver, has_msg_ver;

	/*
	 * Preliminary message verification.
	 */
	if (dlen < sizeof (*nego)) {
		dev_err(sc->ic_dev, CE_WARN, "truncated ic negotiate, len %d",
		    dlen);
		return (EINVAL);
	}
	nego = data;

	if (nego->ic_fwver_cnt == 0) {
		dev_err(sc->ic_dev, CE_WARN, "ic negotiate does not contain "
		    "framework version %u", nego->ic_fwver_cnt);
		return (EINVAL);
	}
	if (nego->ic_msgver_cnt == 0) {
		dev_err(sc->ic_dev, CE_WARN, "ic negotiate does not contain "
		    "message version %u", nego->ic_msgver_cnt);
		return (EINVAL);
	}

	cnt = nego->ic_fwver_cnt + nego->ic_msgver_cnt;
	if (dlen < offsetof(struct vmbus_icmsg_negotiate, ic_ver[cnt])) {
		dev_err(sc->ic_dev, CE_WARN, "ic negotiate does not contain "
		    "versions %d", dlen);
		return (EINVAL);
	}

	error = EOPNOTSUPP;

	/*
	 * Find the best match framework version.
	 */
	has_fw_ver = B_FALSE;
	has_msg_ver = B_FALSE;
	for (i = 0; i < nego->ic_fwver_cnt; ++i) {
		if (VMBUS_ICVER_LE(nego->ic_ver[i], fw_ver)) {
			if (!has_fw_ver) {
				sel_fw_ver = nego->ic_ver[i];
				has_fw_ver = B_TRUE;
			} else if (VMBUS_ICVER_GT(nego->ic_ver[i],
			    sel_fw_ver)) {
				sel_fw_ver = nego->ic_ver[i];
			}
		}
	}
	if (!has_fw_ver) {
		dev_err(sc->ic_dev, CE_WARN, "failed to select framework "
		    "version");
		goto done;
	}

	/*
	 * Find the best match message version.
	 */
	for (i = nego->ic_fwver_cnt;
	    i < nego->ic_fwver_cnt + nego->ic_msgver_cnt; ++i) {
		if (VMBUS_ICVER_LE(nego->ic_ver[i], msg_ver)) {
			if (!has_msg_ver) {
				sel_msg_ver = nego->ic_ver[i];
				has_msg_ver = B_TRUE;
			} else if (VMBUS_ICVER_GT(nego->ic_ver[i],
			    sel_msg_ver)) {
				sel_msg_ver = nego->ic_ver[i];
			}
		}
	}
	if (!has_msg_ver) {
		dev_err(sc->ic_dev, CE_WARN, "failed to select message "
		    "version");
		goto done;
	}

	error = 0;
done:
	if (!has_fw_ver || !has_msg_ver) {
		if (has_fw_ver) {
			dev_err(sc->ic_dev, CE_NOTE, "sel framework version: "
			    "%u.%u",
			    VMBUS_ICVER_MAJOR(sel_fw_ver),
			    VMBUS_ICVER_MINOR(sel_fw_ver));
		}
		for (i = 0; i < nego->ic_fwver_cnt; i++) {
			dev_err(sc->ic_dev, CE_NOTE, "supp framework version: "
			    "%u.%u",
			    VMBUS_ICVER_MAJOR(nego->ic_ver[i]),
			    VMBUS_ICVER_MINOR(nego->ic_ver[i]));
		}

		if (has_msg_ver) {
			dev_err(sc->ic_dev, CE_NOTE, "sel message version: "
			    "%u.%u",
			    VMBUS_ICVER_MAJOR(sel_msg_ver),
			    VMBUS_ICVER_MINOR(sel_msg_ver));
		}
		for (i = nego->ic_fwver_cnt;
		    i < nego->ic_fwver_cnt + nego->ic_msgver_cnt; i++) {
			dev_err(sc->ic_dev, CE_NOTE, "supp message version: "
			    "%u.%u",
			    VMBUS_ICVER_MAJOR(nego->ic_ver[i]),
			    VMBUS_ICVER_MINOR(nego->ic_ver[i]));
		}
	}
	if (error)
		return (error);

	/* Record the selected versions. */
	sc->ic_fwver = sel_fw_ver;
	sc->ic_msgver = sel_msg_ver;

	/* One framework version. */
	nego->ic_fwver_cnt = 1;
	nego->ic_ver[0] = sel_fw_ver;

	/* One message version. */
	nego->ic_msgver_cnt = 1;
	nego->ic_ver[1] = sel_msg_ver;

	/* Update data size. */
	nego->ic_hdr.ic_dsize = VMBUS_IC_NEGOSZ -
	    sizeof (struct vmbus_icmsg_hdr);

	/* Update total size, if necessary. */
	if (dlen < VMBUS_IC_NEGOSZ)
		*dlen0 = VMBUS_IC_NEGOSZ;

	return (0);
}

/*
 * Generic attach/detach functions for utility drivers.
 *
 * statep is the soft state pointer returned by ddi_get_soft_state()
 * for the device instance.
 */
int
vmbus_ic_attach(dev_info_t *dev, vmbus_chan_callback_t cb,
    struct vmbus_ic_softc *sc)
{
	struct vmbus_channel *chan = vmbus_get_channel(dev);
	int error = 0;

	VERIFY3U(VMBUS_IC_NEGOSZ, <, VMBUS_IC_BRSIZE);

	sc->ic_dev = dev;
	sc->ic_buflen = VMBUS_IC_BRSIZE;
	sc->ic_buf = kmem_zalloc(VMBUS_IC_BRSIZE, KM_SLEEP);

	/*
	 * These services are not performance critical and do not need
	 * batched reading. Furthermore, some services such as KVP can
	 * only handle one message from the host at a time.
	 * Turn off batched reading for all util drivers before we open the
	 * channel.
	 */
	vmbus_chan_set_readbatch(chan, B_FALSE);

	error = vmbus_chan_open(chan, VMBUS_IC_BRSIZE, VMBUS_IC_BRSIZE, NULL, 0,
	    cb, sc);
	if (error) {
		kmem_free(sc->ic_buf, VMBUS_IC_BRSIZE);
		return (error);
	}
	return (0);
}

int
vmbus_ic_detach(dev_info_t *dev, struct vmbus_ic_softc *sc)
{
	char *state;
	if (ddi_prop_lookup_string(DDI_DEV_T_ANY, dev, DDI_PROP_DONTPASS,
	    VMBUS_STATE, &state) != DDI_SUCCESS) {
		dev_err(dev, CE_WARN, "cannot find property \"%s\"",
		    VMBUS_STATE);
		return (DDI_FAILURE);
	}

	/*
	 * We only detach devices if we've been asked to by vmbus_child_delete.
	 * So check to see if the state has been set to "offline".
	 */
	if (strcmp(state, VMBUS_STATE_OFFLINE) != 0) {
		ddi_prop_free(state);
		return (DDI_FAILURE);
	}
	ddi_prop_free(state);

	vmbus_chan_close(vmbus_get_channel(dev));
	kmem_free(sc->ic_buf, VMBUS_IC_BRSIZE);

	return (0);
}

int
vmbus_ic_sendresp(struct vmbus_ic_softc *sc, struct vmbus_channel *chan,
    void *data, int dlen, uint64_t xactid)
{
	struct vmbus_icmsg_hdr *hdr;
	int error;

	ASSERT3U(dlen, >=, sizeof (*hdr));
	hdr = data;

	hdr->ic_flags = VMBUS_ICMSG_FLAG_XACT | VMBUS_ICMSG_FLAG_RESP;
	error = vmbus_chan_send(chan, VMBUS_CHANPKT_TYPE_INBAND, 0,
	    data, dlen, xactid);
	if (error != 0)
		dev_err(sc->ic_dev, CE_WARN, "resp send failed: %d", error);
	return (error);
}
