//  BRSyncManager.c
//
//  Created by Michael Carrara on 12/08/19.
//  Copyright (c) 2019 breadwallet LLC.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <pthread.h>

#include "support/BRArray.h"
#include "BRSyncManager.h"
#include "BRPeerManager.h"

/// MARK: - Common Decls & Defs

#if !defined (MAX)
#define MAX(a,b) (((a)>(b))?(a):(b))
#endif

#if !defined (MIN)
#define MIN(a,b) (((a)<(b))?(a):(b))
#endif

#define ONE_WEEK_IN_SECONDS      (7*24*60*60)

/**
 * The BRSyncManager `class` is designed to wrap the existing BRPeerManager `class`,
 * in P2P mode, using the BRPeerSyncManager, as well as provide an equivalent manager
 * when operating in BRD mode, using the BRClientSyncManager.
 *
 * There are at least a couple of likely controversial design decisions included in
 * how the BRSyncManager (and its `child classes`) operates:
 *
 *  1) Should a BRSyncManager interact (i.e. add/remove/update txns in) directly
 *     with a BRWallet or should those interactions be handled externally by its
 *     owner (i.e. BRWalletManager)?
 *
 *     - The existing BRPeerManager, which is long proven, DOES interact with the
 *       a BRWallet to add/remove/update transactions in response to network events
 *     - As such, the BRClientSyncManager was designed to do the same. Namely, it
 *       interacts with the BRWallet to add/remove/update transactions.
 *     - Going forward, if and when BRClientSyncManager is extracted into a generic
 *       component, we might want to revisit this and instead have it merely
 *       announce network events, rather than explicitly act upon them.
 *
 *  2) Should a BRSyncManager interact directly with the filesystem or should they be
 *     handled externally by its owner (i.e. BRWalletManager)?
 *
 *     - The existing BRWallet/BRPeerManager design approach was such that
 *       clients registered transaction callbacks on the BRWallet and network callbacks
 *       on the BRPeerManager. The BRPeerManager has a pointer to the BRWallet
 *       and directly adds/removes/updates transactions based on what the network
 *       is telling it. Interactions with the filesystem are done in the BRWallet
 *       transaction callbacks, as a result of manipulations done by the BRPeerManager.
 *     - As such, the BRClientSyncManager is designed to do the same. Namely, it does
 *       NOT interact directly with the filesystem but instead manipulates the BRWallet
 *       in response to network events it has received.
 */

/**
 * `BRSyncManagerStruct` only contains the minimum amount of information needed
 *  to be able to downcast to the appropriate sync manager implementation.
 *
 *  Fields that look common (like lock, isConnected, etc.) are not placed in the
 *  common structure as it is anticipated that `BRClientSyncManagerStruct` will be
 *  refactored out into a generic component, at some point. If it depended on
 *  fields in `BRSyncManagerStruct`, that refactoring would become more difficult.
 */
struct BRSyncManagerStruct {
    BRSyncMode mode;
};

#define CONFIRMATION_BLOCK_COUNT 6

static uint32_t
_getLastConfirmedSendTxHeight(BRWallet *wallet, uint32_t lastBlockHeight) {
    uint32_t scanHeight = 0;

    size_t transactionCount = BRWalletTransactions (wallet, NULL, 0);

    if (lastBlockHeight >= CONFIRMATION_BLOCK_COUNT && 0 != transactionCount) {
        BRArrayOf(BRTransaction *) transactions = (BRArrayOf(BRTransaction *) ) calloc (transactionCount, sizeof (BRTransaction *));
        transactionCount = BRWalletTransactions (wallet, transactions, transactionCount);

        for (size_t index = 0; index < transactionCount; index++) {
            // ensure:
            // - tx is valid (i.e. no previous transaction spend any of utxos, and no inputs are invalid)
            // - AND the transaction was a SEND
            // - AND the transaction has been confirmed
            if (BRWalletTransactionIsValid (wallet, transactions[index]) &&
                0 != BRWalletAmountSentByTx (wallet, transactions[index]) &&
                TX_UNCONFIRMED != transactions[index]->blockHeight &&
                transactions[index]->blockHeight < (lastBlockHeight - CONFIRMATION_BLOCK_COUNT)) {
                scanHeight = (transactions[index]->blockHeight > scanHeight ?
                                transactions[index]->blockHeight :
                                scanHeight);
            }
        }

        free (transactions);
    }

    return scanHeight;
}

/// MARK: - Sync Manager Decls & Defs

struct BRClientSyncManagerScanStateRecord {
    int requestId;
    BRAddress lastExternalAddress;
    BRAddress lastInternalAddress;
    BRSetOf(BRAddress *) knownAddresses;
    uint64_t begBlockNumber;
    uint64_t endBlockNumber;
    uint8_t isFullScan;
};

typedef struct BRClientSyncManagerScanStateRecord *BRClientSyncManagerScanState;

struct BRClientSyncManagerStruct {
    // !!! must be first !!!
    struct BRSyncManagerStruct common;

    /// Mark: - Immutable Section

    /**
     *  Mutable state lock
     */
    pthread_mutex_t lock;

    /**
     * Wallet being synced
     */
    BRWallet *wallet;

    /**
     * Event callback info
     */
    BRSyncManagerEventContext eventContext;
    BRSyncManagerEventCallback eventCallback;

    /**
     * Client callback info
     */
    BRSyncManagerClientContext clientContext;
    BRSyncManagerClientCallbacks clientCallbacks;

    /*
     * Chain params
     */
    const BRChainParams *chainParams;

    /**
     * The height of the earliest block of interest. Initialized based on the
     * earliest key time of the account being synced.
     */
    uint64_t initBlockHeight;

    /// Mark: - Mutable Sction

    /**
     * The known height of the blockchain, as reported by the "network".
     */
    uint64_t networkBlockHeight;

    /**
     * Flag for whether or not we are connected to the "network".
     */
    uint8_t isConnected;

    /**
     * Contains the height that we have synced to. Initially, this will be the same
     * as `initBlockHeight`. As we download transactions, this moves forward. It
     * can be reset when a `Scan` has been initiated, in which case it reverts to
     * `initBlockHeight`.
     */
    uint64_t syncedBlockHeight;

    /**
     * An identiifer generator for a BRD Request
     */
    int requestIdGenerator;

    /**
     * If we are syncing with BRD, instead of as P2P with PeerManager, then we'll keep a record to
     * ensure we've successfully completed the getTransactions() callbacks to the client.
     */
    struct BRClientSyncManagerScanStateRecord scanState;
};

typedef struct BRClientSyncManagerStruct * BRClientSyncManager;

// When using a BRD sync, offset the start block by N days of Bitcoin blocks; the value of N is
// assumed to be 'the maximum number of days that the blockchain DB could be behind'
#define BWM_MINUTES_PER_BLOCK                   10              // assumed, bitcoin
#define BWM_BRD_SYNC_DAYS_OFFSET                 1
#define BWM_BRD_SYNC_START_BLOCK_OFFSET        ((BWM_BRD_SYNC_DAYS_OFFSET * 24 * 60) / BWM_MINUTES_PER_BLOCK)

#define BRClientSyncManagerAsSyncManager(x)     ((BRSyncManager) (x))

static BRClientSyncManager
BRClientSyncManagerNew(BRSyncManagerEventContext eventContext,
                       BRSyncManagerEventCallback eventCallback,
                       BRSyncManagerClientContext clientContext,
                       BRSyncManagerClientCallbacks clientCallbacks,
                       OwnershipKept const BRChainParams *params,
                       OwnershipKept BRWallet *wallet,
                       uint32_t earliestKeyTime,
                       uint64_t blockHeight);

static BRClientSyncManager
BRSyncManagerAsClientSyncManager(BRSyncManager manager);

static void
BRClientSyncManagerFree(BRClientSyncManager manager);

static uint64_t
BRClientSyncManagerGetBlockHeight(BRClientSyncManager manager);

static void
BRClientSyncManagerConnect(BRClientSyncManager manager);

static void
BRClientSyncManagerDisconnect(BRClientSyncManager manager);

static void
BRClientSyncManagerScan(BRClientSyncManager manager);

static void
BRClientSyncManagerScanToDepth(BRClientSyncManager manager, BRSyncDepth depth);

