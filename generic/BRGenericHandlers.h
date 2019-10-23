//
//  BRGenericHandlers.h
//  Core
//
//  Created by Ed Gamble on 6/19/19.
//  Copyright © 2019 Breadwinner AG. All rights reserved.
//
//  See the LICENSE file at the project root for license information.
//  See the CONTRIBUTORS file at the project root for a list of contributors.

#ifndef BRGenericHandlers_h
#define BRGenericHandlers_h

#include "support/BRInt.h" // UInt256
#include "support/BRArray.h"
#include "support/BRKey.h"
#include "support/BRFileService.h"
#include "BRGenericBase.h"

#ifdef __cplusplus
extern "C" {
#endif

    ///
    /// These BRGeneric*Ref types are coerable to any generic implementation; so for example,
    ///    BRGenericAddressRef <==> BRRippleAddress.
    /// We could instead use `void*` but that would disable compiler type checking.  So, we'll use
    /// a `struct`, see the compiler warnings and then explicitly coerce.
    ///
    typedef struct BRGenericNetworkRefRecord  *BRGenericNetworkRef;
    typedef struct BRGenericAccountRefRecord  *BRGenericAccountRef;
    typedef struct BRGenericAddressRefRecord  *BRGenericAddressRef;
    typedef struct BRGenericTransferRefRecord *BRGenericTransferRef;
    typedef struct BRGenericWalletRefRecord   *BRGenericWalletRef;

    // MARK: - Generic Network

    typedef struct {
    } BRGenericNetworkHandlers;


    // MARK: - Generic Account

    typedef BRGenericAccountRef (*BRGenericAccountCreate) (const char *type, UInt512 seed);
    typedef BRGenericAccountRef (*BRGenericAccountCreateWithPublicKey) (const char *type, BRKey key);
    typedef BRGenericAccountRef (*BRGenericAccountCreateWithSerialization) (const char *type, uint8_t *bytes, size_t bytesCount);
    typedef void (*BRGenericAccountFree) (BRGenericAccountRef account);
    typedef BRGenericAddressRef (*BRGenericAccountGetAddress) (BRGenericAccountRef account);
    typedef uint8_t * (*BRGenericAccountGetSerialization) (BRGenericAccountRef account, size_t *bytesCount);
    typedef void (*BRGenericAccountSignTransferWithSeed) (BRGenericAccountRef account, BRGenericTransferRef transfer, UInt512 seed);
    typedef void (*BRGenericAccountSignTransferWithKey) (BRGenericAccountRef account, BRGenericTransferRef transfer, BRKey *key);

    typedef struct {
        BRGenericAccountCreate create;
        BRGenericAccountCreateWithPublicKey createWithPublicKey;
        BRGenericAccountCreateWithSerialization createWithSerialization;
        BRGenericAccountFree free;
        BRGenericAccountGetAddress getAddress;
        BRGenericAccountGetSerialization getSerialization;
        BRGenericAccountSignTransferWithSeed signTransferWithSeed;
        BRGenericAccountSignTransferWithKey signTransferWithKey;
    } BRGenericAccountHandlers;

    // MARK: - Generic Address

    typedef BRGenericAddressRef (*BRGenericAddressCreate) (const char *string);
    typedef char * (*BRGenericAddressAsString) (BRGenericAddressRef address);
    typedef int (*BRGenericAddressEqual) (BRGenericAddressRef address1,
                                          BRGenericAddressRef address2);
    typedef void (*BRGenericAddressFree) (BRGenericAddressRef address);

    typedef struct {
        BRGenericAddressCreate create;
        BRGenericAddressAsString asString;
        BRGenericAddressEqual equal;
        BRGenericAddressFree free;
    } BRGenericAddressHandlers;

    // MARK: - Generic Transfer

    typedef BRGenericTransferRef (*BRGenericTransferCreate) (BRGenericAddressRef source,
                                                             BRGenericAddressRef target,
                                                             UInt256 amount);
    typedef void (*BRGenericTransferFree) (BRGenericTransferRef transfer);
    typedef BRGenericAddressRef (*BRGenericTransferGetSourceAddress) (BRGenericTransferRef transfer);
    typedef BRGenericAddressRef (*BRGenericTransferGetTargetAddress) (BRGenericTransferRef transfer);
    typedef UInt256 (*BRGenericTransferGetAmount) (BRGenericTransferRef transfer);
    typedef UInt256 (*BRGenericTransferGetFee) (BRGenericTransferRef transfer);
    typedef BRGenericFeeBasis (*BRGenericTransferGetFeeBasis) (BRGenericTransferRef transfer);
    typedef BRGenericTransferDirection (*BRGenericTransferGetDirection) (BRGenericTransferRef transfer);
    typedef BRGenericHash (*BRGenericTransferGetHash) (BRGenericTransferRef transfer);
    typedef uint8_t * (*BRGenericTransferGetSerialization) (BRGenericTransferRef transfer, size_t *bytesCount);

    typedef struct {
        BRGenericTransferCreate create;
        BRGenericTransferFree   free;
        BRGenericTransferGetSourceAddress sourceAddress;
        BRGenericTransferGetTargetAddress targetAddress;
        BRGenericTransferGetAmount amount;
        BRGenericTransferGetFee fee;
        BRGenericTransferGetFeeBasis feeBasis;
        BRGenericTransferGetDirection direction;
        BRGenericTransferGetHash hash;
        BRGenericTransferGetSerialization getSerialization;
    } BRGenericTransferHandlers;

    // MARK: - Generic Wallet

    typedef BRGenericWalletRef (*BRGenericWalletCreate) (BRGenericAccountRef account);
    typedef void (*BRGenericWalletFree) (BRGenericWalletRef wallet);
    typedef UInt256 (*BRGenericWalletGetBalance) (BRGenericWalletRef wallet);
    typedef int (*BRGenericWalletHasAddress) (BRGenericWalletRef wallet,
                                              BRGenericAddressRef address);

    // Unneeded?
    typedef BRGenericTransferRef (*BRGenericWalletCreateTransfer) (BRGenericWalletRef wallet,
                                                                   BRGenericAddressRef target,
                                                                   UInt256 amount,
                                                                   BRGenericFeeBasis estimatedFeeBasis);
    typedef BRGenericFeeBasis (*BRGenericWalletEstimateFeeBasis) (BRGenericWalletRef wallet,
                                                                  BRGenericAddressRef address,
                                                                  UInt256 amount,
                                                                  UInt256 pricePerCostFactor);

    typedef struct {
        BRGenericWalletCreate create;
        BRGenericWalletFree free;
        BRGenericWalletGetBalance balance;
        // set balance
        BRGenericWalletHasAddress hasAddress;
        // ...
        BRGenericWalletCreateTransfer createTransfer; // Unneeded.
        BRGenericWalletEstimateFeeBasis estimateFeeBasis;
    } BRGenericWalletHandlers;

    // MARK: - Generic (Wallet) Manager

    // Create a transfer from the
    typedef BRGenericTransferRef (*BRGenericWalletManagerRecoverTransfer) (const char *hash,
                                                                           const char *from,
                                                                           const char *to,
                                                                           const char *amount,
                                                                           const char *currency,
                                                                           uint64_t timestamp,
                                                                           uint64_t blockHeight);

    typedef BRArrayOf(BRGenericTransferRef) (*BRGenericWalletManagerRecoverTransfersFromRawTransaction) (uint8_t *bytes,
                                                                                                         size_t   bytesCount);

    typedef void (*BRGenericWalletManagerInitializeFileService) (BRFileServiceContext context,
                                                                 BRFileService fileService);

    typedef BRArrayOf(BRGenericTransferRef) (*BRGenericWalletManagerLoadTransfers) (BRFileServiceContext context,
                                                                                    BRFileService fileService);

    typedef BRGenericAPISyncType (*BRGenericWalletManagerGetAPISyncType) (void);

    typedef struct {
        BRGenericWalletManagerRecoverTransfer transferRecover;
        BRGenericWalletManagerRecoverTransfersFromRawTransaction transfersRecoverFromRawTransaction;
        BRGenericWalletManagerInitializeFileService fileServiceInit;
        BRGenericWalletManagerLoadTransfers fileServiceLoadTransfers;
        BRGenericWalletManagerGetAPISyncType apiSyncType;
    } BRGenericManagerHandlers;

    // MARK: - Generic Handlers

    typedef struct BRGenericHandersRecord {
        const char *type;
        BRGenericNetworkHandlers network;
        BRGenericAccountHandlers account;
        BRGenericAddressHandlers address;
        BRGenericTransferHandlers transfer;
        BRGenericWalletHandlers wallet;
        BRGenericManagerHandlers manager;
    } *BRGenericHandlers;

    extern void
    genHandlersInstall (const BRGenericHandlers handlers);

    extern const BRGenericHandlers
    genHandlerLookup (const char *symbol);

#ifdef __cplusplus
}
#endif

#endif /* BRGenericHandlers_h */
