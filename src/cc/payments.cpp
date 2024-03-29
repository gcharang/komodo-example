/******************************************************************************
 * Copyright © 2014-2019 The SuperNET Developers.                             *
 *                                                                            *
 * See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
 * the top-level directory of this distribution for the individual copyright  *
 * holder information and the developer policies on copyright and licensing.  *
 *                                                                            *
 * Unless otherwise agreed in a custom licensing agreement, no part of the    *
 * SuperNET software, including this file may be copied, modified, propagated *
 * or distributed except according to the terms contained in the LICENSE file *
 *                                                                            *
 * Removal or modification of this copyright notice is prohibited.            *
 *                                                                            *
 ******************************************************************************/

#include "CCPayments.h"

/* 
 0) txidopret <- allocation, scriptPubKey, opret
 1) create <-  locked_blocks, minrelease, list of txidopret
 
 2) fund createtxid amount opretflag to global CC address with opret or txidaddr without
 
 3) release amount -> vout[i] will be scriptPubKeys[i] and (amount * allocations[i]) / sumallocations[] (only using vins that have been locked for locked_blocks+). 
 
 4) info txid -> display parameters, funds
 5) list -> all txids
 
 First step is to create txids with the info needed in their opreturns. this info is the weight, scriptPubKey and opret if needed. To do that txidopret is used:
 
 ./c is a script that invokes komodo-cli with the correct -ac_name
 
 ./c paymentstxidopret \"[9,%222102d6f13a8f745921cdb811e32237bb98950af1a5952be7b3d429abd9152f8e388dac%22]\" -> rawhex with txid 95d9fc8d8a3ef63693c7427e59ff5e177ef63b7345d5f6d6497ac262699a8def
 
 ./c paymentstxidopret \"[1,%2221039433dc3749aece1bd568f374a45da3b0bc6856990d7da3cd175399577940a775ac%22]\" -> rawhex txid 00469695a08b975ceaf7258896abbf1455eb0f383e8a98fc650deace4cbf02a1
 
 now we have 2 txid with the required info in the opreturn. one of them has a 9 and the other a 1 for a 90%/10% split.
 
 ./c paymentscreate \"[0,0,%2295d9fc8d8a3ef63693c7427e59ff5e177ef63b7345d5f6d6497ac262699a8def%22,%2200469695a08b975ceaf7258896abbf1455eb0f383e8a98fc650deace4cbf02a1%22]\" -> created txid 318d827cc6d8f25f40517e7fb0982e3f707b4aa749d322483fc336686a87b28a that will be the createtxid that the other rpc calls will use.
 
 lets see if this appears in the list
 
 ./c paymentslist ->
 {
 "result": "success",
 "createtxids": [
 "318d827cc6d8f25f40517e7fb0982e3f707b4aa749d322483fc336686a87b28a"
 ]
 }
 
 It appeared! now lets get more info on it:
 ./c paymentsinfo \"[%22318d827cc6d8f25f40517e7fb0982e3f707b4aa749d322483fc336686a87b28a%22]\"
 {
 "lockedblocks": 0,
 "totalallocations": 10,
 "minrelease": 0,
 "RWRM36sC8jSctyFZtsu7CyDcHYPdZX7nPZ": 0.00000000,
 "REpyKi7avsVduqZ3eimncK4uKqSArLTGGK": 0.00000000,
 "totalfunds": 0.00000000,
 "result": "success"
 }
 
 There are 2 possible places the funds for this createtxid can be, the first is the special address that is derived from combining the globalCC address with the txidaddr. txidaddr is a non-spendable markeraddress created by converting the txid into a 33 byte pubkey by prefixing 0x02 to the txid. It is a 1of2 address, so it doesnt matter that nobody knows the privkey for this txidaddr. the second address is the global CC address and only utxo to that address with an opreturn containing the createtxid are funds valid for this payments CC createtxid
 
 next let us add some funds to it. the funds can be to either of the two addresses, controlled by useopret (defaults to 0)
 
 ./c paymentsfund \"[%22318d827cc6d8f25f40517e7fb0982e3f707b4aa749d322483fc336686a87b28a%22,1,0]\" -> txid 28f69b925bb7a21d2a3ba2327e85eb2031b014e976e43f5c2c6fb8a76767b221, which indeed sent funds to RWRM36sC8jSctyFZtsu7CyDcHYPdZX7nPZ without an opreturn and it appears on the payments info.
 
 ./c paymentsfund \"[%22318d827cc6d8f25f40517e7fb0982e3f707b4aa749d322483fc336686a87b28a%22,1,1]\" -> txid cc93330b5c951b724b246b3b138d00519c33f2a600a7c938bc9e51aff6e20e32, which indeed sent funds to REpyKi7avsVduqZ3eimncK4uKqSArLTGGK with an opreturn and it appears on the payments info.

 
./c paymentsrelease \"[%22318d827cc6d8f25f40517e7fb0982e3f707b4aa749d322483fc336686a87b28a%22,1.5]\" -> a8d5dbbb8ee94c05e75c4f3c5221091f59dcb86e0e9c4e1e3d2cf69e6fce6b81
 
 it used both fund utxos
 
*/

// start of consensus code

CScript EncodePaymentsTxidOpRet(int64_t allocation,std::vector<uint8_t> scriptPubKey,std::vector<uint8_t> destopret)
{
    CScript opret; uint8_t evalcode = EVAL_PAYMENTS;
    opret << OP_RETURN << E_MARSHAL(ss << evalcode << 'T' << allocation << scriptPubKey << destopret);
    return(opret);
}

uint8_t DecodePaymentsTxidOpRet(CScript scriptPubKey,int64_t &allocation,std::vector<uint8_t> &destscriptPubKey,std::vector<uint8_t> &destopret)
{
    std::vector<uint8_t> vopret; uint8_t *script,e,f;
    GetOpReturnData(scriptPubKey, vopret);
    script = (uint8_t *)vopret.data();
    if ( vopret.size() > 2 && E_UNMARSHAL(vopret,ss >> e; ss >> f; ss >> allocation; ss >> destscriptPubKey; ss >> destopret) != 0 )
    {
        if ( e == EVAL_PAYMENTS && f == 'T' )
            return(f);
    }
    return(0);
}

CScript EncodePaymentsFundOpRet(uint256 checktxid)
{
    CScript opret; uint8_t evalcode = EVAL_PAYMENTS;
    opret << OP_RETURN << E_MARSHAL(ss << evalcode << 'F' << checktxid);
    return(opret);
}

uint8_t DecodePaymentsFundOpRet(CScript scriptPubKey,uint256 &checktxid)
{
    std::vector<uint8_t> vopret; uint8_t *script,e,f;
    GetOpReturnData(scriptPubKey, vopret);
    script = (uint8_t *)vopret.data();
    if ( vopret.size() > 2 && E_UNMARSHAL(vopret,ss >> e; ss >> f; ss >> checktxid) != 0 )
    {
        if ( e == EVAL_PAYMENTS && f == 'F' )
            return(f);
    }
    return(0);
}

CScript EncodePaymentsMergeOpRet(uint256 checktxid)
{
    CScript opret; uint8_t evalcode = EVAL_PAYMENTS;
    opret << OP_RETURN << E_MARSHAL(ss << evalcode << 'M' << checktxid);
    return(opret);
}

uint8_t DecodePaymentsMergeOpRet(CScript scriptPubKey,uint256 &checktxid)
{
    std::vector<uint8_t> vopret; uint8_t *script,e,f;
    GetOpReturnData(scriptPubKey, vopret);
    script = (uint8_t *)vopret.data();
    if ( vopret.size() > 2 && E_UNMARSHAL(vopret,ss >> e; ss >> f; ss >> checktxid) != 0 )
    {
        if ( e == EVAL_PAYMENTS && f == 'M' )
            return(f);
    }
    return(0);
}

CScript EncodePaymentsOpRet(int32_t lockedblocks,int32_t minrelease,int64_t totalallocations,std::vector<uint256> txidoprets)
{
    CScript opret; uint8_t evalcode = EVAL_PAYMENTS;
    opret << OP_RETURN << E_MARSHAL(ss << evalcode << 'C' << lockedblocks << minrelease << totalallocations << txidoprets);
    return(opret);
}

uint8_t DecodePaymentsOpRet(CScript scriptPubKey,int32_t &lockedblocks,int32_t &minrelease,int64_t &totalallocations,std::vector<uint256> &txidoprets)
{
    std::vector<uint8_t> vopret; uint8_t *script,e,f;
    GetOpReturnData(scriptPubKey, vopret);
    script = (uint8_t *)vopret.data();
    if ( vopret.size() > 2 && E_UNMARSHAL(vopret,ss >> e; ss >> f; ss >> lockedblocks; ss >> minrelease; ss >> totalallocations; ss >> txidoprets) != 0 )
    {
        if ( e == EVAL_PAYMENTS && f == 'C' && txidoprets.size() > 1 )
            return(f);
    }
    return(0);
}

