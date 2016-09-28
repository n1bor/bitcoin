// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdb.h"

#include "chainparams.h"
#include "hash.h"
#include "pow.h"
#include "uint256.h"
#include "sync.h"
#include <stdint.h>

#include <boost/thread.hpp>

using namespace std;

static const char DB_COINS = 'c';
static const char DB_BLOCK_FILES = 'f';
static const char DB_TXINDEX = 't';
static const char DB_BLOCK_INDEX = 'b';

static const char DB_BEST_BLOCK = 'B';
static const char DB_FLAG = 'F';
static const char DB_REINDEX_FLAG = 'R';
static const char DB_LAST_BLOCK = 'l';

CCoinsViewDB::CCoinsViewDB(size_t nCacheSize, bool fMemory, bool fWipe) : db(GetDataDir() / "chainstate", nCacheSize, fMemory, fWipe, true) 
{
}

bool CCoinsViewDB::GetCoins(const uint256 &txid, CCoins &coins) const {
    return db.Read(make_pair(DB_COINS, txid), coins);
}

bool CCoinsViewDB::HaveCoins(const uint256 &txid) const {
    return db.Exists(make_pair(DB_COINS, txid));
}

uint256 CCoinsViewDB::GetBestBlock() const {
    CChainSnapshot s;
    s.snapshotId=-1;
    return GetBestBlock(&s);
}

uint256 CCoinsViewDB::GetBestBlock(CChainSnapshot* const snapshot) const {
    uint256 hashBestChain;
    
    if (!db.Read(DB_BEST_BLOCK, hashBestChain,snapshot->snapshotId))
        return uint256();
    return hashBestChain;
}

bool CCoinsViewDB::BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock) {
    CDBBatch batch(db);
    size_t count = 0;
    size_t changed = 0;
    for (CCoinsMap::iterator it = mapCoins.begin(); it != mapCoins.end();) {
        if (it->second.flags & CCoinsCacheEntry::DIRTY) {
            if (it->second.coins.IsPruned())
                batch.Erase(make_pair(DB_COINS, it->first));
            else
                batch.Write(make_pair(DB_COINS, it->first), it->second.coins);
            changed++;
        }
        count++;
        CCoinsMap::iterator itOld = it++;
        mapCoins.erase(itOld);
    }
    if (!hashBlock.IsNull())
        batch.Write(DB_BEST_BLOCK, hashBlock);

    LogPrint("coindb", "Committing %u changed transactions (out of %u) to coin database...\n", (unsigned int)changed, (unsigned int)count);
    return db.WriteBatch(batch);
}

int CCoinsViewDB::CreateSnapshot() {
    int snapshotId=db.GetSnapsnot();
    CChainSnapshot *ss=new CChainSnapshot();
    ss->snapshotId=snapshotId;
    snapshots.push_back(ss);
    LogPrintf("snapshotted\n");
    return snapshotId;
}

std::pair<std::vector<CChainSnapshot*>::iterator, 
          std::vector<CChainSnapshot*>::iterator> CCoinsViewDB::GetAllSnapshots(){
    return std::make_pair(snapshots.begin(), snapshots.end());
}

