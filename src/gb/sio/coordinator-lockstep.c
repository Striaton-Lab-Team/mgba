/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/coordinator-lockstep.h>

#include "mgba-util/platform/posix/threading.h"
#include "mgba/gba/interface.h"

#include <mgba/internal/gb/gb.h>

static bool GBSIOLockstepDriverInit(struct GBSIODriver* driver);
static void GBSIOLockstepDriverDeinit(struct GBSIODriver* driver);
static void GBSIOLockstepDriverWriteSB(struct GBSIODriver* driver, uint8_t value);
static uint8_t GBSIOLockstepDriverWriteSC(struct GBSIODriver* driver, uint8_t value);
static void GBSIOLockstepDriverLoadSB(struct GBSIODriver* driver);

static void _onSioAttach(struct SIOLockstepPlayer* player, void* driver);
static void _onSioDetach(struct SIOLockstepPlayer* player, void* driver);
static void _onSioModeSet(struct SIOLockstepPlayer* player, void* driver);
static void _onSioTransferStart(struct SIOLockstepPlayer* player, int32_t eventFinishCycles, void* driver);
static void _setReady(const bool ready, void* driver);

static void _lockstep(struct mTiming*, void* context, uint32_t);

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

void GBSIOLockstepDriverCreate(struct GBSIOLockstepDriver* driver, struct mLockstepUser* user) {
	memset(driver, 0, sizeof(*driver));

	driver->d.init = GBSIOLockstepDriverInit;
	driver->d.deinit = GBSIOLockstepDriverDeinit;
	driver->d.writeSB = GBSIOLockstepDriverWriteSB;
	driver->d.writeSC = GBSIOLockstepDriverWriteSC;
	driver->d.loadPendingSB = GBSIOLockstepDriverLoadSB;

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
	if (!lockstep->lockstepId) {
		unsigned id;
		player = calloc(1, sizeof(*player));
		player->mode = GBA_SIO_NORMAL_8;
		player->playerId = -1;
		player->user = lockstep->user;
		const struct LockstepEventCallbacks callbacks ={
			.on_sio_attach = _onSioAttach,
			.on_sio_detach = _onSioDetach,
			.on_sio_transfer_start = _onSioTransferStart,
			.on_sio_ready = _setReady,
			.on_sio_mode_set = _onSioModeSet,
		};
		player->driver = lockstep;
		player->lockstepEvents = callbacks;

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
		coordinator_reconfigPlayers(coordinator);
		player->cycleOffset = mTimingCurrentTime(&driver->p->p->timing) - coordinator->cycle;

		struct SIOLockstepEvent event = {
			.type = SIO_EV_GB_ATTACH,
			.playerId = player->playerId,
			.timestamp = coordinator_lockstepPlayerTime(player),
		};
		coordinator_enqueueEvent(coordinator, &event, TARGET_ALL & ~TARGET(player->playerId));
	}
	 else {
		MutexLock(&coordinator->mutex);
		player = TableLookup(&coordinator->players, lockstep->lockstepId);
		player->cycleOffset = mTimingCurrentTime(&driver->p->p->timing) - coordinator->cycle;
	}

	int32_t nextEvent;
	coordinator_setReady(coordinator, player, player->playerId, player->mode);
	if (TableSize(&coordinator->players) == 1) {
		coordinator->cycle = mTimingCurrentTime(&lockstep->d.p->p->timing);
		nextEvent = LOCKSTEP_INTERVAL;
	} else {
		coordinator_setReady(coordinator, player, 0, coordinator->transferMode);
		nextEvent = coordinator_untilNextSync(lockstep->coordinator, player);
	}

	MutexUnlock(&coordinator->mutex);
	mTimingSchedule(&lockstep->d.p->p->timing, &lockstep->event, nextEvent);
	return true;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void GBSIOLockstepDriverDeinit(struct GBSIODriver* driver) {
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	MutexLock(&coordinator->mutex);
	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	coordinator_removePlayer(coordinator, player);
	MutexUnlock(&coordinator->mutex);
	mTimingDeschedule(&lockstep->d.p->p->timing, &lockstep->event);
	lockstep->lockstepId = 0;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void GBSIOLockstepDriverWriteSB(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;

	MutexLock(&coordinator->mutex);
	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);

	coordinator->normalData[player->playerId] = value;
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

	MutexLock(&coordinator->mutex);
	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if ((value & 0x81) == 0x81 && coordinator->nAttached > 1) {
		int32_t timestamp = coordinator_lockstepPlayerTime(player);
		struct SIOLockstepEvent event = {
			.type = SIO_EV_TRANSFER_START,
			.timestamp = timestamp,
			.finishCycle = timestamp + GBSIOCyclesPerTransfer[(value >> 1) & 1]
		};
		coordinator_enqueueEvent(coordinator, &event, TARGET_SECONDARY);
		coordinator_waitOnPlayers(coordinator, player);
		coordinator->transferActive = true;
	}
	MutexUnlock(&coordinator->mutex);

	return value;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//
// Coordinator Callbacks
//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void _onSioAttach(struct SIOLockstepPlayer* player, void* driver) {
	UNUSED(player);
	UNUSED(driver);
}

static void _onSioDetach(struct SIOLockstepPlayer* player, void* driver) {
	UNUSED(player);
	UNUSED(driver);
}

static void _onSioModeSet(struct SIOLockstepPlayer* player, void* driver) {
	UNUSED(player);
	UNUSED(driver);
}

static void _onSioTransferStart(struct SIOLockstepPlayer* player, int32_t eventFinishCycles, void* driver) {
	UNUSED(eventFinishCycles);
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	struct GBSIO* sio = lockstep->d.p;
	lockstep->coordinator->normalData[player->playerId] = sio->pendingSB;
	mTimingDeschedule(&sio->p->timing, &sio->event);
	mTimingSchedule(&sio->p->timing, &sio->event, sio->period * (2 - sio->p->doubleSpeed));
}

static void _setReady(const bool ready, void* driver) {
	UNUSED(ready);
	UNUSED(driver);
}

static void _lockstep(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBSIOLockstepDriver* lockstep = context;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);

	int32_t nextEvent = coordinator_lockstepEvent(coordinator, player, timing, cyclesLate);
	mTimingSchedule(timing, &lockstep->event, nextEvent);
}