static void
BRClientSyncManagerSubmit(BRClientSyncManager manager,
                          OwnershipKept BRTransaction *transaction);

static void
BRClientSyncManagerTickTock(BRClientSyncManager manager);

static int
BRClientSyncManagerIsInFullScan(BRClientSyncManager manager);

static void
BRClientSyncManagerAnnounceGetBlockNumber(BRClientSyncManager manager,
                                          int rid,
                                          uint64_t blockHeight);

static void
BRClientSyncManagerAnnounceGetTransactionsItem (BRClientSyncManager manager,
                                                int rid,
                                                OwnershipKept uint8_t *transaction,
                                                size_t transactionLength,
                                                uint64_t timestamp,
                                                uint64_t blockHeight);

static void
BRClientSyncManagerAnnounceGetTransactionsDone (BRClientSyncManager manager,
                                                int rid,
                                                int success);

static void
BRClientSyncManagerAnnounceSubmitTransaction (BRClientSyncManager manager,
                                              int rid,
                                              OwnershipKept BRTransaction *transaction,
                                              int error);

static void
BRClientSyncManagerUpdateBlockNumber (BRClientSyncManager manager);

static void
BRClientSyncManagerUpdateTransactions (BRClientSyncManager manager);

static int
BRClientSyncManagerGenerateRid (BRClientSyncManager manager);

static void
BRClientSyncManagerScanStateInit (BRClientSyncManagerScanState scanState,
                                  BRWallet *wallet,
                                  uint64_t syncedBlockHeight,
                                  uint64_t networkBlockHeight,
                                  int rid);

static void
BRClientSyncManagerScanStateWipe (BRClientSyncManagerScanState scanState);

static int
BRClientSyncManagerScanStateIsInProgress(BRClientSyncManagerScanState scanState);

static uint8_t
BRClientSyncManagerScanStateIsFullScan (BRClientSyncManagerScanState scanState);

static int
BRClientSyncManagerScanStateGetRequestId(BRClientSyncManagerScanState scanState);

static uint64_t
BRClientSyncManagerScanStateGetStartBlockNumber(BRClientSyncManagerScanState scanState);

static uint64_t
BRClientSyncManagerScanStateGetEndBlockNumber(BRClientSyncManagerScanState scanState);

static uint64_t
BRClientSyncManagerScanStateGetSyncedBlockNumber(BRClientSyncManagerScanState scanState);

static BRArrayOf(char *)
BRClientSyncManagerScanStateGetAddresses(BRClientSyncManagerScanState scanState);

static BRArrayOf(char *)
BRClientSyncManagerScanStateAdvanceAndGetNewAddresses (BRClientSyncManagerScanState scanState,
                                                       BRWallet *wallet);

static BRAddress *
_getWalletAddresses (BRWallet *wallet,
                     size_t *addressCount);

static void
_fillWalletAddressSet(BRSetOf(BRAddress *) addresses,
                      BRWallet *wallet);

static BRArrayOf(BRAddress *)
_updateWalletAddressSet(BRSetOf(BRAddress *) addresses,
                        BRWallet *wallet);

/// MARK: - Peer Sync Manager Decls & Defs

struct BRPeerSyncManagerStruct {
    // !!! must be first !!!
    struct BRSyncManagerStruct common;

    /// Mark: - Immutable Section

    /**
     *  Mutable state lock
     */
    pthread_mutex_t lock;

    /**
     * P2P syncing manager
     */
    BRPeerManager *peerManager;

    /**
     * Wallet being synced
     */
    BRWallet *wallet;

    /**
     * Event callback info
     */
    BRSyncManagerEventContext eventContext;
    BRSyncManagerEventCallback eventCallback;

    /// Mark: - Mutable Sction

    /**
     * The known height of the blockchain, as reported by the P2P network.
     */
    uint64_t networkBlockHeight;

    /**
     * Flag for whether or not we are connected to the P2P network.
     */
    uint8_t isConnected;

    /**
     * Flag for whether or not a full sync is in progress, versus when we have
     * caught up to the blockchain and are receiving new blocks.
     */
    uint8_t isFullScan;

};

typedef struct BRPeerSyncManagerStruct * BRPeerSyncManager;

#define BRPeerSyncManagerAsSyncManager(x)     ((BRSyncManager) (x))

static BRPeerSyncManager
BRPeerSyncManagerNew(BRSyncManagerEventContext eventContext,
                     BRSyncManagerEventCallback eventCallback,
                     OwnershipKept const BRChainParams *params,
                     OwnershipKept BRWallet *wallet,
                     uint32_t earliestKeyTime,
                     uint64_t blockHeight,
                     OwnershipKept BRMerkleBlock *blocks[],
                     size_t blocksCount,
                     OwnershipKept const BRPeer peers[],
                     size_t peersCount);

static BRPeerSyncManager
BRSyncManagerAsPeerSyncManager(BRSyncManager manager);

static void
BRPeerSyncManagerFree(BRPeerSyncManager);

static uint64_t
BRPeerSyncManagerGetBlockHeight(BRPeerSyncManager manager);

static void
BRPeerSyncManagerConnect(BRPeerSyncManager manager);

static void
BRPeerSyncManagerDisconnect(BRPeerSyncManager manager);

static void
BRPeerSyncManagerScan(BRPeerSyncManager manager);

static void
BRPeerSyncManagerScanToDepth(BRPeerSyncManager manager, BRSyncDepth depth);

static void
BRPeerSyncManagerSubmit(BRPeerSyncManager manager,
                        OwnershipKept BRTransaction *transaction);

static int
BRPeerSyncManagerIsInFullScan(BRPeerSyncManager manager);

static void
BRPeerSyncManagerTickTock(BRPeerSyncManager manager);

// PeerManager callbacks
static void _BRPeerSyncManagerSyncStarted (void *info);
static void _BRPeerSyncManagerSyncStopped (void *info, int reason);
static void _BRPeerSyncManagerTxStatusUpdate (void *info);
static void _BRPeerSyncManagerSaveBlocks (void *info, int replace, BRMerkleBlock **blocks, size_t count);
static void _BRPeerSyncManagerSavePeers  (void *info, int replace, const BRPeer *peers, size_t count);
static int  _BRPeerSyncManagerNetworkIsReachabele (void *info);
static void _BRPeerSyncManagerThreadCleanup (void *info);
static void _BRPeerSyncManagerTxPublished (void *info, int error);

/// MARK: - Sync Manager Implementation

extern BRSyncManager
BRSyncManagerNewForMode(BRSyncMode mode,
                        BRSyncManagerEventContext eventContext,
                        BRSyncManagerEventCallback eventCallback,
                        BRSyncManagerClientContext clientContext,
                        BRSyncManagerClientCallbacks clientCallbacks,
                        OwnershipKept const BRChainParams *params,
                        OwnershipKept BRWallet *wallet,
                        uint32_t earliestKeyTime,
                        uint64_t blockHeight,
                        OwnershipKept BRMerkleBlock *blocks[],
                        size_t blocksCount,
                        OwnershipKept const BRPeer peers[],
                        size_t peersCount) {
    assert (NULL != eventCallback);
    assert (NULL != params);
    assert (NULL != wallet);
    assert(blocks != NULL || blocksCount == 0);
    assert(peers != NULL || peersCount == 0);

    switch (mode) {
        case SYNC_MODE_BRD_ONLY:
        return BRClientSyncManagerAsSyncManager (BRClientSyncManagerNew (eventContext,
                                                                         eventCallback,
                                                                         clientContext,
                                                                         clientCallbacks,
                                                                         params,
                                                                         wallet,
                                                                         earliestKeyTime,
                                                                         blockHeight));
        case SYNC_MODE_P2P_ONLY:
        return BRPeerSyncManagerAsSyncManager (BRPeerSyncManagerNew (eventContext,
                                                                     eventCallback,
                                                                     params,
                                                                     wallet,
                                                                     earliestKeyTime,
                                                                     blockHeight,
                                                                     blocks,
                                                                     blocksCount,
                                                                     peers,
                                                                     peersCount));
        default:
        assert (0);
        return NULL;
    }
}

extern void
BRSyncManagerFree(BRSyncManager manager) {
    switch (manager->mode) {
        case SYNC_MODE_BRD_ONLY:
        BRClientSyncManagerFree (BRSyncManagerAsClientSyncManager (manager));
        break;
        case SYNC_MODE_P2P_ONLY:
        BRPeerSyncManagerFree (BRSyncManagerAsPeerSyncManager (manager));
        break;
        default:
        assert (0);
        break;
    }
}