bool CCoinsViewDB::UpdateAllSnapshots(){
    LogPrint("snapshot","UpdateAllSnapshots started.\n"); 
    for(CChainSnapshot* const snapshot: snapshots) {
        if(!snapshot->updated && !snapshot->updating){
            //do 1st in naive, but probably OK attempt to avoid races.
            snapshot->updating=true;
            //needs updating all the data
            
            snapshot->blockHash=GetBestBlock(snapshot);
 
            std::unique_ptr<CCoinsViewCursor> pcursor(Cursor(*snapshot));
            CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
            CHashWriter ss2(SER_GETHASH, PROTOCOL_VERSION);
            uint64_t count=0;
            uint64_t outputs=0;
            uint64_t size=0;

            CAmount nTotalAmount = 0;
            int splits=0;
            arith_uint256 oldKeyShifted("0");
            ss<<splits;
            while (pcursor->Valid() ) {
                boost::this_thread::interruption_point();
                uint256 key;
                CCoins coins;
                if (pcursor->GetKey(key) && pcursor->GetValue(coins)) {
                    //LogPrintf("split %d Tx %s\n",splits,key.ToString());
                    arith_uint256 key2 = UintToArith256(key);
                    arith_uint256 byte1=(key2<<248)>>240;
                    arith_uint256 byte2=((key2<<240)>>248);
                    // 2bytes to 65k combinations. Want 8k chunks so need
                    // to lose 3 bits.
                    arith_uint256 keySwitched=byte1+byte2;
                    //LogPrintf("%s %s %s %s\n",key2.ToString(),
                    //        byte1.ToString(),byte2.ToString(),keySwitched.ToString());
                    
                    while(splits != keySwitched>>3){
                        //We need to restart hashing
                        uint256 chunkhash=ss.GetHash();
                        snapshot->mapChunkHash.insert(make_pair(chunkhash,splits));
                        ss2<<chunkhash;
                        //LogPrintf("HASH [%s]\n",chunkhash.ToString());
                        ss.Reset();
                        splits++;
                        ss<<splits;
                    }
                    count++;
                    //This is cut/paste from existing utxo code.
                    //assume correct! Needs checking. If this wrong
                    //attacher could make coins even though hash
                    //matched
                    ss << key;
                    for (unsigned int i=0; i<coins.vout.size(); i++) {
                        const CTxOut &out = coins.vout[i];
                        if (!out.IsNull()) {
                            outputs++;
                            ss << VARINT(i+1);
                            ss << out;
                            nTotalAmount += out.nValue;
                        }
                    }
                    size += 32 + pcursor->GetValueSize();
                    
                    ss << VARINT(0);
                } else {
                    return error("%s: unable to read value", __func__);
                }
                pcursor->Next();

            }
            uint256 chunkhash=ss.GetHash();
            snapshot->mapChunkHash.insert(make_pair(chunkhash,splits));
            ss2<<chunkhash;
            //LogPrintf("HASH [%s]\n",chunkhash.ToString());
            
            
            snapshot->chainStateHash=ss2.GetHash();
            snapshot->updated=true;
            snapshot->updating=false;
            LogPrint("snapshot","snapshot with %u splits, %u tx, %u utxos,  %u bytes, chainhash %s\n",
                    (unsigned int)splits,(unsigned int)count,
                    (unsigned int)outputs,(unsigned int)size,
                    snapshot->chainStateHash.ToString());
            
        }
        
    
    }
    LogPrint("snapshot","UpdateAllSnapshots ran.\n"); 
    return true;
    
}

std::vector<std::pair<uint256,CCoins>> CCoinsViewDB::GetChunk(uint256 chainStateHash,
        uint256 chunkHash){

    std::vector<std::pair<uint256,CCoins>> data;
    
    for(CChainSnapshot* const snapshot: snapshots) {
        //TODO add in validated once miners start mining!
        if(true  || (snapshot->updated && !snapshot->updating)){  
            if(snapshot->chainStateHash==chainStateHash){
                LogPrint("chaindownload","Found Chainstate.\n");
                //Have match so process
                std::map<uint256,int>::iterator it=snapshot->mapChunkHash.find(chunkHash);
                if(it==snapshot->mapChunkHash.end()){
                    LogPrint("chaindownload","Chunk not found.\n");
                } else {
                    // we found it
                    int chunkId=it->second;
                    // now hard bit - working out the 1st possible transactionID!
                    arith_uint256 raw=(uint64_t)chunkId;
                    
                    raw=raw<<3;
                    arith_uint256 byte1=(raw<<248)>>240;
                    arith_uint256 byte2=(raw>>8);
                    arith_uint256 firstTx=byte1+byte2;

                    LogPrint("chaindownload","start %s.\n",firstTx.ToString());
                    
                    std::unique_ptr<CCoinsViewCursor> pcursor(Cursor(*snapshot,ArithToUint256(firstTx)));
                    int txCount=0;
                    while (pcursor->Valid() ) {
                        boost::this_thread::interruption_point();
                        uint256 key;
                        CCoins coins;
                        if (pcursor->GetKey(key) && pcursor->GetValue(coins)) {
                            //LogPrintf("got tx Tx %s\n",key.ToString());
                            //This shuffling is cos leveldb seens to sort with
                            //wrong endian - fix is this.
                            arith_uint256 key2 = UintToArith256(key);
                            arith_uint256 byte1=(key2<<248)>>240;
                            arith_uint256 byte2=((key2<<240)>>248);
                            // 2bytes so 64k combinations. Want 8k chunks so need
                            // to lose 3 bits.
                            arith_uint256 keySwitched=byte1+byte2;
                            keySwitched=keySwitched>>3;
                            int id=keySwitched.GetLow64();
                            //LogPrintf("%s %s %s %s\n",key2.ToString(),
                            //        byte1.ToString(),byte2.ToString(),keySwitched.ToString());
                            
                            if(id != chunkId){
                                
                                LogPrint("chaindownload"," break id %d Tx %s\n",id,key.ToString());
                                break;
                            }
                            ++txCount;
                            data.push_back(make_pair(key,coins));
                        } else {
                            LogPrintf("%s: unable to read value", __func__);
                            return data;
                        }
                        pcursor->Next();

                    }
                    LogPrint("chaindownload","end of database tx=%d\n",txCount);
                }
                return data;
            }
        }  
    }
    
    
    return data;
}


