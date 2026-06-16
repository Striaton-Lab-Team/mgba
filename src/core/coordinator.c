#include <mgba/core/coordinator.h>
#include <mgba/internal/gb/sio/coordinator-lockstep.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio/lockstep.h>

static void _advanceCycle(struct SIOLockstepCoordinator*, struct SIOLockstepPlayer*);

static void _verifyAwake(struct SIOLockstepCoordinator* coordinator) {
#ifdef NDEBUG
	UNUSED(coordinator);
#else
	int i;
	int asleep = 0;
	for (i = 0; i < coordinator->nAttached; ++i) {
		if (!coordinator->attachedPlayers[i]) {
			continue;
		}
		struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		asleep += player->asleep;
	}
	mASSERT_DEBUG(!asleep || asleep < coordinator->nAttached);
#endif
}

void CoordinatorAbortTransfer(struct SIOLockstepCoordinator* coordinator, struct SIOLockstepPlayer* player) {
	mLOG(GBA_SIO, DEBUG, "Aborting in-progress transfer");
	// TODO: Do we need to clean this up better?
	coordinator->transferActive = false;
	coordinator->waiting = 0;

	if (player->playerId != 0) {
		struct SIOLockstepPlayer* runner = TableLookup(&coordinator->players, coordinator->attachedPlayers[0]);
		if (runner) {
			CoordinatorLockstepPlayerWake(runner);
		}
	} else {
		CoordinatorWakePlayers(coordinator);
	}
}

void CoordinatorWaitOnPlayers(struct SIOLockstepCoordinator* coordinator, struct SIOLockstepPlayer* player) {
	mASSERT_LOG(GBA_SIO, !coordinator->waiting, "Multiplayer desynchronized: coordinator still waiting");
	mASSERT_LOG(GBA_SIO, !player->asleep, "Multiplayer desynchronized: player asleep");
	mASSERT_LOG(GBA_SIO, player->playerId == 0,
	            "Multiplayer desynchronized: invalid player %i attempting to coordinate", player->playerId);
	if (coordinator->nAttached < 2) {
		return;
	}

	_advanceCycle(coordinator, player);
	mLOG(GBA_SIO, INFO, "Primary waiting for players to ack");
	// printf("PlayerId %d, Primary waiting for players to ack \n", player->playerId);
	coordinator->waiting = ((1 << coordinator->nAttached) - 1) & ~TARGET(player->playerId);
	CoordinatorLockstepPlayerSleep(player);
	CoordinatorWakePlayers(coordinator);

	_verifyAwake(coordinator);
}

void CoordinatorWakePlayers(struct SIOLockstepCoordinator* coordinator) {
	int i;
	for (i = 1; i < coordinator->nAttached; ++i) {
		if (!coordinator->attachedPlayers[i]) {
			continue;
		}
		struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		CoordinatorLockstepPlayerWake(player);
	}
	// printf("All players woke up \n");
}

void CoordinatorLockstepPlayerWake(struct SIOLockstepPlayer* player) {
	if (!player->asleep) {
		return;
	}
	player->asleep = false;
	player->user->wake(player->user);
}

void CoordinatorAckPlayer(struct SIOLockstepCoordinator* coordinator, struct SIOLockstepPlayer* player) {
	if (player->playerId == 0) {
		return;
	}
	coordinator->waiting &= ~TARGET(player->playerId);
	if (!coordinator->waiting) {
		mLOG(GBA_SIO, INFO, "All players acked, waking primary");
		if (coordinator->transferActive) {
			int i;
			for (i = 0; i < coordinator->nAttached; ++i) {
				if (!coordinator->attachedPlayers[i]) {
					continue;
				}
				struct SIOLockstepPlayer* playerTemp =
				    TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
				playerTemp->dataReceived = true;
			}

			coordinator->transferActive = false;
		}

		struct SIOLockstepPlayer* runner = TableLookup(&coordinator->players, coordinator->attachedPlayers[0]);
		CoordinatorLockstepPlayerWake(runner);
	}
	CoordinatorLockstepPlayerSleep(player);
}

