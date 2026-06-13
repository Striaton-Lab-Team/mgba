/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/coordinator-lockstep.h>

#include "mgba-util/platform/posix/threading.h"
#include "mgba/gba/interface.h"
#include "mgba/internal/sm83/sm83.h"

#include <mgba/internal/gb/gb.h>

static bool GBSIOLockstepDriverInit(struct GBSIODriver* driver);
static void GBSIOLockstepDriverDeinit(struct GBSIODriver* driver);
static void GBSIOLockstepDriverWriteSB(struct GBSIODriver* driver, uint8_t value);
static uint8_t GBSIOLockstepDriverWriteSC(struct GBSIODriver* driver, uint8_t value);
static void GBSIOLockstepDriverLoadSB(struct GBSIODriver* driver);
static int GBSIOLockstepDriverDeviceId(struct GBSIODriver* driver);

static void _onSioAttach(struct SIOLockstepPlayer* player);
static void _onSioDetach(struct SIOLockstepPlayer* player);
static void _onSioModeSet(struct SIOLockstepPlayer* player);
static void _onSioTransferStart(struct SIOLockstepPlayer* player, int32_t eventFinishCycles);
static void _setReady(struct SIOLockstepPlayer* player, bool ready);
static int32_t _coreTiming(struct SIOLockstepPlayer* player);
static void _onPlayerSleep(struct SIOLockstepPlayer* player);

static void _lockstep(struct mTiming*, void* context, uint32_t);

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

