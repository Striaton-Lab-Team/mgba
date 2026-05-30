/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>

#include <mgba/core/core.h>
#include <mgba/core/lockstep.h>
#include <mgba/core/coordinator.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/sio/lockstep.h>
#endif
#ifdef M_CORE_GB
#include <mgba/internal/gb/sio/coordinator-lockstep.h>
#endif

#include <memory>

struct GBSIOLockstepDriver;
struct GBASIOLockstepDriver;

namespace QGBA {

class CoreController;

class MultiplayerController : public QObject {
Q_OBJECT

public:
	MultiplayerController();
	~MultiplayerController();

	bool attachGame(CoreController*);
	void detachGame(CoreController*);

	int attached();
	int playerId(CoreController*) const;
	int saveId(CoreController*) const;

signals:
	void gameAttached();
	void gameDetached();

private:
	union LockstepDriver {
		struct GBSIOLockstepDriver* gb;
		struct GBASIOLockstepDriver* gba;
	};
	struct Player {
		Player(CoreController* controller);

		int id() const;
		bool operator<(const Player&) const;

		CoreController* controller;
		LockstepDriver lockstepDriver = {nullptr};
		int awake = 1;
		int32_t cyclesPosted = 0;
		unsigned waitMask = 0;
		int saveId = 1;
		int preferredId = 0;
		bool attached = false;
	};
	struct LockstepUser : mLockstepThreadUser {
		MultiplayerController* controller;
		int pid;
	};

	Player* player(int id);
	const Player* player(int id) const;
	void fixOrder();

	struct mLockstep m_lockstep;
// 	union {
// 		struct mLockstep m_lockstep;
// #ifdef M_CORE_GB
// 		struct GBSIOLockstep m_gbLockstep;
// #endif
// 	};

#ifdef M_CORE_GBA
	struct SIOLockstepCoordinator m_gbaCoordinator;
#endif

	enum mPlatform m_platform = mPLATFORM_NONE;
	int m_nextPid = 0;
	int m_claimedIds = 0;
	QHash<int, Player> m_pids;
	QList<int> m_players;
	QMutex m_lock;
	QHash<QPair<QString, QString>, int> m_claimedSaves;
};

}