CScript EncodePaymentsSnapsShotOpRet(int32_t lockedblocks,int32_t minrelease,int32_t minimum,int32_t top,int32_t bottom,int8_t fixedAmount,std::vector<std::vector<uint8_t>> excludeScriptPubKeys)
{
    CScript opret; uint8_t evalcode = EVAL_PAYMENTS;
    if ( (strcmp(ASSETCHAINS_SYMBOL, "CFEKPAY") == 0) ) // exempt for now, remove this after game completed.
    {
        minimum = 10000;
        opret << OP_RETURN << E_MARSHAL(ss << evalcode << 'S' << lockedblocks << minrelease << top << bottom << fixedAmount << excludeScriptPubKeys);
    }
    else 
        opret << OP_RETURN << E_MARSHAL(ss << evalcode << 'S' << lockedblocks << minrelease << minimum << top << bottom << fixedAmount << excludeScriptPubKeys);
    return(opret);
}

uint8_t DecodePaymentsSnapsShotOpRet(CScript scriptPubKey,int32_t &lockedblocks,int32_t &minrelease,int32_t &minimum,int32_t &top,int32_t &bottom,int8_t &fixedAmount,std::vector<std::vector<uint8_t>> &excludeScriptPubKeys)
{
    std::vector<uint8_t> vopret; uint8_t *script,e,f;
    GetOpReturnData(scriptPubKey, vopret);
    script = (uint8_t *)vopret.data();
    if ( (strcmp(ASSETCHAINS_SYMBOL, "CFEKPAY") == 0) )  // exempt for now, remove this after game completed.
    {
        minimum = 10000;
        if ( vopret.size() > 2 && E_UNMARSHAL(vopret,ss >> e; ss >> f; ss >> lockedblocks; ss >> minrelease; ss >> top; ; ss >> bottom; ss >> fixedAmount; ss >> excludeScriptPubKeys) != 0 )
        {
            if ( e == EVAL_PAYMENTS && f == 'S' )
                return(f);
        }
    }
    else
    {
        
        if ( vopret.size() > 2 && E_UNMARSHAL(vopret,ss >> e; ss >> f; ss >> lockedblocks; ss >> minrelease; ss >> minimum; ss >> top; ; ss >> bottom; ss >> fixedAmount; ss >> excludeScriptPubKeys) != 0 )
        {
            if ( e == EVAL_PAYMENTS && f == 'S' )
                return(f);
        }
    }
    return(0);
}

CScript EncodePaymentsTokensOpRet(int32_t lockedblocks,int32_t minrelease,int32_t top,std::vector<std::vector<uint8_t>> excludeScriptPubKeys, uint256 tokenid)
{
    CScript opret; uint8_t evalcode = EVAL_PAYMENTS;
    opret << OP_RETURN << E_MARSHAL(ss << evalcode << 'O' << lockedblocks << minrelease << top << excludeScriptPubKeys << tokenid);
    return(opret);
}

uint8_t DecodePaymentsTokensOpRet(CScript scriptPubKey,int32_t &lockedblocks,int32_t &minrelease,int32_t &top,std::vector<std::vector<uint8_t>>  &excludeScriptPubKeys, uint256 &tokenid)
{
    std::vector<uint8_t> vopret; uint8_t *script,e,f;
    GetOpReturnData(scriptPubKey, vopret);
    script = (uint8_t *)vopret.data();
    if ( vopret.size() > 2 && E_UNMARSHAL(vopret,ss >> e; ss >> f; ss >> lockedblocks; ss >> minrelease; ss >> top; ss >> excludeScriptPubKeys; ss >> tokenid) != 0 )
    {
        if ( e == EVAL_PAYMENTS && f == 'O' )
            return(f);
    }
    return(0);
} 

int64_t IsPaymentsvout(struct CCcontract_info *cp,const CTransaction& tx,int32_t v,char *cmpaddr, CScript &ccopret)
{
    char destaddr[64];
    //if ( tx.vout[v].scriptPubKey.IsPayToCryptoCondition() != 0 )
    if ( getCCopret(tx.vout[v].scriptPubKey, ccopret) )
    {
        if ( Getscriptaddress(destaddr,tx.vout[v].scriptPubKey) > 0 && (cmpaddr[0] == 0 || strcmp(destaddr,cmpaddr) == 0) )
            return(tx.vout[v].nValue);
    }
    return(0);
}

void pub2createtxid(char *str)
{
    int i,n;
    char *rev;
    n = (int32_t)strlen(str);
    rev = (char *)malloc(n + 1);
    for (i=0; i<n; i+=2)
    {
        rev[n-2-i] = str[i];
        rev[n-1-i] = str[i+1];
    }
    rev[n] = 0;
    strcpy(str,rev);
    free(rev);
}

bool payments_game(int32_t &top, int32_t &bottom)
{
    uint64_t x;
    uint256 tmphash = chainActive[lastSnapShotHeight]->GetBlockHash();
    memcpy(&x,&tmphash,sizeof(x));
    bottom = ((x & 0xff) % 50);
    if ( bottom == 0 ) bottom = 1;
    top = (((x>>8) & 0xff) % 100);
    if ( top < 50 ) top += 50;
    bottom = (vAddressSnapshot.size()*bottom)/100;
    top = (vAddressSnapshot.size()*top)/100;
    fprintf(stderr, "bottom.%i top.%i\n",bottom,top);
    return true;
}

bool payments_lockedblocks(uint256 blockhash,int32_t lockedblocks,int32_t &blocksleft)
{
    int32_t ht = chainActive.Height();
    CBlockIndex* pblockindex = komodo_blockindex(blockhash);
    if ( pblockindex == 0 || pblockindex->GetHeight()+lockedblocks > ht)
    {
        blocksleft = pblockindex->GetHeight()+lockedblocks - ht;
        fprintf(stderr, "not elegible to be spent yet height.%i vs elegible_ht.%i blocksleft.%i\n",ht,(pblockindex!=0?pblockindex->GetHeight():0)+lockedblocks,blocksleft);
        return false; 
    }
    return true;
}