extern uint64_t
BRSyncManagerGetBlockHeight (BRSyncManager manager) {
    uint64_t blockHeight = 0;
    switch (manager->mode) {
        case SYNC_MODE_BRD_ONLY:
        blockHeight = BRClientSyncManagerGetBlockHeight (BRSyncManagerAsClientSyncManager (manager));
        break;
        case SYNC_MODE_P2P_ONLY:
        blockHeight = BRPeerSyncManagerGetBlockHeight (BRSyncManagerAsPeerSyncManager (manager));
        break;
        default:
        assert (0);
        break;
    }
    return blockHeight;
}

extern void
BRSyncManagerConnect(BRSyncManager manager) {
    switch (manager->mode) {
        case SYNC_MODE_BRD_ONLY:
        BRClientSyncManagerConnect (BRSyncManagerAsClientSyncManager (manager));
        break;
        case SYNC_MODE_P2P_ONLY:
        BRPeerSyncManagerConnect (BRSyncManagerAsPeerSyncManager (manager));
        break;
        default:
        assert (0);
        break;
    }
}

extern void
BRSyncManagerDisconnect(BRSyncManager manager) {
    switch (manager->mode) {
        case SYNC_MODE_BRD_ONLY:
        BRClientSyncManagerDisconnect (BRSyncManagerAsClientSyncManager (manager));
        break;
        case SYNC_MODE_P2P_ONLY:
        BRPeerSyncManagerDisconnect (BRSyncManagerAsPeerSyncManager (manager));
        break;
        default:
        assert (0);
        break;
    }
}

extern void
BRSyncManagerScan(BRSyncManager manager) {
    switch (manager->mode) {
        case SYNC_MODE_BRD_ONLY:
        BRClientSyncManagerScan (BRSyncManagerAsClientSyncManager (manager));
        break;
        case SYNC_MODE_P2P_ONLY:
        BRPeerSyncManagerScan (BRSyncManagerAsPeerSyncManager (manager));
        break;
        default:
        assert (0);
        break;
    }
}

extern void
BRSyncManagerScanToDepth(BRSyncManager manager,
                         BRSyncDepth depth) {
    switch (manager->mode) {
        case SYNC_MODE_BRD_ONLY:
        BRClientSyncManagerScanToDepth (BRSyncManagerAsClientSyncManager (manager), depth);
        break;
        case SYNC_MODE_P2P_ONLY:
        BRPeerSyncManagerScanToDepth (BRSyncManagerAsPeerSyncManager (manager), depth);
        break;
        default:
        assert (0);
        break;
    }
}

extern void
BRSyncManagerSubmit(BRSyncManager manager,
                    OwnershipKept BRTransaction *transaction) {
    switch (manager->mode) {
        case SYNC_MODE_BRD_ONLY:
        BRClientSyncManagerSubmit (BRSyncManagerAsClientSyncManager (manager),
                                   transaction);
        break;
        case SYNC_MODE_P2P_ONLY:
        BRPeerSyncManagerSubmit (BRSyncManagerAsPeerSyncManager (manager),
                                 transaction);
        break;
        default:
        assert (0);
        break;
    }
}

extern void
BRSyncManagerTickTock(BRSyncManager manager) {
    switch (manager->mode) {
        case SYNC_MODE_BRD_ONLY:
        BRClientSyncManagerTickTock (BRSyncManagerAsClientSyncManager (manager));
        break;
        case SYNC_MODE_P2P_ONLY:
        BRPeerSyncManagerTickTock (BRSyncManagerAsPeerSyncManager (manager));
        break;
        default:
        assert (0);
        break;
    }
}

extern void
BRSyncManagerP2PFullScanReport(BRSyncManager manager) {
    switch (manager->mode) {
        case SYNC_MODE_P2P_ONLY:
            if (BRPeerSyncManagerIsInFullScan (BRSyncManagerAsPeerSyncManager (manager)))
                BRPeerSyncManagerTickTock(BRSyncManagerAsPeerSyncManager (manager));
        default:
            break;
    }
}

extern void
BRSyncManagerAnnounceGetBlockNumber(BRSyncManager manager,
                                    int rid,
                                    uint64_t blockHeight) {
    switch (manager->mode) {
        case SYNC_MODE_BRD_ONLY:
        BRClientSyncManagerAnnounceGetBlockNumber (BRSyncManagerAsClientSyncManager (manager),
                                                   rid,
                                                   blockHeight);
        break;
        case SYNC_MODE_P2P_ONLY:
        // this might occur if the owning BRWalletManager changed modes; silently ignore
        break;
        default:
        assert (0);
        break;
    }
}

extern void
BRSyncManagerAnnounceGetTransactionsItem(BRSyncManager manager,
                                         int rid,
                                         OwnershipKept uint8_t *transaction,
                                         size_t transactionLength,
                                         uint64_t timestamp,
                                         uint64_t blockHeight) {
    switch (manager->mode) {
        case SYNC_MODE_BRD_ONLY:
        BRClientSyncManagerAnnounceGetTransactionsItem (BRSyncManagerAsClientSyncManager (manager),
                                                        rid,
                                                        transaction,
                                                        transactionLength,
                                                        timestamp,
                                                        blockHeight);
        break;
        case SYNC_MODE_P2P_ONLY:
        // this might occur if the owning BRWalletManager changed modes; silently ignore
        break;
        default:
        assert (0);
        break;
    }
}

extern void
BRSyncManagerAnnounceGetTransactionsDone(BRSyncManager manager,
                                         int rid,
                                         int success) {
    switch (manager->mode) {
        case SYNC_MODE_BRD_ONLY:
        BRClientSyncManagerAnnounceGetTransactionsDone (BRSyncManagerAsClientSyncManager (manager),
                                                        rid,
                                                        success);
        break;
        case SYNC_MODE_P2P_ONLY:
        // this might occur if the owning BRWalletManager changed modes; silently ignore
        break;
        default:
        assert (0);
        break;
    }
}

extern void
BRSyncManagerAnnounceSubmitTransaction(BRSyncManager manager,
                                       int rid,
                                       OwnershipKept BRTransaction *transaction,
                                       int error) {
    switch (manager->mode) {
        case SYNC_MODE_BRD_ONLY:
        BRClientSyncManagerAnnounceSubmitTransaction (BRSyncManagerAsClientSyncManager (manager),
                                                      rid,
                                                      transaction,
                                                      error);
        break;
        case SYNC_MODE_P2P_ONLY:
        // this might occur if the owning BRWalletManager changed modes; silently ignore
        break;
        default:
        assert (0);
        break;
    }
}

/// MARK: - Client Sync Manager Implementation

static BRClientSyncManager
BRClientSyncManagerNew(BRSyncManagerEventContext eventContext,
                       BRSyncManagerEventCallback eventCallback,
                       BRSyncManagerClientContext clientContext,
                       BRSyncManagerClientCallbacks clientCallbacks,
                       OwnershipKept const BRChainParams *params,
                       OwnershipKept BRWallet *wallet,
                       uint32_t earliestKeyTime,
                       uint64_t blockHeight) {
    BRClientSyncManager manager = (BRClientSyncManager) calloc (1, sizeof(struct BRClientSyncManagerStruct));
    manager->common.mode = SYNC_MODE_BRD_ONLY;

    manager->wallet = wallet;
    manager->eventContext = eventContext;
    manager->eventCallback = eventCallback;
    manager->clientContext = clientContext;
    manager->clientCallbacks = clientCallbacks;
    manager->chainParams = params;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&manager->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    // Find the BRCheckpoint that is at least one week before earliestKeyTime.
    const BRCheckPoint *earliestCheckPoint = BRChainParamsGetCheckpointBefore (params, earliestKeyTime - ONE_WEEK_IN_SECONDS);
    assert (NULL != earliestCheckPoint);

    // Initialize this instance's blockHeight.  This might be out-of-sync with a) the P2P block
    // height which will be derived from the persistently restored blocks and then from the sync()
    // process or b) from the API-based Blockchain DB reported block height which will be updated
    // preriodically when in API sync modes.
    //
    // So, we'll start with the best block height we have and expect it to change. Doing this allows
    // an API-based sync to start immediately rather than waiting for a BRClientSyncManagerUpdateBlockNumber()
    // result in period '1' and then starting the sync in period '2' - where each period is
    // BWM_SLEEP_SECONDS and at least 1 minute.
    //
    // The initial sync will be from `initBlockHeight` to `networkBlockHeight`, regardless of if we
    // have synced, in P2P mode for example, to halfway between those two heights. Since API syncs are
    // "instantaneous", this provides us some safety, and is comparable with how P2P mode operates,
    // which syncs based on its trusted data (aka the blocks). In API mode, we don't have any trusted
    // data so sync on the whole range to be safe.
    manager->initBlockHeight    = MIN (earliestCheckPoint->height, blockHeight);
    manager->networkBlockHeight = MAX (earliestCheckPoint->height, blockHeight);
    manager->syncedBlockHeight  = manager->initBlockHeight;
    manager->isConnected        = 0;

    // the calloc will have taken care of this, but, better safe than sorry in case future dev
    // doesn't take that into account
    BRClientSyncManagerScanStateWipe (&manager->scanState);

    return manager;
}

