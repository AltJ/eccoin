// This file is part of the Eccoin project
// Copyright (c) 2020 The Eccoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "requestmanager.h"

#include "connman.h"

extern std::atomic<int> nPreferredDownload;

extern std::atomic<bool> fImporting;
extern std::atomic<bool> fReindex;

extern bool AlreadyHave(const CInv &inv);

/** Find the last common ancestor two blocks have.
 *  Both pa and pb must be non-NULL. */
CBlockIndex *LastCommonAncestor(CBlockIndex *pa, CBlockIndex *pb)
{
    if (pa->nHeight > pb->nHeight)
    {
        pa = pa->GetAncestor(pb->nHeight);
    }
    else if (pb->nHeight > pa->nHeight)
    {
        pb = pb->GetAncestor(pa->nHeight);
    }

    while (pa != pb && pa && pb)
    {
        pa = pa->pprev;
        pb = pb->pprev;
    }

    // Eventually all chain branches meet at the genesis block.
    assert(pa == pb);
    return pa;
}

bool CRequestManager::AlreadyAskedForBlock(const uint256 &hash)
{
    return (mapBlocksInFlight.count(hash) > 0);
}

CNodeState *CRequestManager::_GetNodeState(const NodeId id)
{
    std::map<NodeId, CNodeState>::iterator it = mapNodeState.find(id);
    if (it == mapNodeState.end())
    {
        return nullptr;
    }
    return &it->second;
}


// TODO : Should remove entrys for pnode from these maps when it is deleted
void CRequestManager::InitializeNodeState(const CNode *pnode)
{
    WRITELOCK(cs_requestmanager);
    mapNodeState.emplace_hint(mapNodeState.end(), std::piecewise_construct, std::forward_as_tuple(pnode->GetId()),
        std::forward_as_tuple(pnode->addr, pnode->GetAddrName()));
    mapNumBlocksInFlight.emplace(pnode->GetId(), 0);
}

void CRequestManager::RemoveNodeState(const NodeId id)
{
    WRITELOCK(cs_requestmanager);
    mapNodeState.erase(id);
}

/** Check whether the last unknown block a peer advertized is not yet known. */
void CRequestManager::_ProcessBlockAvailability(NodeId nodeid)
{
    RECURSIVEREADLOCK(pnetMan->getChainActive()->cs_mapBlockIndex);
    AssertWriteLockHeld(cs_requestmanager);

    std::map<NodeId, CNodeState>::iterator iter = mapNodeState.find(nodeid);
    assert(iter != mapNodeState.end());
    CNodeState* state = &iter->second;

    if (!state->hashLastUnknownBlock.IsNull())
    {
        CBlockIndex *pindex = pnetMan->getChainActive()->LookupBlockIndex(state->hashLastUnknownBlock);
        if (pindex && pindex->nChainWork > 0)
        {
            if (state->pindexBestKnownBlock == NULL || pindex->nChainWork >= state->pindexBestKnownBlock->nChainWork)
            {
                state->pindexBestKnownBlock = pindex;
            }
            state->hashLastUnknownBlock.SetNull();
        }
    }
}

/** Check whether the last unknown block a peer advertized is not yet known. */
void CRequestManager::ProcessBlockAvailability(NodeId nodeid)
{
    RECURSIVEREADLOCK(pnetMan->getChainActive()->cs_mapBlockIndex);
    WRITELOCK(cs_requestmanager);

    std::map<NodeId, CNodeState>::iterator iter = mapNodeState.find(nodeid);
    assert(iter != mapNodeState.end());
    CNodeState* state = &iter->second;

    if (!state->hashLastUnknownBlock.IsNull())
    {
        CBlockIndex *pindex = pnetMan->getChainActive()->LookupBlockIndex(state->hashLastUnknownBlock);
        if (pindex && pindex->nChainWork > 0)
        {
            if (state->pindexBestKnownBlock == NULL || pindex->nChainWork >= state->pindexBestKnownBlock->nChainWork)
            {
                state->pindexBestKnownBlock = pindex;
            }
            state->hashLastUnknownBlock.SetNull();
        }
    }
}

