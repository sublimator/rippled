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

#ifndef RIPPLE_PATHREQUEST_H
#define RIPPLE_PATHREQUEST_H

// A pathfinding request submitted by a client
// The request issuer must maintain a strong pointer

class RippleLineCache;
class PathRequests;

// Return values from parseJson <0 = invalid, >0 = valid
#define PFR_PJ_INVALID              -1
#define PFR_PJ_NOCHANGE             0
#define PFR_PJ_CHANGE               1

class PathRequest : public boost::enable_shared_from_this<PathRequest>, public CountedObject <PathRequest>
{
public:

    static char const* getCountedObjectName () { return "PathRequest"; }

    typedef boost::weak_ptr<PathRequest>    wptr;
    typedef boost::shared_ptr<PathRequest>  pointer;
    typedef const pointer&                  ref;
    typedef const wptr&                     wref;
    typedef std::pair<uint160, uint160>     currIssuer_t;

public:
    // VFALCO TODO Break the cyclic dependency on InfoSub
    explicit PathRequest (boost::shared_ptr <InfoSub> const& subscriber, int id, PathRequests&);
    ~PathRequest ();

    bool        isValid (const boost::shared_ptr<Ledger>&);
    bool        isValid ();
    bool        needsUpdate (bool newOnly, LedgerIndex index);
    Json::Value getStatus ();

    Json::Value doCreate (const boost::shared_ptr<Ledger>&, const RippleLineCache::pointer&,
        const Json::Value&, bool&);
    Json::Value doClose (const Json::Value&);
    Json::Value doStatus (const Json::Value&);
    Json::Value doUpdate (const boost::shared_ptr<RippleLineCache>&, bool fast); // update jvStatus
    InfoSub::pointer getSubscriber ();
    Journal& journal ();

private:
    void setValid ();
    void resetLevel (int level);
    int parseJson (const Json::Value&, bool complete);

    typedef RippleRecursiveMutex LockType;
    typedef LockType::ScopedLockType ScopedLockType;
    LockType mLock;

    PathRequests&                   mOwner;

    boost::weak_ptr<InfoSub>        wpSubscriber;               // Who this request came from
    Json::Value                     jvId;
    Json::Value                     jvStatus;                   // Last result

    // Client request parameters
    RippleAddress                     raSrcAccount;
    RippleAddress                     raDstAccount;
    STAmount                          saDstAmount;
    std::set<currIssuer_t>            sciSourceCurrencies;
    // std::vector<Json::Value>          vjvBridges;
    std::map<currIssuer_t, STPathSet> mContext;

    bool                            bValid;
    LedgerIndex                     iLastIndex;

    int                             iLastLevel;
    bool                            bLastSuccess;

    int                             iIdentifier;

    boost::posix_time::ptime        ptCreated;
    boost::posix_time::ptime        ptQuickReply;
    boost::posix_time::ptime        ptFullReply;

};


class PathRequests
{
public:

    PathRequests (Journal journal, std::shared_ptr <insight::Collector> const& collector)
        : mJournal (journal)
        , mLastIdentifier (0)
        , mLock ("PathRequests", __FILE__, __LINE__)
    {
        mFast = collector->make_event ("pathfind_fast");
        mFull = collector->make_event ("pathfind_full");
    }

    void updateAll (const boost::shared_ptr<Ledger>& ledger, CancelCallback shouldCancel);

    RippleLineCache::pointer getLineCache (Ledger::pointer& ledger, bool authoritative);

    Json::Value makePathRequest (
        boost::shared_ptr <InfoSub> const& subscriber,
        const boost::shared_ptr<Ledger>& ledger,
        const Json::Value& request);

    Journal& journal ()
    {
        return mJournal;
    }

    void reportFast (int milliseconds)
    {
        mFast.notify (static_cast < insight::Event::value_type> (milliseconds));
    }

    void reportFull (int milliseconds)
    {
        mFull.notify (static_cast < insight::Event::value_type> (milliseconds));
    }

private:
    Journal                          mJournal;

    insight::Event                   mFast;
    insight::Event                   mFull;

    // Track all requests
    std::vector<PathRequest::wptr>   mRequests;

    // Use a RippleLineCache
    RippleLineCache::pointer         mLineCache;

    Atomic<int>                      mLastIdentifier;

    typedef RippleRecursiveMutex     LockType;
    typedef LockType::ScopedLockType ScopedLockType;
    LockType                         mLock;

};

#endif

// vim:ts=4