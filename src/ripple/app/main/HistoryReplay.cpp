/*#include <BeastConfig.h>

#include <ripple/unity/app.h>
#include <ripple/unity/net.h>
#include <ripple/unity/rpcx.h>
#include <ripple/unity/websocket.h>
#include <ripple/unity/resource.h>
#include <ripple/unity/sitefiles.h>

#include <ripple/http/Server.h>
#include <fstream>

#include <ripple/rpc/impl/JsonObject.h>
#include <ripple/rpc/impl/JsonWriter.h>

namespace ripple {
*/

// -----------------------------------------------------------------------------

static int maxDeltas (2147483648); // 2 ^ 31

typedef std::map<uint256, std::pair<SLE::pointer, SLE::pointer>> SLEShaMapDelta;

typedef std::function<void (
                    // OpenLedger immutable
                    Ledger::ref,
                    // Tx Meta Applied immutable
                    Ledger::ref,
                    // Txn hash/id
                    uint256&,
                    // TransactionIndex
                    std::uint32_t,
                    // Txn raw bytes
                    Blob&,
                    // Meta raw bytes
                    Blob&)> OnTransaction;

// -----------------------------------------------------------------------------

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
        bool ret = readBytesTo(&a, 1);
        if(ret) out = a;
        return ret;
    }

    bool readUInt32(std::uint32_t& out)
    {
        char a[4];
        bool ret = readBytesTo(a, 4);
        if(ret) out = a[0] >> 24 | a[1] >> 16 | a[2] >> 8 | a[3];
        return ret;
    }

    bool readHash256(uint256& index)
    {
        return readBytesTo(index.begin(), 32);
    }

    template<typename T>
    bool readBytesTo(T to, size_t n) {
        return static_cast<bool> (stream().read((char*) to,  n));
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
        return readBytesTo(&header[0], headerSize);
    }

    bool readVlObject(Blob& object)
    {
        size_t vl;
        if (!readVlLength(vl)) return false;
        object = Blob(vl);
        return readBytesTo(&object[0], vl);
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

std::string transactionTypeHuman(TxType tt)
{
    return TxFormats::getInstance().findByType(tt)->getName();
}

void transactionTypeStats (std::map<TxType, int>& stats,
                           std::string name,
                           Json::Value& json)
{
    Json::Value& to = json[name] = Json::Value(Json::objectValue);
    for(auto& pair : stats)
    {
        to[transactionTypeHuman(pair.first)] = pair.second;
    }
}

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

    bool readTransactions(OnTransaction& onTransaction)
    {
        bool ret = true;

        int transactionIndex = 0;
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

            onTransaction(b4Tx, snapShot, index, transactionIndex, tx, meta);
            transactionIndex++;
        }
        return true;
    }

    bool readAccountState()
    {
        SHAMap::ref accountState (snapShot->peekAccountStateMap());

        while (nextFrame() == Frame::indexedLedgerEntry)
        {
            if(!readLedgerEntryIntoSHAMap(accountState)) return false;
        }
        return true;
    }

    bool loadAccountStateDelta(SHAMap::ref accountState)
    {
        #define aborting(a) if (!(a)) {return false;}

        std::uint32_t modded, deleted, added;

        aborting(readUInt32(modded));
        for (int i = 0; i < modded; ++i)
        {
            aborting(readUpdatedLedgerEntryIntoSHAMap(accountState));
        }

        aborting(readUInt32(deleted));
        for (int i = 0; i < deleted; ++i)
        {
            uint256 index;
            aborting(readHash256(index));
            aborting(accountState->delItem(index));
        }

        aborting(readUInt32(added));
        for (int i = 0; i < added; ++i)
        {
            aborting(readLedgerEntryIntoSHAMap(accountState));
        }

        #undef aborting
        return true;
    }

    void readyMapsForModifying(Ledger::ref ledger)
    {
        ledger->peekAccountStateMap()->clearSynching();
        ledger->peekTransactionMap()->clearSynching();
    }

    bool parse(OnTransaction onTransaction)
    {
        while (!eof ())
        {
            if (!parseNext(onTransaction)) return false;
        }

        return true;
    }

    bool parseNext(OnTransaction onTransaction)
    {
        uint256 expectedTransHash;
        uint256 expectedAccountHash;
        Blob header;

        if (!(readLedgerHeader (header)))
        {
            return eof ();
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

        readyMapsForModifying(snapShot);

        if (nextFrame() == Frame::accountStateTree)
        {
            if (!(readAccountState()))
            {
                assert(false);
                return false;
            }
        }

        snapShot->setImmutable();
        if (!(readTransactions(onTransaction)))
        {
            assert(false);
            return false;
        }

        snapShot = std::make_shared<Ledger> (std::ref(*snapShot), true);
        if (!(loadAccountStateDelta(snapShot->peekAccountStateMap())))
        {
            assert(false);
            return false;
        }

        snapShot->setImmutable();
        historicalState = snapShot->peekAccountStateMap();

        getTreeHashesFrom (header, expectedTransHash, expectedAccountHash);
        assert (snapShot->getTransHash() == expectedTransHash);
        assert (snapShot->getAccountHash() == expectedAccountHash);

        return (snapShot->getTransHash() == expectedTransHash &&
                snapShot->getAccountHash() == expectedAccountHash);
    }
};


