// Copyright (c) 2018-2023 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/utils.h>

#include <evo/evodb.h>
#include <evo/specialtx.h>

#include <chain.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <net.h>
#include <net_processing.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <saltedhasher.h>
#include <sync.h>
#include <util/irange.h>
#include <util/underlying.h>
#include <validation.h>

#include <map>

namespace llmq
{

std::unique_ptr<CQuorumBlockProcessor> quorumBlockProcessor;

static const std::string DB_MINED_COMMITMENT = "q_mc";
static const std::string DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT = "q_mcih";
static const std::string DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT_Q_INDEXED = "q_mcihi";

static const std::string DB_BEST_BLOCK_UPGRADE = "q_bbu2";

CQuorumBlockProcessor::CQuorumBlockProcessor(CEvoDB& evoDb, CConnman& _connman) :
    m_evoDb(evoDb), connman(_connman)
{
    utils::InitQuorumsCache(mapHasMinedCommitmentCache);
}

void CQuorumBlockProcessor::ProcessMessage(const CNode& peer, std::string_view msg_type, CDataStream& vRecv)
{
    if (msg_type != NetMsgType::QFCOMMITMENT) {
        return;
    }

    CFinalCommitment qc;
    vRecv >> qc;

    WITH_LOCK(cs_main, EraseObjectRequest(peer.GetId(), CInv(MSG_QUORUM_FINAL_COMMITMENT, ::SerializeHash(qc))));

    if (qc.IsNull()) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- null commitment from peer=%d\n", __func__, peer.GetId());
        WITH_LOCK(cs_main, Misbehaving(peer.GetId(), 100));
        return;
    }

    const auto& llmq_params_opt = GetLLMQParams(qc.llmqType);
    if (!llmq_params_opt.has_value()) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- invalid commitment type %d from peer=%d\n", __func__,
                 ToUnderlying(qc.llmqType), peer.GetId());
        WITH_LOCK(cs_main, Misbehaving(peer.GetId(), 100));
        return;
    }
    auto type = qc.llmqType;

    // Verify that quorumHash is part of the active chain and that it's the first block in the DKG interval
    const CBlockIndex* pQuorumBaseBlockIndex;
    {
        LOCK(cs_main);
        pQuorumBaseBlockIndex = LookupBlockIndex(qc.quorumHash);
        if (pQuorumBaseBlockIndex == nullptr) {
            LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- unknown block %s in commitment, peer=%d\n", __func__,
                     qc.quorumHash.ToString(), peer.GetId());
            // can't really punish the node here, as we might simply be the one that is on the wrong chain or not
            // fully synced
            return;
        }
        if (::ChainActive().Tip()->GetAncestor(pQuorumBaseBlockIndex->nHeight) != pQuorumBaseBlockIndex) {
            LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- block %s not in active chain, peer=%d\n", __func__,
                     qc.quorumHash.ToString(), peer.GetId());
            // same, can't punish
            return;
        }
        int quorumHeight = pQuorumBaseBlockIndex->nHeight - (pQuorumBaseBlockIndex->nHeight % llmq_params_opt->dkgInterval) + int(qc.quorumIndex);
        if (quorumHeight != pQuorumBaseBlockIndex->nHeight) {
            LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- block %s is not the first block in the DKG interval, peer=%d\n", __func__,
                     qc.quorumHash.ToString(), peer.GetId());
            Misbehaving(peer.GetId(), 100);
            return;
        }
        if (pQuorumBaseBlockIndex->nHeight < (::ChainActive().Height() - llmq_params_opt->dkgInterval)) {
            LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- block %s is too old, peer=%d\n", __func__,
                     qc.quorumHash.ToString(), peer.GetId());
            // TODO: enable punishment in some future version when all/most nodes are running with this fix
            // Misbehaving(peer.GetId(), 100);
            return;
        }
        if (HasMinedCommitment(type, qc.quorumHash)) {
            LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- commitment for quorum hash[%s], type[%d], quorumIndex[%d] is already mined, peer=%d\n",
                     __func__, qc.quorumHash.ToString(), ToUnderlying(type), qc.quorumIndex, peer.GetId());
            // NOTE: do not punish here
            return;
        }
    }

    {
        // Check if we already got a better one locally
        // We do this before verifying the commitment to avoid DoS
        LOCK(minableCommitmentsCs);
        auto k = std::make_pair(type, qc.quorumHash);
        auto it = minableCommitmentsByQuorum.find(k);
        if (it != minableCommitmentsByQuorum.end()) {
            auto jt = minableCommitments.find(it->second);
            if (jt != minableCommitments.end() && jt->second.CountSigners() <= qc.CountSigners()) {
                return;
            }
        }
    }

    if (!qc.Verify(pQuorumBaseBlockIndex, true)) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- commitment for quorum %s:%d is not valid quorumIndex[%d] nversion[%d], peer=%d\n",
                 __func__, qc.quorumHash.ToString(),
                 ToUnderlying(qc.llmqType), qc.quorumIndex, qc.nVersion, peer.GetId());
        WITH_LOCK(cs_main, Misbehaving(peer.GetId(), 100));
        return;
    }

    LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- received commitment for quorum %s:%d, validMembers=%d, signers=%d, peer=%d\n", __func__,
             qc.quorumHash.ToString(), ToUnderlying(qc.llmqType), qc.CountValidMembers(), qc.CountSigners(), peer.GetId());

    AddMineableCommitment(qc);
}

