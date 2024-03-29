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
 *****************************************************************************
To create payments plan start a chain with the following ac_params:
    -ac_snapshot=1440 (or for test chain something smaller, if you like.)
        - this enables the payments airdrop cc to work. 
    -ac_earlytxidcontract=237 (Eval code for prices cc.)
        - this allows to know what contract this chain is paying with the scriptpubkey in the earlytxid op_return. 

./komodod -ac_name=TESTPRC -ac_supply=100000000 -ac_reward=1000000000 -ac_nk=96,5 -ac_blocktime=20 -ac_cc=2 -ac_snapshot=50 -ac_sapling=1 -ac_earlytxidcontract=237 -testnode=1 -gen -genproclimit=1

Then in very early block < 10 or so, do paymentsairdrop eg. 
    `./komodo-cli -ac_name=TESTPRC paymentsairdrop '[10,10,0,3999,0,0]'
Once this tx is confirmed, do `paymentsfund` and decode the raw hex. You can edit the source to not send the tx if requried. 
Get the full `hex` of the vout[0] that pays to CryptoCondition. then place it on chain with the following command: with the hex you got in place of the hex below.
    './komodo-cli -ac_name=TESTPRC opreturn_burn 1 2ea22c8020292ba5c8fd9cc89b12b35bf8f5d00196990ecbb06102b84d9748d11d883ef01e81031210008203000401cc'
copy the hex, and sendrawtransaction, copy the txid returned. 
this places the scriptpubkey that pays the plan into an op_return before block 100, allowing us to retreive it, and nobody to change it.
Restart the daemon with -earlytxid=<txid of opreturn_burn transaction>  eg: 

./komodod -ac_name=TESTPRC -ac_supply=100000000 -ac_reward=1000000000 -ac_nk=96,5 -ac_blocktime=20 -ac_cc=2 -ac_snapshot=50 -ac_sapling=1 -ac_earlytxidcontract=237 -earlytxid=cf89d17fb11037f65c160d0749dddd74dc44d9893b0bb67fe1f96c1f59786496 -testnode=1 -gen -genproclimit=1

mine the chain past block 100, preventing anyone else, creating another payments plan on chain before block 100. 

We call the following in Validation and RPC where the address is needed. 
if ( ASSETCHAINS_EARLYTXIDCONTRACT == EVAL_PRICES && KOMODO_EARLYTXID_SCRIPTPUB.size() == 0 )
    GetKomodoEarlytxidScriptPub();

This will fetch the op_return, calculate the scriptPubKey and save it to the global. 
On daemon restart as soon as validation for BETTX happens the global will be filled, after this the transaction never needs to be looked up again. 
GetKomodoEarlytxidScriptPub is on line #2080 of komodo_bitcoind.h
 */

#include "CCassets.h"
#include "CCPrices.h"

#include <gmp.h>

#define IS_CHARINSTR(c, str) (std::string(str).find((char)(c)) != std::string::npos)

#define NVOUT_CCMARKER 1
#define NVOUT_NORMALMARKER 3

typedef struct OneBetData {
    int64_t positionsize;
    int32_t firstheight;
    int64_t costbasis;
    int64_t profits;

    OneBetData() { positionsize = 0; firstheight = 0; costbasis = 0; profits = 0; }  // it is important to clear costbasis as it will be calculated as minmax from inital value 0
} onebetdata;

typedef struct BetInfo {
    int64_t averageCostbasis, firstprice, lastprice, liquidationprice, equity;
    int64_t rektfee;
    int32_t lastheight;
    int16_t leverage;
    bool isOpen, isRekt;
    uint256 tokenid;

    std::vector<uint16_t> parsed;
    std::vector<onebetdata> bets;
    CPubKey pk;

    BetInfo() { 
        averageCostbasis = firstprice = lastprice = liquidationprice = equity = 0;
        lastheight = 0;
        leverage = 0;
        rektfee = 0;
        isOpen = isRekt = false;
    }
} BetInfo;

/*
CBOPRET creates trustless oracles, which can be used for making a synthetic cash settlement system based on real world prices;
 
 0.5% fee based on betamount, NOT leveraged betamount!!
 0.1% collected by price basis determinant
 0.2% collected by rekt tx
 
 PricesBet -> +/-leverage, amount, synthetic -> opreturn includes current price
    funds are locked into 1of2 global CC address
    for first day, long basis is MAX(correlated,smoothed), short is MIN()
    reference price is the smoothed of the previous block
    if synthetic value + amount goes negative, then anybody can rekt it to collect a rektfee, proof of rekt must be included to show cost basis, rekt price
    original creator can liquidate at anytime and collect (synthetic value + amount) from globalfund
    0.5% of bet -> globalfund
  
 PricesStatus -> bettxid maxsamples returns initial params, cost basis, amount left, rekt:true/false, rektheight, initial synthetic price, current synthetic price, net gain
 
 PricesRekt -> bettxid height -> 0.1% to miner, rest to global CC
 
 PricesClose -> bettxid returns (synthetic value + amount)
 
 PricesList -> all bettxid -> list [bettxid, netgain]
 
 
*/

int32_t prices_syntheticprofits(int64_t &costbasis, int32_t firstheight, int32_t height, int16_t leverage, std::vector<uint16_t> vec, int64_t positionsize, int64_t &profits, int64_t &outprice);

// helpers:

// returns true if there are only digits and no alphas or slashes in 's'
inline bool is_weight_str(std::string s) {
    return 
        std::count_if(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); } ) > 0  &&
        std::count_if(s.begin(), s.end(), [](unsigned char c) { return std::isalpha(c) || c == '/'; } ) == 0;
}


// start of consensus code

CScript prices_betopret(CPubKey mypk,int32_t height,int64_t amount,int16_t leverage,int64_t firstprice,std::vector<uint16_t> vec,uint256 tokenid)
{
    CScript opret;
    opret << OP_RETURN << E_MARSHAL(ss << EVAL_PRICES << 'B' << mypk << height << amount << leverage << firstprice << vec << tokenid);
    return(opret);
}

uint8_t prices_betopretdecode(CScript scriptPubKey,CPubKey &pk,int32_t &height,int64_t &amount,int16_t &leverage,int64_t &firstprice,std::vector<uint16_t> &vec,uint256 &tokenid)
{
    std::vector<uint8_t> vopret; uint8_t e,f;
    
    GetOpReturnData(scriptPubKey,vopret);
    if (vopret.size() > 2 && E_UNMARSHAL(vopret, ss >> e; ss >> f; ss >> pk; ss >> height; ss >> amount; ss >> leverage; ss >> firstprice; ss >> vec; ss >> tokenid) != 0 && e == EVAL_PRICES && f == 'B')
    {
        return(f);
    }
    return(0);
}

CScript prices_addopret(uint256 bettxid,CPubKey mypk,int64_t amount)
{
    CScript opret;
    opret << OP_RETURN << E_MARSHAL(ss << EVAL_PRICES << 'A' << bettxid << mypk << amount);
    return(opret);
}

uint8_t prices_addopretdecode(CScript scriptPubKey,uint256 &bettxid,CPubKey &pk,int64_t &amount)
{
    std::vector<uint8_t> vopret; uint8_t e,f;
    GetOpReturnData(scriptPubKey,vopret);
    if ( vopret.size() > 2 && E_UNMARSHAL(vopret,ss >> e; ss >> f; ss >> bettxid; ss >> pk; ss >> amount) != 0 && e == EVAL_PRICES && f == 'A' )
    {
        return(f);
    }
    return(0);
}

CScript prices_costbasisopret(uint256 bettxid,CPubKey mypk,int32_t height,int64_t costbasis)
{
    CScript opret;
    opret << OP_RETURN << E_MARSHAL(ss << EVAL_PRICES << 'C' << bettxid << mypk << height << costbasis);
    return(opret);
}

uint8_t prices_costbasisopretdecode(CScript scriptPubKey,uint256 &bettxid,CPubKey &pk,int32_t &height,int64_t &costbasis)
{
    std::vector<uint8_t> vopret; uint8_t e,f;
    GetOpReturnData(scriptPubKey,vopret);
    if ( vopret.size() > 2 && E_UNMARSHAL(vopret,ss >> e; ss >> f; ss >> bettxid; ss >> pk; ss >> height; ss >> costbasis) != 0 && e == EVAL_PRICES && f == 'C' )
    {
        return(f);
    }
    return(0);
}

CScript prices_finalopret(uint256 bettxid,int64_t profits,int32_t height,CPubKey mypk,int64_t firstprice,int64_t costbasis,int64_t addedbets,int64_t positionsize,int16_t leverage)
{
    CScript opret;
    opret << OP_RETURN << E_MARSHAL(ss << EVAL_PRICES << 'F' << bettxid << profits << height << mypk << firstprice << costbasis << addedbets << positionsize << leverage);
    return(opret);
}

uint8_t prices_finalopretdecode(CScript scriptPubKey,uint256 &bettxid,int64_t &profits,int32_t &height,CPubKey &pk,int64_t &firstprice,int64_t &costbasis,int64_t &addedbets,int64_t &positionsize,int16_t &leverage)
{
    std::vector<uint8_t> vopret; uint8_t e,f;
    GetOpReturnData(scriptPubKey,vopret);
    if ( vopret.size() > 2 && E_UNMARSHAL(vopret,ss >> e; ss >> f; ss >> bettxid; ss >> profits; ss >> height; ss >> pk; ss >> firstprice; ss >> costbasis; ss >> addedbets; ss >> positionsize; ss >> leverage) != 0 && e == EVAL_PRICES && f == 'F' )
    {
        return(f);
    }
    return(0);
}

// price opret basic validation and retrieval
static uint8_t PricesCheckOpret(const CTransaction & tx, vscript_t &opret)
{
    if (tx.vout.size() > 0 && GetOpReturnData(tx.vout.back().scriptPubKey, opret) && opret.size() > 2 && opret.begin()[0] == EVAL_PRICES && IS_CHARINSTR(opret.begin()[1], "BACF"))
        return opret.begin()[1];
    else
        return (uint8_t)0;
}

// validate bet tx helper
static bool ValidateBetTx(struct CCcontract_info *cp, Eval *eval, const CTransaction & bettx)
{
    uint256 tokenid;
    int64_t positionsize, firstprice;
    int32_t firstheight; 
    int16_t leverage;
    CPubKey pk, pricespk; 
    std::vector<uint16_t> vec;
    if ( ASSETCHAINS_EARLYTXIDCONTRACT == EVAL_PRICES && KOMODO_EARLYTXID_SCRIPTPUB.size() == 0 )
        GetKomodoEarlytxidScriptPub();
    if (bettx.vout.size() < 5 || bettx.vout.size() > 6)
        return eval->Invalid("incorrect vout number for bet tx");

    vscript_t opret;
    if( prices_betopretdecode(bettx.vout.back().scriptPubKey, pk, firstheight, positionsize, leverage, firstprice, vec, tokenid) != 'B')
        return eval->Invalid("cannot decode opreturn for bet tx");

    pricespk = GetUnspendable(cp, 0);

    if (MakeCC1vout(cp->evalcode, bettx.vout[0].nValue, pk) != bettx.vout[0])
        return eval->Invalid("cannot validate vout0 in bet tx with pk from opreturn");
    if (MakeCC1vout(cp->evalcode, bettx.vout[1].nValue, pricespk) != bettx.vout[1])
        return eval->Invalid("cannot validate vout1 in bet tx with global pk");
    if (MakeCC1vout(cp->evalcode, bettx.vout[2].nValue, pricespk) != bettx.vout[2] )
        return eval->Invalid("cannot validate vout2 in bet tx with pk from opreturn");
    // This should be all you need to verify it, maybe also check amount? 
    if ( bettx.vout[4].scriptPubKey != KOMODO_EARLYTXID_SCRIPTPUB )
        return eval->Invalid("the fee was paid to wrong address.");

    int64_t betamount = bettx.vout[2].nValue;
    if (betamount != (positionsize * 199) / 200) {
        return eval->Invalid("invalid position size in the opreturn");
    }

    // validate if normal inputs are really signed by originator pubkey (someone not cheating with originator pubkey)
    CAmount ccOutputs = 0;
    for (auto vout : bettx.vout)
        if (vout.scriptPubKey.IsPayToCryptoCondition())  
            ccOutputs += vout.nValue;
    CAmount normalInputs = TotalPubkeyNormalInputs(bettx, pk);
    if (normalInputs < ccOutputs) {
        return eval->Invalid("bettx normal inputs not signed with pubkey in opret");
    }

    if (leverage > PRICES_MAXLEVERAGE || leverage < -PRICES_MAXLEVERAGE) {
        return eval->Invalid("invalid leverage");
    }

    return true;
}

// validate add funding tx helper
static bool ValidateAddFundingTx(struct CCcontract_info *cp, Eval *eval, const CTransaction & addfundingtx, const CTransaction & vintx)
{
    uint256 bettxid;
    int64_t amount;
    CPubKey pk, pricespk;
    vscript_t vintxOpret;

    if (addfundingtx.vout.size() < 3 || addfundingtx.vout.size() > 4)
        return eval->Invalid("incorrect vout number for add funding tx");

    vscript_t opret;
    if (prices_addopretdecode(addfundingtx.vout.back().scriptPubKey, bettxid, pk, amount) != 'A')
        return eval->Invalid("cannot decode opreturn for add funding tx");

    pricespk = GetUnspendable(cp, 0);
    uint8_t vintxFuncId = PricesCheckOpret(vintx, vintxOpret);
    if (vintxFuncId != 'A' && vintxFuncId != 'B') { // if vintx is bettx
        return eval->Invalid("incorrect vintx funcid");
    }

    if (vintxFuncId == 'B' && vintx.GetHash() != bettxid) {// if vintx is bettx
        return eval->Invalid("incorrect bet txid in opreturn");
    }

    if (MakeCC1vout(cp->evalcode, addfundingtx.vout[0].nValue, pk) != addfundingtx.vout[0])
        return eval->Invalid("cannot validate vout0 in add funding tx with pk from opreturn");
    if (MakeCC1vout(cp->evalcode, addfundingtx.vout[1].nValue, pricespk) != addfundingtx.vout[1])
        return eval->Invalid("cannot validate vout1 in add funding tx with global pk");

    return true;
}

// validate costbasis tx helper
static bool ValidateCostbasisTx(struct CCcontract_info *cp, Eval *eval, const CTransaction & costbasistx, const CTransaction & bettx)
{
    uint256 bettxid;
    int64_t costbasisInOpret;
    CPubKey pk, pricespk;
    int32_t height;

    return true;  //deprecated

    // check basic structure:
    if (costbasistx.vout.size() < 3 || costbasistx.vout.size() > 4)
        return eval->Invalid("incorrect vout count for costbasis tx");

    vscript_t opret;
    if (prices_costbasisopretdecode(costbasistx.vout.back().scriptPubKey, bettxid, pk, height, costbasisInOpret) != 'C')
        return eval->Invalid("cannot decode opreturn for costbasis tx");

    pricespk = GetUnspendable(cp, 0);
    if (CTxOut(costbasistx.vout[0].nValue, CScript() << ParseHex(HexStr(pk)) << OP_CHECKSIG) != costbasistx.vout[0])  //might go to any pk who calculated costbasis
        return eval->Invalid("cannot validate vout0 in costbasis tx with pk from opreturn");
    if (MakeCC1vout(cp->evalcode, costbasistx.vout[1].nValue, pricespk) != costbasistx.vout[1])
        return eval->Invalid("cannot validate vout1 in costbasis tx with global pk");

    if (bettx.GetHash() != bettxid)
        return eval->Invalid("incorrect bettx id");

    if (bettx.vout.size() < 1) // for safety and for check encapsulation
        return eval->Invalid("incorrect bettx no vouts");

    // check costbasis rules:
    if (costbasistx.vout[0].nValue > bettx.vout[1].nValue / 10) {
        return eval->Invalid("costbasis myfee too big");
    }

    uint256 tokenid;
    int64_t positionsize, firstprice;
    int32_t firstheight;
    int16_t leverage;
    CPubKey betpk;
    std::vector<uint16_t> vec;
    if (prices_betopretdecode(bettx.vout.back().scriptPubKey, betpk, firstheight, positionsize, leverage, firstprice, vec, tokenid) != 'B')
        return eval->Invalid("cannot decode opreturn for bet tx");

    if (firstheight + PRICES_DAYWINDOW + PRICES_SMOOTHWIDTH > chainActive.Height()) {
        return eval->Invalid("cannot calculate costbasis yet");
    }
    
    int64_t costbasis = 0, profits, lastprice;
    int32_t retcode = prices_syntheticprofits(costbasis, firstheight, firstheight + PRICES_DAYWINDOW, leverage, vec, positionsize, profits, lastprice);
    if (retcode < 0) 
        return eval->Invalid("cannot calculate costbasis yet");
    std::cerr << "ValidateCostbasisTx() costbasis=" << costbasis << " costbasisInOpret=" << costbasisInOpret << std::endl;
    if (costbasis != costbasisInOpret) {
        //std::cerr << "ValidateBetTx() " << "incorrect costbasis value" << std::endl;
        return eval->Invalid("incorrect costbasis value");
    }

    return true;
}

// validate final tx helper
static bool ValidateFinalTx(struct CCcontract_info *cp, Eval *eval, const CTransaction & finaltx, const CTransaction & bettx)
{
    uint256 bettxid;
    int64_t amount;
    CPubKey pk, pricespk;
    int64_t profits;
    int32_t height;
    int64_t firstprice, costbasis, addedbets, positionsize;
    int16_t leverage;

    if (finaltx.vout.size() < 3 || finaltx.vout.size() > 4) {
        //std::cerr << "ValidateFinalTx()" << " incorrect vout number for final tx =" << finaltx.vout.size() << std::endl;
        return eval->Invalid("incorrect vout number for final tx");
    }

    vscript_t opret;
    if (prices_finalopretdecode(finaltx.vout.back().scriptPubKey, bettxid, profits, height, pk, firstprice, costbasis, addedbets, positionsize, leverage) != 'F')
        return eval->Invalid("cannot decode opreturn for final tx");

    if (bettx.GetHash() != bettxid)
        return eval->Invalid("incorrect bettx id");

    pricespk = GetUnspendable(cp, 0);

    if (CTxOut(finaltx.vout[0].nValue, CScript() << ParseHex(HexStr(pk)) << OP_CHECKSIG) != finaltx.vout[0])
        return eval->Invalid("cannot validate vout0 in final tx with pk from opreturn");

    if( finaltx.vout.size() == 3 && MakeCC1vout(cp->evalcode, finaltx.vout[1].nValue, pricespk) != finaltx.vout[1] ) 
        return eval->Invalid("cannot validate vout1 in final tx with global pk");

    return true;
}

// validate prices tx function
// performed checks:
// basic tx structure (vout num)
// basic tx opret structure
// reference to the bet tx vout
// referenced bet txid in tx opret
// referenced bet tx structure 
// non-final tx has only 1 cc vin
// cc vouts to self with mypubkey from opret
// cc vouts to global pk with global pk
// for bet tx that normal inputs digned with my pubkey from the opret >= cc outputs - disable betting for other pubkeys (Do we need this rule?)
// TODO:
// opret params (firstprice,positionsize...) 
// costbasis calculation
// cashout balance (PricesExactAmounts)
// use the special address for 50% fees
bool PricesValidate(struct CCcontract_info *cp,Eval* eval,const CTransaction &tx, uint32_t nIn)
{
    vscript_t vopret;

    if (strcmp(ASSETCHAINS_SYMBOL, "REKT0") == 0 && chainActive.Height() < 2965)
        return true;
    // check basic opret rules:
    if (PricesCheckOpret(tx, vopret) == 0)
        return eval->Invalid("tx has no prices opreturn");

    uint8_t funcId = vopret.begin()[1];

    CTransaction firstVinTx;
    vscript_t firstVinTxOpret;
    bool foundFirst = false;
    int32_t ccVinCount = 0;
    uint32_t prevoutN = 0;

    // check basic rules:

    // find first cc vin and load vintx (might be either bet or add funding tx):
    for (auto vin : tx.vin) {
        if (cp->ismyvin(vin.scriptSig)) {
            CTransaction vintx;
            uint256 hashBlock;
            vscript_t vintxOpret;

            if (!myGetTransaction(vin.prevout.hash, vintx, hashBlock))
                return eval->Invalid("cannot load vintx");

            if (PricesCheckOpret(vintx, vintxOpret) == 0) {
                //return eval->Invalid("cannot find prices opret in vintx");
                std::cerr << "PricesValidate() " << "cannot find prices opret in vintx" << std::endl;
            }

            if (funcId != 'F' && vintxOpret.begin()[1] == 'B' && prevoutN == 1) {   
                //return eval->Invalid("cannot spend bet marker");
                std::cerr << "PricesValidate() " << " non-final tx cannot spend cc marker vout=" << prevoutN << std::endl;
            }

            if (!foundFirst) {
                prevoutN = vin.prevout.n;
                firstVinTx = vintx;
                firstVinTxOpret = vintxOpret;
                foundFirst = true;
            }
            ccVinCount++;
        }
    }
    if (!foundFirst)   
        return eval->Invalid("prices cc vin not found");

    if (funcId != 'F' && ccVinCount > 1) {// for all prices tx except final tx only one cc vin is allowed
        //return eval->Invalid("only one prices cc vin allowed for this tx");
        std::cerr << "PricesValidate() " << "only one prices cc vin allowed for this tx" << std::endl;
    }

    switch (funcId) {
    case 'B':   // bet 
        return eval->Invalid("unexpected validate for bet funcid");

    case 'A':   // add funding
        // check tx structure:
        if (!ValidateAddFundingTx(cp, eval, tx, firstVinTx)) {
            //return false;  // invalid state is already set in the func
            std::cerr << "PricesValidate() " << "ValidateAddFundingTx = false " << eval->state.GetRejectReason()  << std::endl;
        }

        if (firstVinTxOpret.begin()[1] == 'B') {
            if (!ValidateBetTx(cp, eval, firstVinTx)) {// check tx structure
                // return false;
                std::cerr << "PricesValidate() " << "funcId=A ValidatebetTx = false " << eval->state.GetRejectReason() << std::endl;
            }
        }
        else if (firstVinTxOpret.begin()[1] == 'A') {
            // no need to validate the previous addfunding tx (it was validated when added)
        }

        if (prevoutN != 0) {   // check spending rules
            // return eval->Invalid("incorrect vintx vout to spend");
            std::cerr << "PricesValidate() " << "add fund tx incorrect vout to spend=" << prevoutN << std::endl;
        }
        break;

    case 'C':   // set costbasis 
        if (!ValidateCostbasisTx(cp, eval, tx, firstVinTx)) {
            //return false;
            std::cerr << "PricesValidate() " << "ValidateCostbasisTx=false " << eval->state.GetRejectReason() << std::endl;
        }
        if (!ValidateBetTx(cp, eval, firstVinTx)) {
            //return false;
            std::cerr << "PricesValidate() " << "funcId=C ValidateBetTx=false " << eval->state.GetRejectReason() << std::endl;
        }
        if (prevoutN != 1) {   // check spending rules
            // return eval->Invalid("incorrect vout to spend");
            std::cerr << "PricesValidate() " << "costbasis tx incorrect vout to spend=" << prevoutN << std::endl;
        }
        //return eval->Invalid("test: costbasis is good");
        break;

    case 'F':   // final tx 
        if (!ValidateFinalTx(cp, eval, tx, firstVinTx)) {
            ///return false;
            std::cerr << "PricesValidate() " << "ValidateFinalTx=false " << eval->state.GetRejectReason() << std::endl;
        }
        if (!ValidateBetTx(cp, eval, firstVinTx)) {
            // return false;
            std::cerr << "PricesValidate() " << "ValidateBetTx=false " << eval->state.GetRejectReason() << std::endl;
        }
        if (prevoutN != 1) {   // check spending rules
            // return eval->Invalid("incorrect vout to spend");
            std::cerr << "PricesValidate() "<< "final tx incorrect vout to spend=" << prevoutN << std::endl;
        }
        break;

    default:
        return eval->Invalid("invalid funcid");
    }

    eval->state = CValidationState();
    return true;
}
// end of consensus code

// helper functions for rpc calls in rpcwallet.cpp

int64_t AddPricesInputs(struct CCcontract_info *cp, CMutableTransaction &mtx, char *destaddr, int64_t total, int32_t maxinputs)
{
    int64_t nValue, price, totalinputs = 0; uint256 txid, hashBlock; std::vector<uint8_t> origpubkey; CTransaction vintx; int32_t vout, n = 0;
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > unspentOutputs;

    SetCCunspents(unspentOutputs, destaddr);
    for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> >::const_iterator it = unspentOutputs.begin(); it != unspentOutputs.end(); it++)
    {
        txid = it->first.txhash;
        vout = (int32_t)it->first.index;
        //if (vout == exclvout && txid == excltxid)  // exclude vout which is added directly to vins outside this function
        //    continue;
        if (GetTransaction(txid, vintx, hashBlock, false) != 0 && vout < vintx.vout.size())
        {
            vscript_t vopret;
            uint8_t funcId = PricesCheckOpret(vintx, vopret);
            if (funcId == 'B' && vout == 1)  // skip cc marker
                continue;

            if ((nValue = vintx.vout[vout].nValue) >= total / maxinputs && myIsutxo_spentinmempool(ignoretxid, ignorevin, txid, vout) == 0)
            {
                if (total != 0 && maxinputs != 0)
                    mtx.vin.push_back(CTxIn(txid, vout, CScript()));
                nValue = it->second.satoshis;
                totalinputs += nValue;
                n++;
                if ((total > 0 && totalinputs >= total) || (maxinputs > 0 && n >= maxinputs))
                    break;
            }
        }
    }
    return(totalinputs);
}

UniValue prices_rawtxresult(UniValue &result, std::string rawtx, int32_t broadcastflag)
{
    CTransaction tx;
    if (rawtx.size() > 0)
    {
        result.push_back(Pair("hex", rawtx));
        if (DecodeHexTx(tx, rawtx) != 0)
        {
            if (broadcastflag != 0 && myAddtomempool(tx) != 0)
                RelayTransaction(tx);
            result.push_back(Pair("txid", tx.GetHash().ToString()));
            result.push_back(Pair("result", "success"));
        }
        else 
            result.push_back(Pair("error", "decode hex"));
    }
    else 
        result.push_back(Pair("error", "couldnt finalize CCtx"));
    return(result);
}

static std::string prices_getsourceexpression(const std::vector<uint16_t> &vec) {

    std::string expr;

    for (int32_t i = 0; i < vec.size(); i++) 
    {
        char name[65];
        std::string operand;
        uint16_t opcode = vec[i];
        int32_t value = (opcode & (KOMODO_MAXPRICES - 1));   // index or weight 

        switch (opcode & KOMODO_PRICEMASK)
        {
        case 0: // indices 
            komodo_pricename(name, value);
            operand = std::string(name);
            break;

        case PRICES_WEIGHT: // multiply by weight and consume top of stack by updating price
            operand = std::to_string(value);
            break;

        case PRICES_MULT:   // "*"
            operand = std::string("*");
            break;

        case PRICES_DIV:    // "/"
            operand = std::string("/");
            break;

        case PRICES_INV:    // "!"
            operand = std::string("!");
            break;

        case PRICES_MDD:    // "*//"
            operand = std::string("*//");
            break;

        case PRICES_MMD:    // "**/"
            operand = std::string("**/");
            break;

        case PRICES_MMM:    // "***"
            operand = std::string("***");
            break;

        case PRICES_DDD:    // "///"
            operand = std::string("///");
            break;

        default:
            return "invalid opcode";
            break;
        }

        if (expr.size() > 0)
            expr += std::string(", ");
        expr += operand;
    }
    return expr;
}

// helper functions to get synthetic expression reduced:

// return s true and needed operand count if string is opcode
static bool prices_isopcode(const std::string &s, int &need)
{
    if (s == "!") {
        need = 1;
        return true;
    }
    else if (s == "*" || s == "/") {
        need = 2;
        return true;
    }
    else if (s == "***" || s == "///" || s == "*//" || s == "**/") {
        need = 3;
        return true;
    }
    else
        return false;
}

// split pair onto two quotes divided by "_" 
static void prices_splitpair(const std::string &pair, std::string &upperquote, std::string &bottomquote)
{
    size_t pos = pair.find('_');   // like BTC_USD
    if (pos != std::string::npos) {
        upperquote = pair.substr(0, pos);
        bottomquote = pair.substr(pos + 1);
    }
    else {
        upperquote = pair;
        bottomquote = "";
    }
    //std::cerr << "prices_splitpair: upperquote=" << upperquote << " bottomquote=" << bottomquote << std::endl;
}

// invert pair like BTS_USD -> USD_BTC
static std::string prices_invertpair(const std::string &pair)
{
    std::string upperquote, bottomquote;
    prices_splitpair(pair, upperquote, bottomquote);
    return bottomquote + std::string("_") + upperquote;
}

// invert pairs in operation accordingly to "/" operator, convert operator to * or ***
static void prices_invertoperation(const std::vector<std::string> &vexpr, int p, std::vector<std::string> &voperation)
{
    int need;

    voperation.clear();
    if (prices_isopcode(vexpr[p], need)) {
        if (need > 1) {
            if (need == 2) {
                voperation.push_back(vexpr[p - 2]);
                if (vexpr[p] == "/")
                    voperation.push_back(prices_invertpair(vexpr[p - 1]));
                else
                    voperation.push_back(vexpr[p - 1]);
                voperation.push_back("*");
            }

            if (need == 3) {
                int i;
                std::string::const_iterator c;
                for (c = vexpr[p].begin(), i = -3; c != vexpr[p].end(); c++, i++) {
                    if (*c == '/')
                        voperation.push_back(prices_invertpair(vexpr[p + i]));
                    else
                        voperation.push_back(vexpr[p + i]);
                }
                voperation.push_back("***");
            }
        }
        else if (vexpr[p] == "!") {
            voperation.push_back(prices_invertpair(vexpr[p - 1]));
            // do not add operator
        }
    }

    //std::cerr << "prices_invert inverted=";
    //for (auto v : voperation) std::cerr << v << " ";
    //std::cerr << std::endl;
}

// reduce pairs in the operation, change or remove opcode if reduced
static int prices_reduceoperands(std::vector<std::string> &voperation)
{
    int opcount = voperation.size() - 1;
    int need = opcount;
    //std::cerr << "prices_reduceoperands begin need=" << need << std::endl;

    while (true) {
        int i;
        //std::cerr << "prices_reduceoperands opcount=" << opcount << std::endl;
        for (i = 0; i < opcount; i++) {
            std::string upperquote, bottomquote;
            bool breaktostart = false;

            //std::cerr << "prices_reduceoperands voperation[i]=" << voperation[i] << " i=" << i << std::endl;
            prices_splitpair(voperation[i], upperquote, bottomquote);
            if (upperquote == bottomquote) {
                std::cerr << "prices_reduceoperands erasing i=" << i << std::endl;
                voperation.erase(voperation.begin() + i);
                opcount--;
                //std::cerr << "prices_reduceoperands erased, size=" << voperation.size() << std::endl;

                if (voperation.size() > 0 && voperation.back() == "*")
                    voperation.pop_back();
                breaktostart = true;
                break;
            }


            int j;
            for (j = i + 1; j < opcount; j++) {

                //std::cerr << "prices_reduceoperands voperation[j]=" << voperation[j] << " j=" << j << std::endl;

                std::string upperquotej, bottomquotej;
                prices_splitpair(voperation[j], upperquotej, bottomquotej);
                if (upperquote == bottomquotej || bottomquote == upperquotej) {
                    if (upperquote == bottomquotej)
                        voperation[i] = upperquotej + "_" + bottomquote;
                    else
                        voperation[i] = upperquote + "_" + bottomquotej;
                    //std::cerr << "prices_reduceoperands erasing j=" << j << std::endl;
                    voperation.erase(voperation.begin() + j);
                    opcount--;
                    //std::cerr << "prices_reduceoperands erased, size=" << voperation.size() << std::endl;

                    need--;
                    if (voperation.back() == "***") {
                        voperation.pop_back();
                        voperation.push_back("*");  // convert *** to *
                    }
                    else if (voperation.back() == "*") {
                        voperation.pop_back();      // convert * to nothing
                    }
                    breaktostart = true;
                    break;
                }
            }
            if (breaktostart)
                break;
        }
        if (i >= opcount)  // all seen
            break;
    }

    //std::cerr << "prices_reduceoperands end need=" << need << std::endl;
    return need;
}

// substitute reduced operation in vectored expr
static void prices_substitutereduced(std::vector<std::string> &vexpr, int p, std::vector<std::string> voperation)
{
    int need;
    if (prices_isopcode(vexpr[p], need)) {
        vexpr.erase(vexpr.begin() + p - need, vexpr.begin() + p + 1);
        vexpr.insert(vexpr.begin() + p - need, voperation.begin(), voperation.end());
    }
}

// try to reduce synthetic expression by substituting "BTC_USD, BTC_EUR, 30, /" with "EUR_USD, 30" etc
static std::string prices_getreducedexpr(const std::string &expr)
{
    std::string reduced;

    std::vector<std::string> vexpr;
    SplitStr(expr, vexpr);

    for (size_t i = 0; i < vexpr.size(); i++) {
        int need;

        if (prices_isopcode(vexpr[i], need)) {
            std::vector<std::string> voperation;
            prices_invertoperation(vexpr, i, voperation);
            if (voperation.size() > 0)  {
                int reducedneed = prices_reduceoperands(voperation);
                if (reducedneed < need) {
                    prices_substitutereduced(vexpr, i, voperation);
                }
            }
        }
    }

    for (size_t i = 0; i < vexpr.size(); i++) {
        if (reduced.size() > 0)
            reduced += std::string(", ");
        reduced += vexpr[i];
    }

    //std::cerr << "reduced=" << reduced << std::endl;
    return reduced;
}

// parse synthetic expression into vector of codes
int32_t prices_syntheticvec(std::vector<uint16_t> &vec, std::vector<std::string> synthetic)
{
    int32_t i, need, ind, depth = 0; std::string opstr; uint16_t opcode, weight;
    if (synthetic.size() == 0) {
        std::cerr << "prices_syntheticvec() expression is empty" << std::endl;
        return(-1);
    }
    for (i = 0; i < synthetic.size(); i++)
    {
        need = 0;
        opstr = synthetic[i];
        if (opstr == "*")
            opcode = PRICES_MULT, need = 2;
        else if (opstr == "/")
            opcode = PRICES_DIV, need = 2;
        else if (opstr == "!")
            opcode = PRICES_INV, need = 1;
        else if (opstr == "**/")
            opcode = PRICES_MMD, need = 3;
        else if (opstr == "*//")
            opcode = PRICES_MDD, need = 3;
        else if (opstr == "***")
            opcode = PRICES_MMM, need = 3;
        else if (opstr == "///")
            opcode = PRICES_DDD, need = 3;
        else if (!is_weight_str(opstr) && (ind = komodo_priceind(opstr.c_str())) >= 0)
            opcode = ind, need = 0;
        else if ((weight = atoi(opstr.c_str())) > 0 && weight < KOMODO_MAXPRICES)
        {
            opcode = PRICES_WEIGHT | weight;
            need = 1;
        }
        else {
            std::cerr << "prices_syntheticvec() incorrect opcode=" << opstr << std::endl;
            return(-2);
        }
        if (depth < need) {
            std::cerr << "prices_syntheticvec() incorrect not enough operands for opcode=" << opstr << std::endl;
            return(-3);
        }
        depth -= need;
        std::cerr << "prices_syntheticvec() opcode=" << opcode << " opstr=" << opstr << " need=" << need << " depth=" << depth << std::endl;
        if ((opcode & KOMODO_PRICEMASK) != PRICES_WEIGHT) { // skip weight
            depth++;                                          // increase operands count
            std::cerr << "depth++=" << depth << std::endl;
        }
        if (depth > 3) {
            std::cerr << "prices_syntheticvec() too many operands, last=" << opstr << std::endl;
            return(-4);
        }
        vec.push_back(opcode);
    }
    if (depth != 0)
    {
        fprintf(stderr, "prices_syntheticvec() depth.%d not empty\n", depth);
        return(-5);
    }
    return(0);
}

// calculates price for synthetic expression
int64_t prices_syntheticprice(std::vector<uint16_t> vec, int32_t height, int32_t minmax, int16_t leverage)
{
    int32_t i, value, errcode, depth, retval = -1;
    uint16_t opcode;
    int64_t *pricedata, pricestack[4], a, b, c;

    mpz_t mpzTotalPrice, mpzPriceValue, mpzDen, mpzA, mpzB, mpzC, mpzResult;

    mpz_init(mpzTotalPrice);
    mpz_init(mpzPriceValue);
    mpz_init(mpzDen);

    mpz_init(mpzA);
    mpz_init(mpzB);
    mpz_init(mpzC);
    mpz_init(mpzResult);

    pricedata = (int64_t *)calloc(sizeof(*pricedata) * 3, 1 + PRICES_DAYWINDOW * 2 + PRICES_SMOOTHWIDTH);
    depth = errcode = 0;
    mpz_set_si(mpzTotalPrice, 0);
    mpz_set_si(mpzDen, 0);

    for (i = 0; i < vec.size(); i++)
    {
        opcode = vec[i];
        value = (opcode & (KOMODO_MAXPRICES - 1));   // index or weight 

        mpz_set_ui(mpzResult, 0);  // clear result to test overflow (see below)

        std::cerr << "prices_syntheticprice" << " i=" << i << " mpzTotalPrice=" << mpz_get_si(mpzTotalPrice) << " value=" << value << " depth=" << depth <<  " opcode&KOMODO_PRICEMASK=" << (opcode & KOMODO_PRICEMASK) <<std::endl;
        switch (opcode & KOMODO_PRICEMASK)
        {
        case 0: // indices 
            pricestack[depth] = 0;
            if (komodo_priceget(pricedata, value, height, 1) >= 0)
            {
                std::cerr << "prices_syntheticprice" << " pricedata[0]=" << pricedata[0] << " pricedata[1]=" << pricedata[1] << " pricedata[2]=" << pricedata[2] << std::endl;
                // push price to the prices stack
                /*if (!minmax)
                    pricestack[depth] = pricedata[2];   // use smoothed value if we are over 24h
                else
                {
                    // if we are within 24h use min or max price
                    if (leverage > 0)
                        pricestack[depth] = (pricedata[1] > pricedata[2]) ? pricedata[1] : pricedata[2]; // MAX
                    else
                        pricestack[depth] = (pricedata[1] < pricedata[2]) ? pricedata[1] : pricedata[2]; // MIN
                }*/
                pricestack[depth] = pricedata[2];
            }
            else
                errcode = -1;

            if (pricestack[depth] == 0)
                errcode = -1;

            depth++;
            break;

        case PRICES_WEIGHT: // multiply by weight and consume top of stack by updating price
            if (depth == 1) {
                depth--;
                //price += pricestack[0] * value;
                mpz_set_si(mpzPriceValue, pricestack[0]);
                mpz_mul_si(mpzPriceValue, mpzPriceValue, value);
                mpz_add(mpzTotalPrice, mpzTotalPrice, mpzPriceValue);              // accumulate weight's value  

                // den += value; 
                mpz_add_ui(mpzDen, mpzDen, (uint64_t)value);              // accumulate weight's value  
            }
            else
                errcode = -2;
            break;

        case PRICES_MULT:   // "*"
            if (depth >= 2) {
                b = pricestack[--depth];
                a = pricestack[--depth];
                // pricestack[depth++] = (a * b) / SATOSHIDEN;
                mpz_set_si(mpzA, a);
                mpz_set_si(mpzB, b);
                mpz_mul(mpzResult, mpzA, mpzB);
                mpz_tdiv_q_ui(mpzResult, mpzResult, SATOSHIDEN);
                pricestack[depth++] = mpz_get_si(mpzResult);

            }
            else
                errcode = -3;
            break;

        case PRICES_DIV:    // "/"
            if (depth >= 2) {
                b = pricestack[--depth];
                a = pricestack[--depth];
                // pricestack[depth++] = (a * SATOSHIDEN) / b;
                mpz_set_si(mpzA, a);
                mpz_set_si(mpzB, b);
                mpz_mul_ui(mpzResult, mpzA, SATOSHIDEN);
                mpz_tdiv_q(mpzResult, mpzResult, mpzB);                 
                pricestack[depth++] = mpz_get_si(mpzResult);
            }
            else
                errcode = -4;
            break;

        case PRICES_INV:    // "!"
            if (depth >= 1) {
                a = pricestack[--depth];
                // pricestack[depth++] = (SATOSHIDEN * SATOSHIDEN) / a;
                mpz_set_si(mpzA, a);
                mpz_set_ui(mpzResult, SATOSHIDEN);
                mpz_mul_ui(mpzResult, mpzResult, SATOSHIDEN);           
                mpz_tdiv_q(mpzResult, mpzResult, mpzA);                 
                pricestack[depth++] = mpz_get_si(mpzResult);
            }
            else
                errcode = -5;
            break;

        case PRICES_MDD:    // "*//"
            if (depth >= 3) {
                c = pricestack[--depth];
                b = pricestack[--depth];
                a = pricestack[--depth];
                // pricestack[depth++] = (((a * SATOSHIDEN) / b) * SATOSHIDEN) / c;
                mpz_set_si(mpzA, a);
                mpz_set_si(mpzB, b);
                mpz_set_si(mpzC, c);
                mpz_mul_ui(mpzResult, mpzA, SATOSHIDEN);
                mpz_tdiv_q(mpzResult, mpzResult, mpzB);                 
                mpz_mul_ui(mpzResult, mpzResult, SATOSHIDEN);
                mpz_tdiv_q(mpzResult, mpzResult, mpzC);
                pricestack[depth++] = mpz_get_si(mpzResult);
            }
            else
                errcode = -6;
            break;

        case PRICES_MMD:    // "**/"
            if (depth >= 3) {
                c = pricestack[--depth];
                b = pricestack[--depth];
                a = pricestack[--depth];
                // pricestack[depth++] = (a * b) / c;
                mpz_set_si(mpzA, a);
                mpz_set_si(mpzB, b);
                mpz_set_si(mpzC, c);
                mpz_mul(mpzResult, mpzA, mpzB);
                mpz_tdiv_q(mpzResult, mpzResult, mpzC);
                pricestack[depth++] = mpz_get_si(mpzResult);
            }
            else
                errcode = -7;
            break;

        case PRICES_MMM:    // "***"
            if (depth >= 3) {
                c = pricestack[--depth];
                b = pricestack[--depth];
                a = pricestack[--depth];
                // pricestack[depth++] = (((a * b) / SATOSHIDEN ) * c) / SATOSHIDEN;
                mpz_set_si(mpzA, a);
                mpz_set_si(mpzB, b);
                mpz_set_si(mpzC, c);
                mpz_mul(mpzResult, mpzA, mpzB);
                mpz_tdiv_q_ui(mpzResult, mpzResult, SATOSHIDEN);
                mpz_mul(mpzResult, mpzResult, mpzC);
                mpz_tdiv_q_ui(mpzResult, mpzResult, SATOSHIDEN);
                pricestack[depth++] = mpz_get_si(mpzResult);
            }
            else
                errcode = -8;
            break;
                
        case PRICES_DDD:    // "///"
            if (depth >= 3) {
                c = pricestack[--depth];
                b = pricestack[--depth];
                a = pricestack[--depth];
                //pricestack[depth++] = (((((SATOSHIDEN * SATOSHIDEN) / a) * SATOSHIDEN) / b) * SATOSHIDEN) / c;
                mpz_set_si(mpzA, a);
                mpz_set_si(mpzB, b);
                mpz_set_si(mpzC, c);
                mpz_set_ui(mpzResult, SATOSHIDEN);
                mpz_mul_ui(mpzResult, mpzResult, SATOSHIDEN);
                mpz_tdiv_q(mpzResult, mpzResult, mpzA);
                mpz_mul_ui(mpzResult, mpzResult, SATOSHIDEN);
                mpz_tdiv_q(mpzResult, mpzResult, mpzB);
                mpz_mul_ui(mpzResult, mpzResult, SATOSHIDEN);
                mpz_tdiv_q(mpzResult, mpzResult, mpzC);
                pricestack[depth++] = mpz_get_si(mpzResult);
            }
            else
                errcode = -9;
            break;

        default:
            errcode = -10;
            break;
        }

        std::cerr << "prices_syntheticprice test mpzResult=" << mpz_get_si(mpzResult) << std::endl;
        // check overflow:
        if (mpz_cmp_si(mpzResult, std::numeric_limits<int64_t>::max()) > 0) {
            errcode = -13;
            break;
        }

        if (errcode != 0)
            break;

        if( depth > 0 )
            std::cerr << "prices_syntheticprice top pricestack[depth-1=" << depth-1 << "]=" << pricestack[depth-1] << std::endl;
        else
            std::cerr << "prices_syntheticprice pricestack empty" << std::endl;

    }
    free(pricedata);
    mpz_clear(mpzResult);
    mpz_clear(mpzA);
    mpz_clear(mpzB);
    mpz_clear(mpzC);

    if( mpz_get_si(mpzDen) != 0 )
        mpz_tdiv_q(mpzTotalPrice, mpzTotalPrice, mpzDen);   // price / den
    
    int64_t den = mpz_get_si(mpzDen);
    int64_t priceIndex = mpz_get_si(mpzTotalPrice);

    mpz_clear(mpzDen);
    mpz_clear(mpzTotalPrice);
    mpz_clear(mpzPriceValue);

    if (errcode != 0)
        std::cerr << "prices_syntheticprice errcode in switch=" << errcode << std::endl;

    if (errcode == -13) {
        std::cerr << "prices_syntheticprice overflow in price" << std::endl;
        return errcode;
    }

    if (den == 0) {
        std::cerr << "prices_syntheticprice den==0 return err=-11" << std::endl;
        return(-11);
    }
    else if (depth != 0) {
        std::cerr << "prices_syntheticprice depth!=0 err=-12" << std::endl;
        return(-12);
    }
    else if (errcode != 0) {
        std::cerr << "prices_syntheticprice err=" << errcode << std::endl;
        return(errcode);
    }
    std::cerr << "prices_syntheticprice priceIndex=" << priceIndex << " den=" << den << std::endl;

    return priceIndex;
}

// calculates costbasis and profit/loss for the bet
int32_t prices_syntheticprofits(int64_t &costbasis, int32_t firstheight, int32_t height, int16_t leverage, std::vector<uint16_t> vec, int64_t positionsize,  int64_t &profits, int64_t &outprice)
{
    int64_t price;
#ifndef TESTMODE
    const int32_t COSTBASIS_PERIOD = PRICES_DAYWINDOW;
#else
    const int32_t COSTBASIS_PERIOD = 7;
#endif


    if (height < firstheight) {
        fprintf(stderr, "requested height is lower than bet firstheight.%d\n", height);
        return -1;
    }

    int32_t minmax = (height < firstheight + COSTBASIS_PERIOD);  // if we are within 24h then use min or max value 

    if ((price = prices_syntheticprice(vec, height, minmax, leverage)) < 0)
    {
        fprintf(stderr, "error getting synthetic price at height.%d\n", height);
        return -1;
    }

    // clear lowest positions:
    //price /= PRICES_POINTFACTOR;
    //price *= PRICES_POINTFACTOR;
    outprice = price;
   
    if (minmax)    { // if we are within day window, set temp costbasis to max (or min) price value
        if (leverage > 0 && price > costbasis) {
            costbasis = price;  // set temp costbasis
            std::cerr << "prices_syntheticprofits() minmax costbasis=" << costbasis << std::endl;
        }
        else if (leverage < 0 && (costbasis == 0 || price < costbasis)) {
            costbasis = price;
            std::cerr << "prices_syntheticprofits() minmax costbasis=" << costbasis << std::endl;
        }
        //else {  //-> use the previous value
        //    std::cerr << "prices_syntheticprofits() unchanged costbasis=" << costbasis << " price=" << price << " leverage=" << leverage << std::endl;
        //}
    }
    else   { 
        if (height == firstheight + COSTBASIS_PERIOD) {
            // if costbasis not set, just set it
            //costbasis = price;

            // use calculated minmax costbasis
            std::cerr << "prices_syntheticprofits() use permanent costbasis=" << costbasis << " at height=" << height << std::endl;
        }
    }
    
    // normalize to 10,000,000 to prevent underflow
    //profits = costbasis > 0 ? (((price / PRICES_POINTFACTOR * PRICES_NORMFACTOR) / costbasis) - PRICES_NORMFACTOR / PRICES_POINTFACTOR) * PRICES_POINTFACTOR : 0;
    //double dprofits = costbasis > 0 ? ((double)price / (double)costbasis - 1) : 0;

    //std::cerr << "prices_syntheticprofits() test value1 (price/PRICES_POINTFACTOR * PRICES_NORMFACTOR)=" << (price / PRICES_POINTFACTOR * PRICES_NORMFACTOR) << std::endl;
    //std::cerr << "prices_syntheticprofits() test value2 (price/PRICES_POINTFACTOR * PRICES_NORMFACTOR)/costbasis=" << (costbasis != 0 ? (price / PRICES_POINTFACTOR * PRICES_NORMFACTOR)/costbasis : 0) << std::endl;

    //std::cerr << "prices_syntheticprofits() fractional profits=" << profits << std::endl;
    //std::cerr << "prices_syntheticprofits() profits double=" << (double)price / (double)costbasis -1.0 << std::endl;
    //double dprofits = (double)price / (double)costbasis - 1.0;

    //profits *= ((int64_t)leverage * (int64_t)positionsize);
    //profits /= (int64_t)PRICES_NORMFACTOR;  // de-normalize

    //dprofits *= ((double)leverage * (double)positionsize);

    //dprofits *= leverage * positionsize;
    // profits = dprofits;
    //std::cerr << "prices_syntheticprofits() dprofits=" << dprofits << std::endl;

    if (costbasis > 0)  {
        mpz_t mpzProfits;
        mpz_t mpzCostbasis;
        mpz_t mpzPrice;
        mpz_t mpzLeverage;

        mpz_init(mpzProfits);
        mpz_init(mpzCostbasis);
        mpz_init(mpzPrice);
        mpz_init(mpzLeverage);

        mpz_set_si(mpzCostbasis, costbasis);
        mpz_set_si(mpzPrice, price);
        mpz_mul_ui(mpzPrice, mpzPrice, SATOSHIDEN);                              // (price*SATOSHIDEN)

        mpz_tdiv_q(mpzProfits, mpzPrice, mpzCostbasis);           // profits = (price*SATOSHIDEN)/costbasis  // normalization
        mpz_sub_ui(mpzProfits, mpzProfits, SATOSHIDEN);                          // profits -= SATOSHIDEN

        mpz_set_si(mpzLeverage, leverage);
        mpz_mul(mpzProfits, mpzProfits, mpzLeverage);                            // profits *= leverage
        mpz_mul_ui(mpzProfits, mpzProfits, positionsize);                        // profits *= positionsize
        mpz_tdiv_q_ui(mpzProfits, mpzProfits, SATOSHIDEN);          // profits /= SATOSHIDEN   // de-normalization

        profits = mpz_get_si(mpzProfits);

        mpz_clear(mpzLeverage);
        mpz_clear(mpzProfits);
        mpz_clear(mpzCostbasis);
        mpz_clear(mpzPrice);

    }
    else
        profits = 0;

    std::cerr << "prices_syntheticprofits() profits=" << profits << std::endl;
    return 0; //  (positionsize + addedbets + profits);
}

// makes result json object
void prices_betjson(UniValue &result, std::vector<OneBetData> bets, int16_t leverage, int32_t endheight, int64_t lastprice)
{

    UniValue resultbets(UniValue::VARR);
    int64_t totalbets = 0;
    int64_t totalprofits = 0;

    for (auto b : bets) {
        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("positionsize", ValueFromAmount(b.positionsize)));
        entry.push_back(Pair("profits", ValueFromAmount(b.profits)));
        entry.push_back(Pair("costbasis", ValueFromAmount(b.costbasis)));
        entry.push_back(Pair("firstheight", b.firstheight));
        resultbets.push_back(entry);
        totalbets += b.positionsize;
        totalprofits += b.profits;
    }
    int64_t equity = totalbets + totalprofits;

    result.push_back(Pair("bets", resultbets));
    result.push_back(Pair("leverage", (int64_t)leverage));
    result.push_back(Pair("TotalPositionSize", ValueFromAmount(totalbets)));
    result.push_back(Pair("TotalProfits", ValueFromAmount(totalprofits)));
    result.push_back(Pair("equity", ValueFromAmount(equity)));
    result.push_back(Pair("LastPrice", ValueFromAmount(lastprice)));
    result.push_back(Pair("LastHeight", endheight));
}

// retrieves costbasis from a tx spending bettx vout1 (deprecated)
int64_t prices_costbasis(CTransaction bettx, uint256 &txidCostbasis)
{
    int64_t costbasis = 0;
    // if vout1 is spent, follow and extract costbasis from opreturn
    //uint8_t prices_costbasisopretdecode(CScript scriptPubKey,uint256 &bettxid,CPubKey &pk,int32_t &height,int64_t &costbasis)
    //uint256 txidCostbasis;
    int32_t vini;
    int32_t height;
    txidCostbasis = zeroid;
/*
    if (CCgetspenttxid(txidCostbasis, vini, height, bettx.GetHash(), 1) < 0) {
        std::cerr << "prices_costbasis() no costbasis txid found" << std::endl;
        return 0;
    }

    CTransaction txCostbasis;
    uint256 hashBlock;
    uint256 bettxid;
    CPubKey pk;
    bool isLoaded = false;
    uint8_t funcId = 0;

    if ((isLoaded = myGetTransaction(txidCostbasis, txCostbasis, hashBlock)) &&
        txCostbasis.vout.size() > 0 &&
        (funcId = prices_costbasisopretdecode(txCostbasis.vout.back().scriptPubKey, bettxid, pk, height, costbasis)) != 0) {
        return costbasis;
    }

    std::cerr << "prices_costbasis() cannot load costbasis tx or decode opret" << " isLoaded=" << isLoaded <<  " funcId=" << (int)funcId << std::endl;  */
    return 0;
}

// enumerates and retrieves added bets, returns the last baton txid
int64_t prices_enumaddedbets(uint256 &batontxid, std::vector<OneBetData> &bets, uint256 bettxid)
{
    int64_t addedBetsTotal = 0;
    int32_t vini;
    int32_t height;
    int32_t retcode;

    batontxid = bettxid; // initially set to the source bet tx
    uint256 sourcetxid = bettxid;

    // iterate through batons, adding up vout1 -> addedbets
    while ((retcode = CCgetspenttxid(batontxid, vini, height, sourcetxid, 0)) == 0) {

        CTransaction txBaton;
        CBlockIndex blockIdx;
        uint256 bettxidInOpret;
        CPubKey pk;
        bool isLoaded = false;
        uint8_t funcId = 0;
        int64_t amount;
        EvalRef eval;

        if ((isLoaded = eval->GetTxConfirmed(batontxid, txBaton, blockIdx)) &&
            blockIdx.IsValid() &&
            txBaton.vout.size() > 0 &&
            (funcId = prices_addopretdecode(txBaton.vout.back().scriptPubKey, bettxidInOpret, pk, amount)) != 0) 
        {    
            OneBetData added;

            addedBetsTotal += amount;
            added.positionsize = amount;
            added.firstheight = blockIdx.GetHeight();
            bets.push_back(added);
            std::cerr << "prices_batontxid() added amount=" << amount << std::endl;
        }
        else {
            std::cerr << "prices_batontxid() cannot load or decode add bet tx, isLoaded=" << isLoaded << " funcId=" << (int)funcId << std::endl;
            return -1;
        }
        sourcetxid = batontxid;
    }

    return(addedBetsTotal);
}

// pricesbet rpc impl: make betting tx
UniValue PricesBet(int64_t txfee, int64_t amount, int16_t leverage, std::vector<std::string> synthetic)
{
    fprintf(stderr, "assetchains_contract.%i vs eval_prices.%i\n",ASSETCHAINS_EARLYTXIDCONTRACT, EVAL_PRICES);
    if ( ASSETCHAINS_EARLYTXIDCONTRACT == EVAL_PRICES && KOMODO_EARLYTXID_SCRIPTPUB.size() == 0 )
    {
        // Lock here, as in validation we cannot call lock in the function itself.
        // may not be needed as the validation call to update the global, is called in a LOCK already, and it can only update there and here.
        LOCK(cs_main);
        GetKomodoEarlytxidScriptPub();
    }
    /*
    int32_t nextheight = komodo_nextheight();
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), nextheight); UniValue result(UniValue::VOBJ);
    struct CCcontract_info *cp, C; 
    CPubKey pricespk, mypk; 
    int64_t betamount, firstprice = 0; 
    std::vector<uint16_t> vec; 
    //char myaddr[64]; 
    std::string rawtx;

    if (leverage > PRICES_MAXLEVERAGE || leverage < -PRICES_MAXLEVERAGE)
    {
        result.push_back(Pair("result", "error"));
        result.push_back(Pair("error", "leverage too big"));
        return(result);
    }
    cp = CCinit(&C, EVAL_PRICES);
    if (txfee == 0)
        txfee = PRICES_TXFEE;
    mypk = pubkey2pk(Mypubkey());
    pricespk = GetUnspendable(cp, 0);
    //GetCCaddress(cp, myaddr, mypk);
    if (prices_syntheticvec(vec, synthetic) < 0 || (firstprice = prices_syntheticprice(vec, nextheight - 1, 1, leverage)) < 0 || vec.size() == 0 || vec.size() > 4096)
    {
        result.push_back(Pair("result", "error"));
        result.push_back(Pair("error", "invalid synthetic"));
        return(result);
    }
    if (AddNormalinputs(mtx, mypk, amount + 4 * txfee, 64) >= amount + 4 * txfee)
    {
        betamount = (amount * 199) / 200;
        mtx.vout.push_back(MakeCC1vout(cp->evalcode, txfee, mypk));                                 // vout0 baton for total funding
        //  mtx.vout.push_back(MakeCC1vout(cp->evalcode, (amount - betamount) + 2 * txfee, pricespk));  // vout1, when spent, costbasis is set
        mtx.vout.push_back(MakeCC1vout(cp->evalcode, txfee, pricespk));                             // vout1 cc marker (NVOUT_CCMARKER)
        mtx.vout.push_back(MakeCC1vout(cp->evalcode, betamount, pricespk));                         // vout2 betamount
        mtx.vout.push_back(CTxOut(txfee, CScript() << ParseHex(HexStr(pricespk)) << OP_CHECKSIG));  // vout3 normal marker NVOUT_NORMALMARKER - TODO: remove it as we have cc marker now, when move to the new chain
        if ( ASSETCHAINS_EARLYTXIDCONTRACT == EVAL_PRICES && KOMODO_EARLYTXID_SCRIPTPUB.size() == 0 )
        {
            // Lock here, as in validation we cannot call lock in the function itself.
            // may not be needed as the validation call to update the global, is called in a LOCK already, and it can only update there and here.
            LOCK(cs_main);
            GetKomodoEarlytxidScriptPub();
        }
        mtx.vout.push_back(CTxOut(amount-betamount, KOMODO_EARLYTXID_SCRIPTPUB));

        rawtx = FinalizeCCTx(0, cp, mtx, mypk, txfee, prices_betopret(mypk, nextheight - 1, amount, leverage, firstprice, vec, zeroid));
        return(prices_rawtxresult(result, rawtx, 0));
    }
    result.push_back(Pair("result", "error"));
    result.push_back(Pair("error", "not enough funds"));
    return(result); 
    */
}

// pricesaddfunding rpc impl: add yet another bet
UniValue PricesAddFunding(int64_t txfee, uint256 bettxid, int64_t amount)
{
    int32_t nextheight = komodo_nextheight();
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), nextheight); UniValue result(UniValue::VOBJ);
    struct CCcontract_info *cp, C; CTransaction bettx; 
    CPubKey pricespk, mypk; 
    //int64_t addedbets = 0, betamount, firstprice; 
    std::vector<uint16_t> vec; 
    uint256 batontxid; 
    std::string rawtx; 
    //char myaddr[64];

    cp = CCinit(&C, EVAL_PRICES);
    if (txfee == 0)
        txfee = PRICES_TXFEE;
    mypk = pubkey2pk(Mypubkey());
    pricespk = GetUnspendable(cp, 0);
    //GetCCaddress(cp, myaddr, mypk);
    if (AddNormalinputs(mtx, mypk, amount + 2*txfee, 64) >= amount + 2*txfee)
    {
        std::vector<OneBetData> bets;
        if (prices_enumaddedbets(batontxid, bets, bettxid) >= 0)
        {
            mtx.vin.push_back(CTxIn(batontxid, 0, CScript()));
            mtx.vout.push_back(MakeCC1vout(cp->evalcode, txfee, mypk));         // vout0 baton for total funding
            mtx.vout.push_back(MakeCC1vout(cp->evalcode, amount, pricespk));    // vout1 added amount
            rawtx = FinalizeCCTx(0, cp, mtx, mypk, txfee, prices_addopret(bettxid, mypk, amount));
            return(prices_rawtxresult(result, rawtx, 0));
        }
        else
        {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "couldnt find batonttxid"));
            return(result);
        }
    }
    result.push_back(Pair("result", "error"));
    result.push_back(Pair("error", "not enough funds"));
    return(result);
}

// scan chain from the initial bet's first position upto the chain tip and calculate bet's costbasises and profits, breaks if rekt detected 
int32_t prices_scanchain(std::vector<OneBetData> &bets, int16_t leverage, std::vector<uint16_t> vec, int64_t &lastprice, int32_t &endheight) {

    if (bets.size() == 0)
        return -1;

    bool stop = false;
    for (int32_t height = bets[0].firstheight; ; height++)   // the last datum for 24h is the costbasis value
    {
        int64_t totalbets = 0;
        int64_t totalprofits = 0;

        // scan upto the chain tip
        for (int i = 0; i < bets.size(); i++) {

            if (height > bets[i].firstheight) {

                int32_t retcode = prices_syntheticprofits(bets[i].costbasis, bets[i].firstheight, height, leverage, vec, bets[i].positionsize, bets[i].profits, lastprice);
                if (retcode < 0) {
                    std::cerr << "prices_scanchain() prices_syntheticprofits returned -1, breaking" << std::endl;
                    stop = true;
                    break;
                }
                totalbets += bets[i].positionsize;
                totalprofits += bets[i].profits;
            }
        }

        if (stop)
            break;

        endheight = height;
        int64_t equity = totalbets + totalprofits;
        if (equity < 0)
        {   // we are in loss
            break;
        }
    }

    return 0;
}

// pricescostbasis rpc impl: set cost basis (open price) for the bet (deprecated)
UniValue PricesSetcostbasis(int64_t txfee, uint256 bettxid)
{
    int32_t nextheight = komodo_nextheight();
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), nextheight);
    UniValue result(UniValue::VOBJ);
    struct CCcontract_info *cp, C; CTransaction bettx; uint256 hashBlock, batontxid, tokenid;
    int64_t myfee, positionsize = 0, addedbets, firstprice = 0, lastprice, profits = 0, costbasis = 0, equity;
    int32_t i, firstheight = 0, height, numvouts; int16_t leverage = 0;
    std::vector<uint16_t> vec;
    CPubKey pk, mypk, pricespk; std::string rawtx;
/*
    cp = CCinit(&C, EVAL_PRICES);
    if (txfee == 0)
        txfee = PRICES_TXFEE;

    mypk = pubkey2pk(Mypubkey());
    pricespk = GetUnspendable(cp, 0);
    if (myGetTransaction(bettxid, bettx, hashBlock) != 0 && (numvouts = bettx.vout.size()) > 3)
    {
        if (prices_betopretdecode(bettx.vout[numvouts - 1].scriptPubKey, pk, firstheight, positionsize, leverage, firstprice, vec, tokenid) == 'B')
        {
            if (nextheight <= firstheight + PRICES_DAYWINDOW + 1) {
                result.push_back(Pair("result", "error"));
                result.push_back(Pair("error", "cannot calculate costbasis yet"));
                return(result);
            }

            addedbets = prices_enumaddedbets(batontxid, bettx, bettxid);
            mtx.vin.push_back(CTxIn(bettxid, 1, CScript()));              // spend vin1 with betamount
            //for (i = 0; i < PRICES_DAYWINDOW + 1; i++)   // the last datum for 24h is the actual costbasis value
            //{
                int32_t retcode = prices_syntheticprofits(costbasis, firstheight, firstheight + PRICES_DAYWINDOW, leverage, vec, positionsize, addedbets, profits, lastprice);
                if (retcode < 0) {
                    result.push_back(Pair("result", "error"));
                    result.push_back(Pair("error", "cannot calculate costbasis error getting price"));
                    return(result);
                }
                equity = positionsize + addedbets + profits;
                //if (equity < 0)
                //{   // we are in loss
                //    result.push_back(Pair("rekt", (int64_t)1));
                //    result.push_back(Pair("rektheight", (int64_t)firstheight + i));
                //    break;
                //}
            //}
            //if (i == PRICES_DAYWINDOW + 1)
            //    result.push_back(Pair("rekt", 0));

            prices_betjson(result, profits, costbasis, positionsize, addedbets, leverage, firstheight, firstprice, lastprice, equity);

            if (AddNormalinputs(mtx, mypk, txfee, 4) >= txfee)
            {
                myfee = bettx.vout[1].nValue / 10;   // fee for setting costbasis
                result.push_back(Pair("myfee", myfee));

                mtx.vout.push_back(CTxOut(myfee, CScript() << ParseHex(HexStr(mypk)) << OP_CHECKSIG));
                mtx.vout.push_back(MakeCC1vout(cp->evalcode, bettx.vout[1].nValue - myfee, pricespk));
                rawtx = FinalizeCCTx(0, cp, mtx, mypk, txfee, prices_costbasisopret(bettxid, mypk, firstheight + PRICES_DAYWINDOW , costbasis));  // -1
                return(prices_rawtxresult(result, rawtx, 0));
            }
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "not enough funds"));
            return(result);
        }
    } */
    result.push_back(Pair("result", "error"));
    result.push_back(Pair("error", "deprecated")); 
    return(result);
}


int32_t prices_getbetinfo(uint256 bettxid, BetInfo &betinfo)
{
    CTransaction bettx;
    uint256 hashBlock, batontxid, tokenid;

    if (myGetTransaction(bettxid, bettx, hashBlock) && bettx.vout.size() > 3)
    {
        if (hashBlock.IsNull())
            return -2;

        OneBetData bet1;
        if (prices_betopretdecode(bettx.vout.back().scriptPubKey, betinfo.pk, bet1.firstheight, bet1.positionsize, betinfo.leverage, betinfo.firstprice, betinfo.parsed, betinfo.tokenid) == 'B')
        {
            uint256 finaltxid;
            int32_t vini;
            int32_t finaltxheight; //, endheight;
                                   //std::vector<OneBetData> bets;


            if (CCgetspenttxid(finaltxid, vini, finaltxheight, bettxid, NVOUT_CCMARKER) == 0)
                betinfo.isOpen = false;
            else
                betinfo.isOpen = true;

            //bet1.amount = betinfo.positionsize;
            //bet1.firstheight = firstheight;
            betinfo.bets.push_back(bet1);

            prices_enumaddedbets(batontxid, betinfo.bets, bettxid);

            if (prices_scanchain(betinfo.bets, betinfo.leverage, betinfo.parsed, betinfo.lastprice, betinfo.lastheight) < 0) {
                return -4;
            }

            mpz_t mpzTotalbets;
            mpz_t mpzTotalprofits;
            mpz_t mpzTotalcostbasis;

            mpz_init(mpzTotalbets);
            mpz_init(mpzTotalprofits);
            mpz_init(mpzTotalcostbasis);

            int64_t totalbets = 0;
            int64_t totalprofits = 0;

            for (auto b : betinfo.bets) {
                mpz_t mpzProduct;
                mpz_t mpzProfits;

                mpz_init(mpzProduct);
                mpz_init(mpzProfits);

                //totalprofits += b.profits;
                //dcostbasis += b.amount * (double)b.costbasis;  
                // costbasis += b.amount * (b.costbasis / PRICES_POINTFACTOR);  // prevent int64 overflow (but we have underflow for 1/BTC)
                // std::cerr << "PricesInfo() acc dcostbasis=" << dcostbasis << " b.amount=" << b.amount << " b.costbasis/PRICES_POINTFACTOR=" << (b.costbasis / PRICES_POINTFACTOR) << std::endl;
                //std::cerr << "PricesInfo() acc dcostbasis=" << dcostbasis << " b.amount=" << b.amount << " b.costbasis/PRICES_POINTFACTOR=" << (b.costbasis / PRICES_POINTFACTOR) << std::endl;
                mpz_set_ui(mpzProduct, b.costbasis);
                mpz_mul_ui(mpzProduct, mpzProduct, (uint64_t)b.positionsize);         // b.costbasis * b.amount
                mpz_add(mpzTotalcostbasis, mpzTotalcostbasis, mpzProduct);      //averageCostbasis += b.costbasis * b.amount;

                mpz_add_ui(mpzTotalbets, mpzTotalbets, (uint64_t)b.positionsize);     //totalbets += b.amount;
                mpz_add(mpzTotalprofits, mpzTotalprofits, mpzProfits);          //totalprofits += b.profits;

                totalbets += b.positionsize;
                totalprofits += b.profits;

                mpz_clear(mpzProduct);
                mpz_clear(mpzProfits);
            }

            betinfo.equity = totalbets + totalprofits;
            //int64_t averageCostbasis = 0;

            if (mpz_get_ui(mpzTotalbets) != 0) { //prevent zero div
                mpz_t mpzAverageCostbasis;
                mpz_init(mpzAverageCostbasis);

                //averageCostbasis =  totalcostbasis / totalbets; 
                mpz_mul_ui(mpzTotalcostbasis, mpzTotalcostbasis, SATOSHIDEN);                 // profits *= SATOSHIDEN normalization to prevent loss of significance while division
                mpz_tdiv_q(mpzAverageCostbasis, mpzTotalcostbasis, mpzTotalbets);

                mpz_tdiv_q_ui(mpzAverageCostbasis, mpzAverageCostbasis, SATOSHIDEN);          // profits /= SATOSHIDEN de-normalization

                betinfo.averageCostbasis = mpz_get_ui(mpzAverageCostbasis);
                mpz_clear(mpzAverageCostbasis);
            }

            betinfo.liquidationprice = 0;
            if (betinfo.leverage != 0) {// prevent zero div
                betinfo.liquidationprice = betinfo.averageCostbasis - betinfo.averageCostbasis / betinfo.leverage;
            }

            if (betinfo.equity >= 0)
                betinfo.isRekt = false;
            else
            {
                betinfo.isRekt = true;
                betinfo.rektfee = totalbets / 500;
            }

            mpz_clear(mpzTotalbets);
            mpz_clear(mpzTotalprofits);
            mpz_clear(mpzTotalcostbasis);
            return 0;
        }
        return -3;
    }
    return (-1);
}

// pricesrekt rpc: anyone can rekt a bet at some block where losses reached limit, collecting fee
UniValue PricesRekt(int64_t txfee, uint256 bettxid, int32_t rektheight)
{
    int32_t nextheight = komodo_nextheight();
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), nextheight); UniValue result(UniValue::VOBJ);
    struct CCcontract_info *cp, C; 
    CTransaction bettx; 
/*    uint256 hashBlock, tokenid, batontxid; 
    int64_t firstprice, lastprice = 0, positionsize; 
    int32_t firstheight; 
    int16_t leverage; 
    std::vector<uint16_t> vec; */
    int64_t myfee = 0;
    CPubKey pk, mypk, pricespk; 
    std::string rawtx;
    char destaddr[64];

    cp = CCinit(&C, EVAL_PRICES);
    if (txfee == 0)     // TODO: what did we want tot do with txfee in prices?
        txfee = PRICES_TXFEE;
    mypk = pubkey2pk(Mypubkey());
    pricespk = GetUnspendable(cp, 0);
    GetCCaddress(cp, destaddr, pricespk);

    BetInfo betinfo;
    int32_t retcode = prices_getbetinfo(bettxid, betinfo);
    if (retcode < 0) {
        if (retcode == -1) {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "cant find bettxid or incorrect"));
        }
        else if (retcode == -2) {
            throw std::runtime_error("tx still in mempool");
        }
        else if (retcode == -3)
        {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "cant decode opret"));
            return(result);
        }
        else if (retcode == -4) {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "error scanning chain"));
        }
        return(result);
    }

    int64_t totalbets = 0;
    int64_t totalprofits = 0;

    for (auto b : betinfo.bets) {
        totalbets += b.positionsize;
        totalprofits += b.profits;
    }


    if (!betinfo.isOpen) {
        result.push_back(Pair("result", "error"));
        result.push_back(Pair("error", "position closed"));
        return result;
    }

    prices_betjson(result, betinfo.bets, betinfo.leverage, betinfo.lastheight, betinfo.lastprice); // fill output 
    if (betinfo.isRekt)
    {
        myfee = betinfo.rektfee; // consolation fee for loss
    }
    if (myfee != 0)
    {
        int64_t CCchange = 0, inputsum;

        mtx.vin.push_back(CTxIn(bettxid, NVOUT_CCMARKER, CScript()));  // spend cc marker
        if ((inputsum = AddPricesInputs(cp, mtx, destaddr, myfee + txfee, 64)) > myfee + txfee)  // TODO: why do we take txfee from global addr and not from user's addr?
            CCchange = (inputsum - myfee);
        mtx.vout.push_back(CTxOut(myfee, CScript() << ParseHex(HexStr(mypk)) << OP_CHECKSIG));
        if (CCchange >= txfee)
            mtx.vout.push_back(MakeCC1vout(cp->evalcode, CCchange, pricespk));

        /// mtx.vout.push_back(MakeCC1vout(cp->evalcode, bettx.vout[2].nValue - myfee - txfee, pricespk));  // change
        rawtx = FinalizeCCTx(0, cp, mtx, mypk, txfee, prices_finalopret(bettxid, totalprofits, rektheight, mypk, betinfo.firstprice, 0, totalbets /*- positionsize*/, 0/*positionsize*/, betinfo.leverage));
        return(prices_rawtxresult(result, rawtx, 0));
    }
    else
    {
        result.push_back(Pair("result", "error"));
        result.push_back(Pair("error", "position not rekt"));
        return(result);
    }
}

// pricescashout rpc impl: bettor can cashout hit bet if it is not rekt
UniValue PricesCashout(int64_t txfee, uint256 bettxid)
{
    int32_t nextheight = komodo_nextheight();
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), nextheight); 
    UniValue result(UniValue::VOBJ);
    struct CCcontract_info *cp, C; char destaddr[64]; 
/*    CTransaction bettx; 
    uint256 hashBlock, batontxid, tokenid; 
    int64_t positionsize, firstprice, lastprice = 0; 
    int32_t firstheight; 
    int16_t leverage; 
    std::vector<uint16_t> vec;*/
    int64_t CCchange = 0, inputsum;
    CPubKey pk, mypk, pricespk; 
    std::string rawtx;

    cp = CCinit(&C, EVAL_PRICES);
    if (txfee == 0)
        txfee = PRICES_TXFEE;

    mypk = pubkey2pk(Mypubkey());
    pricespk = GetUnspendable(cp, 0);
    GetCCaddress(cp, destaddr, pricespk);

    BetInfo betinfo;
    int32_t retcode = prices_getbetinfo(bettxid, betinfo);
    if (retcode < 0) {
        if (retcode == -1) {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "cant find bettxid or incorrect"));
        }
        else if (retcode == -2) {
            throw std::runtime_error("tx still in mempool");
        }
        else if (retcode == -3)
        {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "cant decode opret"));
            return(result);
        }
        else if (retcode == -4) {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "error scanning chain"));
        }
        return(result);
    }

    int64_t totalbets = 0;
    int64_t totalprofits = 0;

    for (auto b : betinfo.bets) {
        totalbets += b.positionsize;
        totalprofits += b.profits;
    }
    

    if (!betinfo.isOpen) {
        result.push_back(Pair("result", "error"));
        result.push_back(Pair("error", "position closed"));
        return result;
    }

    prices_betjson(result, betinfo.bets, betinfo.leverage, betinfo.lastheight, betinfo.lastprice); // fill output json

    if (betinfo.isRekt)
    {
        result.push_back(Pair("result", "error"));
        result.push_back(Pair("error", "position rekt"));
        return(result);
    }

    mtx.vin.push_back(CTxIn(bettxid, NVOUT_CCMARKER, CScript()));  // spend cc marker
    if ((inputsum = AddPricesInputs(cp, mtx, destaddr, betinfo.equity + txfee, 64)) > betinfo.equity + txfee)
        CCchange = (inputsum - betinfo.equity);
    mtx.vout.push_back(CTxOut(betinfo.equity, CScript() << ParseHex(HexStr(mypk)) << OP_CHECKSIG));
    if (CCchange >= txfee)
        mtx.vout.push_back(MakeCC1vout(cp->evalcode, CCchange, pricespk));
    // TODO: what should the opret param be:
    rawtx = FinalizeCCTx(0, cp, mtx, mypk, txfee, prices_finalopret(bettxid, totalprofits, nextheight - 1, mypk, betinfo.firstprice, 0, totalbets/*- betinfo.positionsize*/, 0/*betinfo.positionsize*/, betinfo.leverage));
    return(prices_rawtxresult(result, rawtx, 0));
        
}




// pricesinfo rpc impl
UniValue PricesInfo(uint256 bettxid, int32_t refheight)
{
    UniValue result(UniValue::VOBJ);
/*    CTransaction bettx;
    uint256 hashBlock, batontxid, tokenid;
    int64_t positionsize = 0, firstprice = 0, lastprice = 0;
    int32_t firstheight = 0, endheight;
    int16_t leverage = 0;
    std::vector<uint16_t> vec;
    CPubKey pk, mypk, pricespk;
    std::string rawtx; */

    BetInfo betinfo;
    int32_t retcode = prices_getbetinfo(bettxid, betinfo);
    if (retcode < 0) {
        if( retcode == -1 ) {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "cant find bettxid or incorrect"));
        }
        else if (retcode == -2) {
            throw std::runtime_error("tx still in mempool");
        }
        else if (retcode == -3)
        {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "cant decode opret"));
            return(result);
        }
        else if (retcode == -4) {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "error scanning chain"));
        }
        return(result);
    }

    if (!betinfo.isRekt)
        result.push_back(Pair("rekt", 0));
    else
    {
        result.push_back(Pair("rekt", (int64_t)1));
        result.push_back(Pair("rektfee", betinfo.rektfee));
        result.push_back(Pair("rektheight", betinfo.lastheight));
    }

    std::string expr = prices_getsourceexpression(betinfo.parsed);
    result.push_back(Pair("expression", expr));
    result.push_back(Pair("reduced", prices_getreducedexpr(expr)));
//            result.push_back(Pair("batontxid", batontxid.GetHex()));
    result.push_back(Pair("costbasis", ValueFromAmount(betinfo.averageCostbasis)));
#ifdef TESTMODE
    result.push_back(Pair("costbasis_test_period", 7));
#endif

    prices_betjson(result, betinfo.bets, betinfo.leverage, betinfo.lastheight, betinfo.lastprice);

    result.push_back(Pair("LiquidationPrice", ValueFromAmount(betinfo.liquidationprice)));

    return(result);
}

// priceslist rpc impl
UniValue PricesList(uint32_t filter, CPubKey mypk)
{
    UniValue result(UniValue::VARR); 
    std::vector<std::pair<CAddressIndexKey, CAmount> > addressIndex, addressIndexCC;
    struct CCcontract_info *cp, C;
  
    cp = CCinit(&C, EVAL_PRICES);
    //pricespk = GetUnspendable(cp, 0);

    // filters and outputs prices bet txid
    auto AddBetToList = [&](uint256 txid)
    {
        int64_t amount, firstprice; 
        int32_t height; 
        int16_t leverage; 
        uint256 hashBlock, tokenid;
        CPubKey pk, pricespk;
        std::vector<uint16_t> vec;
        CTransaction vintx;

        if (GetTransaction(txid, vintx, hashBlock, false) != 0)
        {
            bool bAppend = false;
            if (vintx.vout.size() > 0 && prices_betopretdecode(vintx.vout.back().scriptPubKey, pk, height, amount, leverage, firstprice, vec, tokenid) == 'B' &&
                (mypk == CPubKey() || mypk == pk))  // if only mypubkey to list
            {
                if (filter == 0)
                    bAppend = true;
                else {
                    int32_t vini;
                    int32_t height;
                    uint256 finaltxid;

                    int32_t spent = CCgetspenttxid(finaltxid, vini, height, txid, NVOUT_CCMARKER);
                    if (filter == 1 && spent < 0 ||  // open positions
                        filter == 2 && spent == 0)   // closed positions
                        bAppend = true;
                }
                if (bAppend)
                    result.push_back(txid.GetHex());
            }
            std::cerr << "PricesList() " << " bettxid=" << txid.GetHex() << " mypk=" << HexStr(mypk) << " opretpk=" << HexStr(pk) << " filter=" << filter << " bAppend=" << bAppend <<  std::endl;
        }
    };


    SetCCtxids(addressIndex, cp->normaladdr, false);        // old normal marker
    for (std::vector<std::pair<CAddressIndexKey, CAmount> >::const_iterator it = addressIndex.begin(); it != addressIndex.end(); it++)
    {
        if( it->first.txindex == NVOUT_NORMALMARKER )
            AddBetToList(it->first.txhash);
    }

    /* for future when switch to cc marker only
    SetCCtxids(addressIndexCC, cp->unspendableCCaddr, true);  // cc marker
    for (std::vector<std::pair<CAddressIndexKey, CAmount> >::const_iterator it = addressIndexCC.begin(); it != addressIndexCC.end(); it++)
    {
        priceslist(it, 1);
    }
    */
    return(result);
}


void prices_addbookentry(uint256 txid)
{
    BetInfo betinfo;
    //if( prices_getbetinfo(txid, betinfo) == 0 )
}

// walk through uxtos on the global address
// calculate the balance:
// + rekt positions
// = opposite positions
// - unbalanced positions
UniValue PricesGetOrderbook()
{
    UniValue result(UniValue::VARR);
    std::vector<std::pair<CAddressIndexKey, CAmount> > addressIndex, addressIndexCC;
    struct CCcontract_info *cp, C;

    cp = CCinit(&C, EVAL_PRICES);

    SetCCtxids(addressIndex, cp->normaladdr, false);        // old normal marker
    for (std::vector<std::pair<CAddressIndexKey, CAmount> >::const_iterator it = addressIndex.begin(); it != addressIndex.end(); it++)
    {
        if (it->first.txindex == NVOUT_NORMALMARKER)
            prices_addbookentry(it->first.txhash);
    }
    return result;
}