// TODO : currently needs cs_mapBlockIndex before locking this, should fix that
void CRequestManager::UpdateBlockAvailability(NodeId nodeid, const uint256 &hash)
{
    CBlockIndex *pindex = pnetMan->getChainActive()->LookupBlockIndex(hash);
    ProcessBlockAvailability(nodeid);
    WRITELOCK(cs_requestmanager);
    std::map<NodeId, CNodeState>::iterator iter = mapNodeState.find(nodeid);
    assert(iter != mapNodeState.end());
    CNodeState* state = &iter->second;
    if (pindex && pindex->nChainWork > 0)
    {
        // An actually better block was announced.
        if (state->pindexBestKnownBlock == NULL || pindex->nChainWork >= state->pindexBestKnownBlock->nChainWork)
        {
            LogPrint("net", "updated peer %d best known block \n", nodeid);
            state->pindexBestKnownBlock = pindex;
        }
    }
    else
    {
        LogPrint("net", "updated peer %d hash last unknown block \n", nodeid);
        // An unknown block was announced; just assume that the latest one is the best one.
        state->hashLastUnknownBlock = hash;
    }
}

bool CRequestManager::PeerHasHeader(const NodeId nodeid, const CBlockIndex *pindex)
{
    READLOCK(cs_requestmanager);
    std::map<NodeId, CNodeState>::iterator iter = mapNodeState.find(nodeid);
    assert(iter != mapNodeState.end());
    CNodeState* state = &iter->second;
    if (state->pindexBestKnownBlock && pindex == state->pindexBestKnownBlock->GetAncestor(pindex->nHeight))
    {
        return true;
    }
    if (state->pindexBestHeaderSent && pindex == state->pindexBestHeaderSent->GetAncestor(pindex->nHeight))
    {
        return true;
    }
    return false;
}

void CRequestManager::MarkBlockAsInFlight(NodeId nodeid, const uint256 &hash, const CBlockIndex *pindex)
{
    // Make sure it's not listed somewhere already.
    MarkBlockAsReceived(hash);
    QueuedBlock newentry = {hash, pindex, pindex != nullptr};
    WRITELOCK(cs_requestmanager);
    mapBlocksInFlight[hash] = std::make_pair(nodeid, newentry);
    if (mapNumBlocksInFlight.count(nodeid) != 0)
    {
        mapNumBlocksInFlight[nodeid] += 1;
    }
}

void CRequestManager::UpdatePreferredDownload(CNode *node)
{
    WRITELOCK(cs_requestmanager);
    std::map<NodeId, CNodeState>::iterator iter = mapNodeState.find(node->GetId());
    assert(iter != mapNodeState.end());
    CNodeState* state = &iter->second;

    nPreferredDownload.fetch_sub(state->fPreferredDownload);

    // Whether this node should be marked as a preferred download node.
    // we allow downloads from inbound nodes; this may have been limited in the past to stop attackers from connecting
    // and offering a bad chain. However, we are connecting to multiple nodes and so can choose the most work
    // chain on that basis.
    state->fPreferredDownload = !node->fOneShot && !node->fClient;

    nPreferredDownload.fetch_add(state->fPreferredDownload);
}

// Returns a bool indicating whether we requested this block.
bool CRequestManager::MarkBlockAsReceived(const uint256 &hash)
{
    WRITELOCK(cs_requestmanager);
    std::map<uint256, std::pair<NodeId, QueuedBlock> >::iterator itInFlight = mapBlocksInFlight.find(hash);
    if (itInFlight != mapBlocksInFlight.end())
    {
        mapNumBlocksInFlight[itInFlight->second.first] -= 1;
        mapBlocksInFlight.erase(itInFlight);
        return true;
    }
    return false;
}

void CRequestManager::SetBestHeaderSent(NodeId nodeid, CBlockIndex* pindex)
{
    WRITELOCK(cs_requestmanager);
    std::map<NodeId, CNodeState>::iterator iter = mapNodeState.find(nodeid);
    assert(iter != mapNodeState.end());
    iter->second.pindexBestHeaderSent = pindex;
}

bool CRequestManager::GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats)
{
    READLOCK(cs_requestmanager);
    std::map<NodeId, CNodeState>::iterator iter = mapNodeState.find(nodeid);
    if(iter != mapNodeState.end())
    {
        return false;
    }
    CNodeState* state = &iter->second;

    stats.nSyncHeight = state->pindexBestKnownBlock ? state->pindexBestKnownBlock->nHeight : -1;
    stats.nCommonHeight = state->pindexLastCommonBlock ? state->pindexLastCommonBlock->nHeight : -1;
    for (const auto &queue : mapBlocksInFlight)
    {
        if (queue.second.first == nodeid)
        {
            if (queue.second.second.pindex)
            {
                stats.vHeightInFlight.push_back(queue.second.second.pindex->nHeight);
            }
        }
    }
    return true;
}