bool CQuorumBlockProcessor::ProcessBlock(const CBlock& block, const CBlockIndex* pindex, CValidationState& state, bool fJustCheck, bool fBLSChecks)
{
    AssertLockHeld(cs_main);

    bool fDIP0003Active = pindex->nHeight >= Params().GetConsensus().DIP0003Height;
    if (!fDIP0003Active) {
        m_evoDb.Write(DB_BEST_BLOCK_UPGRADE, block.GetHash());
        return true;
    }

    if (utils::IsV19Active(pindex->pprev))
        bls::bls_legacy_scheme.store(false);

    llmq::utils::PreComputeQuorumMembers(pindex);

    std::multimap<Consensus::LLMQType, CFinalCommitment> qcs;
    if (!GetCommitmentsFromBlock(block, pindex, qcs, state)) {
        return false;
    }

    auto blockHash = block.GetHash();

    // The following checks make sure that there is always a (possibly null) commitment while in the mining phase
    // until the first non-null commitment has been mined. After the non-null commitment, no other commitments are
    // allowed, including null commitments.
    // Note: must only check quorums that were enabled at the _previous_ block height to match mining logic
    for (const Consensus::LLMQParams& params : utils::GetEnabledQuorumParams(pindex->pprev)) {
        // skip these checks when replaying blocks after the crash
        if (::ChainActive().Tip() == nullptr) {
            break;
        }

        const size_t numCommitmentsRequired = GetNumCommitmentsRequired(params, pindex->nHeight);
        const auto numCommitmentsInNewBlock = qcs.count(params.type);

        if (numCommitmentsRequired < numCommitmentsInNewBlock) {
            return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-qc-not-allowed");
        }

        if (numCommitmentsRequired > numCommitmentsInNewBlock) {
            return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-qc-missing");
        }
        if (llmq::utils::IsQuorumRotationEnabled(params, pindex)) {
            LogPrintf("[ProcessBlock] h[%d] numCommitmentsRequired[%d] numCommitmentsInNewBlock[%d]\n", pindex->nHeight, numCommitmentsRequired, numCommitmentsInNewBlock);
        }
    }

    for (const auto& p : qcs) {
        const auto& qc = p.second;
        if (!ProcessCommitment(pindex->nHeight, blockHash, qc, state, fJustCheck, fBLSChecks)) {
            LogPrintf("[ProcessBlock] failed h[%d] llmqType[%d] version[%d] quorumIndex[%d] quorumHash[%s]\n", pindex->nHeight, ToUnderlying(qc.llmqType), qc.nVersion, qc.quorumIndex, qc.quorumHash.ToString());
            return false;
        }
    }

    m_evoDb.Write(DB_BEST_BLOCK_UPGRADE, blockHash);

    return true;
}

// We store a mapping from minedHeight->quorumHeight in the DB
// minedHeight is inversed so that entries are traversable in reversed order
static std::tuple<std::string, Consensus::LLMQType, uint32_t> BuildInversedHeightKey(Consensus::LLMQType llmqType, int nMinedHeight)
{
    // nMinedHeight must be converted to big endian to make it comparable when serialized
    return std::make_tuple(DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT, llmqType, htobe32(std::numeric_limits<uint32_t>::max() - nMinedHeight));
}

static std::tuple<std::string, Consensus::LLMQType, int, uint32_t> BuildInversedHeightKeyIndexed(Consensus::LLMQType llmqType, int nMinedHeight, int quorumIndex)
{
    // nMinedHeight must be converted to big endian to make it comparable when serialized
    return std::make_tuple(DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT_Q_INDEXED, llmqType, quorumIndex, htobe32(std::numeric_limits<uint32_t>::max() - nMinedHeight));
}