int32_t CoordinatorLockstepPlayerTime(struct SIOLockstepPlayer* player) {
	// struct mTiming timing = player->coordinatorDriver.getCoreTiming(player);
	// return mTimingCurrentTime(&timing) - player->cycleOffset;
	return player->coordinatorDriver.getCoreTiming(player);
}

int32_t CoordinatorUntilNextSync(struct SIOLockstepCoordinator* coordinator, struct SIOLockstepPlayer* player) {
	int32_t cycle = coordinator->cycle - CoordinatorLockstepPlayerTime(player);
	if (player->playerId == 0) {
		if (coordinator->nAttached < 2) {
			cycle += UNLOCKED_INTERVAL;
		} else {
			cycle += LOCKSTEP_INTERVAL;
		}
	}
	return cycle;
}

void _advanceCycle(struct SIOLockstepCoordinator* coordinator, struct SIOLockstepPlayer* player) {
	int32_t newCycle = CoordinatorLockstepPlayerTime(player);
	mASSERT_DEBUG(newCycle - coordinator->cycle >= 0);
	coordinator->nextHardSync -= newCycle - coordinator->cycle;
	coordinator->cycle = newCycle;
}

void CoordinatorRemovePlayer(struct SIOLockstepCoordinator* coordinator, struct SIOLockstepPlayer* player) {
	struct SIOLockstepEvent event = {
		.type = SIO_EV_DETACH,
		.playerId = player->playerId,
		.timestamp = CoordinatorLockstepPlayerTime(player),
	};
	CoordinatorEnqueueEvent(coordinator, &event, TARGET_ALL & ~TARGET(player->playerId));

	coordinator->waiting = 0;
	coordinator->transferActive = false;

	TableRemove(&coordinator->players, player->lockstepId);
	CoordinatorReconfigPlayers(coordinator);

	struct SIOLockstepPlayer* runner = TableLookup(&coordinator->players, coordinator->attachedPlayers[0]);
	if (runner) {
		CoordinatorLockstepPlayerWake(runner);
	}
	_verifyAwake(coordinator);
}

void CoordinatorReconfigPlayers(struct SIOLockstepCoordinator* coordinator) {
	size_t players = TableSize(&coordinator->players);
	memset(coordinator->attachedPlayers, 0, sizeof(coordinator->attachedPlayers));
	if (players == 0) {
		mLOG(GBA_SIO, WARN, "Reconfiguring player IDs with no players attached somehow?");
	} else if (players == 1) {
		struct TableIterator iter;
		mASSERT_LOG(GBA_SIO, TableIteratorStart(&coordinator->players, &iter),
		            "Trying to reconfigure 1 player with empty player list");
		unsigned p0 = TableIteratorGetKey(&coordinator->players, &iter);
		coordinator->attachedPlayers[0] = p0;

		struct SIOLockstepPlayer* player = TableIteratorGetValue(&coordinator->players, &iter);
		// struct mTiming timing = player->coordinatorDriver.getCoreTiming(player);
		coordinator->cycle = player->coordinatorDriver.getCoreTiming(player);
		coordinator->nextHardSync = HARD_SYNC_INTERVAL;

		if (player->playerId != 0) {
			player->playerId = 0;
			if (player->user->playerIdChanged) {
				player->user->playerIdChanged(player->user, player->playerId);
			}
		}

		if (!coordinator->transferActive) {
			coordinator->transferMode = player->mode;
		}
	} else {
		struct UIntList playerPreferences[MAX_GBAS];

		int i;
		for (i = 0; i < MAX_GBAS; ++i) {
			UIntListInit(&playerPreferences[i], 4);
		}

		// Collect the first four players' requested player IDs so we can sort through them later
		int seen = 0;
		struct TableIterator iter;
		mASSERT_LOG(GBA_SIO, TableIteratorStart(&coordinator->players, &iter),
		            "Trying to reconfigure %" PRIz "u players with empty player list", players);
		do {
			unsigned pid = TableIteratorGetKey(&coordinator->players, &iter);
			struct SIOLockstepPlayer* player = TableIteratorGetValue(&coordinator->players, &iter);
			int requested = MAX_GBAS - 1;
			if (player->user->requestedId) {
				requested = player->user->requestedId(player->user);
			}
			if (requested < 0) {
				continue;
			}
			if (requested >= MAX_GBAS) {
				requested = MAX_GBAS - 1;
			}

			*UIntListAppend(&playerPreferences[requested]) = pid;
			++seen;
		} while (TableIteratorNext(&coordinator->players, &iter) && seen < MAX_GBAS);

		// Now sort each requested player ID to figure out who gets which ID
		seen = 0;
		for (i = 0; i < MAX_GBAS; ++i) {
			int j;
			for (j = 0; j <= i; ++j) {
				while (UIntListSize(&playerPreferences[j]) && seen < MAX_GBAS) {
					unsigned pid = *UIntListGetPointer(&playerPreferences[j], 0);
					UIntListShift(&playerPreferences[j], 0, 1);
					struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, pid);
					if (!player) {
						mLOG(GBA_SIO, ERROR, "Player list appears to have changed unexpectedly. PID %u missing.", pid);
						continue;
					}
					coordinator->attachedPlayers[seen] = pid;
					if (player->playerId != seen) {
						player->playerId = seen;
						if (player->user->playerIdChanged) {
							player->user->playerIdChanged(player->user, player->playerId);
						}
					}
					++seen;
				}
			}
		}

		for (i = 0; i < MAX_GBAS; ++i) {
			UIntListDeinit(&playerPreferences[i]);
		}
	}

	int nAttached = 0;
	size_t i;
	for (i = 0; i < MAX_GBAS; ++i) {
		unsigned pid = coordinator->attachedPlayers[i];
		if (!pid) {
			continue;
		}
		struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, pid);
		if (!player) {
			coordinator->attachedPlayers[i] = 0;
		} else {
			++nAttached;
		}
	}
	coordinator->nAttached = nAttached;
}