void GBSIOLockstepDriverCreate(struct GBSIOLockstepDriver* driver, struct mLockstepUser* user) {
	memset(driver, 0, sizeof(*driver));

	driver->d.init = GBSIOLockstepDriverInit;
	driver->d.deinit = GBSIOLockstepDriverDeinit;
	driver->d.writeSB = GBSIOLockstepDriverWriteSB;
	driver->d.writeSC = GBSIOLockstepDriverWriteSC;
	driver->d.loadPendingSB = GBSIOLockstepDriverLoadSB;
	driver->d.deviceId = GBSIOLockstepDriverDeviceId;

	driver->event.context = driver;
	driver->event.callback = _lockstep;
	driver->event.name = "GB SIO Lockstep";
	driver->event.priority = 0x80;

	driver->user = user;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

bool GBSIOLockstepDriverInit(struct GBSIODriver* driver) {
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct SIOLockstepPlayer* player;
	coordinator->normalData[0] = 0xFFFFFFFF;
	coordinator->normalData[1] = 0xFFFFFFFF;
	coordinator->normalData[2] = 0xFFFFFFFF;
	coordinator->normalData[3] = 0xFFFFFFFF;
	if (!lockstep->lockstepId) {
		unsigned id;
		player = calloc(1, sizeof(*player));
		player->mode = GBA_SIO_NORMAL_8;
		player->playerId = -1;
		player->user = lockstep->user;
		const struct LockstepCoordinatorDriver coordDriver ={
			.onAttach = _onSioAttach,
			.onDetach = _onSioDetach,
			.onSetMode = _onSioModeSet,
			.onTransferStart = _onSioTransferStart,
			.onReady = _setReady,
			.onPlayerSleep = _onPlayerSleep,
			.getCoreTiming = _coreTiming
		};
		player->driver = lockstep;
		player->coordinatorDriver = coordDriver;

		int i;
		for (i = 0; i < 8 - 1; ++i) {
			player->buffer[i].next = &player->buffer[i + 1];
		}
		player->freeList = &player->buffer[0];

		MutexLock(&coordinator->mutex);
		while (true) {
			if (coordinator->nextId == UINT_MAX) {
				coordinator->nextId = 0;
			}
			++coordinator->nextId;
			id = coordinator->nextId;
			if (!TableLookup(&coordinator->players, id)) {
				TableInsert(&coordinator->players, id, player);
				lockstep->lockstepId = id;
				player->lockstepId = id;
				break;
			}
		}
		CoordinatorReconfigPlayers(coordinator);
		player->cycleOffset = _coreTiming(player) - coordinator->cycle;

		struct SIOLockstepEvent event = {
			.type = SIO_EV_GB_ATTACH,
			.playerId = player->playerId,
			.timestamp = CoordinatorLockstepPlayerTime(player),
		};
		CoordinatorEnqueueEvent(coordinator, &event, TARGET_ALL & ~TARGET(player->playerId));
	}
	 else {
		MutexLock(&coordinator->mutex);
		player = TableLookup(&coordinator->players, lockstep->lockstepId);
		player->cycleOffset = _coreTiming(player) - coordinator->cycle;
	}

	int32_t nextEvent;
	CoordinatorSetReady(coordinator, player, player->playerId, player->mode);
	if (TableSize(&coordinator->players) == 1) {
		coordinator->cycle = _coreTiming(player);
		nextEvent = LOCKSTEP_INTERVAL;
	} else {
		CoordinatorSetReady(coordinator, player, 0, coordinator->transferMode);
		nextEvent = CoordinatorUntilNextSync(lockstep->coordinator, player);
	}

	MutexUnlock(&coordinator->mutex);
	struct mTiming timing = lockstep->d.p->p->timing;
	//timing.masterCycles /= 4;
	mTimingSchedule(&timing, &lockstep->event, nextEvent  / 4);
	return true;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static int GBSIOLockstepDriverDeviceId(struct GBSIODriver* driver) {
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	int playerId = 0;
	MutexLock(&coordinator->mutex);
	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (player && player->playerId >= 0) {
		playerId = player->playerId;
	}
	MutexUnlock(&coordinator->mutex);
	return playerId;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void GBSIOLockstepDriverDeinit(struct GBSIODriver* driver) {
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	MutexLock(&coordinator->mutex);
	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (player) {
		CoordinatorRemovePlayer(coordinator, player);
	}
	MutexUnlock(&coordinator->mutex);
	mTimingDeschedule(&lockstep->d.p->p->timing, &lockstep->event);
	lockstep->lockstepId = 0;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void GBSIOLockstepDriverWriteSB(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct GBSIO* sio = lockstep->d.p;

	MutexLock(&coordinator->mutex);
	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);

	coordinator->normalData[player->playerId] = value;
	sio->pendingSB = value;
	printf("PlayerId %d, Write value to SB: %d \n", player->playerId, value);
	MutexUnlock(&coordinator->mutex);
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void GBSIOLockstepDriverLoadSB(struct GBSIODriver* driver) {
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;

	MutexLock(&coordinator->mutex);
	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);

	int partnerPlayerId = player->playerId == 0 ? 1 : 0; // GB has only tooie

	struct GBSIO* sio = driver->p;
	sio->pendingSB = coordinator->normalData[partnerPlayerId];

	MutexUnlock(&coordinator->mutex);
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static uint8_t GBSIOLockstepDriverWriteSC(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct GBSIO* sio = lockstep->d.p;
	MutexLock(&coordinator->mutex);

	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	printf("PlayerId %d, Write value to SC: %d \n", player->playerId, value);
	if ((value & 0x81) == 0x81 && coordinator->nAttached > 1 && player->playerId == 0) {
		int32_t timestamp = CoordinatorLockstepPlayerTime(player);
		struct SIOLockstepEvent event = {
			.type = SIO_EV_TRANSFER_START,
			.timestamp = timestamp,
			.finishCycle = timestamp + GBSIOCyclesPerTransfer[(value >> 1) & 1]
		};
		lockstep->coordinator->normalData[player->playerId] = sio->pendingSB;
		CoordinatorEnqueueEvent(coordinator, &event, TARGET_SECONDARY);
		printf("PlayerId %d, Master transmit event started \n", player->playerId);
		CoordinatorWaitOnPlayers(coordinator, player);
		coordinator->transferActive = true;
	}
	else {
		printf("PlayerId %d, Could not start transfer \n", player->playerId);
	}
	MutexUnlock(&coordinator->mutex);

	return value;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//
// Coordinator Callbacks
//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void _onSioAttach(struct SIOLockstepPlayer* player) {
	UNUSED(player);
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void _onSioDetach(struct SIOLockstepPlayer* player) {
	UNUSED(player);
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void _onSioModeSet(struct SIOLockstepPlayer* player) {
	UNUSED(player);
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void _onSioTransferStart(struct SIOLockstepPlayer* player, int32_t eventFinishCycles) {
	UNUSED(eventFinishCycles);
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) player->driver;
	struct GBSIO* sio = lockstep->d.p;
	if (!sio->awaitExternalClock) {
		lockstep->coordinator->normalData[player->playerId] = 0xFF;
		printf("PlayerId %d, Slave is not ready \n", player->playerId);
		return;
	}
	lockstep->coordinator->normalData[player->playerId] = sio->pendingSB;
	sio->remainingBits = 8;
	mTimingDeschedule(&sio->p->timing, &sio->event);
	mTimingSchedule(&sio->p->timing, &sio->event, sio->period * (2 - sio->p->doubleSpeed));
	printf("PlayerId %d, Slave transmit event started \n", player->playerId);
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void _setReady(struct SIOLockstepPlayer* player, const bool ready) {
	UNUSED(player);
	UNUSED(ready);
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void _lockstep(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBSIOLockstepDriver* lockstep = context;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);

	int32_t nextEvent = CoordinatorLockstepEvent(coordinator, player, timing, cyclesLate);
	if (nextEvent < 4) nextEvent = 16;
	mTimingSchedule(timing, &lockstep->event, (nextEvent  / 4));
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static int32_t _coreTiming(struct SIOLockstepPlayer* player) {
	struct GBSIODriver* lockstep = (struct GBSIODriver*) player->driver;
	int32_t cycles = mTimingCurrentTime(&lockstep->p->p->timing);
	return cycles;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void _onPlayerSleep(struct SIOLockstepPlayer* player) {
	struct GBSIODriver* lockstep = (struct GBSIODriver*) player->driver;
	lockstep->p->p->cpu->nextEvent = 0;
	GBInterrupt(lockstep->p->p);
}

