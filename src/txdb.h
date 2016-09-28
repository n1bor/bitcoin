// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include "coins.h"
#include "dbwrapper.h"
#include "chain.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <boost/function.hpp>

class CBlockIndex;
class CCoinsViewDBCursor;
class uint256;

//! -dbcache default (MiB)
static const int64_t nDefaultDbCache = 300;
//! max. -dbcache (MiB)
static const int64_t nMaxDbCache = sizeof(void*) > 4 ? 16384 : 1024;
//! min. -dbcache (MiB)
static const int64_t nMinDbCache = 4;
//! Max memory allocated to block tree DB specific cache, if no -txindex (MiB)
static const int64_t nMaxBlockDBCache = 2;
//! Max memory allocated to block tree DB specific cache, if -txindex (MiB)
// Unlike for the UTXO database, for the txindex scenario the leveldb cache make
// a meaningful difference: https://github.com/bitcoin/bitcoin/pull/8273#issuecomment-229601991
static const int64_t nMaxBlockDBAndTxIndexCache = 1024;
//! Max memory allocated to coin DB specific cache (MiB)
static const int64_t nMaxCoinsDBCache = 8;

struct CDiskTxPos : public CDiskBlockPos
{
    unsigned int nTxOffset; // after header

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(*(CDiskBlockPos*)this);
        READWRITE(VARINT(nTxOffset));
    }

    CDiskTxPos(const CDiskBlockPos &blockIn, unsigned int nTxOffsetIn) : CDiskBlockPos(blockIn.nFile, blockIn.nPos), nTxOffset(nTxOffsetIn) {
    }

    CDiskTxPos() {
        SetNull();
    }

    void SetNull() {
        CDiskBlockPos::SetNull();
        nTxOffset = 0;
    }
};

class CChainSnapshot
{
public:
        //! Store snapshots
    int snapshotId; 
    uint256 blockHash; //Hash of block at tip when snapshot taken
    uint256 chainStateHash; //simple hash of chunk hashes.
    std::map<uint256,int> mapChunkHash; //Hash of each chunk and id.
    //id is which of the 8129 chucks it is so we can index straight into
    //leveldb and get it.
    bool updating; //Whether we are calculating the hash of hashes
    bool updated; //Whether the hash of hashes has been calculated
    bool validated; //Whether hash is in chain! Soft Fork needed!
    
    CChainSnapshot() :updating(false),updated(false),validated(false) {};
    
};

class CChainDownloadStatus
{
public:
    enum State { NONE, GETTING_HEADERS,
                GETSNAPSHOT_SENT, GETTING_CHUNKS };
    State state;
    uint256 blockHash;
    uint256 chainStateHash;
    std::map<uint256,int> chunkHashIds;
    std::vector<uint256> chunkHashes;
    std::list<uint256> inflightChunks;
    std::vector<uint256> receivedChunks;
    
    CChainDownloadStatus():state(CChainDownloadStatus::NONE) {};
};

/** CCoinsView backed by the coin database (chainstate/) */
class CCoinsViewDB : public CCoinsView
{
private:
    std::vector<CChainSnapshot*> snapshots;
protected:
    CDBWrapper db;
public:
    CCoinsViewDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    bool GetCoins(const uint256 &txid, CCoins &coins) const;
    bool HaveCoins(const uint256 &txid) const;
    uint256 GetBestBlock() const;
    uint256 GetBestBlock(CChainSnapshot* const snapshot) const;
    bool BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock);
    CCoinsViewCursor *Cursor() const;
    int CreateSnapshot();
    void KeepLastNSnapshots(int n);
    bool DeleteSnapshot(int id);
    CCoinsViewCursor *Cursor(const CChainSnapshot &snapshot) const;
    CCoinsViewCursor *Cursor(const CChainSnapshot &snapshot,uint256 txid) const;
    CChainSnapshot *GetLastSnapshot();
    std::pair<std::vector<CChainSnapshot*>::iterator, 
          std::vector<CChainSnapshot*>::iterator> GetAllSnapshots();
    bool UpdateAllSnapshots();

    std::vector<std::pair<uint256,CCoins>> GetChunk(uint256 chainStateHash,
        uint256 chunkHash);
    
    
};

/** Specialization of CCoinsViewCursor to iterate over a CCoinsViewDB */
class CCoinsViewDBCursor: public CCoinsViewCursor
{
public:
    ~CCoinsViewDBCursor() {}

    bool GetKey(uint256 &key) const;
    bool GetValue(CCoins &coins) const;
    unsigned int GetValueSize() const;

    bool Valid() const;
    void Next();

private:
    CCoinsViewDBCursor(CDBIterator* pcursorIn, const uint256 &hashBlockIn):
        CCoinsViewCursor(hashBlockIn), pcursor(pcursorIn) {}
    std::unique_ptr<CDBIterator> pcursor;
    std::pair<char, uint256> keyTmp;

    friend class CCoinsViewDB;
};

/** Access to the block database (blocks/index/) */
class CBlockTreeDB : public CDBWrapper
{
public:
    CBlockTreeDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);
private:
    CBlockTreeDB(const CBlockTreeDB&);
    void operator=(const CBlockTreeDB&);
public:
    bool WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo);
    bool ReadBlockFileInfo(int nFile, CBlockFileInfo &fileinfo);
    bool ReadLastBlockFile(int &nFile);
    bool WriteReindexing(bool fReindex);
    bool ReadReindexing(bool &fReindex);
    bool ReadTxIndex(const uint256 &txid, CDiskTxPos &pos);
    bool WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> > &list);
    bool WriteFlag(const std::string &name, bool fValue);
    bool ReadFlag(const std::string &name, bool &fValue);
    bool LoadBlockIndexGuts(boost::function<CBlockIndex*(const uint256&)> insertBlockIndex);
};

class CChunkData
{
public:
    // header
    uint256 hashChainChunks;
    uint256 hashChunk;
    std::vector<std::pair<uint256,CCoins>> vTxCoins;
    CChunkData()
    {
        SetNull();
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(hashChainChunks);
        READWRITE(hashChunk);
        READWRITE(vTxCoins);
        
    }

    void SetNull()
    {
        hashChainChunks.SetNull();
        hashChunk.SetNull();
        vTxCoins.clear();
    }
    
    bool IsNull() const
    {
        return (vTxCoins.size() == 0);
    }

    std::string ToString() const;
};
#endif // BITCOIN_TXDB_H