bool PaymentsValidate(struct CCcontract_info *cp,Eval* eval,const CTransaction &tx, uint32_t nIn)
{
    // one of two addresses
    // change must go to 1of2 txidaddr
    //    change is/must be in vout[0]
    // only 'F' or 1of2 txidaddr can be spent
    // all vouts must match exactly
    char temp[128], coinaddr[64]={0}, txidaddr[64]; std::string scriptpubkey; uint256 createtxid, blockhash, tokenid; CTransaction plantx; int8_t funcid=0, fixedAmount=0;
    int32_t i,lockedblocks,minrelease; int64_t change,totalallocations; std::vector<uint256> txidoprets; bool fHasOpret = false,fIsMerge = false; CPubKey txidpk,Paymentspk;
    int32_t top,bottom=0,minimum=10000; std::vector<std::vector<uint8_t>> excludeScriptPubKeys; bool fFixedAmount = false; CScript ccopret;
    mpz_t mpzTotalAllocations, mpzAllocation;; mpz_init(mpzTotalAllocations);
    // user marker vout to get the createtxid
    if ( tx.vout.size() == 1 )
    {
        if ( IsPaymentsvout(cp,tx,0,coinaddr,ccopret) != 0 && ccopret.size() > 2 && DecodePaymentsMergeOpRet(ccopret,createtxid) )
        {
            fIsMerge = true;
        } else return(eval->Invalid("not enough vouts"));
    }
    else if ( tx.vout.back().scriptPubKey[0] == OP_RETURN )
    {
        scriptpubkey = HexStr(tx.vout[tx.vout.size()-2].scriptPubKey.begin()+2, tx.vout[tx.vout.size()-2].scriptPubKey.end()-1);  
        fHasOpret = true;
    } 
    else scriptpubkey = HexStr(tx.vout[tx.vout.size()-1].scriptPubKey.begin()+2,tx.vout[tx.vout.size()-1].scriptPubKey.end()-1);
    if ( !fIsMerge )
    {
        strcpy(temp, scriptpubkey.c_str());
        pub2createtxid(temp);
        createtxid = Parseuint256(temp);
    }
    //printf("createtxid.%s\n",createtxid.ToString().c_str());
    
    // use the createtxid to fetch the tx and all of the plans info.
    if ( myGetTransaction(createtxid,plantx,blockhash) != 0 && plantx.vout.size() > 0 )
    {                                                                                                                        
        if ( ((funcid= DecodePaymentsOpRet(plantx.vout[plantx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,totalallocations,txidoprets)) == 'C' || (funcid= DecodePaymentsSnapsShotOpRet(plantx.vout[plantx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,minimum,top,bottom,fixedAmount,excludeScriptPubKeys)) == 'S' || (funcid= DecodePaymentsTokensOpRet(plantx.vout[plantx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,top,excludeScriptPubKeys,tokenid)) == 'O') )
        {
            if ( lockedblocks < 0 || minrelease < 0 || (totalallocations <= 0 && top <= 0 ) )
                return(eval->Invalid("negative values"));
            if ( minimum < 10000 )
                return(eval->Invalid("minimum must be over 10000"));
            Paymentspk = GetUnspendable(cp,0);
            txidpk = CCtxidaddr(txidaddr,createtxid);
            GetCCaddress1of2(cp,coinaddr,Paymentspk,txidpk);
            //fprintf(stderr, "lockedblocks.%i minrelease.%i totalallocations.%i txidopret1.%s txidopret2.%s\n",lockedblocks, minrelease, totalallocations, txidoprets[0].ToString().c_str(), txidoprets[1].ToString().c_str() );
            if ( !CheckTxFee(tx, PAYMENTS_TXFEE+1, chainActive.LastTip()->GetHeight(), chainActive.LastTip()->nTime) )
                return eval->Invalid("txfee is too high");
            // make sure change is in vout 0 and is paying to the contract address.
            if ( (change= IsPaymentsvout(cp,tx,0,coinaddr,ccopret)) == 0 )
                return(eval->Invalid("change is in wrong vout or is wrong tx type"));
            
            if ( !fIsMerge )
            {
                // Get all the script pubkeys and allocations
                std::vector<int64_t> allocations;
                std::vector<CScript> scriptPubKeys;
                int64_t checkallocations = 0;
                i = 0;
                if ( funcid == 'C' )
                {
                    // normal payment
                    for (const uint256& txidopret : txidoprets)
                    {
                        CTransaction tx0; std::vector<uint8_t> scriptPubKey,opret; int64_t allocation;
                        if ( myGetTransaction(txidopret,tx0,blockhash) != 0 && tx0.vout.size() > 1 && DecodePaymentsTxidOpRet(tx0.vout[tx0.vout.size()-1].scriptPubKey,allocation,scriptPubKey,opret) == 'T' )
                        {
                            scriptPubKeys.push_back(CScript(scriptPubKey.begin(), scriptPubKey.end()));
                            allocations.push_back(allocation);
                            //fprintf(stderr, "i.%i scriptpubkey.%s allocation.%li\n",i,scriptPubKeys[i].ToString().c_str(),allocation);
                            checkallocations += allocation;
                            // if we have an op_return to pay to need to check it exists and is paying the correct opret. 
                            if ( !opret.empty() )
                            {
                                if ( !fHasOpret )
                                {
                                    fprintf(stderr, "missing opret.%s in payments release.\n",HexStr(opret.begin(), opret.end()).c_str());
                                    return(eval->Invalid("missing opret in payments release"));
                                }
                                else if ( CScript(opret.begin(),opret.end()) != tx.vout[tx.vout.size()-1].scriptPubKey )
                                {
                                    fprintf(stderr, "opret.%s vs opret.%s\n",HexStr(opret.begin(), opret.end()).c_str(), HexStr(tx.vout[tx.vout.size()-1].scriptPubKey.begin(), tx.vout[tx.vout.size()-1].scriptPubKey.end()).c_str());
                                    return(eval->Invalid("pays incorrect opret"));
                                }
                            }
                        }
                        i++;
                    }
                    mpz_set_si(mpzTotalAllocations,totalallocations);
                }
                else if ( funcid == 'S' )
                {
                    if ( KOMODO_SNAPSHOT_INTERVAL == 0 )
                        return(eval->Invalid("snapshots not activated on this chain"));
                    if ( vAddressSnapshot.size() == 0 )
                        return(eval->Invalid("need first snapshot"));
                    if ( top > 3999 )
                        return(eval->Invalid("transaction too big"));
                    if ( fixedAmount == 7 ) 
                    {
                        // game setting, randomise bottom and top values 
                        fFixedAmount = payments_game(top,bottom);
                    }
                    else if ( fixedAmount != 0 )
                    {
                        fFixedAmount = true;
                    }
                    for (int32_t j = bottom; j < vAddressSnapshot.size(); j++)
                    {
                        auto &address = vAddressSnapshot[j];
                        CScript scriptPubKey = GetScriptForDestination(address.second); bool skip = false;
                        for ( auto skipkey : excludeScriptPubKeys ) 
                        {
                            if ( scriptPubKey == CScript(skipkey.begin(), skipkey.end()) )
                            {
                                skip = true;
                                //fprintf(stderr, "SKIPPED::: %s\n", CBitcoinAddress(address.second).ToString().c_str());
                            } 
                        }
                        if ( !skip )
                        {
                            mpz_init(mpzAllocation); 
                            i++;
                            scriptPubKeys.push_back(scriptPubKey);
                            allocations.push_back(address.first);
                            mpz_set_si(mpzAllocation,address.first);
                            mpz_add(mpzTotalAllocations,mpzTotalAllocations,mpzAllocation); 
                            mpz_clear(mpzAllocation);
                        }
                        if ( i+bottom == top ) // we reached top amount to pay, it can be less than this!
                            break;
                    }
                    if ( i != tx.vout.size()-2 )
                        return(eval->Invalid("pays wrong amount of recipients"));
                }
                else if ( funcid == 'O' )
                {
                    // tokens snapshot.
                }
                // sanity check to make sure we got all the required info, skip for merge type tx
                //fprintf(stderr, " allocations.size().%li scriptPubKeys.size.%li\n",allocations.size(), scriptPubKeys.size());
                if ( (allocations.size() == 0 || scriptPubKeys.size() == 0 || allocations.size() != scriptPubKeys.size()) )
                    return(eval->Invalid("missing data cannot validate"));
                    
                //fprintf(stderr, "totalallocations.%li checkallocations.%li\n",totalallocations, checkallocations);
                if ( funcid == 'C' && totalallocations != checkallocations ) // only check for normal payments release. 
                    return(eval->Invalid("allocation missmatch"));

                // Check vouts go to the right place and pay the right amounts. 
                int64_t amount = 0, checkamount; int32_t n = 0; 
                checkamount = tx.GetValueOut() - change - PAYMENTS_TXFEE;
                mpz_t mpzCheckamount; mpz_init(mpzCheckamount); mpz_set_si(mpzCheckamount,checkamount); 
                for (i = 1; i < (fHasOpret ? tx.vout.size()-2 : tx.vout.size()-1); i++) 
                {
                    if ( scriptPubKeys[n] != tx.vout[i].scriptPubKey )
                    {
                        fprintf(stderr, "pays wrong destination destscriptPubKey.%s voutscriptPubKey.%s\n", HexStr(scriptPubKeys[n].begin(),scriptPubKeys[n].end()).c_str(), HexStr(tx.vout[i].scriptPubKey.begin(),tx.vout[i].scriptPubKey.end()).c_str());
                        return(eval->Invalid("pays wrong address"));
                    }
                    int64_t test;
                    if ( fFixedAmount )
                    {
                        test = checkamount / (top-bottom);
                    }
                    else 
                    {
                        mpz_init(mpzAllocation); 
                        mpz_set_si(mpzAllocation,allocations[n]);
                        mpz_mul(mpzAllocation,mpzAllocation,mpzCheckamount);
                        mpz_cdiv_q(mpzAllocation,mpzAllocation,mpzTotalAllocations);
                        test = mpz_get_si(mpzAllocation);
                        mpz_clear(mpzAllocation);
                    }
                    // Vairance of 1 sat is allowed, for rounding errors.
                    if ( test >= tx.vout[i].nValue+1 && test <= tx.vout[i].nValue-1 )
                    {
                        fprintf(stderr, "vout.%i test.%li vs nVlaue.%li\n",i, test, tx.vout[i].nValue);
                        return(eval->Invalid("amounts do not match"));
                    }
                    if ( test < minimum )
                    {
                        fprintf(stderr, "vout.%i test.%li vs minimum.%i\n",i, test, minimum);
                        return(eval->Invalid("under minimum size"));
                    }
                    amount += tx.vout[i].nValue;
                    n++;
                }
                mpz_clear(mpzTotalAllocations);
                // This is a backup check to make sure there are no extra vouts paying something else!
                if ( checkamount != amount )
                    return(eval->Invalid("amounts do not match"));
                
                if ( amount < minrelease*COIN )
                {
                    fprintf(stderr, "does not meet minrelease amount.%li minrelease.%li\n",amount, (int64_t)minrelease*COIN );
                    return(eval->Invalid("amount is too small"));
                }
            }
            // Check vins
            i = 0; int32_t dust = 0; 
            int32_t blocksleft;
            BOOST_FOREACH(const CTxIn& vin, tx.vin)
            {
                CTransaction txin; 
                if ( myGetTransaction(vin.prevout.hash,txin,blockhash) )
                {
                    // check the vin comes from the CC address's
                    char destaddr[64]; int32_t mergeoffset = 0; CScript opret; uint256 checktxid;
                    Getscriptaddress(destaddr,txin.vout[vin.prevout.n].scriptPubKey);
                    if ( fIsMerge && txin.vout[vin.prevout.n].nValue < COIN )
                        dust++;
                    if ( strcmp(destaddr,coinaddr) != 0 )
                    {
                        // if does not come from address its in the global payments adddress and we need to check the opreturn.
                        uint256 checktxid; int32_t opret_ind;
                        if ( (opret_ind= has_opret(txin, EVAL_PAYMENTS)) == 0 )
                            getCCopret(txin.vout[vin.prevout.n].scriptPubKey,opret); // get op_return from CCvout, 
                        else
                            opret = txin.vout[opret_ind].scriptPubKey; 
                        if ( DecodePaymentsFundOpRet(opret,checktxid) != 'F' || checktxid != createtxid )
                        {
                            fprintf(stderr, "vin.%i is not a payments CC vout: txid.%s\n", i, txin.GetHash().ToString().c_str());
                            return(eval->Invalid("vin is not paymentsCC type"));
                        }
                    }
                    else if ( fIsMerge && getCCopret(txin.vout[vin.prevout.n].scriptPubKey,opret) && opret.size() > 2 && DecodePaymentsMergeOpRet(opret,checktxid) == 'M' )
                    {
                        mergeoffset = PAYMENTS_MERGEOFSET;
                    }
                    //fprintf(stderr, "mergeoffset.%i\n", mergeoffset);
                    // check the chain depth vs locked blocks requirement. 
                    if ( !payments_lockedblocks(blockhash, lockedblocks+mergeoffset, blocksleft) )
                        return(eval->Invalid("vin not elegible"));
                    i++;
                } else return(eval->Invalid("cant get vin transaction"));
            }
            if ( fIsMerge )
            {
                if ( i < 2 )
                    return(eval->Invalid("must have at least 2 vins to carry out merge"));
                else if ( i == dust+1 )
                    return(eval->Invalid("cannot merge only dust"));
            }
        } else return(eval->Invalid("create transaction cannot decode"));
    } else return(eval->Invalid("Could not get contract transaction"));
    return(true);
}
// end of consensus code

// helper functions for rpc calls in rpcwallet.cpp
int64_t AddPaymentsInputs(bool fLockedBlocks,int8_t GetBalance,struct CCcontract_info *cp,CMutableTransaction &mtx,CPubKey txidpk,int64_t total,int32_t maxinputs,uint256 createtxid,int32_t lockedblocks,int64_t minrelease,int32_t &blocksleft)
{
    char coinaddr[64]; CPubKey Paymentspk; int64_t nValue,threshold,price,totalinputs = 0; uint256 txid,checktxid,hashBlock; std::vector<uint8_t> origpubkey; CTransaction vintx; int32_t iter,vout,ht,n = 0;
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > unspentOutputs; CScript ccopret;
    std::vector<std::pair<int32_t,CAmount> > blocksleft_balance;
    if ( GetBalance == 0 )
    {
        if ( maxinputs > CC_MAXVINS )
            maxinputs = CC_MAXVINS;
        if ( maxinputs > 0 )
            threshold = total/maxinputs;
        else threshold = total;
    }
    else threshold = 0;
    Paymentspk = GetUnspendable(cp,0);
    for (iter=0; iter<2; iter++)
    {
        if ( GetBalance == 1 && iter == 1 )
            continue; // getbalance of global paymentsCC address.
        if ( GetBalance == 2 && iter == 0 )
            continue; // get balance of txidpk address. 
        if ( iter == 0 )
            GetCCaddress(cp,coinaddr,Paymentspk);
        else GetCCaddress1of2(cp,coinaddr,Paymentspk,txidpk);
        SetCCunspents(unspentOutputs,coinaddr,true);
        for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> >::const_iterator it=unspentOutputs.begin(); it!=unspentOutputs.end(); it++)
        {
            txid = it->first.txhash;
            vout = (int32_t)it->first.index;
            //fprintf(stderr,"iter.%d %s/v%d %s\n",iter,txid.GetHex().c_str(),vout,coinaddr);
            if ( (vout == 0 || vout == 1) && GetTransaction(txid,vintx,hashBlock,false) != 0 )
            {
                if ( iter == 0 )
                {
                    CScript opret; uint256 checktxid; int32_t opret_ind;
                    if ( (opret_ind= has_opret(vintx, EVAL_PAYMENTS)) == 0 )
                    {
                        // get op_return from CCvout
                        getCCopret(vintx.vout[vout].scriptPubKey,opret);
                    }
                    else
                    {
                        // get op_return from the op_return 
                        opret = vintx.vout[opret_ind].scriptPubKey;
                    }
                    if ( DecodePaymentsFundOpRet(opret,checktxid) != 'F' || checktxid != createtxid )
                    {
                        fprintf(stderr,"bad opret %s vs %s\n",checktxid.GetHex().c_str(),createtxid.GetHex().c_str());
                        continue;
                    }
                }
                if ( (nValue= IsPaymentsvout(cp,vintx,vout,coinaddr,ccopret)) > PAYMENTS_TXFEE && nValue >= threshold && myIsutxo_spentinmempool(ignoretxid,ignorevin,txid,vout) == 0 )
                {
                    int32_t offset = 0;
                    if ( ccopret.size() > 2 && DecodePaymentsMergeOpRet(ccopret,checktxid) == 'M' )
                        offset = PAYMENTS_MERGEOFSET;
                    int32_t tmpblocksleft = 0;
                    if ( fLockedBlocks && !payments_lockedblocks(hashBlock, lockedblocks+offset, tmpblocksleft) )
                    {
                        blocksleft_balance.push_back(std::make_pair(tmpblocksleft,nValue));
                        continue;
                    }
                    if ( (GetBalance == 0 && total != 0 && maxinputs != 0) || GetBalance == 4 )
                        mtx.vin.push_back(CTxIn(txid,vout,CScript()));
                    nValue = it->second.satoshis;
                    if ( nValue < COIN )
                        blocksleft++; // count dust with unused variable.
                    totalinputs += nValue;
                    n++;
                    //fprintf(stderr,"iter.%d %s/v%d %s %.8f\n",iter,txid.GetHex().c_str(),vout,coinaddr,(double)nValue/COIN);
                    if ( GetBalance == 0 && ((total > 0 && totalinputs >= total) || (maxinputs > 0 && n >= maxinputs)) )
                        break; // create tx. We have ebnough inputs to make it. 
                } //else fprintf(stderr,"nValue %.8f vs threshold %.8f\n",(double)nValue/COIN,(double)threshold/COIN);
            }
        }
    }
    if ( GetBalance == 3 && totalinputs < minrelease ) // return elegible balance to be spent, and blocks left until min release can be released.
    {
        int64_t lockedblocks_balance = totalinputs; // inputs that can be spent already. 
        // sort utxos by blocks until able to be spent, smallest at top.
        std::sort(blocksleft_balance.begin(), blocksleft_balance.end());
        // iterate the utxos blocks left vector, to get block height min release is able to be released. 
        for ( auto utxo : blocksleft_balance )
        {
            lockedblocks_balance += utxo.second;
            if ( lockedblocks_balance >= minrelease )
            {
                blocksleft = utxo.first;
                break;
            }
        }
    }
    return(totalinputs);
}

UniValue payments_rawtxresult(UniValue &result,std::string rawtx,int32_t broadcastflag)
{
    CTransaction tx;
    if ( rawtx.size() > 0 )
    {
        result.push_back(Pair("hex",rawtx));
        if ( DecodeHexTx(tx,rawtx) != 0 )
        {
            if ( broadcastflag != 0 && myAddtomempool(tx) != 0 )
                RelayTransaction(tx);
            result.push_back(Pair("txid",tx.GetHash().ToString()));
            result.push_back(Pair("result","success"));
        } else result.push_back(Pair("error","decode hex"));
    } else result.push_back(Pair("error","couldnt finalize payments CCtx"));
    return(result);
}

cJSON *payments_reparse(int32_t *nump,char *jsonstr)
{
    cJSON *params=0; char *newstr; int32_t i,j;
    *nump = 0;
    if ( jsonstr != 0 )
    {
        if ( jsonstr[0] == '"' && jsonstr[strlen(jsonstr)-1] == '"' )
        {
            jsonstr[strlen(jsonstr)-1] = 0;
            jsonstr++;
        }
        newstr = (char *)malloc(strlen(jsonstr)+1);
        for (i=j=0; jsonstr[i]!=0; i++)
        {
            if ( jsonstr[i] == '%' && jsonstr[i+1] == '2' && jsonstr[i+2] == '2' )
            {
                newstr[j++] = '"';
                i += 2;
            }
            else if ( jsonstr[i] == '\'' )
                newstr[j++] = '"';
            else newstr[j++] = jsonstr[i];
        }
        newstr[j] = 0;
        params = cJSON_Parse(newstr);
        if ( 0 && params != 0 )
            printf("new.(%s) -> %s\n",newstr,jprint(params,0));
        free(newstr);
        *nump = cJSON_GetArraySize(params);
    }
    return(params);
}

uint256 payments_juint256(cJSON *obj)
{
    uint256 tmp; bits256 t = jbits256(obj,0);
    memcpy(&tmp,&t,sizeof(tmp));
    return(revuint256(tmp));
}

int32_t payments_parsehexdata(std::vector<uint8_t> &hexdata,cJSON *item,int32_t len)
{
    char *hexstr; int32_t val;
    if ( (hexstr= jstr(item,0)) != 0 && ((val= is_hexstr(hexstr,0)) == len*2 || (val > 0 && len == 0)) )
    {
        val >>= 1;
        hexdata.resize(val);
        decode_hex(&hexdata[0],val,hexstr);
        return(0);
    } else return(-1);
}

UniValue PaymentsRelease(struct CCcontract_info *cp,char *jsonstr)
{
    int32_t nextheight = komodo_nextheight();
    //int32_t latestheight,nextheight = komodo_nextheight();
    CMutableTransaction tmpmtx,mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(),nextheight); UniValue result(UniValue::VOBJ); uint256 createtxid,hashBlock,tokenid;
    CTransaction tx,txO; CPubKey mypk,txidpk,Paymentspk; int32_t i,n,m,numoprets=0,lockedblocks,minrelease; int64_t newamount,inputsum,amount,CCchange=0,totalallocations=0,checkallocations=0,allocation; CTxOut vout; CScript onlyopret; char txidaddr[64],destaddr[64]; std::vector<uint256> txidoprets;
    int32_t top,bottom=0,blocksleft=0,minimum=10000; std::vector<std::vector<uint8_t>> excludeScriptPubKeys; int8_t funcid,fixedAmount=0; bool fFixedAmount = false;
    mpz_t mpzTotalAllocations; mpz_init(mpzTotalAllocations);
    cJSON *params = payments_reparse(&n,jsonstr);
    mypk = pubkey2pk(Mypubkey());
    Paymentspk = GetUnspendable(cp,0);
    if ( params != 0 && n == 2 )
    {
        createtxid = payments_juint256(jitem(params,0));
        amount = jdouble(jitem(params,1),0) * SATOSHIDEN + 0.0000000049;
        if ( myGetTransaction(createtxid,tx,hashBlock) != 0 && tx.vout.size() > 0 )
        {
            if ( ((funcid= DecodePaymentsOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,totalallocations,txidoprets)) == 'C' || (funcid= DecodePaymentsSnapsShotOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,minimum,top,bottom,fixedAmount,excludeScriptPubKeys)) == 'S' || (funcid= DecodePaymentsTokensOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,top,excludeScriptPubKeys,tokenid)) == 'O') )
            {
                if ( lockedblocks < 0 || minrelease < 0 || (totalallocations <= 0 && top <= 0 ) )
                {
                    result.push_back(Pair("result","error"));
                    result.push_back(Pair("error","negative parameter"));
                    if ( params != 0 )
                        free_json(params);
                    return(result);
                }
                // set minimum size to 10k sat otherwise the tx will be invalid.
                if ( minimum < 10000 )
                    minimum = 10000;
                //latestheight = (nextheight - lockedblocks - 1);
                if ( amount < minrelease*COIN )
                {
                    result.push_back(Pair("result","error"));
                    result.push_back(Pair("error","amount too smal"));
                    result.push_back(Pair("amount",ValueFromAmount(amount)));
                    result.push_back(Pair("minrelease",ValueFromAmount(minrelease*COIN)));
                    if ( params != 0 )
                        free_json(params);
                    return(result);
                }
                txidpk = CCtxidaddr(txidaddr,createtxid);
                mtx.vout.push_back(MakeCC1of2vout(EVAL_PAYMENTS,0,Paymentspk,txidpk));
                //fprintf(stderr, "funcid.%i\n", funcid);
                if ( funcid == 'C' )
                {
                    // normal payments
                    for (i=0; i<m; i++)
                    {
                        std::vector<uint8_t> scriptPubKey,opret;
                        vout.nValue = 0;
                        if ( myGetTransaction(txidoprets[i],txO,hashBlock) != 0 && txO.vout.size() > 1 && DecodePaymentsTxidOpRet(txO.vout[txO.vout.size()-1].scriptPubKey,allocation,scriptPubKey,opret) == 'T' )
                        {
                            vout.nValue = allocation;
                            vout.scriptPubKey.resize(scriptPubKey.size());
                            memcpy(&vout.scriptPubKey[0],&scriptPubKey[0],scriptPubKey.size());
                            checkallocations += allocation;
                            if ( opret.size() > 0 )
                            {
                                onlyopret.resize(opret.size());
                                memcpy(&onlyopret[0],&opret[0],opret.size());
                                numoprets++;
                            }
                        } else break;
                        mtx.vout.push_back(vout);
                    }
                    result.push_back(Pair("numoprets",(int64_t)numoprets));
                    if ( i != m )
                    {
                        result.push_back(Pair("result","error"));
                        result.push_back(Pair("error","invalid txidoprets[i]"));
                        result.push_back(Pair("txi",(int64_t)i));
                        if ( params != 0 )
                            free_json(params);
                        return(result);
                    }
                    else if ( checkallocations != totalallocations )
                    {
                        result.push_back(Pair("result","error"));
                        result.push_back(Pair("error","totalallocations mismatch"));
                        result.push_back(Pair("checkallocations",(int64_t)checkallocations));
                        result.push_back(Pair("totalallocations",(int64_t)totalallocations));
                        if ( params != 0 )
                            free_json(params);
                        return(result);
                    }
                    else if ( numoprets > 1 )
                    {
                        result.push_back(Pair("result","error"));
                        result.push_back(Pair("error","too many oprets"));
                        if ( params != 0 )
                            free_json(params);
                        return(result);
                    }
                    // set totalallocations to a mpz_t bignum, for amounts calculation later. 
                    mpz_set_si(mpzTotalAllocations,totalallocations);
                }
                else if ( funcid == 'S' )
                {
                    // normal snapshot
                    if ( vAddressSnapshot.size() == 0 )
                    {
                        result.push_back(Pair("result","error"));
                        result.push_back(Pair("error","first snapshot has not happened yet"));
                        if ( params != 0 )
                            free_json(params);
                        return(result);
                    }
                    if ( top > 3999 )
                    {
                        result.push_back(Pair("result","error"));
                        result.push_back(Pair("error","cannot pay more than 3999 addresses"));
                        if ( params != 0 )
                            free_json(params);
                        return(result);
                    }
                    i = 0;
                    if ( fixedAmount == 7 ) 
                    {
                        // game setting, randomise bottom and top values 
                        fFixedAmount = payments_game(top,bottom);
                    }
                    else if ( fixedAmount != 0 )
                    {
                        fFixedAmount = true;
                    }
                    for (int32_t j = bottom; j < vAddressSnapshot.size(); j++)
                    {
                        auto &address = vAddressSnapshot[j];
                        CScript scriptPubKey = GetScriptForDestination(address.second); bool skip = false;
                        for ( auto skipkey : excludeScriptPubKeys ) 
                        {
                            if ( scriptPubKey == CScript(skipkey.begin(), skipkey.end()) )
                            {
                                skip = true;
                                //fprintf(stderr, "SKIPPED::: %s\n", CBitcoinAddress(address.second).ToString().c_str());
                            } 
                        }
                        if ( !skip )
                        {
                            mpz_t mpzAllocation; mpz_init(mpzAllocation);
                            i++;
                            //fprintf(stderr, "address: %s nValue.%li \n", CBitcoinAddress(address.second).ToString().c_str(), address.first);
                            vout.nValue = address.first;
                            vout.scriptPubKey = scriptPubKey;
                            mpz_set_si(mpzAllocation,address.first); 
                            mpz_add(mpzTotalAllocations,mpzTotalAllocations,mpzAllocation); 
                            mtx.vout.push_back(vout);
                            mpz_clear(mpzAllocation);
                        }
                        if ( i+bottom == top ) // we reached top amount to pay, it can be less than this!
                            break;
                    }
                    m = i; // this is the amount we got, either top, or all of the address on the chain.
                }
                else if ( funcid == 'O' )
                {
                    // token snapshot
                }
                newamount = amount;
                int64_t totalamountsent = 0;
                mpz_t mpzAmount; mpz_init(mpzAmount); mpz_set_si(mpzAmount,amount);
                for (i=0; i<m; i++)
                {
                    mpz_t mpzValue; mpz_init(mpzValue);
                    if ( fFixedAmount )
                    {
                        mtx.vout[i+1].nValue = amount / (top-bottom);
                        //fprintf(stderr, "amount.%li / top-bottom.%i = value.%li\n", amount, (top-bottom-2), mtx.vout[i+1].nValue);
                    }
                    else 
                    {
                        mpz_set_si(mpzValue,mtx.vout[i+1].nValue);
                        mpz_mul(mpzValue,mpzValue,mpzAmount); 
                        mpz_cdiv_q(mpzValue,mpzValue,mpzTotalAllocations); 
                        if ( mpz_fits_slong_p(mpzValue) ) 
                            mtx.vout[i+1].nValue = mpz_get_si(mpzValue);
                        else
                        {
                            result.push_back(Pair("result","error"));
                            result.push_back(Pair("error","value too big, try releasing a smaller amount"));
                            if ( params != 0 )
                                free_json(params);
                            return(result);
                        } 
                    }
                    //fprintf(stderr, "nValue.%li \n", mtx.vout[i+1].nValue);
                    mpz_clear(mpzValue);
                    if ( mtx.vout[i+1].nValue < minimum )
                    {
                        result.push_back(Pair("result","error"));
                        result.push_back(Pair("error","value too small, try releasing a larger amount"));
                        if ( params != 0 )
                            free_json(params);
                        return(result);
                    }
                    totalamountsent += mtx.vout[i+1].nValue;
                } 
                if ( totalamountsent < amount ) newamount = totalamountsent;
                fprintf(stderr, "newamount.%li totalamountsent.%li\n", newamount, totalamountsent);
                mpz_clear(mpzAmount); mpz_clear(mpzTotalAllocations);
            }
            else
            {
                result.push_back(Pair("result","error"));
                result.push_back(Pair("error","couldnt decode paymentscreate txid opret"));
                if ( params != 0 )
                    free_json(params);
                return(result);
            }
            if ( (inputsum= AddPaymentsInputs(true,0,cp,mtx,txidpk,newamount+2*PAYMENTS_TXFEE,CC_MAXVINS/2,createtxid,lockedblocks,minrelease,blocksleft)) >= newamount+2*PAYMENTS_TXFEE )
            {
                std::string rawtx;
                if ( (CCchange= (inputsum - newamount - 2*PAYMENTS_TXFEE)) >= PAYMENTS_TXFEE )
                    mtx.vout[0].nValue = CCchange;
                mtx.vout.push_back(CTxOut(PAYMENTS_TXFEE,CScript() << ParseHex(HexStr(txidpk)) << OP_CHECKSIG));
                GetCCaddress1of2(cp,destaddr,Paymentspk,txidpk);
                CCaddr1of2set(cp,Paymentspk,txidpk,cp->CCpriv,destaddr);
                rawtx = FinalizeCCTx(0,cp,mtx,mypk,PAYMENTS_TXFEE,onlyopret);
                if ( params != 0 )
                    free_json(params);
                result.push_back(Pair("amount",ValueFromAmount(amount)));
                result.push_back(Pair("newamount",ValueFromAmount(newamount)));
                return(payments_rawtxresult(result,rawtx,1));
            }
            else
            {
                result.push_back(Pair("result","error"));
                result.push_back(Pair("error","couldnt find enough locked funds"));
            }
        }
        else
        {
            result.push_back(Pair("result","error"));
            result.push_back(Pair("error","couldnt find paymentscreate txid"));
        }
    }
    else
    {
        result.push_back(Pair("result","error"));
        result.push_back(Pair("error","parameters error"));
    }
    if ( params != 0 )
        free_json(params);
    return(result);
}