static BRClientSyncManager
BRSyncManagerAsClientSyncManager(BRSyncManager manager) {
    if (manager->mode == SYNC_MODE_BRD_ONLY) {
        return (BRClientSyncManager) manager;
    }
    assert (0);
    return NULL;
}

static void
BRClientSyncManagerFree(BRClientSyncManager manager) {
    if (0 == pthread_mutex_lock (&manager->lock)) {
        BRClientSyncManagerScanStateWipe (&manager->scanState);
        pthread_mutex_unlock (&manager->lock);
    }

    pthread_mutex_destroy(&manager->lock);
    memset (manager, 0, sizeof(*manager));
    free (manager);
}

static uint64_t
BRClientSyncManagerGetBlockHeight(BRClientSyncManager manager) {
    uint64_t blockHeight = 0;
    if (0 == pthread_mutex_lock (&manager->lock)) {
        blockHeight = manager->networkBlockHeight;
        pthread_mutex_unlock (&manager->lock);
    }
    return blockHeight;
}

static void
BRClientSyncManagerConnect(BRClientSyncManager manager) {
    uint8_t needEvent = 0;

    if (0 == pthread_mutex_lock (&manager->lock)) {
        if (!manager->isConnected) {
            manager->isConnected = 1;
            needEvent = 1;
        }

        // Send event while holding the state lock so that event
        // callbacks are ordered to reflect state transitions.

        if (needEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRClientSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_CONNECTED
                                    });
        }

        pthread_mutex_unlock (&manager->lock);
    } else {
        assert (0);
    }

    BRClientSyncManagerUpdateBlockNumber (manager);
    BRClientSyncManagerUpdateTransactions (manager);
}

static void
BRClientSyncManagerDisconnect(BRClientSyncManager manager) {
    uint8_t needConnectionEvent = 0;
    uint8_t needSyncEvent       = 0;

    if (0 == pthread_mutex_lock (&manager->lock)) {
        if (manager->isConnected) {
            // We are connected. Check for a full scan in progress
            // and then wipe the current scan state so that a new one will be
            // triggered.
            manager->isConnected = 0;
            needConnectionEvent = 1;
            needSyncEvent = BRClientSyncManagerScanStateIsFullScan (&manager->scanState);
            BRClientSyncManagerScanStateWipe (&manager->scanState);
        }

        // Send event while holding the state lock so that event
        // callbacks are ordered to reflect state transitions.

        if (needSyncEvent) {
            // TODO(fix): What should the error code be?
            manager->eventCallback (manager->eventContext,
                                    BRClientSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_SYNC_STOPPED,
                                        { .syncStopped = { -1 }}
                                    });
        }

        if (needConnectionEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRClientSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_DISCONNECTED
                                    });
        }

        pthread_mutex_unlock (&manager->lock);
    } else {
        assert (0);
    }
}

static void
BRClientSyncManagerScan(BRClientSyncManager manager) {
    BRClientSyncManagerScanToDepth (manager, SYNC_DEPTH_HIGH);
}

static void
BRClientSyncManagerScanToDepth(BRClientSyncManager manager, BRSyncDepth depth) {
    uint8_t needConnectionEvent = 0;
    uint8_t needSyncEvent       = 0;

    if (0 == pthread_mutex_lock (&manager->lock)) {
        // Mirror BRPeerManager's behaviour in that BRPeerManagerRescan only
        // has an effect if we are connected (i.e. it does not perform a
        // connect)
        if (manager->isConnected) {
            needConnectionEvent = 1;
            // We are already connected. Checkf for a full scan in progress
            // and then wipe the current scan state so that a new one will be
            // triggered.
            needSyncEvent = BRClientSyncManagerScanStateIsFullScan (&manager->scanState);
            BRClientSyncManagerScanStateWipe (&manager->scanState);

            // Reset the height that we've synced to to be the initial height.
            // This will trigger a full sync.
            switch (depth) {
                case SYNC_DEPTH_LOW: {
                    uint32_t scanHeight = _getLastConfirmedSendTxHeight (manager->wallet, manager->networkBlockHeight);
                    manager->syncedBlockHeight = 0 == scanHeight ? manager->initBlockHeight : scanHeight;
                    break;
                }
                case SYNC_DEPTH_MEDIUM: {
                    const BRCheckPoint *checkpoint = BRChainParamsGetCheckpointBeforeBlockNumber (manager->chainParams, manager->networkBlockHeight);
                    manager->syncedBlockHeight = NULL == checkpoint ? manager->initBlockHeight : checkpoint->height;
                    break;
                }
                case SYNC_DEPTH_HIGH: {
                    manager->syncedBlockHeight = manager->initBlockHeight;
                    break;
                }
            }
        }

        // Send event while holding the state lock so that event
        // callbacks are ordered to reflect state transitions.

        if (needSyncEvent) {
            // TODO(fix): What should the error code be?
            manager->eventCallback (manager->eventContext,
                                    BRClientSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_SYNC_STOPPED,
                                        { .syncStopped = { -1 }}
                                    });
        }

        if (needConnectionEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRClientSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_DISCONNECTED
                                    });

            manager->eventCallback (manager->eventContext,
                                    BRClientSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_CONNECTED
                                    });
        }
        pthread_mutex_unlock (&manager->lock);
    } else {
        assert (0);
    }

    BRClientSyncManagerUpdateBlockNumber (manager);
    BRClientSyncManagerUpdateTransactions (manager);
}

static void
BRClientSyncManagerSubmit(BRClientSyncManager manager,
                          OwnershipKept BRTransaction *transaction) {
    uint8_t needClientCall = 0;
    int rid                = -1;

    if (0 == pthread_mutex_lock (&manager->lock)) {
        needClientCall = manager->isConnected;
        rid = needClientCall ? BRClientSyncManagerGenerateRid (manager) : rid;

        pthread_mutex_unlock (&manager->lock);
    } else {
        assert (0);
    }

    // Send event WITHOUT holding the state lock as this type of event
    // has no impact on the sync manager's state.

    if (needClientCall) {
        size_t txLength = BRTransactionSerialize (transaction, NULL, 0);
        uint8_t *tx = calloc (txLength, sizeof(uint8_t));
        BRTransactionSerialize(transaction, tx, txLength);

        manager->clientCallbacks.funcSubmitTransaction (manager->clientContext,
                                                        BRClientSyncManagerAsSyncManager (manager),
                                                        tx,
                                                        txLength,
                                                        transaction->txHash,
                                                        rid);

        free (tx);
    } else {
        // TODO(fix): What should the error code be?
        manager->eventCallback (manager->eventContext,
                                BRClientSyncManagerAsSyncManager (manager),
                                (BRSyncManagerEvent) {
                                    SYNC_MANAGER_TXN_SUBMITTED,
                                    { .submitted = {transaction, -1} },
                                });
    }
}

static void
BRClientSyncManagerTickTock(BRClientSyncManager manager) {
    BRClientSyncManagerUpdateBlockNumber (manager);
    BRClientSyncManagerUpdateTransactions (manager);
}