void CCoinsViewDB::KeepLastNSnapshots(int n){
    // this is very bad logic! TODO tidy up!
    for (unsigned i=snapshots.size()-n-1; i<snapshots.size(); --i) {
        LogPrint("snapshot","Deleting snapshot %d\n",i);
        DeleteSnapshot(snapshots[i]->snapshotId);
    }   
}

bool CCoinsViewDB::DeleteSnapshot(int id){
    
    for (unsigned i=0; i<snapshots.size(); ++i) {
        if(snapshots[i]->snapshotId==id){
            db.ReleaseSnapshot(id);
            delete snapshots[i];
            snapshots.erase(snapshots.begin()+i);
            return true;
        }
    }
    return false;
}

CBlockTreeDB::CBlockTreeDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "blocks" / "index", nCacheSize, fMemory, fWipe) {
}

bool CBlockTreeDB::ReadBlockFileInfo(int nFile, CBlockFileInfo &info) {
    return Read(make_pair(DB_BLOCK_FILES, nFile), info);
}

bool CBlockTreeDB::WriteReindexing(bool fReindexing) {
    if (fReindexing)
        return Write(DB_REINDEX_FLAG, '1');
    else
        return Erase(DB_REINDEX_FLAG);
}

bool CBlockTreeDB::ReadReindexing(bool &fReindexing) {
    fReindexing = Exists(DB_REINDEX_FLAG);
    return true;
}

bool CBlockTreeDB::ReadLastBlockFile(int &nFile) {
    return Read(DB_LAST_BLOCK, nFile);
}