UniValue PaymentsFund(struct CCcontract_info *cp,char *jsonstr)
{
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight()); UniValue result(UniValue::VOBJ);
    CPubKey Paymentspk,mypk,txidpk; uint256 txid,hashBlock; int64_t amount,totalallocations; CScript opret; CTransaction tx; char txidaddr[64]; std::string rawtx; int32_t n,useopret = 0,lockedblocks,minrelease; std::vector<uint256> txidoprets;
    int32_t top,bottom,minimum=10000; std::vector<std::vector<uint8_t>> excludeScriptPubKeys; // snapshot 
    uint256 tokenid; int8_t fixedAmount;
    cJSON *params = payments_reparse(&n,jsonstr);
    mypk = pubkey2pk(Mypubkey());
    Paymentspk = GetUnspendable(cp,0);
    if ( params != 0 && n > 1 && n <= 3 )
    {
        txid = payments_juint256(jitem(params,0));
        amount = jdouble(jitem(params,1),0) * SATOSHIDEN + 0.0000000049;
        if ( n == 3 )
            useopret = jint(jitem(params,2),0) != 0;
        if ( myGetTransaction(txid,tx,hashBlock) == 0 || tx.vout.size() == 1 || (DecodePaymentsOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,totalallocations,txidoprets) == 0 && DecodePaymentsSnapsShotOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,minimum,top,bottom,fixedAmount,excludeScriptPubKeys) == 0 && DecodePaymentsTokensOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,top,excludeScriptPubKeys,tokenid) == 0) )
        {
            result.push_back(Pair("result","error"));
            result.push_back(Pair("error","invalid createtxid"));
        }
        else if ( AddNormalinputs(mtx,mypk,amount+PAYMENTS_TXFEE,60) > 0 )
        {
            if ( lockedblocks < 0 || minrelease < 0 || (totalallocations <= 0 && top <= 0 ) )
            {
                result.push_back(Pair("result","error"));
                result.push_back(Pair("error","negative parameter"));
                if ( params != 0 )
                    free_json(params);
                return(result);
            }
            if ( useopret == 0 )
            {
                txidpk = CCtxidaddr(txidaddr,txid);
                mtx.vout.push_back(MakeCC1of2vout(EVAL_PAYMENTS,amount,Paymentspk,txidpk));
            }
            else
            {
                mtx.vout.push_back(MakeCC1vout(EVAL_PAYMENTS,amount,Paymentspk));
                opret = EncodePaymentsFundOpRet(txid);
                // Use the below one along with other FinalizeCCTx/return, to get the ccvout scriptpubkey
                /*std::vector<std::vector<unsigned char>> vData = std::vector<std::vector<unsigned char>>();
                if ( makeCCopret(opret, vData) )
                    mtx.vout.push_back(MakeCC1vout(EVAL_PAYMENTS,amount,Paymentspk,&vData)); */
            }
            rawtx = FinalizeCCTx(0,cp,mtx,mypk,PAYMENTS_TXFEE,opret);
            //rawtx = FinalizeCCTx(0,cp,mtx,mypk,PAYMENTS_TXFEE,CScript()); // use this one to get ccvout scriptpubkey.
            if ( params != 0 )
                free_json(params);
            //return(payments_rawtxresult(result,rawtx,0)); // disable sending for CCvout, as we only need to decode the tx.
            return(payments_rawtxresult(result,rawtx,1));
        }
        else
        {
            result.push_back(Pair("result","error"));
            result.push_back(Pair("error","couldnt find enough funds"));
        }
    }
    else
    {
        result.push_back(Pair("result","error"));
        result.push_back(Pair("error","parameters error"));
    }
    if ( params != 0 )
        free_json(params);
    return(result);
}