static int
BRClientSyncManagerIsInFullScan(BRClientSyncManager manager) {
    int isFullScan = 0;

    if (0 == pthread_mutex_lock (&manager->lock)) {
        isFullScan = BRClientSyncManagerScanStateIsFullScan (&manager->scanState);
        pthread_mutex_unlock (&manager->lock);
    } else {
        assert (0);
    }
    return isFullScan;
}

static void
BRClientSyncManagerAnnounceGetBlockNumber(BRClientSyncManager manager,
                                          int rid,
                                          uint64_t blockHeight) {
    uint8_t needEvent = 0;

    if (0 == pthread_mutex_lock (&manager->lock)) {
        // Never move the block height "backwards"; always maintain our knowledge
        // of the maximum height observed
        if (blockHeight > manager->networkBlockHeight && manager->isConnected) {
            manager->networkBlockHeight = blockHeight;
            needEvent = 1;
        }

        // Send event while holding the state lock so that we
        // don't broadcast a height updated while disconnected,
        // for example.

        if (needEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRClientSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_BLOCK_HEIGHT_UPDATED,
                                        { .blockHeightUpdated = { blockHeight }}
                                    });
        }

        pthread_mutex_unlock (&manager->lock);
    } else {
        assert (0);
    }
}

static void
BRClientSyncManagerAnnounceSubmitTransaction (BRClientSyncManager manager,
                                              int rid,
                                              OwnershipKept BRTransaction *txn,
                                              int error) {
    // register a copy of the transaction with the wallet if the submission was successful AND
    // the wallet isn't already aware of it
    if (0 == error && NULL == BRWalletTransactionForHash (manager->wallet, txn->txHash)) {
        BRTransaction *transaction = BRTransactionCopy (txn);

        // BRWalletRegisterTransaction doesn't reliably report if the txn was added to the wallet.
        BRWalletRegisterTransaction (manager->wallet, transaction);
        if (BRWalletTransactionForHash (manager->wallet, transaction->txHash) != transaction) {
            // If our transaction did not make it into the wallet, deallocate it
            BRTransactionFree (transaction);
        }
    }

    manager->eventCallback (manager->eventContext,
                            BRClientSyncManagerAsSyncManager (manager),
                            (BRSyncManagerEvent) {
                                SYNC_MANAGER_TXN_SUBMITTED,
                                { .submitted = {txn, error} },
                            });
}

static void
BRClientSyncManagerAnnounceGetTransactionsItem (BRClientSyncManager manager,
                                                int rid,
                                                OwnershipKept uint8_t *txn,
                                                size_t txnLength,
                                                uint64_t timestamp,
                                                uint64_t blockHeight) {
    BRTransaction *transaction = BRTransactionParse (txn, txnLength);
    uint8_t needRegistration = NULL != transaction && BRTransactionIsSigned (transaction);
    if (needRegistration) {
        if (0 == pthread_mutex_lock (&manager->lock)) {
            // confirm completion is for in-progress sync
            needRegistration &= (rid == BRClientSyncManagerScanStateGetRequestId (&manager->scanState) && manager->isConnected);
            pthread_mutex_unlock (&manager->lock);
        } else {
            assert (0);
        }
    }

    if (needRegistration) {
        BRTransaction *walletTxn = BRWalletTransactionForHash (manager->wallet, transaction->txHash);
        if (NULL != walletTxn) {
            // Wallet already knows about this txn; so just update the block info
            BRWalletUpdateTransactions (manager->wallet, &transaction->txHash, 1, (uint32_t) blockHeight, (uint32_t) timestamp);
        } else {
            // Set the transaction's block info according to what has been announced
            transaction->timestamp = (uint32_t) timestamp;
            transaction->blockHeight = (uint32_t) blockHeight;

            // BRWalletRegisterTransaction doesn't reliably report if the txn was added to the wallet.
            BRWalletRegisterTransaction (manager->wallet, transaction);
            if (BRWalletTransactionForHash (manager->wallet, transaction->txHash) == transaction) {
                // If our transaction made it into the wallet, do not deallocate it
                transaction = NULL;
            }
        }
    }

    // Free if ownership hasn't been passed
    if (NULL != transaction) {
        BRTransactionFree (transaction);
    }
}

static void
BRClientSyncManagerAnnounceGetTransactionsDone (BRClientSyncManager manager,
                                                int rid,
                                                int success) {
    uint8_t needSyncEvent        = 0;
    uint8_t needClientCall       = 0;
    uint64_t begBlockNumber      = 0;
    uint64_t endBlockNumber      = 0;
    size_t addressCount          = 0;
    BRArrayOf(char *) addresses  = NULL;
    BRSyncManagerEvent syncEvent = {0};

    if (0 == pthread_mutex_lock (&manager->lock)) {
        // confirm completion is for in-progress sync
        if (rid == BRClientSyncManagerScanStateGetRequestId (&manager->scanState) &&
            manager->isConnected) {
            // check for a successful completion
            if (success) {

                // check if the first unused addresses have changed since last completion
                addresses = BRClientSyncManagerScanStateAdvanceAndGetNewAddresses (&manager->scanState,
                                                                                   manager->wallet);
                addressCount = NULL != addresses ? array_count(addresses) : 0;
                if (0 != addressCount) {
                    // ... we've discovered a new address (i.e. there were transactions announce)

                    // store sync data for callback outside of lock
                    begBlockNumber = BRClientSyncManagerScanStateGetStartBlockNumber (&manager->scanState);
                    endBlockNumber = BRClientSyncManagerScanStateGetEndBlockNumber (&manager->scanState);

                    // store control flow flags
                    needClientCall = 1;

                } else {
                    // .. we haven't discovered any new addresses and we just finished the range

                    // store synced block height
                    manager->syncedBlockHeight = BRClientSyncManagerScanStateGetSyncedBlockNumber (&manager->scanState);;

                    // store control flow flags
                    needSyncEvent = BRClientSyncManagerScanStateIsFullScan (&manager->scanState);
                    syncEvent = (BRSyncManagerEvent) {SYNC_MANAGER_SYNC_STOPPED, { .syncStopped = { 0 }}};

                    // reset sync state
                    BRClientSyncManagerScanStateWipe (&manager->scanState);

                }
            } else {
                // store control flow flags
                needSyncEvent = BRClientSyncManagerScanStateIsFullScan (&manager->scanState);

                // TODO(fix): What should the error code be?
                syncEvent = (BRSyncManagerEvent) {SYNC_MANAGER_SYNC_STOPPED, { .syncStopped = { -1 }}};

                // reset sync state on failure
                BRClientSyncManagerScanStateWipe (&manager->scanState);
            }
        }

        // Send event while holding the state lock so that event
        // callbacks are ordered to reflect state transitions.

        if (needSyncEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRClientSyncManagerAsSyncManager (manager),
                                    syncEvent);
        }

        pthread_mutex_unlock (&manager->lock);
    } else {
        assert (0);
    }

    if (needClientCall) {
        // Callback to 'client' to get all transactions (for all wallet addresses) between
        // a {beg,end}BlockNumber.  The client will gather the transactions and then call
        // bwmAnnounceTransaction()  (for each one or with all of them).
        manager->clientCallbacks.funcGetTransactions (manager->clientContext,
                                                      BRClientSyncManagerAsSyncManager (manager),
                                                      (const char **) addresses,
                                                      addressCount,
                                                      begBlockNumber,
                                                      endBlockNumber,
                                                      rid);
    }

    if (NULL != addresses) {
        for (size_t index = 0; index < addressCount; index++) {
            free (addresses[index]);
        }
        array_free (addresses);
    }
}