bool CQuorumBlockProcessor::ProcessCommitment(int nHeight, const uint256& blockHash, const CFinalCommitment& qc, CValidationState& state, bool fJustCheck, bool fBLSChecks)
{
    AssertLockHeld(cs_main);

    const auto& llmq_params_opt = GetLLMQParams(qc.llmqType);
    if (!llmq_params_opt.has_value()) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- invalid commitment type %d\n", __func__, ToUnderlying(qc.llmqType));
        return false;
    }
    const auto& llmq_params = llmq_params_opt.value();

    uint256 quorumHash = GetQuorumBlockHash(llmq_params, nHeight, qc.quorumIndex);

    LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s height=%d, type=%d, quorumIndex=%d, quorumHash=%s, signers=%s, validMembers=%d, quorumPublicKey=%s fJustCheck[%d] processing commitment from block.\n", __func__,
             nHeight, ToUnderlying(qc.llmqType), qc.quorumIndex, quorumHash.ToString(), qc.CountSigners(), qc.CountValidMembers(), qc.quorumPublicKey.ToString(), fJustCheck);

    // skip `bad-qc-block` checks below when replaying blocks after the crash
    if (::ChainActive().Tip() == nullptr) {
        quorumHash = qc.quorumHash;
    }

    if (quorumHash.IsNull()) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s height=%d, type=%d, quorumIndex=%d, quorumHash=%s, signers=%s, validMembers=%d, quorumPublicKey=%s quorumHash is null.\n", __func__,
                 nHeight, ToUnderlying(qc.llmqType), qc.quorumIndex, quorumHash.ToString(), qc.CountSigners(), qc.CountValidMembers(), qc.quorumPublicKey.ToString());
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-qc-block");
    }
    if (quorumHash != qc.quorumHash) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s height=%d, type=%d, quorumIndex=%d, quorumHash=%s, qc.quorumHash=%s signers=%s, validMembers=%d, quorumPublicKey=%s non equal quorumHash.\n", __func__,
                 nHeight, ToUnderlying(qc.llmqType), qc.quorumIndex, quorumHash.ToString(), qc.quorumHash.ToString(), qc.CountSigners(), qc.CountValidMembers(), qc.quorumPublicKey.ToString());
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-qc-block");
    }

    if (qc.IsNull()) {
        if (!qc.VerifyNull()) {
            LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s height=%d, type=%d, quorumIndex=%d, quorumHash=%s, signers=%s, validMembers=%dqc verifynull failed.\n", __func__,
                     nHeight, ToUnderlying(qc.llmqType), qc.quorumIndex, quorumHash.ToString(), qc.CountSigners(), qc.CountValidMembers());
            return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-qc-invalid-null");
        }
        return true;
    }

    if (HasMinedCommitment(llmq_params.type, quorumHash)) {
        // should not happen as it's already handled in ProcessBlock
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-qc-dup");
    }

    if (!IsMiningPhase(llmq_params, nHeight)) {
        // should not happen as it's already handled in ProcessBlock
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-qc-height");
    }

    const auto* pQuorumBaseBlockIndex = LookupBlockIndex(qc.quorumHash);

    if (!qc.Verify(pQuorumBaseBlockIndex, fBLSChecks)) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s height=%d, type=%d, quorumIndex=%d, quorumHash=%s, signers=%s, validMembers=%d, quorumPublicKey=%s qc verify failed.\n", __func__,
                 nHeight, ToUnderlying(qc.llmqType), qc.quorumIndex, quorumHash.ToString(), qc.CountSigners(), qc.CountValidMembers(), qc.quorumPublicKey.ToString());
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-qc-invalid");
    }

    if (fJustCheck) {
        return true;
    }

    bool rotation_enabled = utils::IsQuorumRotationEnabled(llmq_params, pQuorumBaseBlockIndex);

    if (rotation_enabled) {
        LogPrint(BCLog::LLMQ, "[ProcessCommitment] height[%d] pQuorumBaseBlockIndex[%d] quorumIndex[%d] qversion[%d] Built\n",
                 nHeight, pQuorumBaseBlockIndex->nHeight, qc.quorumIndex, qc.nVersion);
    }

    // Store commitment in DB
    auto cacheKey = std::make_pair(llmq_params.type, quorumHash);
    m_evoDb.Write(std::make_pair(DB_MINED_COMMITMENT, cacheKey), std::make_pair(qc, blockHash));

    if (rotation_enabled) {
        m_evoDb.Write(BuildInversedHeightKeyIndexed(llmq_params.type, nHeight, int(qc.quorumIndex)), pQuorumBaseBlockIndex->nHeight);
    } else {
        m_evoDb.Write(BuildInversedHeightKey(llmq_params.type, nHeight), pQuorumBaseBlockIndex->nHeight);
    }

    {
        LOCK(minableCommitmentsCs);
        mapHasMinedCommitmentCache[qc.llmqType].erase(qc.quorumHash);
        minableCommitmentsByQuorum.erase(cacheKey);
        minableCommitments.erase(::SerializeHash(qc));
    }

    LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- processed commitment from block. type=%d, quorumIndex=%d, quorumHash=%s, signers=%s, validMembers=%d, quorumPublicKey=%s\n", __func__,
             ToUnderlying(qc.llmqType), qc.quorumIndex, quorumHash.ToString(), qc.CountSigners(), qc.CountValidMembers(), qc.quorumPublicKey.ToString());

    return true;
}