struct TransactionLedgers {
    Ledger::pointer beforeTx,
                    afterTx,
                    afterHistorical;

    TransactionLedgers(Ledger::ref a, Ledger::ref b, Ledger::ref c) :
                       beforeTx(a), afterTx(b), afterHistorical(c) {}

    void resultDelta(SHAMap::Delta& delta)
    {
        afterHistorical->peekAccountStateMap()->compare (
                    afterTx->peekAccountStateMap(), delta, maxDeltas);
    }

    void historicalDelta(SHAMap::Delta& delta)
    {
        beforeTx->peekAccountStateMap()->compare (
                    afterHistorical->peekAccountStateMap(), delta, maxDeltas);
    }
};


SerializedTransaction::pointer transactionFromBlob(Blob& tx)
{
    Serializer serializer (tx);
    SerializerIterator sit (serializer);
    return std::make_shared<SerializedTransaction> (sit);
}

bool getMetaBlob(Ledger::ref ledger, uint256& txid,  Blob& meta)
{
    auto item = ledger->peekTransactionMap()->peekItem (txid);
    if (item == nullptr)
    {
        return false;
    }
    else
    {
        SerializerIterator it (item->peekSerializer ());
        it.getVL (); // skip transaction
        meta = it.getVL();
        return true;
    }
}

bool differenceIsOnlyOrderOfIndexes(SLE::ref a, SLE::ref b)
{
    for (auto const& obj : *a)
    {
        if (obj.getFName () == sfIndexes) {
            continue;
        }
        if (!b->hasMatchingEntry (obj)) {
            return false;
        }
    }

    auto& aIndexes (a->getFieldV256(sfIndexes));
    auto& bIndexes (b->getFieldV256(sfIndexes));

    if (aIndexes.size() != bIndexes.size())
    {
        return false;
    }

    std::set<uint256> indA, indB;
    for(auto& h : aIndexes) indA.insert(h);
    for(auto& h : bIndexes) indB.insert(h);

    return indA == indB;
}

size_t filterDeltas (SHAMap::Delta& deltas, SLEShaMapDelta& filteredDeltas) {

    for (auto&  pair : deltas) {
        auto& item = pair.second;
        SHAMapItem::ref a = item.first;
        SHAMapItem::ref b = item.second;
        SLE::pointer sle_a;
        SLE::pointer sle_b;

        if (a != nullptr) {
            sle_a = (std::make_shared<SLE>(a->peekSerializer (),
                                           a->getTag()));
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
                if (differenceIsOnlyOrderOfIndexes(sle_a, sle_b))
                {
                    equal = true;
                }
            }

            if (!equal)
            {
                filteredDeltas[pair.first] = std::make_pair(sle_a, sle_b);
            }
        }
        else {
            filteredDeltas[pair.first] = std::make_pair(sle_a, sle_b);
        }
    }

    return filteredDeltas.size();
}


class HistoryReplayer
{
public:
    std::map<uint256, TER> unapplied;
    std::map<TxType, int> errorsBytype;
    std::map<TxType, int> txnsByType;

    HistoryLoader hl;

    Json::Value report;
    Json::Value errorsReport;

    int totalTxns = 0,
        failedTxns = 0,
        metaEqual = 0,
        stateEqual = 0,
        // Only ordering of Indexes are different in directory nodes
        effectivelyEqual = 0;

    HistoryReplayer(HistoryLoader& hl_) :
        hl(hl_),
        report(Json::objectValue),
        errorsReport(Json::objectValue)
    {

    }