static void
BRClientSyncManagerUpdateTransactions (BRClientSyncManager manager) {
    int rid                      = 0;
    uint8_t needSyncEvent        = 0;
    uint8_t needClientCall       = 0;
    uint64_t begBlockNumber      = 0;
    uint64_t endBlockNumber      = 0;
    size_t addressCount          = 0;
    BRArrayOf(char *) addresses  = NULL;

    if (0 == pthread_mutex_lock (&manager->lock)) {
        // check if we are connect and the prior sync has completed.
        if (!BRClientSyncManagerScanStateIsInProgress (&manager->scanState) &&
            manager->isConnected) {

            BRClientSyncManagerScanStateInit (&manager->scanState,
                                              manager->wallet,
                                              manager->syncedBlockHeight,
                                              manager->networkBlockHeight,
                                              BRClientSyncManagerGenerateRid (manager));

            // get the addresses to query the BDB with
            addresses = BRClientSyncManagerScanStateGetAddresses (&manager->scanState);
            assert (NULL != addresses);
            addressCount = array_count (addresses);
            assert (0 != addressCount);

            // store sync data for callback outside of lock
            rid = BRClientSyncManagerScanStateGetRequestId (&manager->scanState);
            begBlockNumber = BRClientSyncManagerScanStateGetStartBlockNumber (&manager->scanState);
            endBlockNumber = BRClientSyncManagerScanStateGetEndBlockNumber (&manager->scanState);

            // store control flow flags
            needSyncEvent = BRClientSyncManagerScanStateIsFullScan (&manager->scanState);
            needClientCall = 1;
        }

        // Send event while holding the state lock so that event
        // callbacks are ordered to reflect state transitions.

        if (needSyncEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRClientSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_SYNC_STARTED,
                                    });
        }

        pthread_mutex_unlock (&manager->lock);
    } else {
        assert (0);
    }

    if (needClientCall) {
        // Callback to 'client' to get all transactions (for all wallet addresses) between
        // a {beg,end}BlockNumber.  The client will gather the transactions and then call
        // bwmAnnounceTransaction()  (for each one or with all of them).
        manager->clientCallbacks.funcGetTransactions (manager->clientContext,
                                                      BRClientSyncManagerAsSyncManager (manager),
                                                      (const char **) addresses,
                                                      addressCount,
                                                      begBlockNumber,
                                                      endBlockNumber,
                                                      rid);
    }

    if (NULL != addresses) {
        for (size_t index = 0; index < addressCount; index++) {
            free (addresses[index]);
        }
        array_free (addresses);
    }
}

static void
BRClientSyncManagerUpdateBlockNumber(BRClientSyncManager manager) {
    uint8_t needClientCall = 0;
    int rid                = -1;

    if (0 == pthread_mutex_lock (&manager->lock)) {
        needClientCall = manager->isConnected;
        rid = needClientCall ? BRClientSyncManagerGenerateRid (manager) : rid;

        pthread_mutex_unlock (&manager->lock);
    } else {
        assert (0);
    }

    if (needClientCall) {
        manager->clientCallbacks.funcGetBlockNumber (manager->clientContext,
                                                     BRClientSyncManagerAsSyncManager (manager),
                                                     rid);
    }
}

static int
BRClientSyncManagerGenerateRid (BRClientSyncManager manager) {
    return ++manager->requestIdGenerator;
}

static void
BRClientSyncManagerScanStateInit (BRClientSyncManagerScanState scanState,
                                  BRWallet *wallet,
                                  uint64_t syncedBlockHeight,
                                  uint64_t networkBlockHeight,
                                  int rid) {
    // update the `endBlockNumber` to the current block height;
    // since this is exclusive on the end height, we need to increment by
    // one to make sure we get the last block
    scanState->endBlockNumber = MAX (syncedBlockHeight, networkBlockHeight) + 1;

    // update the `startBlockNumber` to the last synced height;
    // provide a bit of buffer and request the last X blocks, regardless
    scanState->begBlockNumber = MIN (syncedBlockHeight, (scanState->endBlockNumber >=  BWM_BRD_SYNC_START_BLOCK_OFFSET
                                                         ? scanState->endBlockNumber - BWM_BRD_SYNC_START_BLOCK_OFFSET
                                                         : 0));

    // check that we don't have an overflow
    assert (scanState->endBlockNumber > scanState->begBlockNumber);

    // generate addresses
    BRWalletUnusedAddrs (wallet, NULL, SEQUENCE_GAP_LIMIT_EXTERNAL, 0);
    BRWalletUnusedAddrs (wallet, NULL, SEQUENCE_GAP_LIMIT_INTERNAL, 1);

    // save the last known external and internal addresses
    BRWalletUnusedAddrs(wallet, &scanState->lastExternalAddress, 1, 0);
    BRWalletUnusedAddrs(wallet, &scanState->lastInternalAddress, 1, 1);

    // save the current requestId
    scanState->requestId = rid;

    // mark as sync or not
    scanState->isFullScan = ((scanState->endBlockNumber - scanState->begBlockNumber) > BWM_BRD_SYNC_START_BLOCK_OFFSET);

    // build the set of initial wallet addresses
    assert (NULL == scanState->knownAddresses);
    scanState->knownAddresses = BRSetNew (BRAddressHash, BRAddressEq, SEQUENCE_GAP_LIMIT_INTERNAL + SEQUENCE_GAP_LIMIT_EXTERNAL);
    _fillWalletAddressSet (scanState->knownAddresses, wallet);
}

static void
BRClientSyncManagerScanStateWipe (BRClientSyncManagerScanState scanState) {
    if (NULL != scanState->knownAddresses) {
        BRSetFreeAll (scanState->knownAddresses, free);
    }
    memset (scanState, 0, sizeof(*scanState));
}

static int
BRClientSyncManagerScanStateIsInProgress(BRClientSyncManagerScanState scanState) {
    return 0 != scanState->requestId;
}

static uint8_t
BRClientSyncManagerScanStateIsFullScan (BRClientSyncManagerScanState scanState) {
    return scanState->isFullScan;
}

static int
BRClientSyncManagerScanStateGetRequestId(BRClientSyncManagerScanState scanState) {
    return scanState->requestId;
}

static uint64_t
BRClientSyncManagerScanStateGetStartBlockNumber(BRClientSyncManagerScanState scanState) {
    return scanState->begBlockNumber;
}

static uint64_t
BRClientSyncManagerScanStateGetEndBlockNumber(BRClientSyncManagerScanState scanState) {
    return scanState->endBlockNumber;
}

static uint64_t
BRClientSyncManagerScanStateGetSyncedBlockNumber(BRClientSyncManagerScanState scanState) {
    return scanState->endBlockNumber - 1;
}

static BRArrayOf(char *)
BRClientSyncManagerScanStateGetAddresses(BRClientSyncManagerScanState scanState) {
    size_t addressCount = BRSetCount(scanState->knownAddresses);

    BRArrayOf(BRAddress *) addresses;
    array_new (addresses, addressCount);
    array_set_count (addresses, addressCount);

    size_t index = 0;
    FOR_SET (BRAddress *, knownAddress, scanState->knownAddresses) {
        addresses[index] = malloc (sizeof(BRAddress));
        *addresses[index] = *knownAddress;
        index++;
    }

    return (BRArrayOf(char *)) addresses;
}

static BRArrayOf(char *)
BRClientSyncManagerScanStateAdvanceAndGetNewAddresses (BRClientSyncManagerScanState scanState,
                                                       BRWallet *wallet) {
    BRArrayOf(char *) newAddresses = NULL;

    // generate addresses
    BRWalletUnusedAddrs (wallet, NULL, SEQUENCE_GAP_LIMIT_EXTERNAL, 0);
    BRWalletUnusedAddrs (wallet, NULL, SEQUENCE_GAP_LIMIT_INTERNAL, 1);

    // get the first unused address
    BRAddress externalAddress = BR_ADDRESS_NONE;
    BRAddress internalAddress = BR_ADDRESS_NONE;
    BRWalletUnusedAddrs (wallet, &externalAddress, 1, 0);
    BRWalletUnusedAddrs (wallet, &internalAddress, 1, 1);

    // check if the first unused addresses have changed since last completion
    if (!BRAddressEq (&externalAddress, &scanState->lastExternalAddress) ||
        !BRAddressEq (&internalAddress, &scanState->lastInternalAddress)) {
        // ... we've discovered a new address (i.e. there were transactions announce)
        // so we need to requery the same range including the newly derived addresses

        // store the first unused addresses for comparison in the next complete call
        scanState->lastExternalAddress = externalAddress;
        scanState->lastInternalAddress = internalAddress;

        // get the list of newly discovered addresses
        newAddresses = (BRArrayOf(char *)) _updateWalletAddressSet (scanState->knownAddresses, wallet);
    }

    return newAddresses;
}