bool CQuorumBlockProcessor::UndoBlock(const CBlock& block, const CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);

    if (!utils::IsV19Active(pindex->pprev))
        bls::bls_legacy_scheme.store(true);

    llmq::utils::PreComputeQuorumMembers(pindex, true);

    std::multimap<Consensus::LLMQType, CFinalCommitment> qcs;
    CValidationState dummy;
    if (!GetCommitmentsFromBlock(block, pindex, qcs, dummy)) {
        return false;
    }

    for (auto& p : qcs) {
        auto& qc = p.second;
        if (qc.IsNull()) {
            continue;
        }

        m_evoDb.Erase(std::make_pair(DB_MINED_COMMITMENT, std::make_pair(qc.llmqType, qc.quorumHash)));

        const auto& llmq_params_opt = GetLLMQParams(qc.llmqType);
        assert(llmq_params_opt.has_value());

        if (llmq::utils::IsQuorumRotationEnabled(llmq_params_opt.value(), pindex)) {
            m_evoDb.Erase(BuildInversedHeightKeyIndexed(qc.llmqType, pindex->nHeight, int(qc.quorumIndex)));
        } else {
            m_evoDb.Erase(BuildInversedHeightKey(qc.llmqType, pindex->nHeight));
        }

        {
            LOCK(minableCommitmentsCs);
            mapHasMinedCommitmentCache[qc.llmqType].erase(qc.quorumHash);
        }

        // if a reorg happened, we should allow to mine this commitment later
        AddMineableCommitment(qc);
    }

    m_evoDb.Write(DB_BEST_BLOCK_UPGRADE, pindex->pprev->GetBlockHash());

    return true;
}

// TODO remove this with 0.15.0
bool CQuorumBlockProcessor::UpgradeDB()
{
    LOCK(cs_main);

    if (::ChainActive().Tip() == nullptr) {
        // should have no records
        return m_evoDb.IsEmpty();
    }

    uint256 bestBlock;
    if (m_evoDb.GetRawDB().Read(DB_BEST_BLOCK_UPGRADE, bestBlock) && bestBlock == ::ChainActive().Tip()->GetBlockHash()) {
        return true;
    }

    LogPrintf("CQuorumBlockProcessor::%s -- Upgrading DB...\n", __func__);

    if (::ChainActive().Height() >= Params().GetConsensus().DIP0003EnforcementHeight) {
        const auto* pindex = ::ChainActive()[Params().GetConsensus().DIP0003EnforcementHeight];
        while (pindex != nullptr) {
            if (fPruneMode && ((pindex->nStatus & BLOCK_HAVE_DATA) == 0)) {
                // Too late, we already pruned blocks we needed to reprocess commitments
                return false;
            }
            CBlock block;
            bool r = ReadBlockFromDisk(block, pindex, Params().GetConsensus());
            assert(r);

            std::multimap<Consensus::LLMQType, CFinalCommitment> qcs;
            CValidationState dummyState;
            GetCommitmentsFromBlock(block, pindex, qcs, dummyState);

            for (const auto& p : qcs) {
                const auto& qc = p.second;
                if (qc.IsNull()) {
                    continue;
                }
                const auto* pQuorumBaseBlockIndex = LookupBlockIndex(qc.quorumHash);
                m_evoDb.GetRawDB().Write(std::make_pair(DB_MINED_COMMITMENT, std::make_pair(qc.llmqType, qc.quorumHash)), std::make_pair(qc, pindex->GetBlockHash()));
                const auto& llmq_params_opt = GetLLMQParams(qc.llmqType);
                assert(llmq_params_opt.has_value());
                if (llmq::utils::IsQuorumRotationEnabled(llmq_params_opt.value(), pQuorumBaseBlockIndex)) {
                    m_evoDb.GetRawDB().Write(BuildInversedHeightKeyIndexed(qc.llmqType, pindex->nHeight, int(qc.quorumIndex)), pQuorumBaseBlockIndex->nHeight);
                } else {
                    m_evoDb.GetRawDB().Write(BuildInversedHeightKey(qc.llmqType, pindex->nHeight), pQuorumBaseBlockIndex->nHeight);
                }
            }

            m_evoDb.GetRawDB().Write(DB_BEST_BLOCK_UPGRADE, pindex->GetBlockHash());

            pindex = ::ChainActive().Next(pindex);
        }
    }

    LogPrintf("CQuorumBlockProcessor::%s -- Upgrade done...\n", __func__);
    return true;
}

