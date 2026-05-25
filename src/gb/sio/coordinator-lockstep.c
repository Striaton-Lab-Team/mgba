/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/coordinator-lockstep.h>

#include "mgba-util/platform/posix/threading.h"
#include "mgba/core/lockstep.h"
#include "mgba/gba/interface.h"

#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/io.h>

#define LOCKSTEP_INCREMENT 512

static bool GBSIOLockstepDriverInit(struct GBSIODriver* driver);
static void GBSIOLockstepDriverDeinit(struct GBSIODriver* driver);
static void GBSIOLockstepDriverWriteSB(struct GBSIODriver* driver, uint8_t value);
static uint8_t GBSIOLockstepDriverWriteSC(struct GBSIODriver* driver, uint8_t value);
static void _GBSIOLockstepNodeProcessEvents(struct mTiming* timing, void* driver, uint32_t cyclesLate);

static void _onSioAttach(struct SIOLockstepPlayer* player, void* driver);
static void _onSioDetach(struct SIOLockstepPlayer* player, void* driver);
static void _onSioModeSet(struct SIOLockstepPlayer* player, void* driver);
static void _onSioTransferStart(struct SIOLockstepPlayer* player, int32_t eventFinishCycles, void* driver);
static void _setReady(const bool ready, void* driver);

static void _lockstep(struct mTiming*, void* context, uint32_t);

