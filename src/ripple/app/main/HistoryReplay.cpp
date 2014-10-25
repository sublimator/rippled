/*

I added these two constructors into Ledger.cpp/Ledger.h

Ledger::Ledger (Blob const& rawLedger, SHAMap::ref accountState)
    : mClosed (false)
    , mValidated (false)
    , mValidHash (false)
    , mAccepted (false)
    , mImmutable (false)
{
    Serializer s (rawLedger);
    setRaw (s, false);
    initializeFees ();
    mAccountStateMap = accountState;
}

Ledger::Ledger (Blob const& rawLedger)
    : mClosed (false)
    , mValidated (false)
    , mValidHash (false)
    , mAccepted (false)
    , mImmutable (false)
{
    Serializer s (rawLedger);
    setRaw (s, false);
    initializeFees ();
}
*/

class StreamReader
{
public:
    // This could be &std::cin or &std::ifstream
    std::istream* istream_;
    std::istream& stream()
    {
        return *istream_;
    }

    StreamReader (std::istream* stream) : istream_(stream) {}

    bool readSize8(size_t& out)
    {
        unsigned char a;
        // bool ret = stream().read((char  *) &a, 1);
        bool ret = stream().read((char  *) &a, 1);
        if(ret) out = a;
        return ret;
    }

    bool readUInt32(std::uint32_t& out)
    {
        char a[4];
        bool ret = stream().read(a, 4);
        if(ret) out = a[0] >> 24 | a[1] >> 16 | a[2] >> 8 | a[3];
        return ret;
    }

    bool readHash256(uint256& index)
    {
        return stream().read(((char *) index.begin ()), 32);
    }

    bool eof()
    {
        return stream().eof();
    }

    bool readVlLength(size_t& result)
    {
        size_t b1;
        if(!readSize8(b1))
        {
            return false;
        };

        if (b1 <= 192)
        {
            result = b1;
        }
        else if (b1 <= 240)
        {
            size_t b2;
            if(!readSize8(b2))
            {
                return false;
            }
            result = 193 + (b1 - 193) * 256 + b2;
        }
        else if (b1 <= 254)
        {
            size_t b2, b3;
            if(!readSize8(b2) || !readSize8(b3))
            {
                return false;
            }
            result = 12481 + ((b1 - 241) * 65536) + (b2 * 256) + b3;
        }
        else
        {
            return false;
        }
        return true;
    }

    bool readLedgerHeader(Blob& header)
    {
        static size_t headerSize (118);
        header = Blob(headerSize);
        // read into the Blob
        return stream().read((char *) &header[0], headerSize);
    }

    bool readVlObject(Blob& object)
    {
        size_t vl;
        if (!readVlLength(vl)) return false;
        object = Blob(vl);
        // read into the Blob
        return stream().read((char *) &object[0], vl);
    }

    bool readIndexedVlObject(uint256& index, Blob& object)
    {
        if (!readHash256(index))
        {
            return false;
        };
        return readVlObject(object);
    }

    bool readLedgerEntryIntoSHAMap(SHAMap::ref accountState, bool update)
    {
         uint256 index;
         Blob le;

         if (!readIndexedVlObject(index, le))
             return false;

        // Make an item out of the blob
        SHAMapItem item (index, le);
        // add it to the account state map
        if (update)
        {
            return accountState->updateGiveItem(
                    std::make_shared<SHAMapItem>(item), false, false);
        }
        else
        {
            return accountState->addItem(item, false, false);
        }
    }

    bool readLedgerEntryIntoSHAMap(SHAMap::ref accountState)
    {
        return readLedgerEntryIntoSHAMap(accountState, false);
    }

    bool readUpdatedLedgerEntryIntoSHAMap(SHAMap::ref accountState)
    {
        return readLedgerEntryIntoSHAMap(accountState, true);
    }
};

void getTreeHashesFrom(Blob& header, uint256& transHash,
                                     uint256& accountHash)
{
    Serializer s (header);
    SerializerIterator sits (s);

    (void) sits.get32 ();
    (void) sits.get64 ();
    (void) sits.get256 ();

    transHash   = sits.get256 ();
    accountHash = sits.get256 ();
}

typedef std::function<void (
                    // OpenLedger immutable
                    Ledger::ref,
                    // Tx Meta Applied immutable
                    Ledger::ref,
                    // Txn hash/id
                    uint256&,
                    // Txn raw bytes
                    Blob&,
                    // Meta raw bytes
                    Blob&)> OnLedger;


class HistoryLoader : public StreamReader
{
public:
    HistoryLoader (std::istream* stream) : StreamReader(stream) {}

    struct Frame
    {
        enum Type
        {
            error                 = -1,
            accountStateTree      =  0,
            accountStateTreeEnd   =  1,
            loadAccountStateDelta =  2,
            indexedLedgerEntry    =  3,
            indexedTransaction    =  4
        };
    };

    Frame::Type frame;
    SHAMap::pointer historicalState;
    Ledger::pointer snapShot;