bool CQuorumBlockProcessor::GetCommitmentsFromBlock(const CBlock& block, const CBlockIndex* pindex, std::multimap<Consensus::LLMQType, CFinalCommitment>& ret, CValidationState& state)
{
    AssertLockHeld(cs_main);

    const auto& consensus = Params().GetConsensus();

    ret.clear();

    for (const auto& tx : block.vtx) {
        if (tx->nType == TRANSACTION_QUORUM_COMMITMENT) {
            CFinalCommitmentTxPayload qc;
            if (!GetTxPayload(*tx, qc)) {
                // should not happen as it was verified before processing the block
                LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s height=%d GetTxPayload fails\n", __func__, pindex->nHeight);
                return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-qc-payload");
            }

            const auto& llmq_params_opt = GetLLMQParams(qc.commitment.llmqType);
            if (!llmq_params_opt.has_value()) {
                // should not happen as it was verified before processing the block
                return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-qc-commitment-type");
            }

            // only allow one commitment per type and per block (This was changed with rotation)
            if (!utils::IsQuorumRotationEnabled(llmq_params_opt.value(), pindex)) {
                if (ret.count(qc.commitment.llmqType) != 0) {
                    return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-qc-dup");
                }
            }

            ret.emplace(qc.commitment.llmqType, std::move(qc.commitment));
        }
    }

    if (pindex->nHeight < consensus.DIP0003Height && !ret.empty()) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-qc-premature");
    }

    return true;
}

bool CQuorumBlockProcessor::IsMiningPhase(const Consensus::LLMQParams& llmqParams, int nHeight)
{
    AssertLockHeld(cs_main);

    // Note: This function can be called for new blocks
    assert(nHeight <= ::ChainActive().Height() + 1);

    int quorumCycleStartHeight = nHeight - (nHeight % llmqParams.dkgInterval);
    int quorumCycleMiningStartHeight = quorumCycleStartHeight + llmqParams.dkgMiningWindowStart;
    int quorumCycleMiningEndHeight = quorumCycleStartHeight + llmqParams.dkgMiningWindowEnd;

    if (nHeight >= quorumCycleMiningStartHeight && nHeight <= quorumCycleMiningEndHeight) {
        LogPrint(BCLog::LLMQ, "[IsMiningPhase] nHeight[%d] llmqType[%d] quorumCycleStartHeight[%d] -- mining[%d-%d]\n", nHeight, ToUnderlying(llmqParams.type), quorumCycleStartHeight, quorumCycleMiningStartHeight, quorumCycleMiningEndHeight);
        return true;
    }
    LogPrint(BCLog::LLMQ, "[IsMiningPhase] nHeight[%d] llmqType[%d] quorumCycleStartHeight[%d] -- NOT mining[%d-%d]\n", nHeight, ToUnderlying(llmqParams.type), quorumCycleStartHeight, quorumCycleMiningStartHeight, quorumCycleMiningEndHeight);

    return false;
}

size_t CQuorumBlockProcessor::GetNumCommitmentsRequired(const Consensus::LLMQParams& llmqParams, int nHeight) const
{
    AssertLockHeld(cs_main);

    if (!IsMiningPhase(llmqParams, nHeight)) return 0;

    // Note: This function can be called for new blocks
    assert(nHeight <= ::ChainActive().Height() + 1);
    const auto *const pindex = ::ChainActive().Height() < nHeight ? ::ChainActive().Tip() : ::ChainActive().Tip()->GetAncestor(nHeight);

    bool rotation_enabled = utils::IsQuorumRotationEnabled(llmqParams, pindex);
    size_t quorums_num = rotation_enabled ? llmqParams.signingActiveQuorumCount : 1;
    size_t ret{0};

    for (const auto quorumIndex : irange::range(quorums_num)) {
        uint256 quorumHash = GetQuorumBlockHash(llmqParams, nHeight, quorumIndex);
        if (!quorumHash.IsNull() && !HasMinedCommitment(llmqParams.type, quorumHash)) ++ret;
    }

    return ret;
}

