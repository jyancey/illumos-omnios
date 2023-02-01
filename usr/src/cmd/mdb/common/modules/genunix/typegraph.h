/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2002 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef	_MDB_TYPEGRAPH_H
#define	_MDB_TYPEGRAPH_H

#ifdef	__cplusplus
extern "C" {
#endif

extern int typegraph(uintptr_t, uint_t, int, const mdb_arg_t *);
extern int istype(uintptr_t, uint_t, int, const mdb_arg_t *);
extern int notype(uintptr_t, uint_t, int, const mdb_arg_t *);
extern int whattype(uintptr_t, uint_t, int, const mdb_arg_t *);
extern int findfalse(uintptr_t, uint_t, int, const mdb_arg_t *);
extern int findlocks(uintptr_t, uint_t, int, const mdb_arg_t *);
extern int typegraph_walk_init(mdb_walk_state_t *);
extern int typeconflict_walk_step(mdb_walk_state_t *);
extern int typeunknown_walk_step(mdb_walk_state_t *);

#ifdef	__cplusplus
}
#endif

#endif	/* _MDB_TYPEGRAPH_H */