    Frame::Type nextFrame()
    {
        size_t a;
        if (readSize8(a))
        {
            frame = static_cast<Frame::Type>(a);
        }
        else
        {
            frame = Frame::error;
        }
        return frame;
    }

    bool readTransactions(OnLedger& onLedger)
    {
        bool ret = true;

        while (nextFrame() == Frame::indexedTransaction)
        {
            uint256 index;
            Blob tx, meta;

            if (!(ret = readIndexedVlObject(index, tx))) break;
            if (!(ret = readVlObject(meta))) break;

            // Ledger is immutable
            Ledger::pointer b4Tx = snapShot;
            snapShot = std::make_shared<Ledger> (std::ref(*b4Tx), true);

            nextFrame(); // TODO
            loadAccountStateDelta(snapShot->peekAccountStateMap());
            snapShot->addTransaction(index, Serializer(tx), Serializer(meta));
            snapShot->setImmutable();

            onLedger(b4Tx, snapShot, index, tx, meta);
        }
        return ret;
    }

    bool readAccountState(SHAMap::ref accountState)
    {
        while (nextFrame() == Frame::indexedLedgerEntry)
        {
            if(!readLedgerEntryIntoSHAMap(accountState)) return false;
        }
        return true;
    }

    bool loadAccountStateDelta(SHAMap::ref accountState)
    {
        std::uint32_t modded, deleted, added;


        readUInt32(modded);
        for (int i = 0; i < modded; ++i)
        {
            readUpdatedLedgerEntryIntoSHAMap(accountState);
        }

        readUInt32(deleted);
        for (int i = 0; i < deleted; ++i)
        {
            uint256 index;
            readHash256(index);
            accountState->delItem(index);
        }

        readUInt32(added);
        for (int i = 0; i < added; ++i)
        {
            readLedgerEntryIntoSHAMap(accountState);
        }

        // Getting lazy about error handling ...
        // this `bool` for error thing is a PITA
        return true;
    }

    void readyMapsForModifying(Ledger::ref ledger)
    {
        ledger->peekAccountStateMap()->clearSynching();
        ledger->peekTransactionMap()->clearSynching();
    }

    // It would be nice to make this a `next()` function
    bool parse(OnLedger onLedger)
    {
        bool ret = true;

        while (!eof ())
        {
            uint256 expectedTransHash;
            uint256 expectedAccountHash;
            Blob header;

            if (!(ret = readLedgerHeader (header)))
            {
                if(eof ())
                {
                    ret = true;
                }
                break;
            };

            if (historicalState != nullptr)
            {
                snapShot = std::make_shared<Ledger> (
                    header, historicalState->snapShot(true));
            }
            else
            {
                snapShot = std::make_shared<Ledger> (header);
            }

            SHAMap::ref accountState (snapShot->peekAccountStateMap());
            readyMapsForModifying(snapShot);

            if (nextFrame() == Frame::accountStateTree)
            {
                if (!(ret = readAccountState(accountState)))
                {
                    assert(false);
                    break;
                }
            }

            snapShot->setImmutable();
            if (!(ret = readTransactions(onLedger)))
            {
                assert(false);
                break;
            }

            snapShot = std::make_shared<Ledger> (std::ref(*snapShot), true);
            if (!(ret = loadAccountStateDelta(snapShot->peekAccountStateMap())))
            {
                assert(false);
                break;
            }

            snapShot->setImmutable();
            historicalState = snapShot->peekAccountStateMap();

            getTreeHashesFrom (header, expectedTransHash, expectedAccountHash);
            assert (snapShot->getTransHash() == expectedTransHash);
            assert (snapShot->getAccountHash() == expectedAccountHash);

            if (snapShot->getTransHash() != expectedTransHash ||
                snapShot->getAccountHash() != expectedAccountHash)
                return false;
        }

        return ret;
    }
};

std::string transactionTypeHuman(TxType tt)
{
    return TxFormats::getInstance().findByType(tt)->getName();
}


struct TransactionLedgers {
    Ledger::pointer beforeTx,
                    afterTx,
                    afterHistorical;

    TransactionLedgers(Ledger::ref a, Ledger::ref b, Ledger::ref c) :
                       beforeTx(a), afterTx(b), afterHistorical(c) {}

    void resultDelta(SHAMap::Delta& delta)
    {
        afterHistorical->peekAccountStateMap()->compare (
                    afterTx->peekAccountStateMap(), delta, 10000000);
    }

    void historicalDelta(SHAMap::Delta& delta)
    {
        beforeTx->peekAccountStateMap()->compare (
                    afterHistorical->peekAccountStateMap(), delta, 10000000);
    }
};

class HistoryReplayer
{
public:
    std::set<uint256> unapplied;
    std::map<TxType, int> errors_by_type;
    std::map<TxType, int> txns_by_type;

    HistoryLoader hl;

    int total_txs   = 0,
        failed_txs  = 0,
        meta_equal  = 0,
        state_equal = 0,
        dirs_only   = 0;

    HistoryReplayer(HistoryLoader& hl_) : hl(hl_) {}