// WARNING: This method returns uint256() on the first block of the DKG interval (because the block hash is not known yet)
uint256 CQuorumBlockProcessor::GetQuorumBlockHash(const Consensus::LLMQParams& llmqParams, int nHeight, int quorumIndex)
{
    AssertLockHeld(cs_main);

    int quorumStartHeight = nHeight - (nHeight % llmqParams.dkgInterval) + quorumIndex;

    uint256 quorumBlockHash;
    if (!GetBlockHash(quorumBlockHash, quorumStartHeight)) {
        LogPrint(BCLog::LLMQ, "[GetQuorumBlockHash] llmqType[%d] h[%d] qi[%d] quorumStartHeight[%d] quorumHash[EMPTY]\n", ToUnderlying(llmqParams.type), nHeight, quorumIndex, quorumStartHeight);
        return {};
    }

    LogPrint(BCLog::LLMQ, "[GetQuorumBlockHash] llmqType[%d] h[%d] qi[%d] quorumStartHeight[%d] quorumHash[%s]\n", ToUnderlying(llmqParams.type), nHeight, quorumIndex, quorumStartHeight, quorumBlockHash.ToString());
    return quorumBlockHash;
}

bool CQuorumBlockProcessor::HasMinedCommitment(Consensus::LLMQType llmqType, const uint256& quorumHash) const
{
    bool fExists;
    {
        LOCK(minableCommitmentsCs);
        if (mapHasMinedCommitmentCache[llmqType].get(quorumHash, fExists)) {
            return fExists;
        }
    }

    fExists = m_evoDb.Exists(std::make_pair(DB_MINED_COMMITMENT, std::make_pair(llmqType, quorumHash)));

    LOCK(minableCommitmentsCs);
    mapHasMinedCommitmentCache[llmqType].insert(quorumHash, fExists);

    return fExists;
}

CFinalCommitmentPtr CQuorumBlockProcessor::GetMinedCommitment(Consensus::LLMQType llmqType, const uint256& quorumHash, uint256& retMinedBlockHash) const
{
    auto key = std::make_pair(DB_MINED_COMMITMENT, std::make_pair(llmqType, quorumHash));
    std::pair<CFinalCommitment, uint256> p;
    if (!m_evoDb.Read(key, p)) {
        return nullptr;
    }
    retMinedBlockHash = p.second;
    return std::make_unique<CFinalCommitment>(p.first);
}

// The returned quorums are in reversed order, so the most recent one is at index 0
std::vector<const CBlockIndex*> CQuorumBlockProcessor::GetMinedCommitmentsUntilBlock(Consensus::LLMQType llmqType, const CBlockIndex* pindex, size_t maxCount) const
{
    AssertLockNotHeld(m_evoDb.cs);
    LOCK(m_evoDb.cs);

    auto dbIt = m_evoDb.GetCurTransaction().NewIteratorUniquePtr();

    auto firstKey = BuildInversedHeightKey(llmqType, pindex->nHeight);
    auto lastKey = BuildInversedHeightKey(llmqType, 0);

    dbIt->Seek(firstKey);

    std::vector<const CBlockIndex*> ret;
    ret.reserve(maxCount);

    while (dbIt->Valid() && ret.size() < maxCount) {
        decltype(firstKey) curKey;
        int quorumHeight;
        if (!dbIt->GetKey(curKey) || curKey >= lastKey) {
            break;
        }
        if (std::get<0>(curKey) != DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT || std::get<1>(curKey) != llmqType) {
            break;
        }

        uint32_t nMinedHeight = std::numeric_limits<uint32_t>::max() - be32toh(std::get<2>(curKey));
        if (nMinedHeight > static_cast<uint32_t>(pindex->nHeight)) {
            break;
        }

        if (!dbIt->GetValue(quorumHeight)) {
            break;
        }

        const auto* pQuorumBaseBlockIndex = pindex->GetAncestor(quorumHeight);
        assert(pQuorumBaseBlockIndex);
        ret.emplace_back(pQuorumBaseBlockIndex);

        dbIt->Next();
    }

    return ret;
}