    void process() {
        hl.parse([&]( Ledger::ref beforeTransactionApplied,
                      Ledger::ref afterHistoricalResult,
                      uint256& txid,
                      std::uint32_t transactionIndex,
                      Blob& tx,
                      Blob& meta )
        {

            // Create a snaphot of the ledger
            Ledger::pointer replayLedger (
                    std::make_shared<Ledger> (
                        std::ref(*beforeTransactionApplied), true) );

            TransactionEngine engine (replayLedger);

            bool applied;
            SerializedTransaction::pointer st (transactionFromBlob(tx));
            TER result (engine.applyTransaction (*st,
                                                 tapNO_CHECK_SIGN,
                                                 applied));

            replayLedger->setImmutable();
            if (applied)
            {
                totalTxns++;

                Blob reMeta;
                getMetaBlob(replayLedger, txid, reMeta);

                TransactionLedgers tl (beforeTransactionApplied,
                                       replayLedger, // after Transaction
                                       afterHistoricalResult);

                onTransactionApplied ( tl, txid, transactionIndex,
                                      *st, meta, reMeta );
            }
            else
            {
                unapplied.insert(std::make_pair(txid, result));
            }

        });
    }

    ~HistoryReplayer() {};

    void prepareReport() {
        report["errors"] =  errorsReport;
        Json::Value& stats   =  report["stats"] =
                                Json::Value(Json::objectValue);

        stats["total_transactions"] = totalTxns;
        stats["meta_equal"] = metaEqual;
        stats["state_equal"] = stateEqual;
        stats["state_effectively_equal"] = effectivelyEqual;
        stats["failed_transactions"] = failedTxns;
        stats["unapplied"] = static_cast<std::uint32_t>(unapplied.size());

        transactionTypeStats(errorsBytype, "errors_by_type", stats);
        transactionTypeStats(txnsByType, "txns_by_type", stats);
    }

    void onTransactionApplied (
                  TransactionLedgers& tl,
                  uint256 txid,
                  std::uint32_t transactionIndex,
                  SerializedTransaction& tx,
                  Blob& meta,
                  Blob& reMeta)
    {

        std::cout << ".";
        txnsByType[tx.getTxnType()]++;

        SHAMap::Delta deltas;
        tl.resultDelta(deltas);

        bool failed = true;
        bool metaIsEqual = false;

        if (meta == reMeta)
        {
            metaIsEqual=true;
            metaEqual++;
            failed = false;
        }
        if (deltas.size() == 0)
        {
            stateEqual++;
            failed = false;
        }

        SLEShaMapDelta filteredDeltas;
        if (failed && filterDeltas(deltas, filteredDeltas) == 0)
        {
            failed = !metaIsEqual;
            effectivelyEqual++;
        }
        if (failed)
        {
            failedTxns++;
            errorsBytype[tx.getTxnType()]++;

            Json::Value error (Json::objectValue);
            error["tx_json"] = tx.getJson(0);

            Json::Value&
                deltasJson = error["deltas"] = Json::Value(Json::arrayValue);

            for(auto& pair: filteredDeltas)
            {
                SLE::ref h (pair.second.first),
                         r (pair.second.second);

                Json::Value& delta = deltasJson.append (Json::objectValue);

                delta["historical"] = h ? h->getJson(0): "missing";
                delta["replayed"] = r ? r->getJson(0): "missing";
            }

            auto metaJson = [&](Json::Value& j, Blob& m) {
                j = TransactionMetaSet (txid,
                                        tl.beforeTx->getLedgerSeq(),
                                        m).getJson(0); };

            metaJson(error["historical_meta"], meta);
            metaJson(error["replayed_meta"], reMeta);

            errorsReport[to_string(txid)] = error;
        }
    }
};

void processHistoricalTransactions()
{
    auto t = beast::Time::getCurrentTime();

    HistoryLoader hl( &std::cin );
    HistoryReplayer hr (hl);
    hr.process();
    hr.prepareReport();

    static std::string reportName ("replay-report.json");

    std::ofstream ofs (reportName, std::ofstream::out);

    Json::StyledStreamWriter writer;
    writer.write (ofs, hr.report);

    std::cout << ("\n\n"  "Wrote report to $CWD/") << reportName << std::endl;
    std::cout << "Reprocessing took ms: " << (beast::Time::getCurrentTime() - t)
                                              .inMilliseconds() << std::endl;

    writer.write(std::cout, hr.report["stats"]);
}

// } //  </namespace:ripple>