bool CRequestManager::GetPreferHeaders(CNode *node)
{
    READLOCK(cs_requestmanager);
    std::map<NodeId, CNodeState>::iterator iter = mapNodeState.find(node->GetId());
    assert(iter != mapNodeState.end());
    return iter->second.fPreferHeaders;
}

void CRequestManager::SetPreferHeaders(CNode *node)
{
    WRITELOCK(cs_requestmanager);
    std::map<NodeId, CNodeState>::iterator iter = mapNodeState.find(node->GetId());
    assert(iter != mapNodeState.end());
    iter->second.fPreferHeaders = true;
}

int CRequestManager::GetBlocksInFlight(NodeId nodeid)
{
    READLOCK(cs_requestmanager);
    std::map<NodeId, int16_t>::iterator iter = mapNumBlocksInFlight.find(nodeid);
    assert(iter != mapNumBlocksInFlight.end());
    return iter->second;
}

void CRequestManager::StartDownload(CNode* node)
{
    WRITELOCK(cs_requestmanager);
    std::map<NodeId, CNodeState>::iterator iter = mapNodeState.find(node->GetId());
    assert(iter != mapNodeState.end());
    CNodeState* state = &iter->second;

    // Download if this is a nice peer, or we have no nice peers and this one
    // might do.
    bool fFetch = state->fPreferredDownload || (nPreferredDownload.load() == 0 && !node->fOneShot);

    if (!state->fSyncStarted && !node->fClient && !fImporting && !fReindex)
    {
        if (fFetch ||
            pnetMan->getChainActive()->pindexBestHeader.load()->GetBlockTime() > GetAdjustedTime() - 24 * 60 * 60)
        {
            state->fSyncStarted = true;
            const CBlockIndex *pindexStart = pnetMan->getChainActive()->pindexBestHeader;
            /**
             * If possible, start at the block preceding the currently best
             * known header. This ensures that we always get a non-empty list of
             * headers back as long as the peer is up-to-date. With a non-empty
             * response, we can initialise the peer's known best block. This
             * wouldn't be possible if we requested starting at pindexBestHeader
             * and got back an empty response.
             */
            if (pindexStart->pprev)
            {
                pindexStart = pindexStart->pprev;
            }

            LogPrint("net", "initial getheaders (%d) to peer=%d (startheight:%d)\n", pindexStart->nHeight, node->id,
                node->nStartingHeight);
            g_connman->PushMessage(
                node, NetMsgType::GETHEADERS, pnetMan->getChainActive()->chainActive.GetLocator(pindexStart), uint256());
        }
    }
}

bool CRequestManager::IsBlockInFlight(const uint256 &hash)
{
    return mapBlocksInFlight.count(hash);
}

void CRequestManager::TrackTxRelay(const CTransaction &tx)
{
    CInv inv(MSG_TX, tx.GetId());
    LOCK(cs_mapRelay);
    // Expire old relay messages
    while (!vRelayExpiration.empty() && vRelayExpiration.front().first < GetTime())
    {
        mapRelay.erase(vRelayExpiration.front().second);
        vRelayExpiration.pop_front();
    }
    // Save original serialized message so newer versions are preserved
    auto ret = mapRelay.emplace(inv.hash, tx);
    if (ret.second)
    {
        vRelayExpiration.push_back(std::make_pair(GetTime() + 15 * 60, ret.first));
    }
}

bool CRequestManager::FindAndPushTx(CNode* node, const uint256 &hash)
{
    LOCK(cs_mapRelay);
    // Send stream from relay memory
    auto mi = mapRelay.find(hash);
    if (mi != mapRelay.end())
    {
        g_connman->PushMessage(node, NetMsgType::TX, mi->second);
        return true;
    }
    return false;
}

void CRequestManager::SetPeerFirstHeaderReceived(CNode* node, CBlockIndex* pindexLast)
{
    WRITELOCK(cs_requestmanager);
    std::map<NodeId, CNodeState>::iterator iter = mapNodeState.find(node->GetId());
    assert(iter != mapNodeState.end());
    CNodeState* state = &iter->second;
    // During the initial peer handshake we must receive the initial headers which should be greater
    // than or equal to our block height at the time of requesting GETHEADERS. This is because the peer has
    // advertised a height >= to our own. Furthermore, because the headers max returned is as much as 2000 this
    // could not be a mainnet re-org.
    if (!state->fFirstHeadersReceived)
    {
        // We want to make sure that the peer doesn't just send us any old valid header. The block height of the
        // last header they send us should be equal to our block height at the time we made the GETHEADERS
        // request.
        if (pindexLast && state->nFirstHeadersExpectedHeight <= pindexLast->nHeight)
        {
            state->fFirstHeadersReceived = true;
            LogPrint("net", "Initial headers received for peer=%d\n", node->GetId());
        }
    }
}