std::optional<const CBlockIndex*> CQuorumBlockProcessor::GetLastMinedCommitmentsByQuorumIndexUntilBlock(Consensus::LLMQType llmqType, const CBlockIndex* pindex, int quorumIndex, size_t cycle) const
{
    AssertLockNotHeld(m_evoDb.cs);
    LOCK(m_evoDb.cs);

    auto dbIt = m_evoDb.GetCurTransaction().NewIteratorUniquePtr();

    auto firstKey = BuildInversedHeightKeyIndexed(llmqType, pindex->nHeight, quorumIndex);
    auto lastKey = BuildInversedHeightKeyIndexed(llmqType, 0, quorumIndex);

    size_t currentCycle = 0;

    dbIt->Seek(firstKey);

    while (dbIt->Valid()) {
        decltype(firstKey) curKey;
        int quorumHeight;
        if (!dbIt->GetKey(curKey) || curKey >= lastKey) {
            return std::nullopt;
        }
        if (std::get<0>(curKey) != DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT_Q_INDEXED || std::get<1>(curKey) != llmqType) {
            return std::nullopt;
        }

        uint32_t nMinedHeight = std::numeric_limits<uint32_t>::max() - be32toh(std::get<3>(curKey));
        if (nMinedHeight > static_cast<uint32_t>(pindex->nHeight)) {
            return std::nullopt;
        }

        if (!dbIt->GetValue(quorumHeight)) {
            return std::nullopt;
        }

        const auto* pQuorumBaseBlockIndex = pindex->GetAncestor(quorumHeight);
        assert(pQuorumBaseBlockIndex);

        if (currentCycle == cycle) {
            return std::make_optional(pQuorumBaseBlockIndex);
        }

        currentCycle++;

        dbIt->Next();
    }

    return std::nullopt;
}

std::vector<std::pair<int, const CBlockIndex*>> CQuorumBlockProcessor::GetLastMinedCommitmentsPerQuorumIndexUntilBlock(Consensus::LLMQType llmqType, const CBlockIndex* pindex, size_t cycle) const
{
    const auto& llmq_params_opt = GetLLMQParams(llmqType);
    assert(llmq_params_opt.has_value());
    std::vector<std::pair<int, const CBlockIndex*>> ret;

    for (const auto quorumIndex : irange::range(llmq_params_opt->signingActiveQuorumCount)) {
        std::optional<const CBlockIndex*> q = GetLastMinedCommitmentsByQuorumIndexUntilBlock(llmqType, pindex, quorumIndex, cycle);
        if (q.has_value()) {
            ret.emplace_back(quorumIndex, q.value());
        }
    }

    return ret;
}

std::vector<const CBlockIndex*> CQuorumBlockProcessor::GetMinedCommitmentsIndexedUntilBlock(Consensus::LLMQType llmqType, const CBlockIndex* pindex, size_t maxCount) const
{
    std::vector<const CBlockIndex*> ret;

    size_t cycle = 0;

    while (ret.size() < maxCount) {
        std::vector<std::pair<int, const CBlockIndex*>> cycleRet = GetLastMinedCommitmentsPerQuorumIndexUntilBlock(llmqType, pindex, cycle);

        if (cycleRet.empty()) {
            return ret;
        }

        std::vector<const CBlockIndex*> cycleRetTransformed;
        std::transform(cycleRet.begin(),
                       cycleRet.end(),
                       std::back_inserter(cycleRetTransformed),
                       [](const std::pair<int, const CBlockIndex*>& p) { return p.second; });

        size_t needToCopy = maxCount - ret.size();

        std::copy_n(cycleRetTransformed.begin(),
                    std::min(needToCopy, cycleRetTransformed.size()),
                    std::back_inserter(ret));
        cycle++;
    }

    return ret;
}

// The returned quorums are in reversed order, so the most recent one is at index 0
std::map<Consensus::LLMQType, std::vector<const CBlockIndex*>> CQuorumBlockProcessor::GetMinedAndActiveCommitmentsUntilBlock(const CBlockIndex* pindex) const
{
    std::map<Consensus::LLMQType, std::vector<const CBlockIndex*>> ret;

    for (const auto& params : Params().GetConsensus().llmqs) {
        auto& v = ret[params.type];
        v.reserve(params.signingActiveQuorumCount);
        if (utils::IsQuorumRotationEnabled(params, pindex)) {
            std::vector<std::pair<int, const CBlockIndex*>> commitments = GetLastMinedCommitmentsPerQuorumIndexUntilBlock(params.type, pindex, 0);
            std::transform(commitments.begin(), commitments.end(), std::back_inserter(v),
                           [](const std::pair<int, const CBlockIndex*>& p) { return p.second; });
        } else {
            auto commitments = GetMinedCommitmentsUntilBlock(params.type, pindex, params.signingActiveQuorumCount);
            std::copy(commitments.begin(), commitments.end(), std::back_inserter(v));
        }
    }

    return ret;
}

bool CQuorumBlockProcessor::HasMineableCommitment(const uint256& hash) const
{
    LOCK(minableCommitmentsCs);
    return minableCommitments.count(hash) != 0;
}