UniValue PaymentsMerge(struct CCcontract_info *cp,char *jsonstr)
{
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight()); UniValue result(UniValue::VOBJ);
    CPubKey Paymentspk,mypk,txidpk; uint256 createtxid,hashBlock; int64_t inputsum,totalallocations=0; CScript opret; CTransaction tx; char txidaddr[64],destaddr[64]; std::string rawtx; 
    int32_t n,lockedblocks,minrelease,top,bottom,minimum=10000,blocksleft; std::vector<uint256> txidoprets;
    std::vector<std::vector<uint8_t>> excludeScriptPubKeys; // snapshot 
    uint256 tokenid; int8_t fixedAmount;
    cJSON *params = payments_reparse(&n,jsonstr);
    mypk = pubkey2pk(Mypubkey());
    Paymentspk = GetUnspendable(cp,0);
    if ( params != 0 && n == 1 )
    {
        createtxid = payments_juint256(jitem(params,0));
        txidpk = CCtxidaddr(txidaddr,createtxid);
        if ( myGetTransaction(createtxid,tx,hashBlock) == 0 || tx.vout.size() == 1 || (DecodePaymentsOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,totalallocations,txidoprets) == 0 && DecodePaymentsSnapsShotOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,minimum,top,bottom,fixedAmount,excludeScriptPubKeys) == 0 && DecodePaymentsTokensOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,top,excludeScriptPubKeys,tokenid) == 0) )
        {
            result.push_back(Pair("result","error"));
            result.push_back(Pair("error","invalid createtxid"));
        }
        else if ( (inputsum= AddPaymentsInputs(true,4,cp,mtx,txidpk,0,CC_MAXVINS,createtxid,lockedblocks,minrelease,blocksleft)) > 0 && mtx.vin.size() > 1 )
        {
            int32_t dust = blocksleft;
            if ( mtx.vin.size() == dust+1 )
            {
                result.push_back(Pair("result","error"));
                result.push_back(Pair("error","cannot merge only dust"));
            } 
            else 
            { 
                // encode the checktxid into the end of the ccvout, along with 'M' to flag merge type tx. 
                opret = EncodePaymentsMergeOpRet(createtxid);
                std::vector<std::vector<unsigned char>> vData = std::vector<std::vector<unsigned char>>();
                // try to pay to diffrent pubkey here... change txidpk.
                if ( makeCCopret(opret, vData) )
                    mtx.vout.push_back(MakeCC1of2vout(EVAL_PAYMENTS,inputsum-PAYMENTS_TXFEE,Paymentspk,txidpk,&vData));
                GetCCaddress1of2(cp,destaddr,Paymentspk,txidpk);
                CCaddr1of2set(cp,Paymentspk,txidpk,cp->CCpriv,destaddr);
                rawtx = FinalizeCCTx(0,cp,mtx,mypk,PAYMENTS_TXFEE,CScript());
                if ( params != 0 )
                    free_json(params);
                return(payments_rawtxresult(result,rawtx,1));
            }
        }
        else
        {
            result.push_back(Pair("result","error"));
            result.push_back(Pair("error","couldnt find enough funds"));
        }
    }
    else
    {
        result.push_back(Pair("result","error"));
        result.push_back(Pair("error","parameters error"));
    }
    if ( params != 0 )
        free_json(params);
    return(result);
}