    void process() {
        hl.parse([&]( Ledger::ref beforeTransactionApplied,
                      Ledger::ref afterHistoricalResult,
                      uint256& txid,
                      Blob& tx,
                      Blob& meta )
        {

            // Create a snaphot of the ledger
            Ledger::pointer ledger (
                    std::make_shared<Ledger> (
                        std::ref(*beforeTransactionApplied), true) );

            TransactionEngine engine (ledger);

            bool applied;
            Serializer serializer (tx);
            SerializerIterator sit (serializer);
            SerializedTransaction st (sit);

            (void) engine.applyTransaction (
                                            st,
                                            // tapNONE means we should have metadata
                                            tapNONE | tapNO_CHECK_SIGN,
                                            applied);

            ledger->setImmutable();
            if (applied)
            {
                total_txs++;

                Blob reMeta;
                {
                    auto item = ledger->peekTransactionMap()->peekItem (txid);
                    SerializerIterator it (item->peekSerializer ());
                    it.getVL (); // skip transaction
                    reMeta = it.getVL();
                }


                TransactionLedgers tl (beforeTransactionApplied,
                                       ledger, // after Transaction
                                       afterHistoricalResult);

                onTransactionApplied(tl, st, meta, reMeta);
            }
            else
            {
                unapplied.insert(txid); // TODO
            }

        });
    }

    ~HistoryReplayer() {};

    void onTransactionApplied (
                  TransactionLedgers& tl,
                  SerializedTransaction& tx,
                  Blob& meta,
                  Blob& reMeta)
    {

        txns_by_type[tx.getTxnType()]++;

        SHAMap::Delta deltas;
        tl.resultDelta(deltas);

        bool failed = true;

        if (meta == reMeta)
        {
            meta_equal++;
            failed = false;
        }
        if (deltas.size() == 0)
        {
            state_equal++;
            failed = false;
        }

        int n_deltas = deltas.size();

        for (auto&  pair : deltas) {
            auto& item = pair.second;
            SHAMapItem::ref a = item.first;
            SHAMapItem::ref b = item.second;
            SLE::pointer sle_a;
            SLE::pointer sle_b;

            if (a != nullptr) {
                sle_a = (std::make_shared<SLE>(a->peekSerializer (),
                                               a->getTag()));
                // Skip Lists change every ledger
                if (sle_a ->getType() == ltLEDGER_HASHES) continue;
            }

            if (b != nullptr) {
                sle_b = (std::make_shared<SLE>(b->peekSerializer (),
                                               b->getTag()));
                if (sle_b ->getType() == ltLEDGER_HASHES) continue;
            }

            if(a != nullptr && b != nullptr)
            {
                assert (a->getTag() == b->getTag());
                bool equal = false;

                if ((sle_a -> getType()) == ltDIR_NODE)
                {
                    std::set<uint256> indA, indB;

                    for(auto& h : sle_a->getFieldV256(sfIndexes)) indA.insert(h);
                    for(auto& h : sle_b->getFieldV256(sfIndexes)) indB.insert(h);

                    if (indA == indB)
                    {
                        n_deltas--;
                        equal = true;
                    }
                }

                if (!equal)
                {
                    std::cout << "historical: " << sle_a->getJson(0) << std::endl;
                    std::cout << "replayed: " << sle_b->getJson(0) << std::endl;
                }
            }

            if (a != nullptr && b == nullptr)
            {
                std::cout << "historical: " << sle_a->getJson(0) << std::endl;
                std::cout << "replayed: missing" << std::endl;
            }

            if (b != nullptr && a == nullptr)
            {
                std::cout << "historical: missing" << std::endl;
                std::cout << "replayed: " << sle_b->getJson(0) << std::endl;
            }

        }

        if (failed && n_deltas == 0)
        {
            failed = false;
            dirs_only++;
        }
        if (failed)
        {
            errors_by_type[tx.getTxnType()]++;
            failed_txs++;
            std::cout << "--------------------------------------------------------" << std::endl;
        }
    }
};

void processHistoricalTransactions()
{
    auto t = beast::Time::getCurrentTime();

    HistoryLoader hl( &std::cin );
    HistoryReplayer hr (hl);
    hr.process();

    std::cout << "total transactions:  " << hr.total_txs << std::endl;
    std::cout << "meta_equal:          " << hr.meta_equal << std::endl;
    std::cout << "state_equal:         " << hr.state_equal << std::endl;
    std::cout << "dirs_only_unequal:   " << hr.dirs_only << std::endl;
    std::cout << "failed transactions: " << hr.failed_txs << std::endl;

    std::cout << "\nErrors by type: " << std::endl;
    for(auto& pair : hr.errors_by_type)
    {
        std::cout << transactionTypeHuman(pair.first) << ": "
                                << pair.second
                  << std::endl;
    }

    std::cout << "\nTransactions by type: " << std::endl;
    for(auto& pair : hr.txns_by_type)
    {
        std::cout << transactionTypeHuman(pair.first) << ": "
                                << pair.second
                  << std::endl;
    }

    std::cout << "took ms: " << (beast::Time::getCurrentTime() - t)
                                .inMilliseconds() << std::endl;

}