void CRequestManager::SetPeerSyncStartTime(CNode* node)
{
    int64_t now = GetTime();
    WRITELOCK(cs_requestmanager);
    std::map<NodeId, CNodeState>::iterator iter = mapNodeState.find(node->GetId());
    assert(iter != mapNodeState.end());
    CNodeState* state = &iter->second;
    state->nSyncStartTime = now; // reset the time because more headers needed
}

std::vector<NodeId> CRequestManager::UpdateBestKnowBlockAll(CBlockIndex* pindexLast)
{
    std::vector<NodeId> nodes;
    READLOCK(cs_requestmanager);
    for (auto &state : mapNodeState)
    {
        if (state.second.pindexBestKnownBlock == nullptr || pindexLast->nChainWork > state.second.pindexBestKnownBlock->nChainWork)
        {
            nodes.push_back(state.first);
        }
    }
    return nodes;
}

void CRequestManager::RequestNextBlocksToDownload(CNode* node)
{

    int nBlocksInFlight = 0;
    {
        READLOCK(cs_requestmanager);
        std::map<NodeId, int16_t>::iterator iter = mapNumBlocksInFlight.find(node->GetId());
        assert(iter != mapNumBlocksInFlight.end());
        nBlocksInFlight = iter->second;
    }
    // TODO : chose a better number than 64 and make it a variable
    if (!node->fDisconnect && !node->fClient && nBlocksInFlight < 64)
    {
        std::vector<CBlockIndex *> vToDownload;
        // TODO : find a better number than 64 and make it a variable
        FindNextBlocksToDownload(node, 64 - nBlocksInFlight, vToDownload);
        LogPrint("net", "vToDownload size = %u for peer %d \n", vToDownload.size(), node->GetId());
        std::vector<CInv> vGetBlocks;
        for (CBlockIndex *pindex : vToDownload)
        {
            CInv inv(MSG_BLOCK, pindex->GetBlockHash());
            // TODO : split alreadyhave into alreadyhaveBLOCK and alreadyhaveTX
            if (!AlreadyHaveBlock(inv))
            {
                vGetBlocks.emplace_back(inv);
            }
        }
        if (!vGetBlocks.empty())
        {
            std::vector<CInv> vToFetchNew;
            {
                READLOCK(cs_requestmanager);
                for (CInv &inv : vGetBlocks)
                {
                    // If this block is already in flight then don't ask for it again during the IBD process.
                    //
                    // If it's an additional source for a new peer then it would have been added already in
                    // FindNextBlocksToDownload().
                    std::map<uint256, std::pair<NodeId, QueuedBlock> >::iterator itInFlight = mapBlocksInFlight.find(inv.hash);
                    if (itInFlight != mapBlocksInFlight.end())
                    {
                        // block already incoming, move on
                        LogPrint("net", "block %s already in flight, continue \n", inv.hash.ToString().c_str());
                        continue;
                    }
                    vToFetchNew.push_back(inv);
                }
            }
            if (vToFetchNew.empty() == false)
            {
                vGetBlocks.swap(vToFetchNew);
                g_connman->PushMessage(node, NetMsgType::GETDATA, vGetBlocks);
                for (auto &block : vGetBlocks)
                {
                    MarkBlockAsInFlight(node->GetId(), block.hash, pnetMan->getChainActive()->LookupBlockIndex(block.hash));
                }
            }
            else
            {
                LogPrint("net", "vToFetchNew was empty for peer %d \n", node->GetId());
            }
        }
        else
        {
            LogPrint("net", "vGetBlocks was empty for peer %d \n", node->GetId());
        }
    }
}