CCoinsViewCursor *CCoinsViewDB::Cursor() const
{
    CCoinsViewDBCursor *i = new CCoinsViewDBCursor(const_cast<CDBWrapper*>(&db)->NewIterator(), GetBestBlock());
    /* It seems that there are no "const iterators" for LevelDB.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    i->pcursor->Seek(DB_COINS);
    // Cache key of first record
    i->pcursor->GetKey(i->keyTmp);
    return i;
}

CCoinsViewCursor *CCoinsViewDB::Cursor(const CChainSnapshot &snapshot) const
{
    CCoinsViewDBCursor *i = new CCoinsViewDBCursor(const_cast<CDBWrapper*>(&db)->NewIterator(snapshot.snapshotId), GetBestBlock());
    /* It seems that there are no "const iterators" for LevelDB.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    i->pcursor->Seek(DB_COINS);
    // Cache key of first record
    i->pcursor->GetKey(i->keyTmp);
    return i;
}

CCoinsViewCursor *CCoinsViewDB::Cursor(const CChainSnapshot &snapshot,uint256 txid) const
{
    CCoinsViewDBCursor *i = new CCoinsViewDBCursor(const_cast<CDBWrapper*>(&db)->NewIterator(snapshot.snapshotId), GetBestBlock());
    /* It seems that there are no "const iterators" for LevelDB.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    i->pcursor->Seek(make_pair(DB_COINS, txid));
    // Cache key of first record
    i->pcursor->GetKey(i->keyTmp);
    return i;
}

bool CCoinsViewDBCursor::GetKey(uint256 &key) const
{
    // Return cached key
    if (keyTmp.first == DB_COINS) {
        key = keyTmp.second;
        return true;
    }
    return false;
}

bool CCoinsViewDBCursor::GetValue(CCoins &coins) const
{
    return pcursor->GetValue(coins);
}

unsigned int CCoinsViewDBCursor::GetValueSize() const
{
    return pcursor->GetValueSize();
}

bool CCoinsViewDBCursor::Valid() const
{
    return keyTmp.first == DB_COINS;
}

void CCoinsViewDBCursor::Next()
{
    pcursor->Next();
    if (!pcursor->Valid() || !pcursor->GetKey(keyTmp))
        keyTmp.first = 0; // Invalidate cached key after last record so that Valid() and GetKey() return false
}

bool CBlockTreeDB::WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<int, const CBlockFileInfo*> >::const_iterator it=fileInfo.begin(); it != fileInfo.end(); it++) {
        batch.Write(make_pair(DB_BLOCK_FILES, it->first), *it->second);
    }
    batch.Write(DB_LAST_BLOCK, nLastFile);
    for (std::vector<const CBlockIndex*>::const_iterator it=blockinfo.begin(); it != blockinfo.end(); it++) {
        batch.Write(make_pair(DB_BLOCK_INDEX, (*it)->GetBlockHash()), CDiskBlockIndex(*it));
    }
    return WriteBatch(batch, true);
}

bool CBlockTreeDB::ReadTxIndex(const uint256 &txid, CDiskTxPos &pos) {
    return Read(make_pair(DB_TXINDEX, txid), pos);
}

bool CBlockTreeDB::WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> >&vect) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<uint256,CDiskTxPos> >::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Write(make_pair(DB_TXINDEX, it->first), it->second);
    return WriteBatch(batch);
}

bool CBlockTreeDB::WriteFlag(const std::string &name, bool fValue) {
    return Write(std::make_pair(DB_FLAG, name), fValue ? '1' : '0');
}

bool CBlockTreeDB::ReadFlag(const std::string &name, bool &fValue) {
    char ch;
    if (!Read(std::make_pair(DB_FLAG, name), ch))
        return false;
    fValue = ch == '1';
    return true;
}

bool CBlockTreeDB::LoadBlockIndexGuts(boost::function<CBlockIndex*(const uint256&)> insertBlockIndex)
{
    std::unique_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_BLOCK_INDEX, uint256()));

    // Load mapBlockIndex
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key;
        if (pcursor->GetKey(key) && key.first == DB_BLOCK_INDEX) {
            CDiskBlockIndex diskindex;
            if (pcursor->GetValue(diskindex)) {
                // Construct block index object
                CBlockIndex* pindexNew = insertBlockIndex(diskindex.GetBlockHash());
                pindexNew->pprev          = insertBlockIndex(diskindex.hashPrev);
                pindexNew->nHeight        = diskindex.nHeight;
                pindexNew->nFile          = diskindex.nFile;
                pindexNew->nDataPos       = diskindex.nDataPos;
                pindexNew->nUndoPos       = diskindex.nUndoPos;
                pindexNew->nVersion       = diskindex.nVersion;
                pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
                pindexNew->nTime          = diskindex.nTime;
                pindexNew->nBits          = diskindex.nBits;
                pindexNew->nNonce         = diskindex.nNonce;
                pindexNew->nStatus        = diskindex.nStatus;
                pindexNew->nTx            = diskindex.nTx;

                if (!CheckProofOfWork(pindexNew->GetBlockHash(), pindexNew->nBits, Params().GetConsensus()))
                    return error("LoadBlockIndex(): CheckProofOfWork failed: %s", pindexNew->ToString());

                pcursor->Next();
            } else {
                return error("LoadBlockIndex() : failed to read value");
            }
        } else {
            break;
        }
    }

    return true;
}

std::string CChunkData::ToString() const
{
    std::stringstream s;
    s << strprintf("CChunkData(hashChainChunks=%s, hashChunk=%s, txCount=%d)\n",
        hashChainChunks.ToString(),
        hashChunk.ToString(),
        vTxCoins.size());
    return s.str();
}