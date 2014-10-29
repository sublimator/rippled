// -----------------------------------------------------------------------------

typedef std::map<uint256, SLE::pointer> SLEMap;
// use &(sfCreatedNode/sfModifiedNode/sfDeletedNode) in lieu of some new enum
typedef std::pair<SField::ptr, SLE::pointer> AffectedNode;

// -----------------------------------------------------------------------------

uint256 offerOwnerDirectory(SLE::ref offer)
{
    RippleAddress account (offer->getFieldAccount(sfAccount));
    uint256 ownerDir (Ledger::getOwnerDirIndex(account.getAccountID()));
    return Ledger::getDirNodeIndex(ownerDir,
                                   offer->getFieldU64(sfOwnerNode));
}

uint256 offerBookDirectory(SLE::ref offer)
{
    uint256 bookDir (offer->getFieldH256(sfBookDirectory));
    return Ledger::getDirNodeIndex(bookDir, offer->getFieldU64(sfBookNode));
}

uint256 trustlineDirectory(SLE::ref line,
                           SField::ref limitField,
                           SField::ref nodeField)
{
    Account account (line->getFieldAmount(limitField).issue().account);
    uint256 ownerDir (Ledger::getOwnerDirIndex(account));
    return Ledger::getDirNodeIndex(ownerDir, line->getFieldU64(nodeField));
}

std::vector<uint256> offerDirectoryIndexes(SLE::ref offer)
{
    std::vector<uint256> ret;
    ret.push_back(offerOwnerDirectory(offer));
    ret.push_back(offerBookDirectory(offer));
    return ret;
}

std::vector<uint256> lineDirectoryIndexes(SLE::ref line)
{
    std::vector<uint256> ret;
    ret.push_back(trustlineDirectory(line, sfLowLimit, sfLowNode));
    ret.push_back(trustlineDirectory(line, sfHighLimit, sfHighNode));
    return ret;
}
// -----------------------------------------------------------------------------

class ExtrapolatedMetaData
{
public:
    std::set<uint256> removed;
    SLEMap updated;
    SLEMap added;

    // This is a light wrapper around SerializedTransaction, with ledgerIndex
    // field.
    Transaction::pointer txn;
    TransactionMetaSet::pointer meta;
    // Must be exactly as just before this transaction was applied.
    // This will not mutate the shamap.
    SHAMap::pointer state;

    // The SHAMap stores only bytes in the leaves, not SLE::pointers. If we were
    // serializing and reserializing each time things could get mighty slow.
    // TODO: test this assumption.
    std::shared_ptr<SLEMap> stateCache;

    ExtrapolatedMetaData( Transaction::pointer txn_,
                          TransactionMetaSet::pointer meta_,
                          SHAMap::ref accountState,
                          std::shared_ptr<SLEMap> cache):
                          txn(txn_),
                          meta(meta_),
                          state(accountState),
                          stateCache(cache)
    {
        // Doing work in the constructor, naughty ...
        // TODO: MetaDataExtrapolater makes -> ExtrapolatedMetaData
        process();
    }

    bool process() {
        std::vector<AffectedNode> finalNodes;
        collapseAndExtrapolateAffectedNodes(finalNodes);

        // Now we need to maintain the indexes.
        for(auto& pair: finalNodes)
        {
            SLE::ref sle      =  pair.second;
            SField::ref field = *pair.first;
            auto let = sle->getType();
            uint256 sleIndex = sle->getIndex();

            // TODO: Later we'll have a proliferation of ledger entry types so
            // it would be nice to have this mess a bit more maintainable.
            if (field == sfCreatedNode)
            {
                // TODO: extract addToDirectories() method
                // Add the Offers/Trusts to the DirectoryNode indexes
                if (let == ltOFFER || let == ltRIPPLE_STATE)
                {
                    auto indexes = let == ltOFFER ?
                                                offerDirectoryIndexes(sle):
                                                lineDirectoryIndexes(sle);
                    for (uint256& index : indexes)
                    {
                        getDirectoryIndexes(index).push_back(sleIndex);
                    }
                }
            }
            else if (field == sfDeletedNode)
            {
                // TODO: extract removeFromDirectories() method
                // Remove the Offers/Trusts to the DirectoryNode indexes
                if (sle->getType() == ltOFFER)
                {
                    auto indexes = offerDirectoryIndexes(sle);
                    unstableRemoveFromIndexes(indexes.at(0), sleIndex);
                    stableRemoveFromIndexes(indexes.at(1), sleIndex);
                }
                else if (sle->getType() == ltRIPPLE_STATE)
                {
                    for (uint256& index : lineDirectoryIndexes(sle))
                    {
                        unstableRemoveFromIndexes(index, sle->getIndex());
                    }
                }
            }
        }
        return true;
    }