void GBSIOLockstepDriverCreate(struct GBSIOLockstepDriver* driver, struct mLockstepUser* user) {
	memset(driver, 0, sizeof(*driver));

	driver->d.init = GBSIOLockstepDriverInit;
	driver->d.deinit = GBSIOLockstepDriverDeinit;
	driver->d.writeSB = GBSIOLockstepDriverWriteSB;
	driver->d.writeSC = GBSIOLockstepDriverWriteSC;

	driver->event.context = driver;
	driver->event.callback = _lockstep;
	driver->event.name = "GB SIO Lockstep";
	driver->event.priority = 0x80;

	driver->user = user;
}

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
		for (i = 0; i < MAX_LOCKSTEP_EVENTS - 1; ++i) {
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

static void _finishTransfer(struct GBSIODriver* driver) {
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	struct GBSIO* sio = lockstep->d.p;
	if (node->transferFinished) {
		return;
	}
	sio->pendingSB = node->p->pendingSB[!node->id];
	if (GBRegisterSCIsEnable(sio->p->memory.io[GB_REG_SC])) {
		sio->remainingBits = 8;
		mTimingDeschedule(&sio->p->timing, &sio->event);
		mTimingSchedule(&sio->p->timing, &sio->event, 0);
	}
	node->transferFinished = true;
}


static int32_t _masterUpdate(struct GBSIOLockstepNode* node) {
	enum mLockstepPhase transferActive;
	ATOMIC_LOAD(transferActive, node->p->d.transferActive);

	bool needsToWait = false;
	int i;
	switch (transferActive) {
	case TRANSFER_IDLE:
		// If the master hasn't initiated a transfer, it can keep going.
		node->nextEvent += LOCKSTEP_INCREMENT;
		break;
	case TRANSFER_STARTING:
		// Start the transfer, but wait for the other GBs to catch up
		node->transferFinished = false;
		needsToWait = true;
		ATOMIC_STORE(node->p->d.transferActive, TRANSFER_STARTED);
		node->nextEvent += 4;
		break;
	case TRANSFER_STARTED:
		// All the other GBs have caught up and are sleeping, we can all continue now
		node->nextEvent += 4;
		ATOMIC_STORE(node->p->d.transferActive, TRANSFER_FINISHING);
		break;
	case TRANSFER_FINISHING:
		// Finish the transfer
		// We need to make sure the other GBs catch up so they don't get behind
		node->nextEvent += node->d.p->period * (2 - node->d.p->p->doubleSpeed) - 8; // Split the cycles to avoid waiting too long
#ifndef NDEBUG
		ATOMIC_ADD(node->p->d.transferId, 1);
#endif
		needsToWait = true;
		ATOMIC_STORE(node->p->d.transferActive, TRANSFER_FINISHED);
		break;
	case TRANSFER_FINISHED:
		// Everything's settled. We're done.
		_finishTransfer(node);
		ATOMIC_STORE(node->p->masterClaimed, false);
		node->nextEvent += LOCKSTEP_INCREMENT;
		ATOMIC_STORE(node->p->d.transferActive, TRANSFER_IDLE);
		break;
	}
	int mask = 0;
	for (i = 1; i < node->p->d.attached; ++i) {
		mask |= 1 << i;
	}
	if (mask) {
		if (needsToWait) {
			if (!node->p->d.wait(&node->p->d, mask)) {
				abort();
			}
		} else {
			node->p->d.signal(&node->p->d, mask);
		}
	}
	// Tell the other GBs they can continue up to where we were
	node->p->d.addCycles(&node->p->d, 0, node->eventDiff);
#ifndef NDEBUG
	node->phase = node->p->d.transferActive;
#endif
	if (needsToWait) {
		return 0;
	}
	return node->nextEvent;
}

static uint32_t _slaveUpdate(struct GBSIOLockstepNode* node) {
	enum mLockstepPhase transferActive;
	int id;

	ATOMIC_LOAD(transferActive, node->p->d.transferActive);
	ATOMIC_LOAD(id, node->id);

	bool signal = false;
	switch (transferActive) {
	case TRANSFER_IDLE:
		node->p->d.addCycles(&node->p->d, id, LOCKSTEP_INCREMENT);
		break;
	case TRANSFER_STARTING:
	case TRANSFER_FINISHING:
		break;
	case TRANSFER_STARTED:
		if (node->p->d.unusedCycles(&node->p->d, id) > node->eventDiff) {
			break;
		}
		node->transferFinished = false;
		signal = true;
		break;
	case TRANSFER_FINISHED:
		if (node->p->d.unusedCycles(&node->p->d, id) > node->eventDiff) {
			break;
		}
		_finishTransfer(node);
		signal = true;
		break;
	}
#ifndef NDEBUG
	node->phase = node->p->d.transferActive;
#endif
	if (signal) {
		node->p->d.signal(&node->p->d, 1 << id);
	}
	return 0;
}

static void _GBSIOLockstepNodeProcessEvents(struct mTiming* timing, void* user, uint32_t cyclesLate) {
	struct GBSIOLockstepNode* node = user;
	mLockstepLock(&node->p->d);
	if (node->p->d.attached < 2) {
		mTimingSchedule(timing, &node->event, (GBSIOCyclesPerTransfer[0] >> 1) * (2 - node->d.p->p->doubleSpeed) - cyclesLate);
		mLockstepUnlock(&node->p->d);
		return;
	}
	int32_t cycles = 0;
	node->nextEvent -= cyclesLate;
	if (node->nextEvent <= 0) {
		int id;
		ATOMIC_LOAD(id, node->id);
		if (!id) {
			cycles = _masterUpdate(node);
		} else {
			cycles = _slaveUpdate(node);
			cycles += node->p->d.useCycles(&node->p->d, id, node->eventDiff);
		}
		node->eventDiff = 0;
	} else {
		cycles = node->nextEvent;
	}
	mLockstepUnlock(&node->p->d);

	if (cycles > 0) {
		node->nextEvent = 0;
		node->eventDiff += cycles;
		mTimingDeschedule(timing, &node->event);
		mTimingSchedule(timing, &node->event, cycles);
	} else {
		GBInterrupt(node->d.p->p);
		mTimingSchedule(timing, &node->event, cyclesLate + 1);
	}
}

static void GBSIOLockstepDriverWriteSB(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOLockstepNode* node = (struct GBSIOLockstepNode*) driver;
	int id;
	ATOMIC_LOAD(id, node->id);
	node->p->pendingSB[id] = value;
}

static uint8_t GBSIOLockstepDriverWriteSC(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOLockstepNode* node = (struct GBSIOLockstepNode*) driver;
	int attached;
	ATOMIC_LOAD(attached, node->p->d.attached);

	if ((value & 0x81) == 0x81 && attached > 1) {
		mLockstepLock(&node->p->d);
		bool claimed = false;
		if (ATOMIC_CMPXCHG(node->p->masterClaimed, claimed, true)) {
			int id;
			ATOMIC_LOAD(id, node->id);
			if (id != 0) {
				unsigned sb;
				node->p->players[0]->id = 1;
				node->id = 0;
				node->p->players[1] = node->p->players[0];
				node->p->players[0] = node;
				sb = node->p->pendingSB[0];
				node->p->pendingSB[0] = node->p->pendingSB[1];
				node->p->pendingSB[1] = sb;
			}
			ATOMIC_STORE(node->p->d.transferActive, TRANSFER_STARTING);
			ATOMIC_STORE(node->p->d.transferCycles, GBSIOCyclesPerTransfer[(value >> 1) & 1]);
			mTimingDeschedule(&driver->p->p->timing, &driver->p->event);
			mTimingDeschedule(&driver->p->p->timing, &node->event);
			mTimingSchedule(&driver->p->p->timing, &node->event, 0);
		} else {
			mLOG(GB_SIO, DEBUG, "GBSIOLockstepNodeWriteSC() failed to write to masterClaimed\n");
		}
		mLockstepUnlock(&node->p->d);
	}
	return value;
}

static void _onSioAttach(struct SIOLockstepPlayer* player, void* driver) {
	const struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	if (player->playerId == 0) {
		struct GBSIO* sio = lockstep->d.p;
		sio->siocnt = GBASIOMultiplayerClearSlave(sio->siocnt);
	}
}

static void _onSioDetach(struct SIOLockstepPlayer* player, void* driver) {
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	if (player->mode == GBA_SIO_MULTI) {
		struct GBSIO* sio = lockstep->d.p;
		sio->siocnt = GBASIOMultiplayerSetId(sio->siocnt, player->playerId);
		sio->siocnt = GBASIOMultiplayerSetSlave(sio->siocnt, player->playerId || lockstep->coordinator->nAttached < 2);
	}
}

static void _onSioModeSet(struct SIOLockstepPlayer* player, void* driver) {
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
}

static void _onSioTransferStart(struct SIOLockstepPlayer* player, int32_t eventFinishCycles, void* driver) {
	struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	struct GBSIO* sio = lockstep->d.p;
	lockstep->coordinator->normalData[player->playerId] = sio->pendingSB;
	mTimingDeschedule(&sio->p->timing, &sio->);
	mTimingSchedule(&sio->p->timing, &sio->completeEvent, eventFinishCycles);
}

static void _setReady(const bool ready, void* driver) {
	const struct GBSIOLockstepDriver* lockstep = (struct GBSIOLockstepDriver*) driver;
	struct GBSIO* sio = lockstep->d.p;
}

static void _lockstep(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBSIOLockstepDriver* lockstep = context;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);

	int32_t nextEvent = coordinator_lockstepEvent(coordinator, player, timing, cyclesLate);
	mTimingSchedule(timing, &lockstep->event, nextEvent);
}