void CQuorumBlockProcessor::AddMineableCommitment(const CFinalCommitment& fqc)
{
    bool relay = false;
    uint256 commitmentHash = ::SerializeHash(fqc);

    {
        LOCK(minableCommitmentsCs);

        auto k = std::make_pair(fqc.llmqType, fqc.quorumHash);
        auto ins = minableCommitmentsByQuorum.emplace(k, commitmentHash);
        if (ins.second) {
            minableCommitments.emplace(commitmentHash, fqc);
            relay = true;
        } else {
            const auto& oldFqc = minableCommitments.at(ins.first->second);
            if (fqc.CountSigners() > oldFqc.CountSigners()) {
                // new commitment has more signers, so override the known one
                ins.first->second = commitmentHash;
                minableCommitments.erase(ins.first->second);
                minableCommitments.emplace(commitmentHash, fqc);
                relay = true;
            }
        }
    }

    // We only relay the new commitment if it's new or better then the old one
    if (relay) {
        CInv inv(MSG_QUORUM_FINAL_COMMITMENT, commitmentHash);
        connman.RelayInv(inv);
    }
}

bool CQuorumBlockProcessor::GetMineableCommitmentByHash(const uint256& commitmentHash, llmq::CFinalCommitment& ret) const
{
    LOCK(minableCommitmentsCs);
    auto it = minableCommitments.find(commitmentHash);
    if (it == minableCommitments.end()) {
        return false;
    }
    ret = it->second;
    return true;
}

// Will return nullopt if no commitment should be mined
// Will return a null commitment if no mineable commitment is known and none was mined yet
std::optional<std::vector<CFinalCommitment>> CQuorumBlockProcessor::GetMineableCommitments(const Consensus::LLMQParams& llmqParams, int nHeight) const
{
    AssertLockHeld(cs_main);

    std::vector<CFinalCommitment> ret;

    if (GetNumCommitmentsRequired(llmqParams, nHeight) == 0) {
        // no commitment required
        return std::nullopt;
    }

    // Note: This function can be called for new blocks
    assert(nHeight <= ::ChainActive().Height() + 1);
    const auto *const pindex = ::ChainActive().Height() < nHeight ? ::ChainActive().Tip() : ::ChainActive().Tip()->GetAncestor(nHeight);

    bool rotation_enabled = utils::IsQuorumRotationEnabled(llmqParams, pindex);
    bool basic_bls_enabled = utils::IsV19Active(pindex);
    size_t quorums_num = rotation_enabled ? llmqParams.signingActiveQuorumCount : 1;

    std::stringstream ss;
    for (const auto quorumIndex : irange::range(quorums_num)) {
        CFinalCommitment cf;

        uint256 quorumHash = GetQuorumBlockHash(llmqParams, nHeight, quorumIndex);
        if (quorumHash.IsNull()) {
            break;
        }

        if (HasMinedCommitment(llmqParams.type, quorumHash)) continue;

        LOCK(minableCommitmentsCs);

        auto k = std::make_pair(llmqParams.type, quorumHash);
        auto it = minableCommitmentsByQuorum.find(k);
        if (it == minableCommitmentsByQuorum.end()) {
            // null commitment required
            cf = CFinalCommitment(llmqParams, quorumHash);
            cf.quorumIndex = static_cast<int16_t>(quorumIndex);
            cf.nVersion = CFinalCommitment::GetVersion(rotation_enabled, basic_bls_enabled);
            ss << "{ created nversion[" << cf.nVersion << "] quorumIndex[" << cf.quorumIndex << "] }";
        } else {
            cf = minableCommitments.at(it->second);
            ss << "{ cached nversion[" << cf.nVersion << "] quorumIndex[" << cf.quorumIndex << "] }";
        }

        ret.push_back(std::move(cf));
    }

    LogPrint(BCLog::LLMQ, "GetMineableCommitments cf height[%d] content: %s\n", nHeight, ss.str());

    if (ret.empty()) {
        return std::nullopt;
    }
    return std::make_optional(ret);
}

bool CQuorumBlockProcessor::GetMineableCommitmentsTx(const Consensus::LLMQParams& llmqParams, int nHeight, std::vector<CTransactionRef>& ret) const
{
    AssertLockHeld(cs_main);
    std::optional<std::vector<CFinalCommitment>> qcs = GetMineableCommitments(llmqParams, nHeight);
    if (!qcs.has_value()) {
        return false;
    }

    for (const auto& f : qcs.value()) {
        CFinalCommitmentTxPayload qc;
        qc.nHeight = nHeight;
        qc.commitment = f;
        CMutableTransaction tx;
        tx.nVersion = 3;
        tx.nType = TRANSACTION_QUORUM_COMMITMENT;
        SetTxPayload(tx, qc);
        ret.push_back(MakeTransactionRef(tx));
    }

    return true;
}

} // namespace llmq