static BRAddress *
_getWalletAddresses (BRWallet *wallet,size_t *addressCount) {
    assert (addressCount);

    size_t addrCount = BRWalletAllAddrs (wallet, NULL, 0);

    BRAddress *addrs = (BRAddress *) calloc (2 * addrCount, sizeof (BRAddress));
    BRWalletAllAddrs (wallet, addrs, addrCount);

    memcpy (addrs + addrCount, addrs, addrCount * sizeof(BRAddress));
    for (size_t index = 0; index < addrCount; index++)
        addrs[addrCount + index] = BRWalletAddressToLegacy (wallet, &addrs[index]);

    *addressCount = 2 * addrCount;
    return addrs;
}

static void
_fillWalletAddressSet(BRSetOf(BRAddress *) addresses, BRWallet *wallet) {
    size_t addressCount = 0;
    BRAddress *addressArray = _getWalletAddresses (wallet, &addressCount);

    for (size_t index = 0; index < addressCount; index++) {
        if (!BRSetContains (addresses, &addressArray[index])) {
            BRAddress *address = malloc (sizeof(BRAddress));
            *address = addressArray[index];
            BRSetAdd (addresses, address);
        }
    }

    free (addressArray);
}

static BRArrayOf(BRAddress *)
_updateWalletAddressSet(BRSetOf(BRAddress *) addresses, BRWallet *wallet) {
    size_t addressCount = 0;
    BRAddress *addressArray = _getWalletAddresses (wallet, &addressCount);

    BRArrayOf(BRAddress *) newAddresses;
    array_new (newAddresses, addressCount);

    for (size_t index = 0; index < addressCount; index++) {
        if (!BRSetContains (addresses, &addressArray[index])) {
            // one copy remains owned by the address set
            BRAddress *address = malloc (sizeof(BRAddress));
            *address = addressArray[index];
            BRSetAdd (addresses, address);

            // one copy owned by the returned array
            address = malloc (sizeof(BRAddress));
            *address = addressArray[index];
            array_add (newAddresses, address);
        }
    }

    free (addressArray);
    return newAddresses;
}

/// MARK: - Peer Sync Manager Implementation

static BRPeerSyncManager
BRPeerSyncManagerNew(BRSyncManagerEventContext eventContext,
                     BRSyncManagerEventCallback eventCallback,
                     OwnershipKept const BRChainParams *params,
                     OwnershipKept BRWallet *wallet,
                     uint32_t earliestKeyTime,
                     uint64_t blockHeight,
                     OwnershipKept BRMerkleBlock *blocks[],
                     size_t blocksCount,
                     OwnershipKept const BRPeer peers[],
                     size_t peersCount) {
    BRPeerSyncManager manager = (BRPeerSyncManager) calloc (1, sizeof(struct BRPeerSyncManagerStruct));
    manager->common.mode = SYNC_MODE_P2P_ONLY;

    manager->wallet = wallet;
    manager->eventContext = eventContext;
    manager->eventCallback = eventCallback;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&manager->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    // Find the BRCheckpoint that is at least one week before earliestKeyTime.
    const BRCheckPoint *earliestCheckPoint = BRChainParamsGetCheckpointBefore (params, earliestKeyTime - ONE_WEEK_IN_SECONDS);
    assert (NULL != earliestCheckPoint);

    // The initial sync will be based on the `blocks` provided to the peer manager as the starting
    // point up to the block height advertised on the P2P network, regardless of if we have synced,
    // in API mode for example, to halfway between those two heights. This is due to how the P2P
    // verifies data it receives from the network.
    manager->networkBlockHeight = MAX (earliestCheckPoint->height, blockHeight);
    manager->isConnected = 0;

    manager->peerManager = BRPeerManagerNew (params,
                                             wallet,
                                             earliestKeyTime,
                                             blocks, array_count(blocks),
                                             peers,  array_count(peers));

    BRPeerManagerSetCallbacks (manager->peerManager,
                               manager,
                               _BRPeerSyncManagerSyncStarted,
                               _BRPeerSyncManagerSyncStopped,
                               _BRPeerSyncManagerTxStatusUpdate,
                               _BRPeerSyncManagerSaveBlocks,
                               _BRPeerSyncManagerSavePeers,
                               _BRPeerSyncManagerNetworkIsReachabele,
                               _BRPeerSyncManagerThreadCleanup);

    return manager;
}

static BRPeerSyncManager
BRSyncManagerAsPeerSyncManager(BRSyncManager manager) {
    if (manager->mode == SYNC_MODE_P2P_ONLY) {
        return (BRPeerSyncManager) manager;
    }
    assert (0);
    return NULL;
}

static void
BRPeerSyncManagerFree(BRPeerSyncManager manager) {
    BRPeerManagerDisconnect (manager->peerManager);
    BRPeerManagerFree (manager->peerManager);

    pthread_mutex_destroy(&manager->lock);
    memset (manager, 0, sizeof(*manager));
    free (manager);
}

static uint64_t
BRPeerSyncManagerGetBlockHeight(BRPeerSyncManager manager) {
    uint64_t blockHeight = 0;
    if (0 == pthread_mutex_lock (&manager->lock)) {
        blockHeight = manager->networkBlockHeight;
        pthread_mutex_unlock (&manager->lock);
    }
    return blockHeight;
}

static void
BRPeerSyncManagerConnect(BRPeerSyncManager manager) {
    BRPeerManagerConnect (manager->peerManager);
}

static void
BRPeerSyncManagerDisconnect(BRPeerSyncManager manager) {
    BRPeerManagerDisconnect (manager->peerManager);
}

static void
BRPeerSyncManagerScan(BRPeerSyncManager manager) {
    BRPeerSyncManagerScanToDepth (manager, SYNC_DEPTH_HIGH);
}

static void
BRPeerSyncManagerScanToDepth(BRPeerSyncManager manager, BRSyncDepth depth) {
    switch (depth) {
        case SYNC_DEPTH_LOW: {
            uint32_t scanHeight = _getLastConfirmedSendTxHeight (manager->wallet, BRPeerManagerLastBlockHeight (manager->peerManager));
            if (0 != scanHeight) {
                BRPeerManagerRescanFromBlockNumber (manager->peerManager, scanHeight);
            } else {
                BRPeerManagerRescan (manager->peerManager);
            }
            break;
        }
        case SYNC_DEPTH_MEDIUM: {
            BRPeerManagerRescanFromLastHardcodedCheckpoint (manager->peerManager);
            break;
        }
        case SYNC_DEPTH_HIGH: {
            BRPeerManagerRescan (manager->peerManager);
            break;
        }
    }
}

typedef struct {
    BRPeerSyncManager manager;
    BRTransaction *transaction;
} SubmitTransactionInfo;

static void
BRPeerSyncManagerSubmit(BRPeerSyncManager manager,
                        OwnershipKept BRTransaction *transaction) {
    SubmitTransactionInfo *info = malloc (sizeof (SubmitTransactionInfo));
    info->manager = manager;
    info->transaction = transaction;

    // create a copy to hand to the wallet as once that is done, ownership is lost
    transaction = BRTransactionCopy (transaction);
    BRPeerManagerPublishTx (manager->peerManager,
                            transaction,
                            info,
                            _BRPeerSyncManagerTxPublished);
}

static void
BRPeerSyncManagerTickTock(BRPeerSyncManager manager) {
    BRSyncPercentComplete progressComplete  = AS_SYNC_PERCENT_COMPLETE (100.0 * BRPeerManagerSyncProgress (manager->peerManager, 0));
    BRSyncTimestamp       progressTimestamp = AS_SYNC_TIMESTAMP (BRPeerManagerLastBlockTimestamp(manager->peerManager));

    uint8_t needSyncEvent = progressComplete > 0.0 && progressComplete < 100.0;
    if (needSyncEvent) {
        if (0 == pthread_mutex_lock (&manager->lock)) {
            needSyncEvent &= manager->isConnected && manager->isFullScan;

            // Send event while holding the state lock so that we
            // don't broadcast a progress updated after a disconnected
            // event, for example.

            if (needSyncEvent) {
                manager->eventCallback (manager->eventContext,
                                        BRPeerSyncManagerAsSyncManager (manager),
                                        (BRSyncManagerEvent) {
                                            SYNC_MANAGER_SYNC_PROGRESS,
                                            { .syncProgress = {
                                                progressTimestamp,
                                                progressComplete }}
                                        });
            }

            pthread_mutex_unlock (&manager->lock);
        } else {
            assert (0);
        }
    }
}