    void removeFromIndexes(const uint256& dirNodeIndex,
                           const uint256& index,
                           bool stable)
    {
        std::vector<uint256>& v = getDirectoryIndexes(dirNodeIndex);
        auto iter = std::find(v.begin(), v.end(), index);
        if (iter != v.end())
        {
            if (stable)
            {
                v.erase(iter);
            }
            else
            {
                *iter = v[v.size () - 1];
                v.pop_back();
            }
        }
    }

    void unstableRemoveFromIndexes(const uint256& dirNodeIndex,
                                   const uint256& index)
    {
        removeFromIndexes(dirNodeIndex, index, false);
    }

    void stableRemoveFromIndexes(const uint256& dirNodeIndex,
                                 const uint256& index)

    {
        removeFromIndexes(dirNodeIndex, index, true);
    }

    // return a mutable reference to a given DirectoryNode's Indexes
    std::vector<uint256>& getDirectoryIndexes(const uint256& dirNodeIndex)
    {
        // STVector256 svIndexes = getSLE(dirNodeIndex)->getFieldV256(sfIndexes);
        // Above is a copy, but we want a reference.

        // This is done for performance/convenience reasons.
        // Hey, getPField is a public API, get off my back!
        // Or maybe I'm just confused about const REF& ?? There's an API to get
        // one of those guys, but he was lazy to work.

        STVector256& svIndexes (*dynamic_cast<STVector256*>(
                    getSLE(dirNodeIndex)->getPField(sfIndexes)));
        std::vector<uint256>&  vuiIndexes  = svIndexes.peekValue ();
        return vuiIndexes;
    }

    void updateDefaults(STObject& created)
    {
        // TODO: Use the KnownFomats to do this programmatically, in a
        // maintainable way :) Later we'll have a proliferation of ledger entry
        // types.

        LedgerEntryType let(ledgerEntryType(created));

        ensureDefault(created, sfFlags);
        switch (let)
        {
            case ltDIR_NODE:
                ensureDefault(created, sfIndexes);
                if (created.isFieldPresent(sfExchangeRate))
                {
                    ensureDefault(created, sfTakerGetsCurrency);
                    ensureDefault(created, sfTakerGetsIssuer);
                    ensureDefault(created, sfTakerPaysIssuer);
                    ensureDefault(created, sfTakerPaysCurrency);
                }
                break;
            case ltRIPPLE_STATE:
                ensureDefault(created, sfHighNode);
                ensureDefault(created, sfLowNode);
                break;
            case ltOFFER:
                ensureDefault(created, sfOwnerNode);
                ensureDefault(created, sfBookNode);
                break;
            case ltACCOUNT_ROOT:
                ensureDefault(created, sfOwnerCount);
                break;
            default:
                break;
        }
    }

    void ensureDefault(STObject& created, SField::ref field)
    {
        if(!created.isFieldPresent(field))
        {
            created.giveObject (created.makeDefaultObject (field));
        }
    }

    void collapseAndExtrapolateAffectedNodes(std::vector<AffectedNode>& to)
    {
        for(auto& affected : meta->getNodes()) {
            SLE::pointer node (finalNode(affected));
            to.push_back(std::make_pair(&affected.getFName(), node));

            auto pair (std::make_pair(node->getIndex(), node));

            if (affected.getFName() == sfModifiedNode)
            {
                updated.insert(pair);
            }
            else if (affected.getFName() == sfDeletedNode)
            {
                removed.insert(node->getIndex());
            }
            else if (affected.getFName() == sfCreatedNode)
            {
                added.insert(pair);
            }
        }
    }

    LedgerEntryType ledgerEntryType(STObject& from)
    {
        return static_cast<LedgerEntryType>(from.getFieldU16 (sfLedgerEntryType));
    }

