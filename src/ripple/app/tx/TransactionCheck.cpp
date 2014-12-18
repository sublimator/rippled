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

namespace ripple {

// VFALCO TODO move this into TransactionEngine.cpp

// Double check a transaction's metadata to make sure no system invariants were broken

bool TransactionEngine::checkInvariants (TER result, const STTx& txn, TransactionEngineParams params)
{
    // VFALCO I deleted a bunch of code that was wrapped in #if 0.
    //        If you need it, check the commit log.

    #if 1
        uint32                  txnSeq      = txn.getFieldU32 (sfSequence);

        LedgerEntryAction       leaAction;

        uint256                 srcActId    = getAccountRootIndex (txn.getFieldAccount (sfAccount));
        SLE::pointer            origSrcAct  = mLedger->getSLE (srcActId);
        SLE::pointer            newSrcAct   = mNodes.getEntry (srcActId, leaAction);

        if (!newSrcAct || !origSrcAct)
        {
            WriteLog (lsFATAL, TransactionEngine) << "Transaction created or destroyed its issuing account";
            // assert (false);
            return false;
        }

        if ((newSrcAct->getFieldU32 (sfSequence) != (txnSeq + 1)) ||
                (origSrcAct->getFieldU32 (sfSequence) != txnSeq))
        {
            WriteLog (lsFATAL, TransactionEngine) << "Transaction mangles sequence numbers";
            WriteLog (lsFATAL, TransactionEngine) << "t:" << txnSeq << " o: " << origSrcAct->getFieldU32 (sfSequence)
                                                  << " n: " << newSrcAct->getFieldU32 (sfSequence);
            // assert (false);
            return false;
        }

        std::int64_t  xrpChange = txn.getFieldAmount (sfFee).getSNValue ();

        for (LedgerEntrySet::const_iterator it = mNodes.begin (), end = mNodes.end (); it != end; ++it)
        {
            const LedgerEntrySetEntry& entry = it->second;

            if (entry.mAction == taaMODIFY)
            {
    #if 0

                if (entry.mEntry->getType () == ltRIPPLE_STATE)
                {
                    // if this transaction pushes a ripple state over its limit, make sure it also modifies
                    // an offer placed by that same user
                }

    #endif

                if (entry.mEntry->getType () == ltACCOUNT_ROOT)
                {
                    // account modified
                    xrpChange += entry.mEntry->getFieldAmount (sfBalance).getSNValue ();
                    xrpChange -= mLedger->getSLE (it->first)->getFieldAmount (sfBalance).getSNValue ();
                }
            }
            else if (entry.mAction == taaCREATE)
            {
                if (entry.mEntry->getType () == ltRIPPLE_STATE)
                {
                    if (entry.mEntry->getFieldAmount (sfLowLimit).getIssuer () ==
                            entry.mEntry->getFieldAmount (sfHighLimit).getIssuer ())
                    {
                        WriteLog (lsFATAL, TransactionEngine) << "Ripple line to self";
                        // assert (false);
                        return false;
                    }
                }

                if (entry.mEntry->getType () == ltACCOUNT_ROOT) // account created
                    xrpChange += entry.mEntry->getFieldAmount (sfBalance).getSNValue ();
            }
        }

        if (xrpChange != 0)
        {
            WriteLog (lsFATAL, TransactionEngine) << "Transaction creates/destroys XRP: " << xrpChange ;
            // assert (false);
            return false;
        }

    #endif

        return true;
    }

} // ripple
