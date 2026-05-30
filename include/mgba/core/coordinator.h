
#ifndef M_CORE_COORDINATOR_H
#define M_CORE_COORDINATOR_H
#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOCKSTEP_INTERVAL 4096
#define UNLOCKED_INTERVAL 4096
#define TARGET_ALL 0xF
#define HARD_SYNC_INTERVAL 0x80000
#define TARGET(P) (1 << (P))

#define MAX_LOCKSTEP_EVENTS 8

#define TARGET_PRIMARY 0x1
#define TARGET_SECONDARY ((TARGET_ALL) & ~(TARGET_PRIMARY))
#include "mgba-util/platform/posix/threading.h"
#include "mgba-util/table.h"
#include "mGBA/gba/interface.h"


enum SIOLockstepEventType {
	SIO_EV_GB_ATTACH,
	SIO_EV_GBA_ATTACH,
	SIO_EV_DETACH,
	SIO_EV_HARD_SYNC,
	SIO_EV_MODE_SET,
	SIO_EV_TRANSFER_START,
};

struct SIOLockstepPlayer;
struct SIOLockstepEvent {
	enum SIOLockstepEventType type;
	int32_t timestamp;
	struct SIOLockstepEvent* next;
	int playerId;

	enum SIOMode mode; //FIXME
	int32_t finishCycle;

};

struct SIOLockstepPLayer;
struct LockstepEventCallbacks {
	void (*on_sio_attach)(struct SIOLockstepPlayer* player, void* driver);
	void (*on_sio_detach)(struct SIOLockstepPlayer* player, void* driver);
	void (*on_sio_mode_set)(struct SIOLockstepPlayer* player, void* driver);
	void (*on_sio_transfer_start)(struct SIOLockstepPlayer* player, int32_t eventFinishCycles, void* driver);
	void (*on_sio_ready)(const bool ready, void* driver);
};

struct SIOLockstepPlayer {
	void* driver;
	struct LockstepEventCallbacks lockstepEvents;
	const struct mTiming* (*core_timing)(void* driver);

	int playerId;
	enum SIOMode mode;
	enum SIOMode otherModes[4]; //FIXME
	bool asleep;
	int32_t cycleOffset;
	struct SIOLockstepEvent* queue;
	bool dataReceived;
	struct mLockstepUser* user;
	unsigned lockstepId;

	struct SIOLockstepEvent buffer[MAX_LOCKSTEP_EVENTS];
	struct SIOLockstepEvent* freeList;
};

struct SIOLockstepCoordinator {
	struct Table players;
	Mutex mutex;

	unsigned nextId;

	unsigned attachedPlayers[4];
	int nAttached;
	uint32_t waiting;

	bool transferActive;
	enum SIOMode transferMode;

	int32_t cycle;
	int32_t nextHardSync;

	uint16_t multiData[4];
	uint32_t normalData[4];
};

void CoordinatorWaitOnPlayers(struct SIOLockstepCoordinator*, struct SIOLockstepPlayer*);
void CoordinatorAckPlayer(struct SIOLockstepCoordinator*, struct SIOLockstepPlayer*);
void CoordinatorWakePlayers(struct SIOLockstepCoordinator*);

int32_t CoordinatorLockstepPlayerTime(struct SIOLockstepPlayer*);
void CoordinatorLockstepPlayerWake(struct SIOLockstepPlayer*);
void CoordinatorLockstepPlayerSleep(struct SIOLockstepPlayer*);

void CoordinatorRemovePlayer(struct SIOLockstepCoordinator*, struct SIOLockstepPlayer*);
void CoordinatorReconfigPlayers(struct SIOLockstepCoordinator*);
int32_t CoordinatorUntilNextSync(struct SIOLockstepCoordinator*, struct SIOLockstepPlayer*);
void CoordinatorEnqueueEvent(struct SIOLockstepCoordinator*, const struct SIOLockstepEvent*, uint32_t target);
void CoordinatorSetReady(struct SIOLockstepCoordinator*, struct SIOLockstepPlayer* activePlayer, int playerId, enum SIOMode mode);
void CoordinatorHardSync(struct SIOLockstepCoordinator*, struct SIOLockstepPlayer*);
void CoordinatorAbortTransfer(struct SIOLockstepCoordinator* coordinator, struct SIOLockstepPlayer* player);
int32_t CoordinatorLockstepEvent(struct SIOLockstepCoordinator* coordinator, struct SIOLockstepPlayer* player, struct mTiming* timing, uint32_t cyclesLate);

void CoordinatorInit(struct SIOLockstepCoordinator*);
void CoordinatorDeinit(struct SIOLockstepCoordinator*);

void CoordinatorAttachDriver(struct SIOLockstepCoordinator*, enum mPlatform platform, void* driver);
void CoordinatorDetachDriver(struct SIOLockstepCoordinator*, enum mPlatform platform, void* driver);

size_t CoordinatorAttached(struct SIOLockstepCoordinator*);

#ifdef __cplusplus
}
#endif

#endif