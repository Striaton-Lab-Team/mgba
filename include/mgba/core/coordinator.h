
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
struct LockstepCoordinatorDriver {
	void (*onAttach)(struct SIOLockstepPlayer* player);
	void (*onDetach)(struct SIOLockstepPlayer* player);
	void (*onSetMode)(struct SIOLockstepPlayer* player);
	void (*onTransferStart)(struct SIOLockstepPlayer* player, int32_t eventFinishCycles);
	void (*onReady)(struct SIOLockstepPlayer* player, bool ready);
	void (*onPlayerSleep)(struct SIOLockstepPlayer* player);
	int32_t (*getCoreTiming)(struct SIOLockstepPlayer* player);
};

struct SIOLockstepPlayer {
	void* driver;
	struct LockstepCoordinatorDriver coordinatorDriver;

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
bool CoordinatorLockstepOtherPlayerWaiting(struct SIOLockstepCoordinator* coordinator, struct SIOLockstepPlayer* player);

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