void CoordinatorSetReady(struct SIOLockstepCoordinator* coordinator, struct SIOLockstepPlayer* activePlayer,
                         int playerId, enum SIOMode mode) {
	activePlayer->otherModes[playerId] = mode;
	bool ready = true;
	int i;
	for (i = 0; ready && i < coordinator->nAttached; ++i) {
		ready = activePlayer->otherModes[i] == activePlayer->mode;
	}
	if (activePlayer->mode == GBA_SIO_MULTI) {
		activePlayer->coordinatorDriver.onReady(activePlayer, ready);
	}
}

void CoordinatorHardSync(struct SIOLockstepCoordinator* coordinator, struct SIOLockstepPlayer* player) {
	mASSERT_DEBUG(player->playerId == 0);
	struct SIOLockstepEvent event = {
		.type = SIO_EV_HARD_SYNC,
		.playerId = 0,
		.timestamp = CoordinatorLockstepPlayerTime(player),
	};
	CoordinatorEnqueueEvent(coordinator, &event, TARGET_SECONDARY);
	CoordinatorWaitOnPlayers(coordinator, player);
}

void CoordinatorLockstepPlayerSleep(struct SIOLockstepPlayer* player) {
	if (player->asleep) {
		return;
	}
	// printf("PlayerId %d, Going to sleep \n", player->playerId);
	player->asleep = true;
	player->user->sleep(player->user);
	player->coordinatorDriver.onPlayerSleep(player);
}

bool CoordinatorLockstepOtherPlayerWaiting(struct SIOLockstepCoordinator* coordinator,
                                           struct SIOLockstepPlayer* player) {
	// FIXME this is shoddy
	int i;
	for (i = 1; i < coordinator->nAttached; ++i) {
		if (!coordinator->attachedPlayers[i]) {
			continue;
		}
		struct SIOLockstepPlayer* attachedPlayer = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		if (attachedPlayer->playerId == player->playerId) {
			continue;
		}
		return attachedPlayer->asleep;
	}
	return false;
}