UniValue PaymentsTxidopret(struct CCcontract_info *cp,char *jsonstr)
{
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight()); UniValue result(UniValue::VOBJ); CPubKey mypk; std::string rawtx;
    std::vector<uint8_t> scriptPubKey,opret; int32_t n,retval0,retval1=0; int64_t allocation;
    cJSON *params = payments_reparse(&n,jsonstr);
    mypk = pubkey2pk(Mypubkey());
    if ( params != 0 && n > 1 && n <= 3 )
    {
        allocation = (int64_t)jint(jitem(params,0),0);
        retval0 = payments_parsehexdata(scriptPubKey,jitem(params,1),0);
        CScript test = CScript(scriptPubKey.begin(),scriptPubKey.end());
        txnouttype whichType;
        if (!::IsStandard(test, whichType))
        {
            result.push_back(Pair("result","error"));
            result.push_back(Pair("error","scriptPubkey is not valid payment."));
        }
        else 
        {
            if ( n == 3 )
                retval1 = payments_parsehexdata(opret,jitem(params,2),0);
            if ( allocation > 0 && retval0 == 0 && retval1 == 0 && AddNormalinputs(mtx,mypk,PAYMENTS_TXFEE*2,10) > 0 )
            {
                rawtx = FinalizeCCTx(0,cp,mtx,mypk,PAYMENTS_TXFEE,EncodePaymentsTxidOpRet(allocation,scriptPubKey,opret));
                if ( params != 0 )
                    free_json(params);
                return(payments_rawtxresult(result,rawtx,1));
            }
            result.push_back(Pair("result","error"));
            result.push_back(Pair("error","invalid params or cant find txfee"));
        }
    }
    else
    {
        result.push_back(Pair("result","error"));
        result.push_back(Pair("error","parameters error"));
        result.push_back(Pair("n",(int64_t)n));
        fprintf(stderr,"(%s) %p\n",jsonstr,params);
    }
    if ( params != 0 )
        free_json(params);
    return(result);
}

