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

#ifndef _BARRIER_H
#define	_BARRIER_H

#include <pthread.h>

#if defined(__linux__)
typedef pthread_barrier_t barrier_t;
#else
#include <dispatch/dispatch.h>

typedef struct barrier {
	pthread_mutex_t bar_lock;	/* protects bar_numin */
	int bar_numin;			/* current number of waiters */

	dispatch_semaphore_t bar_sem;	/* where everyone waits */
	int bar_nthr;			/* # of waiters to trigger release */
} barrier_t;
#endif

extern void barrier_init(barrier_t *, int);
extern int barrier_wait(barrier_t *);

#endif /* _BARRIER_H */
