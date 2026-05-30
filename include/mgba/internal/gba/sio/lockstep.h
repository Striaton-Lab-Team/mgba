/* Copyright (c) 2013-2024 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LOCKSTEP_H
#define GBA_SIO_LOCKSTEP_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/lockstep.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gba/sio.h>

struct GBASIOLockstepDriver {
	struct GBASIODriver d;
	struct SIOLockstepCoordinator* coordinator;
	struct mTimingEvent event;
	unsigned lockstepId;

	struct mLockstepUser* user;
};

void GBASIOLockstepDriverCreate(struct GBASIOLockstepDriver*, struct mLockstepUser*);

CXX_GUARD_END

#endif