static int
BRPeerSyncManagerIsInFullScan(BRPeerSyncManager manager) {
    int isInFullScan = 0;
    if (0 == pthread_mutex_lock (&manager->lock)) {
        isInFullScan = manager->isFullScan;
        pthread_mutex_unlock (&manager->lock);
    } else {
        assert (0);
    }
    return isInFullScan;
}

/// MARK: - Peer Manager Callbacks

static void
_BRPeerSyncManagerSaveBlocks (void *info,
                              int replace,
                              OwnershipKept BRMerkleBlock **blocks,
                              size_t count) {
    BRPeerSyncManager manager = (BRPeerSyncManager) info;
    manager->eventCallback (manager->eventContext,
                            BRPeerSyncManagerAsSyncManager (manager),
                            (BRSyncManagerEvent) {
                                replace ? SYNC_MANAGER_SET_BLOCKS : SYNC_MANAGER_ADD_BLOCKS,
                                {
                                    .blocks = { blocks, count }
                                }
                            });
}

static void
_BRPeerSyncManagerSavePeers  (void *info,
                              int replace,
                              OwnershipKept const BRPeer *peers,
                              size_t count) {
    BRPeerSyncManager manager = (BRPeerSyncManager) info;
    manager->eventCallback (manager->eventContext,
                            BRPeerSyncManagerAsSyncManager (manager),
                            (BRSyncManagerEvent) {
                                replace ? SYNC_MANAGER_SET_PEERS : SYNC_MANAGER_ADD_PEERS,
                                {
                                    .peers = { (BRPeer *) peers, count }
                                }
                            });
}

static void
_BRPeerSyncManagerSyncStarted (void *info) {
    BRPeerSyncManager manager = (BRPeerSyncManager) info;

    // This callback occurs when a sync has started. The behaviour of this function is
    // defined as:
    //   - If we are not in a connected state, signal that we are now connected.
    //   - If we were already in a (full scan) syncing state, signal the termination of that
    //     sync
    //   - Always signal the start of a sync

    if (0 == pthread_mutex_lock (&manager->lock)) {
        uint8_t needConnectionEvent = !manager->isConnected;
        uint8_t needSyncStartedEvent = 1; // syncStarted callback always indicates a full scan
        uint8_t needSyncStoppedEvent = manager->isFullScan;

        manager->isConnected = needConnectionEvent ? 1 : manager->isConnected;
        manager->isFullScan = needSyncStartedEvent ? 1 : manager->isFullScan;

        _peer_log ("syncStarted: needConnect:%"PRIu8", needStart:%"PRIu8", needStop:%"PRIu8"\n",
                   needConnectionEvent, needSyncStartedEvent, needSyncStoppedEvent);

        // Send event while holding the state lock so that we
        // don't broadcast a events out of order.

        if (needSyncStoppedEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRPeerSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_SYNC_STOPPED,
                                        { .syncStopped = { -1 }},
                                    });
        }

        if (needConnectionEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRPeerSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_CONNECTED,
                                    });
        }

        if (needSyncStartedEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRPeerSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_SYNC_STARTED,
                                    });
        }

        pthread_mutex_unlock (&manager->lock);
    } else {
        assert (0);
    }
}

static void
_BRPeerSyncManagerSyncStopped (void *info, int reason) {
    BRPeerSyncManager manager = (BRPeerSyncManager) info;

    // This callback occurs when a sync has stopped. This MAY mean we have disconnected or it
    // may mean that we have "caught up" to the blockchain. So, we need to first get the connectivity
    // state of the `BRPeerManager`. The behaviour of this function is defined as:
    //   - If we were in a (full scan) syncing state, signal the termination of that
    //     sync
    //   - If we were connected and are now disconnected, signal that we are now disconnected.

    if (0 == pthread_mutex_lock (&manager->lock)) {
        uint8_t isConnected = BRPeerStatusDisconnected != BRPeerManagerConnectStatus (manager->peerManager);

        uint8_t needSyncStoppedEvent = manager->isFullScan;
        uint8_t needDisconnectionEvent = !isConnected && manager->isConnected;

        manager->isConnected = needDisconnectionEvent ? 0 : isConnected;
        manager->isFullScan = needSyncStoppedEvent ? 0 : manager->isFullScan;

        _peer_log ("syncStopped: needStop:%"PRIu8", needDisconnect:%"PRIu8"\n",
                   needSyncStoppedEvent, needDisconnectionEvent);

        // Send event while holding the state lock so that we
        // don't broadcast a events out of order.

        if (needSyncStoppedEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRPeerSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_SYNC_STOPPED,
                                        { .syncStopped = { reason }},
                                    });
        }

        if (needDisconnectionEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRPeerSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_DISCONNECTED,
                                    });
        }
        pthread_mutex_unlock (&manager->lock);
    } else {
        assert (0);
    }
}

static void
_BRPeerSyncManagerTxStatusUpdate (void *info) {
    BRPeerSyncManager manager = (BRPeerSyncManager) info;

    // This callback occurs under a number of scenarios.
    //
    // One of those scenarios is when a peer has disconnected. Thus, it provides an opportunity
    // to check if we the `BRPeerManager` is in the disconnected state as it has been observed
    // that the `syncStopped` callback is not always called by the BRPeerManager` when this happens.
    //
    // Another scenario is when a block has been related by the P2P network. Thus, it provides an
    // opportunity to get the current block height and update accordingly.
    //
    // The behaviour of this function is defined as:
    //   - If we were connected and are now disconnected, signal that we are now disconnected.
    //   - If we were in a (full scan) syncing state and are now disconnected, signal the termination of that
    //     sync
    //   - If the block height has changed, signal the new value

    if (0 == pthread_mutex_lock (&manager->lock)) {
        uint8_t isConnected = BRPeerStatusDisconnected != BRPeerManagerConnectStatus (manager->peerManager);
        uint64_t blockHeight = BRPeerManagerLastBlockHeight (manager->peerManager);

        uint8_t needSyncStoppedEvent = !isConnected && manager->isConnected && manager->isFullScan;
        uint8_t needDisconnectionEvent = !isConnected && manager->isConnected;
        uint8_t needBlockHeightEvent = blockHeight > manager->networkBlockHeight;

        manager->isConnected = needDisconnectionEvent ? 0 : manager->isConnected;
        manager->isFullScan = needSyncStoppedEvent ? 0 : manager->isFullScan;

        // Never move the block height "backwards"; always maintain our knowledge
        // of the maximum height observed
        manager->networkBlockHeight = MAX (blockHeight, manager->networkBlockHeight);

        _peer_log ("txStatusUpdate: needStop:%"PRIu8", needDisconnect:%"PRIu8"\n",
                   needSyncStoppedEvent, needDisconnectionEvent);

        // Send event while holding the state lock so that we
        // don't broadcast a events out of order.

        if (needBlockHeightEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRPeerSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_BLOCK_HEIGHT_UPDATED,
                                        { .blockHeightUpdated = { blockHeight }}
                                    });
        }

        if (needSyncStoppedEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRPeerSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_SYNC_STOPPED,
                                    });
        }

        if (needDisconnectionEvent) {
            manager->eventCallback (manager->eventContext,
                                    BRPeerSyncManagerAsSyncManager (manager),
                                    (BRSyncManagerEvent) {
                                        SYNC_MANAGER_DISCONNECTED,
                                    });
        }

        manager->eventCallback (manager->eventContext,
                                BRPeerSyncManagerAsSyncManager (manager),
                                (BRSyncManagerEvent) {
                                    SYNC_MANAGER_TXNS_UPDATED
                                });

        pthread_mutex_unlock (&manager->lock);
    } else {
        assert (0);
    }
}

static int
_BRPeerSyncManagerNetworkIsReachabele (void *info) {
    return 1;
}

static void
_BRPeerSyncManagerThreadCleanup (void *info) {
}

static void
_BRPeerSyncManagerTxPublished (void *info,
                               int error) {
    BRPeerSyncManager manager    = ((SubmitTransactionInfo*) info)->manager;
    BRTransaction *transaction = ((SubmitTransactionInfo*) info)->transaction;
    free (info);

    manager->eventCallback (manager->eventContext,
                            BRPeerSyncManagerAsSyncManager (manager),
                            (BRSyncManagerEvent) {
                                SYNC_MANAGER_TXN_SUBMITTED,
                                { .submitted = {transaction, error} },
                            });
}