void CoordinatorEnqueueEvent(struct SIOLockstepCoordinator* coordinator, const struct SIOLockstepEvent* event,
                             uint32_t target) {
	mLOG(GBA_SIO, INFO, "Enqueuing event of type %X from %i for target %X at timestamp %X", event->type,
	     event->playerId, target, event->timestamp);

	int i;
	for (i = 0; i < coordinator->nAttached; ++i) {
		if (!(target & TARGET(i))) {
			continue;
		}
		struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		mASSERT_LOG(GBA_SIO, player->freeList, "No free events");
		struct SIOLockstepEvent* newEvent = player->freeList;
		player->freeList = newEvent->next;

		memcpy(newEvent, event, sizeof(*event));
		struct SIOLockstepEvent** previous = &player->queue;
		struct SIOLockstepEvent* next = player->queue;
		while (next) {
			int32_t until = newEvent->timestamp - next->timestamp;
			if (until < 0) {
				break;
			}
			previous = &next->next;
			next = next->next;
		}
		newEvent->next = next;
		*previous = newEvent;
	}
}

int32_t CoordinatorLockstepEvent(struct SIOLockstepCoordinator* coordinator, struct SIOLockstepPlayer* player,
                                 struct mTiming* timing, uint32_t cyclesLate) {
	MutexLock(&coordinator->mutex);
	bool wasDetach = false;
	if (player->queue && player->queue->type == SIO_EV_DETACH) {
		mLOG(GBA_SIO, INFO, "Player %i detached at timestamp %X, picking up the pieces", player->queue->playerId,
		     player->queue->timestamp);
		wasDetach = true;
	}
	if (player->playerId == 0 && CoordinatorLockstepPlayerTime(player) - coordinator->cycle >= 0) {
		// We are the clock owner; advance the shared clock. However, if we just became
		// the clock owner (by the previous one disconnecting) we might be slightly
		// behind the shared clock. We should wait a bit if needed in that case.
		_advanceCycle(coordinator, player);
		if (!coordinator->transferActive) {
			CoordinatorWakePlayers(coordinator);
		}
		if (coordinator->nextHardSync < 0) {
			if (!coordinator->waiting) {
				CoordinatorHardSync(coordinator, player);
			}
			coordinator->nextHardSync += HARD_SYNC_INTERVAL;
		}
	}

	int32_t nextEvent = CoordinatorUntilNextSync(coordinator, player);
	while (true) {
		struct SIOLockstepEvent* event = player->queue;
		if (!event) {
			break;
		}
		int32_t playerTime = CoordinatorLockstepPlayerTime(player);
		if (event->timestamp > playerTime) {
			break;
		}
		player->queue = event->next;
		struct SIOLockstepEvent reply = {
			.playerId = player->playerId,
			.timestamp = CoordinatorLockstepPlayerTime(player),
		};
		mLOG(GBA_SIO, INFO, "Got event of type %X from %i at timestamp %X", event->type, event->playerId,
		     event->timestamp);
		switch (event->type) {
		case SIO_EV_GBA_ATTACH:
			CoordinatorSetReady(coordinator, player, event->playerId, -1);
			player->coordinatorDriver.onAttach(player);
			reply.mode = player->mode;
			reply.type = SIO_EV_MODE_SET;
			CoordinatorEnqueueEvent(coordinator, &reply, TARGET(event->playerId));
			break;
		case SIO_EV_GB_ATTACH:
			CoordinatorSetReady(coordinator, player, event->playerId, -1);
			player->coordinatorDriver.onAttach(player);
			reply.mode = player->mode;
			reply.type = SIO_EV_MODE_SET;
			CoordinatorEnqueueEvent(coordinator, &reply, TARGET(event->playerId));
			break;
		case SIO_EV_HARD_SYNC:
			CoordinatorAckPlayer(coordinator, player);
			break;
		case SIO_EV_TRANSFER_START:
			nextEvent = event->finishCycle - CoordinatorLockstepPlayerTime(player) - cyclesLate;
			player->coordinatorDriver.onTransferStart(player, nextEvent);
			CoordinatorAckPlayer(coordinator, player);
			break;
		case SIO_EV_MODE_SET:
			if (coordinator->transferActive && player->mode != event->mode) {
				mLOG(GBA_SIO, INFO, "Switching modes while transfer is active");
				CoordinatorAbortTransfer(coordinator, player);
			}
			player->coordinatorDriver.onDetach(player);
			CoordinatorSetReady(coordinator, player, event->playerId, event->mode);
			if (event->playerId == 0) {
				CoordinatorAckPlayer(coordinator, player);
			}
			break;
		case SIO_EV_DETACH:
			CoordinatorSetReady(coordinator, player, event->playerId, -1);
			CoordinatorSetReady(coordinator, player, player->playerId, player->mode);
			reply.mode = player->mode;
			reply.type = SIO_EV_MODE_SET;
			CoordinatorEnqueueEvent(coordinator, &reply, ~TARGET(event->playerId));
			player->coordinatorDriver.onDetach(player);
			wasDetach = true;
			break;
		}
		event->next = player->freeList;
		player->freeList = event;
	}
	if (player->queue && player->queue->timestamp - CoordinatorLockstepPlayerTime(player) < nextEvent) {
		nextEvent = player->queue->timestamp - CoordinatorLockstepPlayerTime(player);
	}

	if (player->playerId != 0 && nextEvent <= LOCKSTEP_INTERVAL) {
		if (!player->queue || wasDetach) {
			CoordinatorLockstepPlayerSleep(player);
			// XXX: Is there a better way to gain sync lock at the beginning?
			if (nextEvent < 64) {
				nextEvent = 64;
			}
			_verifyAwake(coordinator);
		}
	}
	MutexUnlock(&coordinator->mutex);

	mASSERT_DEBUG(nextEvent > 0);

	return nextEvent;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//
// Coordinator
//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

void CoordinatorInit(struct SIOLockstepCoordinator* coordinator) {
	memset(coordinator, 0, sizeof(*coordinator));
	MutexInit(&coordinator->mutex);
	TableInit(&coordinator->players, 8, free);
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

void CoordinatorDeinit(struct SIOLockstepCoordinator* coordinator) {
	MutexDeinit(&coordinator->mutex);
	TableDeinit(&coordinator->players);
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

void CoordinatorAttachDriver(struct SIOLockstepCoordinator* coordinator, enum mPlatform platform, void* driver) {
	switch (platform) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA: {
		struct GBASIOLockstepDriver* gbaLockstepDriver = (struct GBASIOLockstepDriver*) driver;
		if (gbaLockstepDriver->coordinator && gbaLockstepDriver->coordinator != coordinator) {
			// TODO
			abort();
		}
		gbaLockstepDriver->coordinator = coordinator;
		break;
	}

#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB: {
		struct GBSIOLockstepDriver* gbLockstepDriver = (struct GBSIOLockstepDriver*) driver;
		if (gbLockstepDriver->coordinator && gbLockstepDriver->coordinator != coordinator) {
			// TODO
			abort();
		}
		gbLockstepDriver->coordinator = coordinator;
		break;
	}
#endif
	default:
		break;
	}
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

void CoordinatorDetachDriver(struct SIOLockstepCoordinator* coordinator, enum mPlatform platform, void* driver) {
	switch (platform) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA: {
		struct GBASIOLockstepDriver* gbaLockstepDriver = (struct GBASIOLockstepDriver*) driver;
		if (gbaLockstepDriver->coordinator != coordinator) {
			// TODO
			abort();
			return;
		}
		MutexLock(&coordinator->mutex);
		struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, gbaLockstepDriver->lockstepId);
		if (player) {
			CoordinatorRemovePlayer(coordinator, player);
		}
		MutexUnlock(&coordinator->mutex);
		gbaLockstepDriver->coordinator = NULL;
		break;
	}

#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB: {
		struct GBSIOLockstepDriver* gbLockstepDriver = (struct GBSIOLockstepDriver*) driver;
		if (gbLockstepDriver->coordinator != coordinator) {
			// TODO
			abort();
			return;
		}
		MutexLock(&coordinator->mutex);
		struct SIOLockstepPlayer* player = TableLookup(&coordinator->players, gbLockstepDriver->lockstepId);
		if (player) {
			CoordinatorRemovePlayer(coordinator, player);
		}
		MutexUnlock(&coordinator->mutex);
		gbLockstepDriver->coordinator = NULL;
		break;
	}
#endif
	default:
		break;
	}
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//

size_t CoordinatorAttached(struct SIOLockstepCoordinator* coordinator) {
	return coordinator->nAttached;
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////-//
