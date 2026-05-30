/* Copyright (c) 2013-2016 Jeffrey Pfau
*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_LOCKSTEP_H
#define GB_SIO_LOCKSTEP_H

#include <mgba-util/common.h>

CXX_GUARD_START
#include <mgba/core/coordinator.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gb/sio.h>

struct GBSIOLockstepDriver {
	struct GBSIODriver d;
	struct SIOLockstepCoordinator* coordinator;
	struct mTimingEvent event;
	unsigned lockstepId;

	struct mLockstepUser* user;
};

void GBSIOLockstepDriverCreate(struct GBSIOLockstepDriver*, struct mLockstepUser*);


CXX_GUARD_END

#endif
