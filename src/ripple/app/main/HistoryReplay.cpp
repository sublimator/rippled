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

std::string transactionTypeHuman(TxType tt)
{
    return TxFormats::getInstance().findByType(tt)->getName();
}

void transactionTypeStats (std::map<TxType, int> stats,
                           std::string name,
                           Json::Value& json)
{
    Json::Value& to = json["name"] = Json::Value(Json::objectValue);
    for(auto& pair : stats)
    {
        to[transactionTypeHuman(pair.first)] = pair.second;
    }
}

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

        int transaction_index = 0;
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

            onTransaction(b4Tx, snapShot, index, transaction_index, tx, meta);
            transaction_index++;
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
    bool parse(OnTransaction onTransaction)
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
            if (!(ret = readTransactions(onTransaction)))
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
    // TODO, check everything else is actually equal!!!!

    // ordered set
    std::set<uint256> indA, indB;

    for(auto& h : a->getFieldV256(sfIndexes)) indA.insert(h);
    for(auto& h : b->getFieldV256(sfIndexes)) indB.insert(h);

    return indA == indB;
}

class HistoryReplayer
{
public:
    std::set<uint256> unapplied;
    std::map<TxType, int> errors_by_type;
    std::map<TxType, int> txns_by_type;

    HistoryLoader hl;

    Json::Value rootReport;
    Json::Value errorsReport;

    int total_txs   = 0,
        failed_txs  = 0,
        meta_equal  = 0,
        state_equal = 0,
        dirs_only   = 0;

    HistoryReplayer(HistoryLoader& hl_) :
        hl(hl_),
        rootReport(Json::objectValue),
        errorsReport(Json::objectValue)
    {

    }

    void process() {
        hl.parse([&]( Ledger::ref beforeTransactionApplied,
                      Ledger::ref afterHistoricalResult,
                      uint256& txid,
                      std::uint32_t transaction_index,
                      Blob& tx,
                      Blob& meta )
        {

            // Create a snaphot of the ledger
            Ledger::pointer ledger (
                    std::make_shared<Ledger> (
                        std::ref(*beforeTransactionApplied), true) );

            TransactionEngine engine (ledger);

            bool applied;
            SerializedTransaction::pointer st (transactionFromBlob(tx));
            (void) engine.applyTransaction (*st,
                                            // tapNONE means we should have metadata
                                            tapNONE | tapNO_CHECK_SIGN,
                                            applied);

            ledger->setImmutable();
            if (applied)
            {
                total_txs++;

                Blob reMeta;
                getMetaBlob(ledger, txid, reMeta);

                TransactionLedgers tl (beforeTransactionApplied,
                                       ledger, // after Transaction
                                       afterHistoricalResult);

                onTransactionApplied(tl, txid, transaction_index, *st, meta, reMeta);
            }
            else
            {
                unapplied.insert(txid); // TODO
            }

        });
    }

    ~HistoryReplayer() {};

    void prepareReport() {
        rootReport["errors"] =  errorsReport;
        Json::Value& stats = rootReport["stats"] = Json::Value(Json::objectValue);

        stats["total_transactions"]  = total_txs;
        stats["meta_equal"]          = meta_equal;
        stats["state_equal"]         = state_equal;
        stats["dirs_only_unequal"]   = dirs_only;
        stats["failed_transactions"] = failed_txs;

        transactionTypeStats(errors_by_type, "errors_by_type", stats);
        transactionTypeStats(txns_by_type, "txns_by_type", stats);
    }

    void onTransactionApplied (
                  TransactionLedgers& tl,
                  uint256 txid,
                  std::uint32_t transaction_index,
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
        std::map<uint256, std::pair<SLE::pointer, SLE::pointer>> filteredDeltas;

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
                        n_deltas--;
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

        if (failed && n_deltas == 0)
        {
            failed = false;
            dirs_only++;
        }
        if (failed)
        {
            errors_by_type[tx.getTxnType()]++;
            failed_txs++;

            Json::Value deltasJson (Json::arrayValue);
            Json::Value error (Json::objectValue);

            // just using an ordered map for shits and giggles (inherent ordering)
            for(auto& pair: filteredDeltas)
            {

                // TODO: Why didn't this work? Segfaulted
                // SLE::pointer historical (pair.second.first);
                // SLE::pointer replayed (pair.second.second);

                Json::Value delta (Json::objectValue);

                if (pair.second.first)
                {
                    // TODO: To just use hex or not??
                    delta["historical"] = pair.second.first->getJson(0);
                }
                else {
                    delta["historical"] = "missing";
                }
                if (pair.second.second)
                {
                    delta["replayed"] = pair.second.second->getJson(0);
                }
                else {
                    delta["replayed"] = "missing";
                }

                deltasJson.append(delta);
            }

            error["tx_json"] = tx.getJson(0); // is this a copy ???
            error["deltas"] = deltasJson;

            // This is pretty damn slow ...
            // error["historical_meta"] = TransactionMetaSet (txid, tl.beforeTx->getLedgerSeq(), meta).getJson(0);
            // error["replayed_meta"] = TransactionMetaSet (txid, tl.beforeTx->getLedgerSeq(), reMeta).getJson(0);

            error["historical_meta"] = strHex(meta);
            error["replayed_meta"] = strHex(reMeta);

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

    Json::StyledStreamWriter writer;
    writer.write (std::cout, hr.rootReport);

    std::cout << "took ms: " << (beast::Time::getCurrentTime() - t)
                                .inMilliseconds() << std::endl;

}

// } //  </namespace:ripple>