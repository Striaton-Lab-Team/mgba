/* Copyright (c) 2013-2024 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/lockstep.h>

#include "mgba-util/platform/posix/threading.h"

#include <mgba/core/coordinator.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>

#define DRIVER_ID 0x6B636F4C
#define DRIVER_STATE_VERSION 1

static bool GBASIOLockstepDriverInit(struct GBASIODriver* driver);
static void GBASIOLockstepDriverDeinit(struct GBASIODriver* driver);
static void GBASIOLockstepDriverReset(struct GBASIODriver* driver);
static uint32_t GBASIOLockstepDriverId(const struct GBASIODriver* driver);
static void GBASIOLockstepDriverSetMode(struct GBASIODriver* driver, enum SIOMode mode);
static bool GBASIOLockstepDriverHandlesMode(struct GBASIODriver* driver, enum SIOMode mode);
static int GBASIOLockstepDriverConnectedDevices(struct GBASIODriver* driver);
static int GBASIOLockstepDriverDeviceId(struct GBASIODriver* driver);
static uint16_t GBASIOLockstepDriverWriteSIOCNT(struct GBASIODriver* driver, uint16_t value);
static uint16_t GBASIOLockstepDriverWriteRCNT(struct GBASIODriver* driver, uint16_t value);
static bool GBASIOLockstepDriverStart(struct GBASIODriver* driver);
static void GBASIOLockstepDriverFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]);
static uint8_t GBASIOLockstepDriverFinishNormal8(struct GBASIODriver* driver);
static uint32_t GBASIOLockstepDriverFinishNormal32(struct GBASIODriver* driver);

static void _onSioAttach(struct SIOLockstepPlayer* player);
static void _onSioDetach(struct SIOLockstepPlayer* player);
static void _onSioModeSet(struct SIOLockstepPlayer* player);
static void _onSioTransferStart(struct SIOLockstepPlayer* player, int32_t eventFinishCycles);
static void _setReady(struct SIOLockstepPlayer* player, bool ready);
static struct mTiming _coreTiming(struct SIOLockstepPlayer* player);
static void _onPlayerSleep(struct SIOLockstepPlayer* player);

static void _lockstep(struct mTiming*, void* context, uint32_t);
static void _setData(struct SIOLockstepCoordinator* coordinator, uint32_t id, struct GBASIO* sio);

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

void GBASIOLockstepDriverCreate(struct GBASIOLockstepDriver* driver, struct mLockstepUser* user) {
	memset(driver, 0, sizeof(*driver));

	driver->d.init = GBASIOLockstepDriverInit;
	driver->d.deinit = GBASIOLockstepDriverDeinit;
	driver->d.reset = GBASIOLockstepDriverReset;
	driver->d.driverId = GBASIOLockstepDriverId;
	driver->d.loadState = NULL;
	driver->d.saveState = NULL;
	driver->d.setMode = GBASIOLockstepDriverSetMode;
	driver->d.handlesMode = GBASIOLockstepDriverHandlesMode;
	driver->d.deviceId = GBASIOLockstepDriverDeviceId;
	driver->d.connectedDevices = GBASIOLockstepDriverConnectedDevices;
	driver->d.writeSIOCNT = GBASIOLockstepDriverWriteSIOCNT;
	driver->d.writeRCNT = GBASIOLockstepDriverWriteRCNT;
	driver->d.start = GBASIOLockstepDriverStart;
	driver->d.finishMultiplayer = GBASIOLockstepDriverFinishMultiplayer;
	driver->d.finishNormal8 = GBASIOLockstepDriverFinishNormal8;
	driver->d.finishNormal32 = GBASIOLockstepDriverFinishNormal32;

	driver->event.context = driver;
	driver->event.callback = _lockstep;
	driver->event.name = "GBA SIO Lockstep";
	driver->event.priority = 0x80;

	driver->user = user;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//
// Driver callbacks
//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static bool GBASIOLockstepDriverInit(struct GBASIODriver* driver) {
	GBASIOLockstepDriverReset(driver);
	return true;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void GBASIOLockstepDriverDeinit(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
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

static void GBASIOLockstepDriverReset(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct SIOLockstepPlayer* player;
	if (!lockstep->lockstepId) {
		unsigned id;
		player = calloc(1, sizeof(*player));
		player->mode = driver->p->mode;
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
		CoordinatorReconfigPlayers(coordinator);
		player->cycleOffset = mTimingCurrentTime(&driver->p->p->timing) - coordinator->cycle;
		if (player->playerId != 0) {
			struct SIOLockstepEvent event = {
				.type = SIO_EV_GBA_ATTACH,
				.playerId = player->playerId,
				.timestamp = CoordinatorLockstepPlayerTime(player),
			};
			CoordinatorEnqueueEvent(coordinator, &event, TARGET_ALL & ~TARGET(player->playerId));
		}
	} else {
		MutexLock(&coordinator->mutex);
		player = TableLookup(&coordinator->players, lockstep->lockstepId);
		player->cycleOffset = mTimingCurrentTime(&driver->p->p->timing) - coordinator->cycle;
	}

	if (coordinator->transferActive) {
		CoordinatorAbortTransfer(coordinator, player);
		player->asleep = false;
	}
	if (player->playerId == 0 && coordinator->nAttached > 1) {
		coordinator->waiting = 0;
		// We will immediately go back to sleep when the initial mode gets set,
		// so we need to clear this here to avoid triggering an assert later.
		player->asleep = false;
		CoordinatorWakePlayers(coordinator);
	}

	if (mTimingIsScheduled(&lockstep->d.p->p->timing, &lockstep->event)) {
		MutexUnlock(&coordinator->mutex);
		return;
	}

	int32_t nextEvent;
	CoordinatorSetReady(coordinator, player, player->playerId, player->mode);
	if (TableSize(&coordinator->players) == 1) {
		coordinator->cycle = mTimingCurrentTime(&lockstep->d.p->p->timing);
		nextEvent = LOCKSTEP_INTERVAL;
	} else {
		CoordinatorSetReady(coordinator, player, 0, coordinator->transferMode);
		nextEvent = CoordinatorUntilNextSync(lockstep->coordinator, player);
	}
	MutexUnlock(&coordinator->mutex);
	mTimingSchedule(&lockstep->d.p->p->timing, &lockstep->event, nextEvent);
}

static uint32_t GBASIOLockstepDriverId(const struct GBASIODriver* driver) {
	UNUSED(driver);
	return DRIVER_ID;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void GBASIOLockstepDriverSetMode(struct GBASIODriver* driver, enum SIOMode mode) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	MutexLock(&coordinator->mutex);
	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (mode != player->mode) {
		mLOG(GBA_SIO, DEBUG, "Switching mode from %d to %d", player->mode, mode);
		player->mode = mode;
		struct SIOLockstepEvent event = {
			.type = SIO_EV_MODE_SET,
			.playerId = player->playerId,
			.timestamp = CoordinatorLockstepPlayerTime(player),
			.mode = mode,
		};
		if (player->playerId == 0) {
			coordinator->transferMode = mode;
			CoordinatorWaitOnPlayers(coordinator, player);
		}
		CoordinatorSetReady(coordinator, player, player->playerId, mode);
		CoordinatorEnqueueEvent(coordinator, &event, TARGET_ALL & ~TARGET(player->playerId));
	}
	MutexUnlock(&coordinator->mutex);
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static bool GBASIOLockstepDriverHandlesMode(struct GBASIODriver* driver, enum SIOMode mode) {
	UNUSED(driver);
	UNUSED(mode);
	return true;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static int GBASIOLockstepDriverConnectedDevices(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	if (!lockstep->lockstepId) {
		return 0;
	}
	MutexLock(&coordinator->mutex);
	int attached = coordinator->nAttached - 1;
	MutexUnlock(&coordinator->mutex);
	return attached;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static int GBASIOLockstepDriverDeviceId(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
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

static uint16_t GBASIOLockstepDriverWriteSIOCNT(struct GBASIODriver* driver, uint16_t value) {
	UNUSED(driver);
	mLOG(GBA_SIO, DEBUG, "Lockstep: SIOCNT <- %04X", value);
	return value;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static uint16_t GBASIOLockstepDriverWriteRCNT(struct GBASIODriver* driver, uint16_t value) {
	UNUSED(driver);
	mLOG(GBA_SIO, DEBUG, "Lockstep: RCNT <- %04X", value);
	return value;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static bool GBASIOLockstepDriverStart(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct GBASIO* sio = lockstep->d.p;
	bool ret = false;
	MutexLock(&coordinator->mutex);
	if (coordinator->transferActive) {
		mLOG(GBA_SIO, GAME_ERROR, "Transfer restarted unexpectedly");
		goto out;
	}
	if (coordinator->nAttached < 2) {
		mLOG(GBA_SIO, DEBUG, "Attempted to start transfer with no secondary players");
		goto out;
	}
	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (player->playerId != 0) {
		mLOG(GBA_SIO, DEBUG, "Secondary player attempted to start transfer");
		goto out;
	}
	mLOG(GBA_SIO, DEBUG, "Transfer starting at %08X", coordinator->cycle);
	memset(coordinator->multiData, 0xFF, sizeof(coordinator->multiData));

	_setData(coordinator, 0, sio);

	int32_t timestamp = CoordinatorLockstepPlayerTime(player);
	struct SIOLockstepEvent event = {
		.type = SIO_EV_TRANSFER_START,
		.timestamp = timestamp,
		.finishCycle = timestamp + GBASIOTransferCycles(player->mode, sio->siocnt, coordinator->nAttached - 1),
	};
	CoordinatorEnqueueEvent(coordinator, &event, TARGET_SECONDARY);
	CoordinatorWaitOnPlayers(coordinator, player);
	coordinator->transferActive = true;
	ret = true;
out:
	MutexUnlock(&coordinator->mutex);
	return ret;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void GBASIOLockstepDriverFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	MutexLock(&coordinator->mutex);
	if (coordinator->transferMode == GBA_SIO_MULTI) {
		struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
		if (!player->dataReceived) {
			mLOG(GBA_SIO, WARN, "MULTI did not receive data. Are we running behind?");
			memset(data, 0xFF, sizeof(uint16_t) * 4);
		} else {
			mLOG(GBA_SIO, DEBUG, "MULTI transfer finished: %04X %04X %04X %04X",
			     coordinator->multiData[0],
			     coordinator->multiData[1],
			     coordinator->multiData[2],
			     coordinator->multiData[3]);
			memcpy(data, coordinator->multiData, sizeof(uint16_t) * 4);
		}
		player->dataReceived = false;
		if (player->playerId == 0) {
			CoordinatorHardSync(coordinator, player);
		}
	}
	MutexUnlock(&coordinator->mutex);
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static uint8_t GBASIOLockstepDriverFinishNormal8(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	uint8_t data = 0xFF;
	MutexLock(&coordinator->mutex);
	if (coordinator->transferMode == GBA_SIO_NORMAL_8) {
		struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
		if (player->playerId > 0) {
			if (!player->dataReceived) {
				mLOG(GBA_SIO, WARN, "NORMAL did not receive data. Are we running behind?");
			} else {
				data = coordinator->normalData[player->playerId - 1];
				mLOG(GBA_SIO, DEBUG, "NORMAL8 transfer finished: %02X", data);
			}
		}
		player->dataReceived = false;
		if (player->playerId == 0) {
			CoordinatorHardSync(coordinator, player);
		}
	}
	MutexUnlock(&coordinator->mutex);
	return data;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static uint32_t GBASIOLockstepDriverFinishNormal32(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	uint32_t data = 0xFFFFFFFF;
	MutexLock(&coordinator->mutex);
	if (coordinator->transferMode == GBA_SIO_NORMAL_32) {
		struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
		if (player->playerId > 0) {
			if (!player->dataReceived) {
				mLOG(GBA_SIO, WARN, "Did not receive data. Are we running behind?");
			} else {
				data = coordinator->normalData[player->playerId - 1];
				mLOG(GBA_SIO, DEBUG, "NORMAL32 transfer finished: %08X", data);
			}
		}
		player->dataReceived = false;
		if (player->playerId == 0) {
			CoordinatorHardSync(coordinator, player);
		}
	}
	MutexUnlock(&coordinator->mutex);
	return data;
}

static void _setData(struct SIOLockstepCoordinator* coordinator, uint32_t id, struct GBASIO* sio) {
	switch (coordinator->transferMode) {
	case GBA_SIO_MULTI:
		coordinator->multiData[id] = sio->p->memory.io[GBA_REG(SIOMLT_SEND)];
		break;
	case GBA_SIO_NORMAL_8:
		coordinator->normalData[id] = sio->p->memory.io[GBA_REG(SIODATA8)];
		break;
	case GBA_SIO_NORMAL_32:
		coordinator->normalData[id] = sio->p->memory.io[GBA_REG(SIODATA32_LO)];
		coordinator->normalData[id] |= sio->p->memory.io[GBA_REG(SIODATA32_HI)] << 16;
		break;
	case GBA_SIO_UART:
	case GBA_SIO_GPIO:
	case GBA_SIO_JOYBUS:
		mLOG(GBA_SIO, WARN, "Unsupported mode %i in lockstep", coordinator->transferMode);
		// TODO: Should we handle this or just abort?
		break;
	}
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//
// Coordinator Callbacks 
//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void _onSioAttach(struct SIOLockstepPlayer* player) {
	const struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) player->driver;
	if (player->playerId == 0) {
		struct GBASIO* sio = lockstep->d.p;
		sio->siocnt = GBASIOMultiplayerClearSlave(sio->siocnt);
	}
}

static void _onSioDetach(struct SIOLockstepPlayer* player) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) player->driver;
	if (player->mode == GBA_SIO_MULTI) {
		struct GBASIO* sio = lockstep->d.p;
		sio->siocnt = GBASIOMultiplayerSetId(sio->siocnt, player->playerId);
		sio->siocnt = GBASIOMultiplayerSetSlave(sio->siocnt, player->playerId || lockstep->coordinator->nAttached < 2);
	}
}

static void _onSioModeSet(struct SIOLockstepPlayer* player) {
	UNUSED(player);
}

static void _onSioTransferStart(struct SIOLockstepPlayer* player, int32_t eventFinishCycles) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) player->driver;
	struct GBASIO* sio = lockstep->d.p;
	sio->siocnt |= 0x80;
	_setData(lockstep->coordinator, player->playerId, sio);
	mTimingDeschedule(&sio->p->timing, &sio->completeEvent);
	mTimingSchedule(&sio->p->timing, &sio->completeEvent, eventFinishCycles);
}

static void _setReady(struct SIOLockstepPlayer* player, bool ready) {
	const struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) player->driver;
	struct GBASIO* sio = lockstep->d.p;

	sio->siocnt = GBASIOMultiplayerSetReady(sio->siocnt, ready);
	sio->rcnt = GBASIORegisterRCNTSetSd(sio->rcnt, ready);
}

static struct mTiming _coreTiming(struct SIOLockstepPlayer* player) {
	struct GBASIODriver* lockstep = (struct GBASIODriver*) player->driver;
	return lockstep->p->p->timing;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static void _onPlayerSleep(struct SIOLockstepPlayer* player) {
	struct GBASIODriver* lockstep = (struct GBASIODriver*) player->driver;
	lockstep->p->p->cpu->nextEvent = 0;
	GBAInterrupt(lockstep->p->p);
}

static void _lockstep(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBASIOLockstepDriver* lockstep = context;
	struct SIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);

	mASSERT_LOG(GBA_SIO, player->playerId >= 0 && player->playerId < 4, "Invalid multiplayer ID %i", player->playerId);
	int32_t nextEvent = CoordinatorLockstepEvent(coordinator, player, timing, cyclesLate);
	mTimingSchedule(timing, &lockstep->event, nextEvent);
}