UniValue PaymentsCreate(struct CCcontract_info *cp,char *jsonstr)
{
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight());
    UniValue result(UniValue::VOBJ); CTransaction tx; CPubKey Paymentspk,mypk; char markeraddr[64]; std::vector<uint256> txidoprets; uint256 hashBlock; int32_t i,n,numoprets=0,lockedblocks,minrelease; std::string rawtx; int64_t totalallocations = 0;
    cJSON *params = payments_reparse(&n,jsonstr);
    if ( params != 0 && n >= 4 )
    {
        lockedblocks = juint(jitem(params,0),0);
        minrelease = juint(jitem(params,1),0);
        if ( lockedblocks < 0 || minrelease < 0 )
        {
            result.push_back(Pair("result","error"));
            result.push_back(Pair("error","negative parameter"));
            if ( params != 0 )
                free_json(params);
            return(result);
        }
        for (i=0; i<n-2; i++)
            txidoprets.push_back(payments_juint256(jitem(params,2+i)));
        for (i=0; i<txidoprets.size(); i++)
        {
            std::vector<uint8_t> scriptPubKey,opret; int64_t allocation;
            if ( myGetTransaction(txidoprets[i],tx,hashBlock) != 0 && tx.vout.size() > 1 && DecodePaymentsTxidOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,allocation,scriptPubKey,opret) == 'T' )
            {
                totalallocations += allocation;
                if ( opret.size() > 0 )
                    numoprets++;
            }
            else
            {
                result.push_back(Pair("result","error"));
                result.push_back(Pair("error","invalid txidopret"));
                result.push_back(Pair("txid",txidoprets[i].GetHex()));
                result.push_back(Pair("txi",(int64_t)i));
                if ( params != 0 )
                    free_json(params);
                return(result);
            }
        }
        if ( numoprets > 1 )
        {
            result.push_back(Pair("result","error"));
            result.push_back(Pair("error","too many opreturns"));
            result.push_back(Pair("numoprets",(int64_t)numoprets));
            if ( params != 0 )
                free_json(params);
            return(result);
        }
        mypk = pubkey2pk(Mypubkey());
        Paymentspk = GetUnspendable(cp,0);
        if ( AddNormalinputs(mtx,mypk,2*PAYMENTS_TXFEE,60) > 0 )
        {
            mtx.vout.push_back(MakeCC1of2vout(cp->evalcode,PAYMENTS_TXFEE,Paymentspk,Paymentspk));
            rawtx = FinalizeCCTx(0,cp,mtx,mypk,PAYMENTS_TXFEE,EncodePaymentsOpRet(lockedblocks,minrelease,totalallocations,txidoprets));
            if ( params != 0 )
                free_json(params);
            return(payments_rawtxresult(result,rawtx,1));
        }
        result.push_back(Pair("result","error"));
        result.push_back(Pair("error","not enough normal funds"));
    }
    else
    {
        result.push_back(Pair("result","error"));
        result.push_back(Pair("error","parameters error"));
    }
    if ( params != 0 )
        free_json(params);
    return(result);
}

UniValue PaymentsAirdrop(struct CCcontract_info *cp,char *jsonstr)
{
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight());
    UniValue result(UniValue::VOBJ); 
    uint256 hashBlock; CTransaction tx; CPubKey Paymentspk,mypk; char markeraddr[64]; std::string rawtx; 
    int32_t lockedblocks,minrelease,top,bottom,n,i,minimum=10000; std::vector<std::vector<uint8_t>> excludeScriptPubKeys; int8_t fixedAmount;
    if ( KOMODO_SNAPSHOT_INTERVAL == 0 )
    {
        result.push_back(Pair("result","error"));
        result.push_back(Pair("error","cannot use airdrop wihtout -ac_snapshot set."));
        return(result);
    }
    cJSON *params = payments_reparse(&n,jsonstr);
    if ( params != 0 && n >= 5 )
    {
        lockedblocks = juint(jitem(params,0),0);
        minrelease = juint(jitem(params,1),0);
        minimum = juint(jitem(params,2),0);
        if ( minimum < 10000 ) minimum = 10000;
        top = juint(jitem(params,3),0);
        bottom = juint(jitem(params,4),0);
        fixedAmount = juint(jitem(params,5),0); // fixed amount is a flag, set to 7 does game mode, 0 normal snapshot, anything else fixed allocations. 
        if ( lockedblocks < 0 || minrelease < 0 || top <= 0 || bottom < 0 || minimum < 0 || fixedAmount < 0 || top > 3999 )
        {
            result.push_back(Pair("result","error"));
            result.push_back(Pair("error","negative parameter, or top over 3999"));
            if ( params != 0 )
                free_json(params);
            return(result);
        }
        if ( n > 6 )
        {
            for (i=0; i<n-6; i++)
            {
                /* TODO: Change this RPC to take an address. Because a tokens airdrop needs its own RPC anyway.
                CTxDestination destination = DecodeDestination(name_);
                CScript scriptPubKey = GetScriptForDestination(destination);
                */
                char *inputhex = jstri(params,6+i);
                std::vector<uint8_t> scriptPubKey;
                int32_t len = strlen(inputhex)/2;
                scriptPubKey.resize(len);
                decode_hex((uint8_t *)scriptPubKey.data(),len,(char *)inputhex);
                excludeScriptPubKeys.push_back(scriptPubKey);
            }
        }
        mypk = pubkey2pk(Mypubkey());
        Paymentspk = GetUnspendable(cp,0);
        if ( AddNormalinputs(mtx,mypk,2*PAYMENTS_TXFEE,60) > 0 )
        {
            mtx.vout.push_back(MakeCC1of2vout(cp->evalcode,PAYMENTS_TXFEE,Paymentspk,Paymentspk));
            CScript tempopret = EncodePaymentsSnapsShotOpRet(lockedblocks,minrelease,minimum,top,bottom,fixedAmount,excludeScriptPubKeys);
            if ( tempopret.size() > 10000 ) // TODO: Check this!
            {
                result.push_back(Pair("result","error"));
                result.push_back(Pair("error","op_return is too big, try with less exclude addresses."));
                if ( params != 0 )
                    free_json(params);
                return(result);
            }
            rawtx = FinalizeCCTx(0,cp,mtx,mypk,PAYMENTS_TXFEE,tempopret);
            if ( params != 0 )
                free_json(params);
            return(payments_rawtxresult(result,rawtx,1));
        } 
        result.push_back(Pair("result","error"));
        result.push_back(Pair("error","not enough normal funds"));
    }
    else
    {
        result.push_back(Pair("result","error"));
        result.push_back(Pair("error","parameters error"));
    }
    if ( params != 0 )
        free_json(params);
    return(result);
}

