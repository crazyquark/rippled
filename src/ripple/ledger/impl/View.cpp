//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <BeastConfig.h>
#include <ripple/ledger/View.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/protocol/Quality.h>

namespace ripple {

// VFALCO NOTE A copy of the other one for now
/** Maximum number of entries in a directory page
    A change would be protocol-breaking.
*/
#ifndef DIR_NODE_MAX
#define DIR_NODE_MAX  32
#endif

//------------------------------------------------------------------------------
//
// Observers
//
//------------------------------------------------------------------------------

Fees::Fees (BasicView const& view, Config const& config)
    : base_(config.FEE_DEFAULT)
    , units_(config.TRANSACTION_FEE_BASE)
    , reserve_(config.FEE_ACCOUNT_RESERVE)
    , increment_(config.FEE_OWNER_RESERVE)
{
    auto const sle = view.read(keylet::fees());
    if (sle)
    {
        // VFALCO NOTE Why getFieldIndex and not isFieldPresent?
        if (sle->getFieldIndex (sfBaseFee) != -1)
            base_ = sle->getFieldU64 (sfBaseFee);

        if (sle->getFieldIndex (sfReferenceFeeUnits) != -1)
            units_ = sle->getFieldU32 (sfReferenceFeeUnits);

        if (sle->getFieldIndex (sfReserveBase) != -1)
            reserve_ = sle->getFieldU32 (sfReserveBase);

        if (sle->getFieldIndex (sfReserveIncrement) != -1)
            increment_ = sle->getFieldU32 (sfReserveIncrement);
    }
}

//------------------------------------------------------------------------------

bool
isGlobalFrozen (BasicView const& view,
    AccountID const& issuer)
{
    // VFALCO Perhaps this should assert
    if (isXRP (issuer))
        return false;
    auto const sle =
        view.read(keylet::account(issuer));
    if (sle && sle->isFlag (lsfGlobalFreeze))
        return true;
    return false;
}

// Can the specified account spend the specified currency issued by
// the specified issuer or does the freeze flag prohibit it?
static
bool
isFrozen (BasicView const& view, AccountID const& account,
    Currency const& currency, AccountID const& issuer)
{
    if (isXRP (currency))
        return false;
    auto sle =
        view.read(keylet::account(issuer));
    if (sle && sle->isFlag (lsfGlobalFreeze))
        return true;
    if (issuer != account)
    {
        // Check if the issuer froze the line
        sle = view.read(keylet::line(
            account, issuer, currency));
        if (sle && sle->isFlag(
            (issuer > account) ?
                lsfHighFreeze : lsfLowFreeze))
            return true;
    }
    return false;
}

STAmount
accountHolds (BasicView const& view,
    AccountID const& account, Currency const& currency,
        AccountID const& issuer, FreezeHandling zeroIfFrozen,
            Config const& config)
{
    STAmount amount;
    if (isXRP(currency))
    {
        Fees const fees{view, config};
        // XRP: return balance minus reserve
        auto const sle = view.read(
            keylet::account(account));
        auto const reserve = STAmount{fees.reserve(
            sle->getFieldU32(sfOwnerCount))};
        auto const balance =
            sle->getFieldAmount(sfBalance);
        if (balance < reserve)
            amount.clear ();
        else
            amount = balance - reserve;
        WriteLog (lsTRACE, View) << "accountHolds:" <<
            " account=" << to_string (account) <<
            " amount=" << amount.getFullText () <<
            " balance=" << balance.getFullText () <<
            " reserve=" << reserve.getFullText ();
    }
    else
    {
        // IOU: Return balance on trust line modulo freeze
        auto const sle = view.read(keylet::line(
            account, issuer, currency));
        if (! sle)
        {
            amount.clear ({currency, issuer});
        }
        else if ((zeroIfFrozen == fhZERO_IF_FROZEN) &&
            isFrozen(view, account, currency, issuer))
        {
            amount.clear (IssueRef (currency, issuer));
        }
        else
        {
            amount = sle->getFieldAmount (sfBalance);
            if (account > issuer)
            {
                // Put balance in account terms.
                amount.negate();
            }
            amount.setIssuer (issuer);
        }
        WriteLog (lsTRACE, View) << "accountHolds:" <<
            " account=" << to_string (account) <<
            " amount=" << amount.getFullText ();
    }

    return view.balanceHook(
        account, issuer, amount);
}

STAmount
accountFunds (BasicView const& view, AccountID const& id,
    STAmount const& saDefault, FreezeHandling freezeHandling,
        Config const& config)
{
    STAmount saFunds;

    if (!saDefault.native () &&
        saDefault.getIssuer () == id)
    {
        saFunds = saDefault;
        WriteLog (lsTRACE, View) << "accountFunds:" <<
            " account=" << to_string (id) <<
            " saDefault=" << saDefault.getFullText () <<
            " SELF-FUNDED";
    }
    else
    {
        saFunds = accountHolds(view, id,
            saDefault.getCurrency(), saDefault.getIssuer(),
                freezeHandling, config);
        WriteLog (lsTRACE, View) << "accountFunds:" <<
            " account=" << to_string (id) <<
            " saDefault=" << saDefault.getFullText () <<
            " saFunds=" << saFunds.getFullText ();
    }
    return saFunds;
}

void
forEachItem (BasicView const& view, AccountID const& id,
    std::function<void(std::shared_ptr<SLE const> const&)> f)
{
    auto const root = keylet::ownerDir(id);
    auto pos = root;
    for(;;)
    {
        auto sle = view.read(pos);
        if (! sle)
            return;
        // VFALCO NOTE We aren't checking field exists?
        for (auto const& key : sle->getFieldV256(sfIndexes))
            f(view.read(keylet::child(key)));
        auto const next =
            sle->getFieldU64 (sfIndexNext);
        if (! next)
            return;
        pos = keylet::page(root, next);
    }
}

bool
forEachItemAfter (BasicView const& view, AccountID const& id,
    uint256 const& after, std::uint64_t const hint,
        unsigned int limit, std::function<
            bool (std::shared_ptr<SLE const> const&)> f)
{
    auto const rootIndex = keylet::ownerDir(id);
    auto currentIndex = rootIndex;

    // If startAfter is not zero try jumping to that page using the hint
    if (after.isNonZero ())
    {
        auto const hintIndex = keylet::page(rootIndex, hint);
        auto hintDir = view.read(hintIndex);
        if (hintDir)
        {
            for (auto const& key : hintDir->getFieldV256 (sfIndexes))
            {
                if (key == after)
                {
                    // We found the hint, we can start here
                    currentIndex = hintIndex;
                    break;
                }
            }
        }

        bool found = false;
        for (;;)
        {
            auto const ownerDir = view.read(currentIndex);
            if (! ownerDir)
                return found;
            for (auto const& key : ownerDir->getFieldV256 (sfIndexes))
            {
                if (! found)
                {
                    if (key == after)
                        found = true;
                }
                else if (f (view.read(keylet::child(key))) && limit-- <= 1)
                {
                    return found;
                }
            }

            auto const uNodeNext =
                ownerDir->getFieldU64(sfIndexNext);
            if (uNodeNext == 0)
                return found;
            currentIndex = keylet::page(rootIndex, uNodeNext);
        }
    }
    else
    {
        for (;;)
        {
            auto const ownerDir = view.read(currentIndex);
            if (! ownerDir)
                return true;
            for (auto const& key : ownerDir->getFieldV256 (sfIndexes))
                if (f (view.read(keylet::child(key))) && limit-- <= 1)
                    return true;
            auto const uNodeNext =
                ownerDir->getFieldU64 (sfIndexNext);
            if (uNodeNext == 0)
                return true;
            currentIndex = keylet::page(rootIndex, uNodeNext);
        }
    }
}

std::uint32_t
rippleTransferRate (BasicView const& view,
    AccountID const& issuer)
{
    auto const sle = view.read(keylet::account(issuer));
    std::uint32_t quality;
    if (sle && sle->isFieldPresent (sfTransferRate))
        quality = sle->getFieldU32 (sfTransferRate);
    else
        quality = QUALITY_ONE;
    return quality;
}

std::uint32_t
rippleTransferRate (BasicView const& view,
    AccountID const& uSenderID,
        AccountID const& uReceiverID,
            AccountID const& issuer)
{
    // If calculating the transfer rate from
    // or to the issuer of the currency no
    // fees are assessed.
    return (uSenderID == issuer || uReceiverID == issuer)
           ? QUALITY_ONE
           : rippleTransferRate(view, issuer);
}

bool
dirIsEmpty (BasicView const& view,
    Keylet const& k)
{
    auto const sleNode = view.read(k);
    if (! sleNode)
        return true;
    if (! sleNode->getFieldV256 (sfIndexes).empty ())
        return false;
    // If there's another page, it must be non-empty
    return sleNode->getFieldU64 (sfIndexNext) == 0;
}

bool
cdirFirst (BasicView const& view,
    uint256 const& uRootIndex,  // --> Root of directory.
    std::shared_ptr<SLE const>& sleNode,      // <-> current node
    unsigned int& uDirEntry,    // <-- next entry
    uint256& uEntryIndex)       // <-- The entry, if available. Otherwise, zero.
{
    sleNode = view.read(keylet::page(uRootIndex));
    uDirEntry   = 0;
    assert (sleNode);           // Never probe for directories.
    return cdirNext (view, uRootIndex, sleNode, uDirEntry, uEntryIndex);
}

bool 
cdirNext (BasicView const& view,
    uint256 const& uRootIndex,  // --> Root of directory
    std::shared_ptr<SLE const>& sleNode,      // <-> current node
    unsigned int& uDirEntry,    // <-> next entry
    uint256& uEntryIndex)       // <-- The entry, if available. Otherwise, zero.
{
    auto const& svIndexes = sleNode->getFieldV256 (sfIndexes);
    assert (uDirEntry <= svIndexes.size ());
    if (uDirEntry >= svIndexes.size ())
    {
        auto const uNodeNext =
            sleNode->getFieldU64 (sfIndexNext);
        if (! uNodeNext)
        {
            uEntryIndex.zero ();
            return false;
        }
        auto const sleNext = view.read(
            keylet::page(uRootIndex, uNodeNext));
        uDirEntry = 0;
        if (!sleNext)
        {
            // This should never happen
            WriteLog (lsFATAL, View)
                    << "Corrupt directory: index:"
                    << uRootIndex << " next:" << uNodeNext;
            return false;
        }
        sleNode = sleNext;
        return cdirNext (view, uRootIndex,
            sleNode, uDirEntry, uEntryIndex);
    }
    uEntryIndex = svIndexes[uDirEntry++];
    WriteLog (lsTRACE, View) << "dirNext:" <<
        " uDirEntry=" << uDirEntry <<
        " uEntryIndex=" << uEntryIndex;
    return true;
}

//------------------------------------------------------------------------------
//
// Modifiers
//
//------------------------------------------------------------------------------

void
adjustOwnerCount (View& view,
    std::shared_ptr<SLE> const& sle,
        int amount)
{
    assert(amount != 0);
    auto const current =
        sle->getFieldU32 (sfOwnerCount);
    auto adjusted = current + amount;
    if (amount > 0)
    {
        // Overflow is well defined on unsigned
        if (adjusted < current)
        {
            WriteLog (lsFATAL, View) <<
                "Account " << sle->getAccountID(sfAccount) <<
                " owner count exceeds max!";
            adjusted =
                std::numeric_limits<std::uint32_t>::max ();
        }
    }
    else
    {
        // Underflow is well defined on unsigned
        if (adjusted > current)
        {
            WriteLog (lsFATAL, View) <<
                "Account " << sle->getAccountID (sfAccount) <<
                " owner count set below 0!";
            adjusted = 0;
            assert(false);
        }
    }
    sle->setFieldU32 (sfOwnerCount, adjusted);
    view.update(sle);
}

bool
dirFirst (View& view,
    uint256 const& uRootIndex,  // --> Root of directory.
    std::shared_ptr<SLE>& sleNode,      // <-> current node
    unsigned int& uDirEntry,    // <-- next entry
    uint256& uEntryIndex)       // <-- The entry, if available. Otherwise, zero.
{
    sleNode = view.peek(keylet::page(uRootIndex));
    uDirEntry   = 0;
    assert (sleNode);           // Never probe for directories.
    return dirNext (view, uRootIndex, sleNode, uDirEntry, uEntryIndex);
}

bool 
dirNext (View& view,
    uint256 const& uRootIndex,  // --> Root of directory
    std::shared_ptr<SLE>& sleNode,      // <-> current node
    unsigned int& uDirEntry,    // <-> next entry
    uint256& uEntryIndex)       // <-- The entry, if available. Otherwise, zero.
{
    auto const& svIndexes = sleNode->getFieldV256 (sfIndexes);
    assert (uDirEntry <= svIndexes.size ());
    if (uDirEntry >= svIndexes.size ())
    {
        auto const uNodeNext =
            sleNode->getFieldU64 (sfIndexNext);
        if (! uNodeNext)
        {
            uEntryIndex.zero ();
            return false;
        }
        auto const sleNext = view.peek(
            keylet::page(uRootIndex, uNodeNext));
        uDirEntry = 0;
        if (!sleNext)
        {
            // This should never happen
            WriteLog (lsFATAL, View)
                    << "Corrupt directory: index:"
                    << uRootIndex << " next:" << uNodeNext;
            return false;
        }
        sleNode = sleNext;
        return dirNext (view, uRootIndex,
            sleNode, uDirEntry, uEntryIndex);
    }
    uEntryIndex = svIndexes[uDirEntry++];
    WriteLog (lsTRACE, View) << "dirNext:" <<
        " uDirEntry=" << uDirEntry <<
        " uEntryIndex=" << uEntryIndex;
    return true;
}

TER
dirAdd (View& view,
    std::uint64_t&                          uNodeDir,
    uint256 const&                          uRootIndex, // VFALCO Should be Keylet
    uint256 const&                          uLedgerIndex,
    std::function<void (SLE::ref, bool)>    fDescriber)
{
    WriteLog (lsTRACE, View) << "dirAdd:" <<
        " uRootIndex=" << to_string (uRootIndex) <<
        " uLedgerIndex=" << to_string (uLedgerIndex);

    SLE::pointer        sleNode;
    STVector256         svIndexes;
    auto const k = keylet::page(uRootIndex);
    auto sleRoot = view.peek(k);

    if (! sleRoot)
    {
        // No root, make it.
        sleRoot = std::make_shared<SLE>(k);
        sleRoot->setFieldH256 (sfRootIndex, uRootIndex);
        view.insert (sleRoot);
        fDescriber (sleRoot, true);
        sleNode     = sleRoot;
        uNodeDir    = 0;
    }
    else
    {
        uNodeDir = sleRoot->getFieldU64 (sfIndexPrevious);       // Get index to last directory node.

        if (uNodeDir)
        {
            // Try adding to last node.
            sleNode = view.peek (keylet::page(uRootIndex, uNodeDir));

            assert (sleNode);
        }
        else
        {
            // Try adding to root.  Didn't have a previous set to the last node.
            sleNode     = sleRoot;
        }

        svIndexes = sleNode->getFieldV256 (sfIndexes);

        if (DIR_NODE_MAX != svIndexes.size ())
        {
            // Add to current node.
            view.update(sleNode);
        }
        // Add to new node.
        else if (!++uNodeDir)
        {
            return tecDIR_FULL;
        }
        else
        {
            // Have old last point to new node
            sleNode->setFieldU64 (sfIndexNext, uNodeDir);
            view.update(sleNode);

            // Have root point to new node.
            sleRoot->setFieldU64 (sfIndexPrevious, uNodeDir);
            view.update (sleRoot);

            // Create the new node.
            sleNode = std::make_shared<SLE>(
                keylet::page(uRootIndex, uNodeDir));
            sleNode->setFieldH256 (sfRootIndex, uRootIndex);
            view.insert (sleNode);

            if (uNodeDir != 1)
                sleNode->setFieldU64 (sfIndexPrevious, uNodeDir - 1);

            fDescriber (sleNode, false);

            svIndexes   = STVector256 ();
        }
    }

    svIndexes.push_back (uLedgerIndex); // Append entry.
    sleNode->setFieldV256 (sfIndexes, svIndexes);   // Save entry.

    WriteLog (lsTRACE, View) <<
        "dirAdd:   creating: root: " << to_string (uRootIndex);
    WriteLog (lsTRACE, View) <<
        "dirAdd:  appending: Entry: " << to_string (uLedgerIndex);
    WriteLog (lsTRACE, View) <<
        "dirAdd:  appending: Node: " << strHex (uNodeDir);

    return tesSUCCESS;
}

// Ledger must be in a state for this to work.
TER
dirDelete (View& view,
    const bool                      bKeepRoot,      // --> True, if we never completely clean up, after we overflow the root node.
    const std::uint64_t&            uNodeDir,       // --> Node containing entry.
    uint256 const&                  uRootIndex,     // --> The index of the base of the directory.  Nodes are based off of this.
    uint256 const&                  uLedgerIndex,   // --> Value to remove from directory.
    const bool                      bStable,        // --> True, not to change relative order of entries.
    const bool                      bSoft)          // --> True, uNodeDir is not hard and fast (pass uNodeDir=0).
{
    std::uint64_t uNodeCur = uNodeDir;
    SLE::pointer sleNode =
        view.peek(keylet::page(uRootIndex, uNodeCur));

    if (!sleNode)
    {
        WriteLog (lsWARNING, View) << "dirDelete: no such node:" <<
            " uRootIndex=" << to_string (uRootIndex) <<
            " uNodeDir=" << strHex (uNodeDir) <<
            " uLedgerIndex=" << to_string (uLedgerIndex);

        if (!bSoft)
        {
            assert (false);
            return tefBAD_LEDGER;
        }
        else if (uNodeDir < 20)
        {
            // Go the extra mile. Even if node doesn't exist, try the next node.

            return dirDelete (view, bKeepRoot,
                uNodeDir + 1, uRootIndex, uLedgerIndex, bStable, true);
        }
        else
        {
            return tefBAD_LEDGER;
        }
    }

    STVector256 svIndexes = sleNode->getFieldV256 (sfIndexes);

    auto it = std::find (svIndexes.begin (), svIndexes.end (), uLedgerIndex);

    if (svIndexes.end () == it)
    {
        if (!bSoft)
        {
            assert (false);
            WriteLog (lsWARNING, View) << "dirDelete: no such entry";
            return tefBAD_LEDGER;
        }

        if (uNodeDir < 20)
        {
            // Go the extra mile. Even if entry not in node, try the next node.
            return dirDelete (view, bKeepRoot, uNodeDir + 1,
                uRootIndex, uLedgerIndex, bStable, true);
        }

        return tefBAD_LEDGER;
    }

    // Remove the element.
    if (svIndexes.size () > 1)
    {
        if (bStable)
        {
            svIndexes.erase (it);
        }
        else
        {
            *it = svIndexes[svIndexes.size () - 1];
            svIndexes.resize (svIndexes.size () - 1);
        }
    }
    else
    {
        svIndexes.clear ();
    }

    sleNode->setFieldV256 (sfIndexes, svIndexes);
    view.update(sleNode);

    if (svIndexes.empty ())
    {
        // May be able to delete nodes.
        std::uint64_t       uNodePrevious   = sleNode->getFieldU64 (sfIndexPrevious);
        std::uint64_t       uNodeNext       = sleNode->getFieldU64 (sfIndexNext);

        if (!uNodeCur)
        {
            // Just emptied root node.

            if (!uNodePrevious)
            {
                // Never overflowed the root node.  Delete it.
                view.erase(sleNode);
            }
            // Root overflowed.
            else if (bKeepRoot)
            {
                // If root overflowed and not allowed to delete overflowed root node.
            }
            else if (uNodePrevious != uNodeNext)
            {
                // Have more than 2 nodes.  Can't delete root node.
            }
            else
            {
                // Have only a root node and a last node.
                auto sleLast = view.peek(keylet::page(uRootIndex, uNodeNext));

                assert (sleLast);

                if (sleLast->getFieldV256 (sfIndexes).empty ())
                {
                    // Both nodes are empty.

                    view.erase (sleNode);  // Delete root.
                    view.erase (sleLast);  // Delete last.
                }
                else
                {
                    // Have an entry, can't delete root node.
                }
            }
        }
        // Just emptied a non-root node.
        else if (uNodeNext)
        {
            // Not root and not last node. Can delete node.

            auto slePrevious =
                view.peek(keylet::page(uRootIndex, uNodePrevious));
            auto sleNext = view.peek(keylet::page(uRootIndex, uNodeNext));
            assert (slePrevious);
            if (!slePrevious)
            {
                WriteLog (lsWARNING, View) << "dirDelete: previous node is missing";
                return tefBAD_LEDGER;
            }
            assert (sleNext);
            if (!sleNext)
            {
                WriteLog (lsWARNING, View) << "dirDelete: next node is missing";
                return tefBAD_LEDGER;
            }

            // Fix previous to point to its new next.
            slePrevious->setFieldU64 (sfIndexNext, uNodeNext);
            view.update (slePrevious);

            // Fix next to point to its new previous.
            sleNext->setFieldU64 (sfIndexPrevious, uNodePrevious);
            view.update (sleNext);

            view.erase(sleNode);
        }
        // Last node.
        else if (bKeepRoot || uNodePrevious)
        {
            // Not allowed to delete last node as root was overflowed.
            // Or, have pervious entries preventing complete delete.
        }
        else
        {
            // Last and only node besides the root.
            auto sleRoot = view.peek (keylet::page(uRootIndex));

            assert (sleRoot);

            if (sleRoot->getFieldV256 (sfIndexes).empty ())
            {
                // Both nodes are empty.

                view.erase(sleRoot);  // Delete root.
                view.erase(sleNode);  // Delete last.
            }
            else
            {
                // Root has an entry, can't delete.
            }
        }
    }

    return tesSUCCESS;
}

TER
trustCreate (View& view,
    const bool      bSrcHigh,
    AccountID const&  uSrcAccountID,
    AccountID const&  uDstAccountID,
    uint256 const&  uIndex,             // --> ripple state entry
    SLE::ref        sleAccount,         // --> the account being set.
    const bool      bAuth,              // --> authorize account.
    const bool      bNoRipple,          // --> others cannot ripple through
    const bool      bFreeze,            // --> funds cannot leave
    STAmount const& saBalance,          // --> balance of account being set.
                                        // Issuer should be noAccount()
    STAmount const& saLimit,            // --> limit for account being set.
                                        // Issuer should be the account being set.
    std::uint32_t uQualityIn,
    std::uint32_t uQualityOut)
{
    WriteLog (lsTRACE, View)
        << "trustCreate: " << to_string (uSrcAccountID) << ", "
        << to_string (uDstAccountID) << ", " << saBalance.getFullText ();

    auto const& uLowAccountID   = !bSrcHigh ? uSrcAccountID : uDstAccountID;
    auto const& uHighAccountID  =  bSrcHigh ? uSrcAccountID : uDstAccountID;

    auto const sleRippleState = std::make_shared<SLE>(
        ltRIPPLE_STATE, uIndex);
    view.insert (sleRippleState);

    std::uint64_t   uLowNode;
    std::uint64_t   uHighNode;

    TER terResult = dirAdd (view,
        uLowNode,
        getOwnerDirIndex (uLowAccountID),
        sleRippleState->getIndex (),
        [uLowAccountID](std::shared_ptr<SLE> const& sle, bool)
            {
                sle->setAccountID (sfOwner, uLowAccountID);
            });

    if (tesSUCCESS == terResult)
    {
        terResult = dirAdd (view,
            uHighNode,
            getOwnerDirIndex (uHighAccountID),
            sleRippleState->getIndex (),
            [uHighAccountID](std::shared_ptr<SLE> const& sle, bool)
                {
                    sle->setAccountID (sfOwner, uHighAccountID);
                });
    }

    if (tesSUCCESS == terResult)
    {
        const bool bSetDst = saLimit.getIssuer () == uDstAccountID;
        const bool bSetHigh = bSrcHigh ^ bSetDst;

        assert (sleAccount->getAccountID (sfAccount) ==
            (bSetHigh ? uHighAccountID : uLowAccountID));
        auto slePeer = view.peek (keylet::account(
            bSetHigh ? uLowAccountID : uHighAccountID));
        assert (slePeer);

        // Remember deletion hints.
        sleRippleState->setFieldU64 (sfLowNode, uLowNode);
        sleRippleState->setFieldU64 (sfHighNode, uHighNode);

        sleRippleState->setFieldAmount (
            bSetHigh ? sfHighLimit : sfLowLimit, saLimit);
        sleRippleState->setFieldAmount (
            bSetHigh ? sfLowLimit : sfHighLimit,
            STAmount ({saBalance.getCurrency (),
                       bSetDst ? uSrcAccountID : uDstAccountID}));

        if (uQualityIn)
            sleRippleState->setFieldU32 (
                bSetHigh ? sfHighQualityIn : sfLowQualityIn, uQualityIn);

        if (uQualityOut)
            sleRippleState->setFieldU32 (
                bSetHigh ? sfHighQualityOut : sfLowQualityOut, uQualityOut);

        std::uint32_t uFlags = bSetHigh ? lsfHighReserve : lsfLowReserve;

        if (bAuth)
        {
            uFlags |= (bSetHigh ? lsfHighAuth : lsfLowAuth);
        }
        if (bNoRipple)
        {
            uFlags |= (bSetHigh ? lsfHighNoRipple : lsfLowNoRipple);
        }
        if (bFreeze)
        {
            uFlags |= (!bSetHigh ? lsfLowFreeze : lsfHighFreeze);
        }

        if ((slePeer->getFlags() & lsfDefaultRipple) == 0)
        {
            // The other side's default is no rippling
            uFlags |= (bSetHigh ? lsfLowNoRipple : lsfHighNoRipple);
        }

        sleRippleState->setFieldU32 (sfFlags, uFlags);
        adjustOwnerCount(view, sleAccount, 1);

        // ONLY: Create ripple balance.
        sleRippleState->setFieldAmount (sfBalance, bSetHigh ? -saBalance : saBalance);

        view.creditHook (uSrcAccountID,
            uDstAccountID, saBalance);
    }

    return terResult;
}

TER
trustDelete (View& view,
    std::shared_ptr<SLE> const& sleRippleState,
        AccountID const& uLowAccountID,
            AccountID const& uHighAccountID)
{
    // Detect legacy dirs.
    bool        bLowNode    = sleRippleState->isFieldPresent (sfLowNode);
    bool        bHighNode   = sleRippleState->isFieldPresent (sfHighNode);
    std::uint64_t uLowNode    = sleRippleState->getFieldU64 (sfLowNode);
    std::uint64_t uHighNode   = sleRippleState->getFieldU64 (sfHighNode);
    TER         terResult;

    WriteLog (lsTRACE, View)
        << "trustDelete: Deleting ripple line: low";
    terResult   = dirDelete(view,
        false,
        uLowNode,
        getOwnerDirIndex (uLowAccountID),
        sleRippleState->getIndex (),
        false,
        !bLowNode);

    if (tesSUCCESS == terResult)
    {
        WriteLog (lsTRACE, View)
                << "trustDelete: Deleting ripple line: high";
        terResult   = dirDelete (view,
            false,
            uHighNode,
            getOwnerDirIndex (uHighAccountID),
            sleRippleState->getIndex (),
            false,
            !bHighNode);
    }

    WriteLog (lsTRACE, View) << "trustDelete: Deleting ripple line: state";
    view.erase(sleRippleState);

    return terResult;
}

TER
offerDelete (View& view,
    std::shared_ptr<SLE> const& sle)
{
    if (! sle)
        return tesSUCCESS;
    auto offerIndex = sle->getIndex ();
    auto owner = sle->getAccountID  (sfAccount);

    // Detect legacy directories.
    bool bOwnerNode = sle->isFieldPresent (sfOwnerNode);
    std::uint64_t uOwnerNode = sle->getFieldU64 (sfOwnerNode);
    uint256 uDirectory = sle->getFieldH256 (sfBookDirectory);
    std::uint64_t uBookNode  = sle->getFieldU64 (sfBookNode);

    TER terResult  = dirDelete (view, false, uOwnerNode,
        getOwnerDirIndex (owner), offerIndex, false, !bOwnerNode);
    TER terResult2 = dirDelete (view, false, uBookNode,
        uDirectory, offerIndex, true, false);

    if (tesSUCCESS == terResult)
        adjustOwnerCount(view, view.peek(
            keylet::account(owner)), -1);

    view.erase(sle);

    return (terResult == tesSUCCESS) ?
        terResult2 : terResult;
}

// Direct send w/o fees:
// - Redeeming IOUs and/or sending sender's own IOUs.
// - Create trust line of needed.
// --> bCheckIssuer : normally require issuer to be involved.
TER
rippleCredit (View& view,
    AccountID const& uSenderID, AccountID const& uReceiverID,
    STAmount const& saAmount, bool bCheckIssuer)
{
    auto issuer = saAmount.getIssuer ();
    auto currency = saAmount.getCurrency ();

    // Make sure issuer is involved.
    assert (
        !bCheckIssuer || uSenderID == issuer || uReceiverID == issuer);
    (void) issuer;

    // Disallow sending to self.
    assert (uSenderID != uReceiverID);

    bool bSenderHigh = uSenderID > uReceiverID;
    uint256 uIndex = getRippleStateIndex (
        uSenderID, uReceiverID, saAmount.getCurrency ());
    auto sleRippleState  = view.peek (keylet::line(uIndex));

    TER terResult;

    assert (!isXRP (uSenderID) && uSenderID != noAccount());
    assert (!isXRP (uReceiverID) && uReceiverID != noAccount());

    if (!sleRippleState)
    {
        STAmount saReceiverLimit({currency, uReceiverID});
        STAmount saBalance = saAmount;

        saBalance.setIssuer (noAccount());

        WriteLog (lsDEBUG, View) << "rippleCredit: "
            "create line: " << to_string (uSenderID) <<
            " -> " << to_string (uReceiverID) <<
            " : " << saAmount.getFullText ();

        auto const sleAccount =
            view.peek(keylet::account(uReceiverID));

        bool noRipple = (sleAccount->getFlags() & lsfDefaultRipple) == 0;

        terResult = trustCreate (view,
            bSenderHigh,
            uSenderID,
            uReceiverID,
            uIndex,
            sleAccount,
            false,
            noRipple,
            false,
            saBalance,
            saReceiverLimit,
            0,
            0);
    }
    else
    {
        view.creditHook (uSenderID,
            uReceiverID, saAmount);

        STAmount saBalance   = sleRippleState->getFieldAmount (sfBalance);

        if (bSenderHigh)
            saBalance.negate ();    // Put balance in sender terms.

        STAmount    saBefore    = saBalance;

        saBalance   -= saAmount;

        WriteLog (lsTRACE, View) << "rippleCredit: " <<
            to_string (uSenderID) <<
            " -> " << to_string (uReceiverID) <<
            " : before=" << saBefore.getFullText () <<
            " amount=" << saAmount.getFullText () <<
            " after=" << saBalance.getFullText ();

        std::uint32_t const uFlags (sleRippleState->getFieldU32 (sfFlags));
        bool bDelete = false;

        // YYY Could skip this if rippling in reverse.
        if (saBefore > zero
            // Sender balance was positive.
            && saBalance <= zero
            // Sender is zero or negative.
            && (uFlags & (!bSenderHigh ? lsfLowReserve : lsfHighReserve))
            // Sender reserve is set.
            && static_cast <bool> (uFlags & (!bSenderHigh ? lsfLowNoRipple : lsfHighNoRipple)) !=
               static_cast <bool> (view.read (keylet::account(uSenderID))->getFlags() & lsfDefaultRipple)
            && !(uFlags & (!bSenderHigh ? lsfLowFreeze : lsfHighFreeze))
            && !sleRippleState->getFieldAmount (
                !bSenderHigh ? sfLowLimit : sfHighLimit)
            // Sender trust limit is 0.
            && !sleRippleState->getFieldU32 (
                !bSenderHigh ? sfLowQualityIn : sfHighQualityIn)
            // Sender quality in is 0.
            && !sleRippleState->getFieldU32 (
                !bSenderHigh ? sfLowQualityOut : sfHighQualityOut))
            // Sender quality out is 0.
        {
            // Clear the reserve of the sender, possibly delete the line!
            adjustOwnerCount(view,
                view.peek(keylet::account(uSenderID)), -1);

            // Clear reserve flag.
            sleRippleState->setFieldU32 (
                sfFlags,
                uFlags & (!bSenderHigh ? ~lsfLowReserve : ~lsfHighReserve));

            // Balance is zero, receiver reserve is clear.
            bDelete = !saBalance        // Balance is zero.
                && !(uFlags & (bSenderHigh ? lsfLowReserve : lsfHighReserve));
            // Receiver reserve is clear.
        }

        if (bSenderHigh)
            saBalance.negate ();

        // Want to reflect balance to zero even if we are deleting line.
        sleRippleState->setFieldAmount (sfBalance, saBalance);
        // ONLY: Adjust ripple balance.

        if (bDelete)
        {
            terResult   = trustDelete (view,
                sleRippleState,
                bSenderHigh ? uReceiverID : uSenderID,
                !bSenderHigh ? uReceiverID : uSenderID);
        }
        else
        {
            view.update (sleRippleState);
            terResult   = tesSUCCESS;
        }
    }

    return terResult;
}

// Calculate the fee needed to transfer IOU assets between two parties.
static
STAmount
rippleTransferFee (BasicView const& view,
    AccountID const& from,
    AccountID const& to,
    AccountID const& issuer,
    STAmount const& saAmount)
{
    if (from != issuer && to != issuer)
    {
        std::uint32_t uTransitRate = rippleTransferRate (view, issuer);

        if (QUALITY_ONE != uTransitRate)
        {
            STAmount saTransferTotal = multiply (
                saAmount, amountFromRate (uTransitRate), saAmount.issue ());
            STAmount saTransferFee = saTransferTotal - saAmount;

            WriteLog (lsDEBUG, View) << "rippleTransferFee:" <<
                " saTransferFee=" << saTransferFee.getFullText ();

            return saTransferFee;
        }
    }

    return saAmount.zeroed();
}

// Send regardless of limits.
// --> saAmount: Amount/currency/issuer to deliver to reciever.
// <-- saActual: Amount actually cost.  Sender pay's fees.
static
TER
rippleSend (View& view,
    AccountID const& uSenderID, AccountID const& uReceiverID,
    STAmount const& saAmount, STAmount& saActual)
{
    auto const issuer   = saAmount.getIssuer ();
    TER             terResult;

    assert (!isXRP (uSenderID) && !isXRP (uReceiverID));
    assert (uSenderID != uReceiverID);

    if (uSenderID == issuer || uReceiverID == issuer || issuer == noAccount())
    {
        // VFALCO Why do we need this bCheckIssuer?
        // Direct send: redeeming IOUs and/or sending own IOUs.
        terResult   = rippleCredit (view, uSenderID, uReceiverID, saAmount, false);
        saActual    = saAmount;
        terResult   = tesSUCCESS;
    }
    else
    {
        // Sending 3rd party IOUs: transit.

        STAmount saTransitFee = rippleTransferFee (view,
            uSenderID, uReceiverID, issuer, saAmount);

        saActual = !saTransitFee ? saAmount : saAmount + saTransitFee;

        saActual.setIssuer (issuer); // XXX Make sure this done in + above.

        WriteLog (lsDEBUG, View) << "rippleSend> " <<
            to_string (uSenderID) <<
            " - > " << to_string (uReceiverID) <<
            " : deliver=" << saAmount.getFullText () <<
            " fee=" << saTransitFee.getFullText () <<
            " cost=" << saActual.getFullText ();

        terResult   = rippleCredit (view, issuer, uReceiverID, saAmount, true);

        if (tesSUCCESS == terResult)
            terResult   = rippleCredit (view, uSenderID, issuer, saActual, true);
    }

    return terResult;
}

TER
accountSend (View& view,
    AccountID const& uSenderID, AccountID const& uReceiverID,
    STAmount const& saAmount)
{
    assert (saAmount >= zero);

    /* If we aren't sending anything or if the sender is the same as the
     * receiver then we don't need to do anything.
     */
    if (!saAmount || (uSenderID == uReceiverID))
        return tesSUCCESS;

    if (!saAmount.native ())
    {
        STAmount saActual;

        WriteLog (lsTRACE, View) << "accountSend: " <<
            to_string (uSenderID) << " -> " << to_string (uReceiverID) <<
            " : " << saAmount.getFullText ();

        return rippleSend (view, uSenderID, uReceiverID, saAmount, saActual);
    }

    view.creditHook (uSenderID,
        uReceiverID, saAmount);

    /* XRP send which does not check reserve and can do pure adjustment.
     * Note that sender or receiver may be null and this not a mistake; this
     * setup is used during pathfinding and it is carefully controlled to
     * ensure that transfers are balanced.
     */

    TER terResult (tesSUCCESS);

    SLE::pointer sender = uSenderID != beast::zero
        ? view.peek (keylet::account(uSenderID))
        : SLE::pointer ();
    SLE::pointer receiver = uReceiverID != beast::zero
        ? view.peek (keylet::account(uReceiverID))
        : SLE::pointer ();

    if (ShouldLog (lsTRACE, View))
    {
        std::string sender_bal ("-");
        std::string receiver_bal ("-");

        if (sender)
            sender_bal = sender->getFieldAmount (sfBalance).getFullText ();

        if (receiver)
            receiver_bal = receiver->getFieldAmount (sfBalance).getFullText ();

        WriteLog (lsTRACE, View) << "accountSend> " <<
            to_string (uSenderID) << " (" << sender_bal <<
            ") -> " << to_string (uReceiverID) << " (" << receiver_bal <<
            ") : " << saAmount.getFullText ();
    }

    if (sender)
    {
        if (sender->getFieldAmount (sfBalance) < saAmount)
        {
            // VFALCO Its laborious to have to mutate the
            //        TER based on params everywhere
            terResult = view.openLedger()
                ? telFAILED_PROCESSING
                : tecFAILED_PROCESSING;
        }
        else
        {
            // Decrement XRP balance.
            sender->setFieldAmount (sfBalance,
                sender->getFieldAmount (sfBalance) - saAmount);
            view.update (sender);
        }
    }

    if (tesSUCCESS == terResult && receiver)
    {
        // Increment XRP balance.
        receiver->setFieldAmount (sfBalance,
            receiver->getFieldAmount (sfBalance) + saAmount);
        view.update (receiver);
    }

    if (ShouldLog (lsTRACE, View))
    {
        std::string sender_bal ("-");
        std::string receiver_bal ("-");

        if (sender)
            sender_bal = sender->getFieldAmount (sfBalance).getFullText ();

        if (receiver)
            receiver_bal = receiver->getFieldAmount (sfBalance).getFullText ();

        WriteLog (lsTRACE, View) << "accountSend< " <<
            to_string (uSenderID) << " (" << sender_bal <<
            ") -> " << to_string (uReceiverID) << " (" << receiver_bal <<
            ") : " << saAmount.getFullText ();
    }

    return terResult;
}

static
bool
updateTrustLine (
    View& view,
    SLE::pointer state,
    bool bSenderHigh,
    AccountID const& sender,
    STAmount const& before,
    STAmount const& after)
{
    std::uint32_t const flags (state->getFieldU32 (sfFlags));

    auto sle = view.peek (keylet::account(sender));
    assert (sle);

    // YYY Could skip this if rippling in reverse.
    if (before > zero
        // Sender balance was positive.
        && after <= zero
        // Sender is zero or negative.
        && (flags & (!bSenderHigh ? lsfLowReserve : lsfHighReserve))
        // Sender reserve is set.
        && static_cast <bool> (flags & (!bSenderHigh ? lsfLowNoRipple : lsfHighNoRipple)) !=
           static_cast <bool> (sle->getFlags() & lsfDefaultRipple)
        && !(flags & (!bSenderHigh ? lsfLowFreeze : lsfHighFreeze))
        && !state->getFieldAmount (
            !bSenderHigh ? sfLowLimit : sfHighLimit)
        // Sender trust limit is 0.
        && !state->getFieldU32 (
            !bSenderHigh ? sfLowQualityIn : sfHighQualityIn)
        // Sender quality in is 0.
        && !state->getFieldU32 (
            !bSenderHigh ? sfLowQualityOut : sfHighQualityOut))
        // Sender quality out is 0.
    {
        // VFALCO Where is the line being deleted?
        // Clear the reserve of the sender, possibly delete the line!
        adjustOwnerCount(view, sle, -1);

        // Clear reserve flag.
        state->setFieldU32 (sfFlags,
            flags & (!bSenderHigh ? ~lsfLowReserve : ~lsfHighReserve));

        // Balance is zero, receiver reserve is clear.
        if (!after        // Balance is zero.
            && !(flags & (bSenderHigh ? lsfLowReserve : lsfHighReserve)))
            return true;
    }

    return false;
}

TER
issueIOU (View& view,
    AccountID const& account,
        STAmount const& amount, Issue const& issue)
{
    assert (!isXRP (account) && !isXRP (issue.account));

    // Consistency check
    assert (issue == amount.issue ());

    // Can't send to self!
    assert (issue.account != account);

    WriteLog (lsTRACE, View) << "issueIOU: " <<
        to_string (account) << ": " <<
        amount.getFullText ();

    bool bSenderHigh = issue.account > account;
    uint256 const index = getRippleStateIndex (
        issue.account, account, issue.currency);
    auto state = view.peek (keylet::line(index));

    if (!state)
    {
        // NIKB TODO: The limit uses the receiver's account as the issuer and
        // this is unnecessarily inefficient as copying which could be avoided
        // is now required. Consider available options.
        STAmount limit({issue.currency, account});
        STAmount final_balance = amount;

        final_balance.setIssuer (noAccount());

        auto receiverAccount = view.peek (keylet::account(account));

        bool noRipple = (receiverAccount->getFlags() & lsfDefaultRipple) == 0;

        return trustCreate (view, bSenderHigh, issue.account, account, index,
            receiverAccount, false, noRipple, false, final_balance, limit, 0, 0);
    }

    STAmount final_balance = state->getFieldAmount (sfBalance);

    if (bSenderHigh)
        final_balance.negate ();    // Put balance in sender terms.

    STAmount const start_balance = final_balance;

    final_balance -= amount;

    auto const must_delete = updateTrustLine(view, state, bSenderHigh, issue.account,
        start_balance, final_balance);

    if (bSenderHigh)
        final_balance.negate ();

    view.creditHook (issue.account,
        account, amount);

    // Adjust the balance on the trust line if necessary. We do this even if we
    // are going to delete the line to reflect the correct balance at the time
    // of deletion.
    state->setFieldAmount (sfBalance, final_balance);
    if (must_delete)
        return trustDelete (view, state,
            bSenderHigh ? account : issue.account,
            bSenderHigh ? issue.account : account);

    view.update (state);

    return tesSUCCESS;
}

TER
redeemIOU (View& view,
    AccountID const& account,
    STAmount const& amount,
    Issue const& issue)
{
    assert (!isXRP (account) && !isXRP (issue.account));

    // Consistency check
    assert (issue == amount.issue ());

    // Can't send to self!
    assert (issue.account != account);

    WriteLog (lsTRACE, View) << "redeemIOU: " <<
        to_string (account) << ": " <<
        amount.getFullText ();

    bool bSenderHigh = account > issue.account;
    uint256 const index = getRippleStateIndex (
        account, issue.account, issue.currency);
    auto state  = view.peek (keylet::line(index));

    if (!state)
    {
        // In order to hold an IOU, a trust line *MUST* exist to track the
        // balance. If it doesn't, then something is very wrong. Don't try
        // to continue.
        WriteLog (lsFATAL, View) << "redeemIOU: " <<
            to_string (account) << " attempts to redeem " <<
            amount.getFullText () << " but no trust line exists!";

        return tefINTERNAL;
    }

    STAmount final_balance = state->getFieldAmount (sfBalance);

    if (bSenderHigh)
        final_balance.negate ();    // Put balance in sender terms.

    STAmount const start_balance = final_balance;

    final_balance -= amount;

    auto const must_delete = updateTrustLine (view, state, bSenderHigh, account,
        start_balance, final_balance);

    if (bSenderHigh)
        final_balance.negate ();

    view.creditHook (account,
        issue.account, amount);

    // Adjust the balance on the trust line if necessary. We do this even if we
    // are going to delete the line to reflect the correct balance at the time
    // of deletion.
    state->setFieldAmount (sfBalance, final_balance);

    if (must_delete)
    {
        return trustDelete (view, state,
            bSenderHigh ? issue.account : account,
            bSenderHigh ? account : issue.account);
    }

    view.update (state);
    return tesSUCCESS;
}

TER
transferXRP (View& view,
    AccountID const& from,
    AccountID const& to,
    STAmount const& amount)
{
    assert (from != beast::zero);
    assert (to != beast::zero);
    assert (from != to);
    assert (amount.native ());

    SLE::pointer sender = view.peek (keylet::account(from));
    SLE::pointer receiver = view.peek (keylet::account(to));

    WriteLog (lsTRACE, View) << "transferXRP: " <<
        to_string (from) <<  " -> " << to_string (to) <<
        ") : " << amount.getFullText ();

    if (sender->getFieldAmount (sfBalance) < amount)
    {
        // VFALCO Its unfortunate we have to keep
        //        mutating these TER everywhere
        // FIXME: this logic should be moved to callers maybe?
        return view.openLedger()
            ? telFAILED_PROCESSING
            : tecFAILED_PROCESSING;
    }

    // Decrement XRP balance.
    sender->setFieldAmount (sfBalance,
        sender->getFieldAmount (sfBalance) - amount);
    view.update (sender);

    receiver->setFieldAmount (sfBalance,
        receiver->getFieldAmount (sfBalance) + amount);
    view.update (receiver);

    return tesSUCCESS;
}

} // ripple