    SLE::pointer getSLE(const uint256& index) {
        SLEMap& cache = *stateCache;
        SLE::pointer existing;

        if (cache.count(index) == 0)
        {
            SHAMapItem::pointer item = state->peekItem (index);
            assert(item != nullptr);
            existing = std::make_shared<SLE>(item->peekSerializer (),
                                             item->getTag ());
            cache[index] = existing;
        }
        else
        {
            existing = cache[index];
        }

        assert(existing != nullptr);
        return existing;
    }

    SLE::pointer overlayOnExisting(uint256& index, STObject& merged)
    {
        // An SLE can only be made from an STObject that is valid for the
        // LedgerEntryType associated KnownFormat, thus we know we can just hack
        // the internals, there will always be an existing slot in the vector to
        // replace the field.

        SLE::pointer existing (getSLE(index));
        auto& data = existing->peekData();
        for (auto const& field: merged)
        {
            int ix = existing->getFieldIndex(field.getFName());
            // TODO, somehow take ownership of these elements
            data.replace(ix, field.clone().release());
        }

        return existing;
    }

    void updateThreading(STObject& merged)
    {
        merged.setFieldH256 (sfPreviousTxnID, txn->getID());
        merged.setFieldU32 (sfPreviousTxnLgrSeq, txn->getLedger());
    }

    bool isThreadedType(LedgerEntryType let)
    {
        return let != ltDIR_NODE;
    }

    void flattenAffectedToFinal(STObject& out, STObject& affected)
    /*
    {
        "ModifiedNode" :
        {
          "FinalFields" :
          {
            "Account" : "rM3X3QSr8icjTGpaF52dozhbT2BZSXJQYM",
            "Balance" : "54249433700",
            "Flags" : 0,
            "OwnerCount" : 90,
            "Sequence" : 43492
          },
          "LedgerEntryType" : "AccountRoot",
          "LedgerIndex" : "F13BE615EDDC53504C862D741B0E1DD42B90AF5C2C4FB1F077B5C2C0BC0F41EB",
          "PreviousFields" :
          {
            "Balance" : "54249433715",
            "Sequence" : 43491
          },
          "PreviousTxnID" : "3752DDAADD8995E42C4687B061CCD2EB353716001905113E7686CB654FED8586",
          "PreviousTxnLgrSeq" : 6220879
        }
    }

        Becomes:

    {
      "Account" : "rM3X3QSr8icjTGpaF52dozhbT2BZSXJQYM",
      "Balance" : "54249433700",
      "Flags" : 0,
      "OwnerCount" : 90,
      "Sequence" : 43492
      "LedgerEntryType" : "AccountRoot",
      "PreviousTxnID" : "3752DDAADD8995E42C4687B061CCD2EB353716001905113E7686CB654FED8586",
      "PreviousTxnLgrSeq" : 6220879
    }

        Note that we discard the `LedgerIndex` and keep only final fields in
        `sfNewFields` and `sfFinalFields`
    */
    {
        for (auto const& it: affected)
        {
            SField::ref field = it.getFName();
            if (it.getSType () != STI_NOTPRESENT && field != sfLedgerIndex)
            {
                if (it.getSType () == STI_OBJECT)
                {
                    if (field == sfNewFields ||
                        field == sfFinalFields )
                    {
                        const STObject* o (dynamic_cast<const STObject*> (&it));
                        for (auto const& it2: *o)
                        {
                            // TODO, somehow take ownership of these elements
                            out.addObject(it2);
                        }
                    }
                }
                else
                {
                    out.addObject(it);
                }
            }
        }
    }

    SLE::pointer finalNode(STObject& affected) {
        STObject final;

        // sfModifiedNode / sfCreatedNode / sfDeletedNode
        SField::ref affectedType (affected.getFName());
        uint256 index (affected.getFieldH256(sfLedgerIndex));
        LedgerEntryType let (ledgerEntryType(affected));

        bool newNode = affectedType == sfCreatedNode;
        bool wasExistingNode = !newNode;
        bool notDeleted = affectedType != sfDeletedNode;

        flattenAffectedToFinal(final, affected);

        if (isThreadedType(let) && notDeleted)
        {
            updateThreading(final);
        }
        if (newNode)
        {
            updateDefaults(final);
        }

        if (wasExistingNode)
        {
            return overlayOnExisting(index, final);
        }
        else
        {
            return (*stateCache)[index] = std::make_shared<SLE>(final, index);
        }
    }
};