UniValue PaymentsInfo(struct CCcontract_info *cp,char *jsonstr)
{
    UniValue result(UniValue::VOBJ),a(UniValue::VARR); CTransaction tx,txO; CPubKey Paymentspk,txidpk; int32_t i,j,n,flag=0,numoprets=0,lockedblocks,minrelease,blocksleft=0; std::vector<uint256> txidoprets; int64_t funds,fundsopret,elegiblefunds,totalallocations=0,allocation; char fundsaddr[64],fundsopretaddr[64],txidaddr[64],*outstr; uint256 createtxid,hashBlock;
    int32_t top,bottom,minimum=10000; std::vector<std::vector<uint8_t>> excludeScriptPubKeys; // snapshot 
    uint256 tokenid; int8_t fixedAmount; CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(),komodo_nextheight());
    cJSON *params = payments_reparse(&n,jsonstr);
    if ( params != 0 && n == 1 )
    {
        Paymentspk = GetUnspendable(cp,0);
        createtxid = payments_juint256(jitem(params,0));
        if ( myGetTransaction(createtxid,tx,hashBlock) != 0 && tx.vout.size() > 0 )
        {
            if ( DecodePaymentsOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,totalallocations,txidoprets) != 0 )
            {
                if ( lockedblocks < 0 || minrelease < 0 || totalallocations <= 0 || txidoprets.size() < 2 )
                {
                    result.push_back(Pair("result","error"));
                    result.push_back(Pair("error","negative parameter"));
                    if ( params != 0 )
                        free_json(params);
                    return(result);
                }
                result.push_back(Pair("plan_type","payments"));
                result.push_back(Pair("lockedblocks",(int64_t)lockedblocks));
                result.push_back(Pair("totalallocations",(int64_t)totalallocations));
                result.push_back(Pair("minrelease",(int64_t)minrelease));
                for (i=0; i<txidoprets.size(); i++)
                {
                    UniValue obj(UniValue::VOBJ); std::vector<uint8_t> scriptPubKey,opret;
                    obj.push_back(Pair("txid",txidoprets[i].GetHex()));
                    if ( myGetTransaction(txidoprets[i],txO,hashBlock) != 0 && txO.vout.size() > 1 && DecodePaymentsTxidOpRet(txO.vout[txO.vout.size()-1].scriptPubKey,allocation,scriptPubKey,opret) == 'T' )
                    {
                        outstr = (char *)malloc(2*(scriptPubKey.size() + opret.size()) + 1);
                        for (j=0; j<scriptPubKey.size(); j++)
                            sprintf(&outstr[j<<1],"%02x",scriptPubKey[j]);
                        outstr[j<<1] = 0;
                        //fprintf(stderr,"scriptPubKey.(%s)\n",outstr);
                        obj.push_back(Pair("scriptPubKey",outstr));
                        if ( opret.size() != 0 )
                        {
                            for (j=0; j<opret.size(); j++)
                                sprintf(&outstr[j<<1],"%02x",opret[j]);
                            outstr[j<<1] = 0;
                            //fprintf(stderr,"opret.(%s)\n",outstr);
                            obj.push_back(Pair("opreturn",outstr));
                            numoprets++;
                        }
                        free(outstr);
                    } else fprintf(stderr,"error decoding voutsize.%d\n",(int32_t)txO.vout.size());
                    a.push_back(obj);
                }
                result.push_back(Pair("numoprets",(int64_t)numoprets));
                if ( numoprets > 1 )
                {
                    flag++;
                    result.push_back(Pair("result","error"));
                    result.push_back(Pair("error","too many opreturns"));
                } else result.push_back(Pair("txidoprets",a));
            }
            else if ( DecodePaymentsSnapsShotOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,minimum,top,bottom,fixedAmount,excludeScriptPubKeys) != 0 )
            {
                if ( lockedblocks < 0 || minrelease < 0 || top <= 0 || bottom < 0 || fixedAmount < 0 || top > 3999 || minimum < 10000 )
                {
                    result.push_back(Pair("result","error"));
                    result.push_back(Pair("error","negative parameter"));
                    if ( params != 0 )
                        free_json(params);
                    return(result);
                }
                if ( fixedAmount == 7 && payments_game(top,bottom))
                    result.push_back(Pair("plan_type","payments_game"));
                else 
                    result.push_back(Pair("plan_type","snapshot"));
                result.push_back(Pair("lockedblocks",(int64_t)lockedblocks));
                result.push_back(Pair("minrelease",(int64_t)minrelease));
                result.push_back(Pair("minimum",(int64_t)minimum));
                result.push_back(Pair("bottom",(int64_t)bottom));
                result.push_back(Pair("top",(int64_t)top));
                result.push_back(Pair("fixedFlag",(int64_t)fixedAmount));
                // TODO: convert to show addresses instead of scriptpubkey.
                for ( auto scriptPubKey : excludeScriptPubKeys )
                    a.push_back(HexStr(scriptPubKey.begin(),scriptPubKey.end()));
                result.push_back(Pair("excludeScriptPubkeys",a));
            }
            else if ( DecodePaymentsTokensOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,top,excludeScriptPubKeys,tokenid) != 0 )
            {
                if ( lockedblocks < 0 || minrelease < 0 || top <= 0 )
                {
                    result.push_back(Pair("result","error"));
                    result.push_back(Pair("error","negative parameter"));
                    if ( params != 0 )
                        free_json(params);
                    return(result);
                }
                result.push_back(Pair("plan_type","token snapshot"));
                result.push_back(Pair("lockedblocks",(int64_t)lockedblocks));
                result.push_back(Pair("minrelease",(int64_t)minrelease));
                result.push_back(Pair("top",(int64_t)top));
                result.push_back(Pair("tokenid",tokenid.ToString()));
                // TODO: show pubkeys instead of scriptpubkeys
                for ( auto scriptPubKey : excludeScriptPubKeys )
                    a.push_back(HexStr(scriptPubKey.begin(),scriptPubKey.end()));
                result.push_back(Pair("excludeScriptPubkeys",a));
            }
            else
            {
                result.push_back(Pair("result","error"));
                result.push_back(Pair("error","couldnt decode valid payments create txid opreturn"));
            }
            if ( flag == 0 )
            {
                txidpk = CCtxidaddr(txidaddr,createtxid);
                GetCCaddress1of2(cp,fundsaddr,Paymentspk,txidpk);
                funds = AddPaymentsInputs(false,2,cp,mtx,txidpk,0,CC_MAXVINS,createtxid,lockedblocks,minrelease,blocksleft);
                result.push_back(Pair(fundsaddr,ValueFromAmount(funds)));
                GetCCaddress(cp,fundsopretaddr,Paymentspk);
                fundsopret = AddPaymentsInputs(false,1,cp,mtx,txidpk,0,CC_MAXVINS,createtxid,lockedblocks,minrelease,blocksleft);
                result.push_back(Pair(fundsopretaddr,ValueFromAmount(fundsopret)));
                result.push_back(Pair("totalfunds",ValueFromAmount(funds+fundsopret)));
                // Blocks until minrelease can be released. 
                elegiblefunds = AddPaymentsInputs(true,3,cp,mtx,txidpk,0,CC_MAXVINS,createtxid,lockedblocks,minrelease,blocksleft);
                result.push_back(Pair("elegiblefunds",ValueFromAmount(elegiblefunds)));
                result.push_back(Pair("min_release_height",chainActive.Height()+blocksleft));
                result.push_back(Pair("result","success"));
            }
        }
        else 
        {
            result.push_back(Pair("result","error"));
            result.push_back(Pair("error","couldnt find valid payments create txid"));
        }
    }
    else
    {
        result.push_back(Pair("result","error"));
        result.push_back(Pair("error","parameters error"));
    }
    if ( params != 0 )
        free_json(params);
    return(result);
}

UniValue PaymentsList(struct CCcontract_info *cp,char *jsonstr)
{
    std::vector<std::pair<CAddressIndexKey, CAmount> > addressIndex; uint256 txid,hashBlock,tokenid;
    UniValue result(UniValue::VOBJ),a(UniValue::VARR); char markeraddr[64],str[65]; CPubKey Paymentspk; CTransaction tx; int32_t lockedblocks,minrelease; std::vector<uint256> txidoprets; int64_t totalallocations=0;
    int32_t top=0,bottom=0,minimum=10000; std::vector<std::vector<uint8_t>> excludeScriptPubKeys; int8_t fixedAmount = 0;
    Paymentspk = GetUnspendable(cp,0);
    GetCCaddress1of2(cp,markeraddr,Paymentspk,Paymentspk);
    SetCCtxids(addressIndex,markeraddr,true);
    for (std::vector<std::pair<CAddressIndexKey, CAmount> >::const_iterator it=addressIndex.begin(); it!=addressIndex.end(); it++)
    {
        txid = it->first.txhash;
        if ( it->first.index == 0 && myGetTransaction(txid,tx,hashBlock) != 0 )
        {
            if ( tx.vout.size() > 0 && (DecodePaymentsOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,totalallocations,txidoprets) == 'C' || DecodePaymentsSnapsShotOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,minimum,top,bottom,fixedAmount,excludeScriptPubKeys) == 'S' || DecodePaymentsTokensOpRet(tx.vout[tx.vout.size()-1].scriptPubKey,lockedblocks,minrelease,top,excludeScriptPubKeys,tokenid) == 'O') )
            {
                if ( lockedblocks < 0 || minrelease < 0 || (totalallocations <= 0 && top <= 0 ) || bottom < 0 || fixedAmount < 0 || minimum < 10000 )
                {
                    result.push_back(Pair("result","error"));
                    result.push_back(Pair("error","negative parameter"));
                    return(result);
                }
                a.push_back(uint256_str(str,txid));
            }
        }
    }
    result.push_back(Pair("result","success"));
    result.push_back(Pair("createtxids",a)); 
    return(result); 
}