// Update pindexLastCommonBlock and add not-in-flight missing successors to vBlocks, until it has
// at most count entries.
void CRequestManager::FindNextBlocksToDownload(CNode *node, unsigned int count, std::vector<CBlockIndex *> &vBlocks)
{
    if (count == 0)
        return;

    NodeId nodeid = node->GetId();
    vBlocks.reserve(vBlocks.size() + count);

    // Make sure pindexBestKnownBlock is up to date, we'll need it.
    ProcessBlockAvailability(nodeid);

    RECURSIVEREADLOCK(pnetMan->getChainActive()->cs_mapBlockIndex);
    WRITELOCK(cs_requestmanager);
    std::map<NodeId, CNodeState>::iterator iter = mapNodeState.find(node->GetId());
    assert(iter != mapNodeState.end());
    CNodeState* state = &iter->second;

    if (state->pindexBestKnownBlock == nullptr ||
        state->pindexBestKnownBlock->nChainWork < pnetMan->getChainActive()->chainActive.Tip()->nChainWork)
    {
        // This peer has nothing interesting.
        LogPrint("net", "not requesting blocks from peer %d, they do not have anything we need because ", node->GetId());
        if (state->pindexBestKnownBlock == nullptr)
        {
            LogPrint("net", "best known block was NULLPTR \n");
        }
        else
        {
            LogPrint("net", "best known block was had LESS work than our tip \n");
        }
        return;
    }

    if (state->pindexLastCommonBlock == nullptr)
    {
        // Bootstrap quickly by guessing a parent of our best tip is the forking point.
        // Guessing wrong in either direction is not a problem.
        state->pindexLastCommonBlock =
            pnetMan->getChainActive()->chainActive[std::min(state->pindexBestKnownBlock->nHeight, pnetMan->getChainActive()->chainActive.Height())];
    }

    // If the peer reorganized, our previous pindexLastCommonBlock may not be an ancestor
    // of its current tip anymore. Go back enough to fix that.
    state->pindexLastCommonBlock = LastCommonAncestor(state->pindexLastCommonBlock, state->pindexBestKnownBlock);
    if (state->pindexLastCommonBlock == state->pindexBestKnownBlock)
    {
        LogPrint("net", "returning, common is the same as best known \n");
        return;
    }

    std::vector<CBlockIndex *> vToFetch;
    CBlockIndex *pindexWalk = state->pindexLastCommonBlock;
    // Never fetch further than the current chain tip + the block download window.  We need to ensure
    // the if running in pruning mode we don't download too many blocks ahead and as a result use to
    // much disk space to store unconnected blocks.
    int nWindowEnd = pnetMan->getChainActive()->chainActive.Height() + BLOCK_DOWNLOAD_WINDOW;

    int nMaxHeight = std::min<int>(state->pindexBestKnownBlock->nHeight, nWindowEnd + 1);
    while (pindexWalk->nHeight < nMaxHeight)
    {
        // Read up to 128 (or more, if more blocks than that are needed) successors of pindexWalk (towards
        // pindexBestKnownBlock) into vToFetch. We fetch 128, because CBlockIndex::GetAncestor may be as expensive
        // as iterating over ~100 CBlockIndex* entries anyway.
        int nToFetch = std::min(nMaxHeight - pindexWalk->nHeight, std::max<int>(count - vBlocks.size(), 128));
        vToFetch.resize(nToFetch);
        pindexWalk = state->pindexBestKnownBlock->GetAncestor(pindexWalk->nHeight + nToFetch);
        vToFetch[nToFetch - 1] = pindexWalk;
        for (unsigned int i = nToFetch - 1; i > 0; i--)
        {
            vToFetch[i - 1] = vToFetch[i]->pprev;
        }

        // Iterate over those blocks in vToFetch (in forward direction), adding the ones that
        // are not yet downloaded and not in flight to vBlocks. In the mean time, update
        // pindexLastCommonBlock as long as all ancestors are already downloaded, or if it's
        // already part of our chain (and therefore don't need it even if pruned).
        for (CBlockIndex *pindex : vToFetch)
        {
            uint256 blockHash = pindex->GetBlockHash();
            if (AlreadyAskedForBlock(blockHash))
            {
                // we already requested this block.
                // TODO : consider also requesting this block from a second peer that has it
                LogPrint("net", "we already requesdted block with hash %s, continue \n", blockHash.ToString().c_str());
                continue;
            }
            if (!pindex->IsValid(BLOCK_VALID_TREE))
            {
                // We consider the chain that this peer is on invalid.
                LogPrint("net", "we consider block with hash %s on a chain that is invalid, return \n", blockHash.ToString().c_str());
                return;
            }
            if (pindex->nStatus & BLOCK_HAVE_DATA || pnetMan->getChainActive()->chainActive.Contains(pindex))
            {
                if (pindex->nChainTx)
                {
                    state->pindexLastCommonBlock = pindex;
                }
            }
            else
            {
                // Return if we've reached the end of the download window.
                if (pindex->nHeight > nWindowEnd)
                {
                    return;
                }

                // Return if we've reached the end of the number of blocks we can download for this peer.
                vBlocks.push_back(pindex);
                if (vBlocks.size() == count)
                {
                    return;
                }
            }
        }
    }
}
