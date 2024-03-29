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

 /*
 The idea of Heir CC is to allow crypto inheritance.
 A special 1of2 CC address is created that is freely spendable by the creator (funds owner).
 The owner may add additional funds to this 1of2 address.
 The heir is only allowed to spend after "the specified amount of idle blocks" (changed to "the owner inactivityTime").
 The idea is that if the address doesnt spend any funds for a year (or whatever amount set), then it is time to allow the heir to spend.
 "The design requires the heir to spend all the funds at once" (this requirement was changed to "after the inactivity time both the heir and owner may freely spend available funds")
 After the first heir spending a flag is set that spending is allowed for the heir whether the owner adds more funds or spends them.
 This Heir contract supports both coins and tokens.
 */

#include "CCHeir.h"


// helper functions for tx creation or validation:

// find the latest owner transaction id
// this function also returns some values from the initial and latest transaction opreturns
// Note: this function is also called from validation code (use non-locking calls)
uint256 FindLatestOwnerTx(uint256 fundingtxid, CPubKey& ownerPubkey, CPubKey& heirPubkey, int64_t& inactivityTime, uint8_t &hasHeirSpendingBegun)
{
    uint8_t eval, funcId;

    // set inital state as the heir has not begun to spend the funds:
    hasHeirSpendingBegun = 0;

    CTransaction fundingtx;
    uint256 hashBlock;
    std::vector<uint8_t> vopret;
    std::string name;
    // get initial funding tx, check if it has an opreturn and deserialize it:
    if (!myGetTransaction(fundingtxid, fundingtx, hashBlock) ||  // NOTE: use non-locking version of GetTransaction as we may be called from validation code
        fundingtx.vout.size() < 2 ||
        !GetOpReturnData(fundingtx.vout.back().scriptPubKey, vopret) ||
        !E_UNMARSHAL(vopret, ss >> eval; ss >> funcId; ss >> ownerPubkey; ss >> heirPubkey; ss >> inactivityTime; ss >> name;) ||
        eval != EVAL_HEIR ||
        funcId != 'F') {
        std::cerr << "FindLatestOwnerTx()" << "cannot load funding tx or parse opret" << std::endl;
        return zeroid;
    }

    // init cc contract object:
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_HEIR);

    // check if pubkeys in funding tx opreturn match 1 of 2 pubkey cc vout:
    if (fundingtx.vout[0] != MakeCC1of2vout(EVAL_HEIR, fundingtx.vout[0].nValue, ownerPubkey, heirPubkey)) {
        std::cerr << "FindLatestOwnerTx()" << "funding tx vout pubkeys do not match pubkeys in the opreturn" << std::endl;
        return zeroid;
    }

    // get the address of cryptocondition '1 of 2 pubkeys':
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>> unspentOutputs;
    char coinaddr[64];
    GetCCaddress1of2(cp, coinaddr, ownerPubkey, heirPubkey);

    // get vector with uxtos for 1of2 address:
    SetCCunspents(unspentOutputs, coinaddr, true);

    int32_t maxBlockHeight = 0;
    uint256 latesttxid = fundingtxid;   // set to initial txid

    // go through uxto's to find the last funding or spending owner tx:
    for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>>::const_iterator it = unspentOutputs.begin(); it != unspentOutputs.end(); it++)
    {
        CTransaction vintx;
        uint256 blockHash;
        std::vector<uint8_t> vopret;
        uint8_t eval, funcId, flagopret;
        uint256 txidopret;

        int32_t blockHeight = (int32_t)it->second.blockHeight;

        // Get a transaction from the returned array
        // unmarshal its opret
        if (myGetTransaction(it->first.txhash, vintx, blockHash) &&     // NOTE: use non-locking version of GetTransaction as we may be called from validation code
            vintx.vout.size() > 0 &&
            GetOpReturnData(vintx.vout.back().scriptPubKey, vopret) &&
            E_UNMARSHAL(vopret, ss >> eval; ss >> funcId; ss >> txidopret; ss >> flagopret) &&
            eval == EVAL_HEIR &&
            (funcId == 'C' || funcId == 'A') &&
            // also check if this is a tx from this funding plan
            fundingtxid == txidopret) {

            // As SetCCunspents returns uxtos not in the chronological order we need to order them by the block height as we need the latest one:
            if (blockHeight > maxBlockHeight) {

                // Now check if this tx was the owner's activity:
                // use pair of cc sdk functions that walk through vin array and find if the tx was signed with the owner's pubkey:   
                if (TotalPubkeyNormalInputs(vintx, ownerPubkey) > 0 || TotalPubkeyCCInputs(vintx, ownerPubkey) > 0) {
                    // reset the lastest txid to this txid if this is owner's activity:
                    latesttxid = it->first.txhash;
                    hasHeirSpendingBegun = flagopret;
                    maxBlockHeight = blockHeight;
                }
            }
        }
    }

    std::cerr << "FindLatestOwnerTx() found latesttxid=" << latesttxid.GetHex() << std::endl;  // debug log
    return latesttxid;
}


// check that spent tx are from this heir funding plan
bool CheckSpentTxns(struct CCcontract_info* cpHeir, Eval* eval, const CTransaction &tx, uint256 heirtxid) {

    // make checks and return invalid state if a rule is broken
    for (auto vin : tx.vin) {

        // if this is Heir cc vin:
        if (cpHeir->ismyvin(vin.scriptSig)) {
            CTransaction vintx;
            uint256 hashBlock;
            std::vector <uint8_t> vinopret;

            // load the tx being spent and check if it has an opreturn and it has correct basic data (eval code and funcid)
            if (myGetTransaction(vin.prevout.hash, vintx, hashBlock) && vintx.vout.size() > 1 &&
                GetOpReturnData(tx.vout.back().scriptPubKey, vinopret) && vinopret.size() > 2 && 
                vinopret.begin()[0] == EVAL_HEIR &&
                (vinopret.begin()[1] == 'F' || vinopret.begin()[1] != 'A' || vinopret.begin()[1] != 'C')) {
                // if vintx is the initial tx then heirtxid should be equal to its txid
                if (vinopret.begin()[1] == 'F') {
                    if (vintx.GetHash() != heirtxid)
                        return eval->Invalid("incorrect funding tx spent");
                }
                // if vintx is add or claim tx the heirtxid should be equal to the txid in the vintx opret
                else  {
                    uint256 vinHeirtxid;
                    uint8_t evalcode, funcid, vinHeirSpendingBegun;

                    if (!E_UNMARSHAL(vinopret, ss >> evalcode; ss >> funcid; ss >> vinHeirtxid; ss >> vinHeirSpendingBegun;))
                        // return invalid state if unserializing function returned false
                        return eval->Invalid("incorrect vintx opreturn data");

                    // check if the tx does not spend some other instance funds
                    if (vinHeirtxid != heirtxid)
                        return eval->Invalid("incorrect vintx spent");
                }
            }
            else 
                return eval->Invalid("could not load vintx or incorrect vintx opreturn");
        }
    }
    return true;
}


// check that enough time has passed from the last owner activity
// also validate if hasHeirSpendingBegun flag is set correctly
bool CheckInactivityTime(struct CCcontract_info* cpHeir, Eval* eval, const CTransaction &tx, uint256 latesttxid, int64_t inactivityTime, CPubKey heirPubkey, uint8_t lastHeirSpendingBegun, uint8_t newHeirSpendingBegun) {

    // check if this is heir claiming funds
    for (auto vout : tx.vout) {
        // is this normal output?
        if (vout.scriptPubKey.IsPayToPublicKey())   {
            // is it to heir pubkey?
            if (vout.scriptPubKey == CScript() << ParseHex(HexStr(heirPubkey)) << OP_CHECKSIG) {
                // check inactivity time
                int32_t numBlocks;
                if (lastHeirSpendingBegun || CCduration(numBlocks, latesttxid) > inactivityTime) {
                    std::cerr << "CheckInactivityTime() " << "inactivity time passed, newHeirSpendingBegun="<< (int)newHeirSpendingBegun << std::endl;
                    if (newHeirSpendingBegun != 1)
                        return eval->Invalid("heir spending flag incorrect, must be 1");
                    else
                        return true;
                }
                else
                {
                    std::cerr << "CheckInactivityTime() " << "inactivity time not passed yet, newHeirSpendingBegun=" << (int)newHeirSpendingBegun << std::endl;
                    if (newHeirSpendingBegun != 0)
                        return eval->Invalid("heir spending flag incorrect, must be 0");
                    else
                        return true;
                }
            }
            else
            {
                std::cerr << "CheckInactivityTime() " << "owner spends, no need to check inactivity time, lastHeirSpendingBegun=" << (int)lastHeirSpendingBegun << ", newHeirSpendingBegun = " << (int)newHeirSpendingBegun  << std::endl;
                if (newHeirSpendingBegun != lastHeirSpendingBegun)
                    return eval->Invalid("heir spending flag incorrect");
                else
                    return true;
            }
        }
    }
    return eval->Invalid("no normal outputs found"); // if no any normal output not found then tx is incorrect
}

// Tx validation entry function, it is a callback actually
// params: 
// cpHeir pointer to contract variable structure
// eval pointer to cc dispatching object, used to return invalid state
// tx is the tx itself
// nIn not used in validation code
bool HeirValidate(struct CCcontract_info* cpHeir, Eval* eval, const CTransaction& tx, uint32_t nIn)
{
    // let's check basic tx structure, that is, has opreturn with correct basic evalcode and funcid
    // Note: we do not check for 'F' or 'A' funcids because we never get into validation code for the initial tx or for an add tx as they have no heir cc vins ever
    std::vector <uint8_t> vopret;
    if( tx.vout.size() < 1 || !GetOpReturnData(tx.vout.back().scriptPubKey, vopret) || vopret.size() < 2 || vopret.begin()[0] != EVAL_HEIR || 
        vopret.begin()[1] != 'C')
        // interrupt the validation and return invalid state:
        return eval->Invalid("incorrect or no opreturn data");  // note that you should not return simply 'false'

    uint8_t evalcode, funcId;

    // let's try to decode opreturn:
    // fundingtxid is this contract instance id (the initial tx id)
    uint256 fundingtxid; //initialized to null
    uint8_t hasHeirSpendingBegun;
    if (!E_UNMARSHAL(vopret, ss >> evalcode; ss >> funcId; ss >> fundingtxid; ss >> hasHeirSpendingBegun;))
        // return invalid state if unserializing function returned false
        return eval->Invalid("incorrect opreturn data");

    // important to check if fundingtxid parsed is okay:
    if( fundingtxid.IsNull() )
        return eval->Invalid("incorrect funding plan id in tx opret");

    CPubKey ownerPubkey, heirPubkey;
    int64_t inactivityTimeSec;
    uint8_t lastHeirSpendingBegun;

    // it is good place to load the initial tx and check if it exist and has correct opretun
    // we are callinng FindLatestOwnerTx function to obtain opreturn parameters and hasHeirSpendingBegun flag,
    // and this function also checks the initial tx:
    uint256 latesttxid = FindLatestOwnerTx(fundingtxid, ownerPubkey, heirPubkey, inactivityTimeSec, lastHeirSpendingBegun);
    if (latesttxid.IsNull()) {
        return eval->Invalid("no or incorrect funding tx found");
    }

    // just log we are in the validation code:
    std::cerr << "HeirValidate funcid=" << (char)funcId << " evalcode=" << (int)cpHeir->evalcode << std::endl;

    // specific funcId validation:
    switch (funcId) {
    case 'F':
    case 'A':
        // return invalid as we never could get here for the initial or add funding tx:
        return eval->Invalid("unexpected HeirValidate for heirfund");

    case 'C':
        // check if correct funding txns are being spent, 
        // like they belong to this contract instance
        if (!CheckSpentTxns(cpHeir, eval, tx, fundingtxid))
            return false;

        // if it is heir claiming the funds check if he is allowed
        // also check if the new flag is set correctly
        if (!CheckInactivityTime(cpHeir, eval, tx, latesttxid, inactivityTimeSec, heirPubkey, lastHeirSpendingBegun, hasHeirSpendingBegun) )
            return false;
        break;

    default:
        std::cerr << "HeirValidate() illegal heir funcid=" << (char)funcId << std::endl;
        return eval->Invalid("unexpected HeirValidate funcid");
    }
    return eval->Valid();   
}
// end of consensus code

// add inputs from fund cc threshold=2 aka 1 of 2 cryptocondition address to transaction object
int64_t Add1of2AddressInputs(CMutableTransaction &mtx, uint256 fundingtxid, char *coinaddr, int64_t amount, int32_t maxinputs)
{
    int64_t totalinputs = 0L;
    int32_t count = 0;

    // Call the cc sdk function SetCCunspents that fills the provider vector with a list of unspent outputs for the provider bitcoin address coinaddr(actually we passed here the 1of2 address where fund is stored)
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>> unspentOutputs;
    SetCCunspents(unspentOutputs, coinaddr, true);  // get a vector of uxtos for the address in coinaddr[]

    // Go through the returned uxtos and add appropriate ones to the transaction's vin array:
    for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>>::const_iterator it = unspentOutputs.begin(); it != unspentOutputs.end(); it++) {
        CTransaction tx;
        uint256 hashBlock;
        std::vector<uint8_t> vopret;

        // Load the current uxto's transaction and check if it has a correct opreturn in the back of array of outputs: 
        if (GetTransaction(it->first.txhash, tx, hashBlock, false) && tx.vout.size() > 0 && GetOpReturnData(tx.vout.back().scriptPubKey, vopret) && vopret.size() > 2)
        {
            uint8_t evalCode, funcId, hasHeirSpendingBegun;
            uint256 txid;

            if (it->first.txhash == fundingtxid ||   // check if this tx is from our contract instance 
                E_UNMARSHAL(vopret, { ss >> evalCode; ss >> funcId; ss >> txid >> hasHeirSpendingBegun; }) && // unserialize opreturn
                evalCode == EVAL_HEIR &&
                fundingtxid == txid) // it is a tx from this funding plan
            {
                // Add the uxto to the transaction's vins, that is, set the txid of the transaction and vout number providing the uxto. 
                // Pass empty CScript() to scriptSig param, it will be filled by FinalizeCCtx:
                mtx.vin.push_back(CTxIn(it->first.txhash, it->first.index, CScript()));
                totalinputs += it->second.satoshis;

                // stop if sufficient inputs found
                // if amount == 0 that would mean to add all available funds to calculate total
                if (amount > 0 && totalinputs >= amount || ++count > maxinputs)
                    break;
            }
        }
    }
    // Return the total inputs' amount which has been added:
    return totalinputs;
}

// heirfund transaction creation code
std::string HeirFund(int64_t amount, std::string heirName, CPubKey heirPubkey, int64_t inactivityTimeSec)
{
    // First, we need to create a mutable version of a transaction object.
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight());

    // Declare and initialize an CCcontract_info object with heir cc contract variables like cc global address, global private key etc.
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_HEIR);

    // Next we need to add some inputs to transaction that are enough to make deposit of the requested amount to the heir fund, some fee for the marker and for miners
    // Let's use a constant fee = 10000 sat.
    // We need the pubkey from the komodod - pubkey param.
    // For adding normal inputs to the mutable transaction there is a corresponding function in the cc SDK.
    const int64_t txfee = 10000;
    CPubKey myPubkey = pubkey2pk(Mypubkey());
    if (AddNormalinputs(mtx, myPubkey, amount + 2 * txfee, 60) > 0) {
        // The parameters passed to the AddNormalinputs() are the tx itself, my pubkey, total value for the funding amount, marker and miners fee, for which the function will add the necessary number of uxto from the user's wallet. The last parameter is the limit of uxto to add. 

        // Now let's add outputs to the transaction. Accordingly to our specification we need two outputs: for the funding deposit and marker
        mtx.vout.push_back(MakeCC1of2vout(EVAL_HEIR, amount, myPubkey, heirPubkey));
        mtx.vout.push_back(MakeCC1vout(EVAL_HEIR, txfee, GetUnspendable(cp, NULL)));   // this creates a 'marker' for the cc heir initial tx. See HeirList for its usage
        // In this example we used two cc sdk functions for creating cryptocondition vouts.
        // MakeCC1of2vout creates a vout with a threshold = 2 cryptocondition allowing to spend funds from this vout with 
        // either myPubkey(which would be the pubkey of the funds owner) or heir pubkey.
        // MakeCC1vout creates a vout with a simple cryptocondition which sends a txfee to cc Heir contract global address(returned by GetUnspendable() function call).
        // We need this output to be able to find all the created heir funding plans.
        // You will always need some kind of marker for any cc contract at least for the initial transaction, otherwise you might lose contract's data in blockchain.
        // We may call this as a 'marker pattern' in cc development.See more about the marker pattern later in the CC contract patterns section.

        // Finishing the creation of the transaction by calling FinalizeCCTx with params of the mtx object itself, the owner pubkey, txfee amount. 
        // Also an opreturn object with the contract data is passed which is created by serializing the needed ids and variables to a CScript object.
        // Note the cast to uint8_t for the constants EVAL_HEIR and 'F' funcid, this is important as it is supposed one-byte size for serialization of these values (otherwise they would be 'int').
        return FinalizeCCTx(0, cp, mtx, myPubkey, txfee, CScript() << OP_RETURN << E_MARSHAL(ss << (uint8_t)EVAL_HEIR << (uint8_t)'F' << myPubkey << heirPubkey << inactivityTimeSec << heirName));
    }
    CCerror = "not enough coins for requested amount and txfee";
    return std::string("");
}

// heiradd rpc transaction creation
std::string HeirAdd(uint256 fundingtxid, int64_t amount)
{
    // Start with creating a mutable transaction object:
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight());

    if (fundingtxid.IsNull()) {
        CCerror = "incorrect funding tx";
        return std::string("");
    }

    // Init the cc contract object :
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_HEIR);

    CPubKey ownerPubkey, heirPubkey;
    int64_t inactivityTimeSec;
    uint8_t hasHeirSpendingBegun = 0;

    // call FindLatestOwnerTx to obtain hasHeirSpendingBegun flag value:
    uint256 latesttxid = FindLatestOwnerTx(fundingtxid, ownerPubkey, heirPubkey, inactivityTimeSec, hasHeirSpendingBegun);
    if (latesttxid.IsNull()) {
        CCerror = "no funding tx found";
        return std::string("");
    }

    const int64_t txfee = 10000;
    CPubKey myPubkey = pubkey2pk(Mypubkey());
    // The parameters passed to the AddNormalinputs() are the tx itself, my pubkey, total value for the funding amount, marker and miners fee, for which the function will add the necessary number of uxto from the user's wallet. The last parameter is the limit of uxto to add. 
    if (AddNormalinputs(mtx, myPubkey, amount + txfee, 60) > 0) {

        // Now let's add outputs to the transaction. Accordingly to our specification we need two outputs: for the funding deposit and marker
        mtx.vout.push_back(MakeCC1of2vout(EVAL_HEIR, amount, ownerPubkey, heirPubkey));

        // Add normal change if any, add opreturn data and sign the transaction:
        return FinalizeCCTx(0, cp, mtx, myPubkey, txfee, CScript() << OP_RETURN << E_MARSHAL(ss << (uint8_t)EVAL_HEIR << (uint8_t)'A' << fundingtxid << hasHeirSpendingBegun));
        // in the opreturn we added a pair of standard ids: cc eval code and functional id, the fundingtxid as the funding plan identifier and also passed further the hasHeirSpendingBegun flag
    }
    CCerror = "insufficient coins for the requested amount and txee";
    return std::string("");
}


// heirclaim rpc transaction creation
std::string HeirClaim(uint256 fundingtxid, int64_t amount)
{
    // Start with creating a mutable transaction object:
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight());

    if (fundingtxid.IsNull()) {
        CCerror = "incorrect funding tx";
        return std::string("");
    }

    // Next, init the cc contract object :
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_HEIR);

    const int64_t txfee = 10000;
    CPubKey ownerPubkey, heirPubkey;
    int64_t inactivityTimeSec;
    uint8_t hasHeirSpendingBegun = 0;

    // Now we need to find the latest owner transaction to calculate the owner's inactivity time:
    // Use a developed helper FindLatestOwnerTx function which returns the lastest txid, heir public key and the hasHeirSpendingBegun flag value :
    uint256 latesttxid = FindLatestOwnerTx(fundingtxid, ownerPubkey, heirPubkey, inactivityTimeSec, hasHeirSpendingBegun);
    if (latesttxid.IsNull()) {
        CCerror = "no funding tx found";
        return std::string("");
    }

    // Now check if the inactivity time has passed from the last owner transaction. 
    // Use cc sdk function which returns time in seconds from the block with the txid in the params to the chain tip block:
    int32_t numBlocks; // not used
    bool isAllowedToHeir = (hasHeirSpendingBegun || CCduration(numBlocks, latesttxid) > inactivityTimeSec) ? true : false;
    CPubKey myPubkey = pubkey2pk(Mypubkey());  // pubkey2pk sdk function converts pubkey from byte array to high-level CPubKey object
    if (myPubkey == heirPubkey && !isAllowedToHeir) {
        CCerror = "spending funds is not allowed for heir yet";
        return std::string("");
    }

    // Let's create the claim transaction inputs and outputs:

    // add normal inputs for txfee amount:
    if (AddNormalinputs(mtx, myPubkey, txfee, 3) <= txfee) {
        CCerror = "not enough normal inputs for txfee";
        return std::string("");
    }

    // Add cc inputs for the requested amount.
    // first get the address of 1 of 2 cryptocondition output where the fund was deposited :
    char coinaddr[65];
    GetCCaddress1of2(cp, coinaddr, ownerPubkey, heirPubkey);

    // add inputs for this address with use of a custom function:
    int64_t inputs;
    if ((inputs = Add1of2AddressInputs(mtx, fundingtxid, coinaddr, amount, 64)) < amount) {
        CCerror = "not enough funds for the amount claimed";
        return std::string("");
    }

    // Now add an normal output to send claimed funds to and cc change output for the fund remainder:
    mtx.vout.push_back(CTxOut(amount, CScript() << ParseHex(HexStr(myPubkey)) << OP_CHECKSIG));

    // add cc change, if needed
    if (inputs > amount)
        mtx.vout.push_back(MakeCC1of2vout(EVAL_HEIR, inputs - amount, ownerPubkey, heirPubkey));

    // use cc sdk functions to get user's private key and set cc contract variables to notify that 1 of 2 cryptocondition is in use
    uint8_t mypriv[32];
    Myprivkey(mypriv);
    CCaddr1of2set(cp, ownerPubkey, heirPubkey, mypriv, coinaddr);

    // Add normal change if any, add opreturn data and sign the transaction :
    return FinalizeCCTx(0, cp, mtx, myPubkey, txfee, CScript() << OP_RETURN << E_MARSHAL(ss << (uint8_t)EVAL_HEIR << (uint8_t)'C' << fundingtxid << (myPubkey == heirPubkey ? (uint8_t)1 : hasHeirSpendingBegun)));
    // in the opreturn we added a pair of standard ids: cc eval code and functional id plus the fundingtxid as the funding plan identifier
    // We use a special flag hasHeirSpendingBegun that is turned to 1 when the heir first time spends funds. 
    // That means that it is no need further in checking the owner's inactivity time
    // Once set to 'true' this flag should be set to true in the successive transaction opreturns
}

// heirlist rpc implementation. Use marker uxtos to list all heir initial transactions
UniValue HeirList()
{
    // rpc object to return the array of initial txids
    UniValue result(UniValue::VARR);

    // init cc contract object:
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_HEIR);

    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>> unspentOutputs;
    char markeraddr[65];

    // get the address of heir global address as heir marker's address
    GetCCaddress(cp, markeraddr, GetUnspendable(cp, NULL));

    // get list of uxtos on marker's address
    SetCCunspents(unspentOutputs, markeraddr, true);

    // list all uxtos with markers
    for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>>::const_iterator it = unspentOutputs.begin(); it != unspentOutputs.end(); it++) {
        
        CTransaction fundingtx;
        uint256 hashBlock;
        const int32_t markerVoutNum = 1;
        std::vector<uint8_t> vopret;

        // load tx with unspent marker, check if its opreturn has heir eval code and function id
        if (it->first.index == markerVoutNum && GetTransaction(it->first.txhash, fundingtx, hashBlock, false) &&
            fundingtx.vout.size() > 0  &&  // vout bounds checking
            GetOpReturnData(fundingtx.vout.back().scriptPubKey, vopret) &&
            vopret.size() > 2 &&  // opreturn bounds checking
            vopret.begin()[0] == EVAL_HEIR &&
            vopret.begin()[1] == 'F')
        {
            // add txid to list
            result.push_back(it->first.txhash.GetHex());
        }
    }
    return result;
}

// heirinfo implementation returns some data about a heir plan identified by funding txid 
// which could be obtained by heirlist rpc call
UniValue HeirInfo(uint256 fundingtxid)
{
    // rpc object to return the resulting info
    UniValue result(UniValue::VOBJ);

    CTransaction fundingtx;
    uint256 hashBlock;
    std::vector<uint8_t> vopret;
    uint8_t eval, funcId;
    CPubKey ownerPubkey, heirPubkey;
    std::string name;
    int64_t inactivityTime;

    // get initial funding tx and set it as initial lasttx:
    if (GetTransaction(fundingtxid, fundingtx, hashBlock, false) && 
        fundingtx.vout.size() &&   // vout bound checking
        GetOpReturnData(fundingtx.vout.back().scriptPubKey, vopret) &&  
        E_UNMARSHAL(vopret, ss >> eval; ss >> funcId; ss >> ownerPubkey; ss >> heirPubkey; ss >> inactivityTime; ss >> name;) &&
        eval == EVAL_HEIR &&
        funcId == 'F')
    {
        // call FindLatestOwnerTx function to get hasHeirSpendingBegun flag.
        uint8_t hasHeirSpendingBegun;
        uint256 latestFundingTxid = FindLatestOwnerTx(fundingtxid, ownerPubkey, heirPubkey, inactivityTime, hasHeirSpendingBegun);

        if (latestFundingTxid == zeroid) {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "could not find latest tx"));
            return result;
        }

        // output some data about this heir cc contract instance (funding plan):

        // pubkeys:
        result.push_back(Pair("fundingtxid", fundingtxid.GetHex()));
        result.push_back(Pair("name", name));
        
        result.push_back(Pair("OwnerPubKey", HexStr(ownerPubkey)));
        result.push_back(Pair("HeirPubKey", HexStr(heirPubkey)));

        struct CCcontract_info *cp, C;
        cp = CCinit(&C, EVAL_HEIR);

        // get 1 of 2 address:
        char coinaddr[65];
        GetCCaddress1of2(cp, coinaddr, ownerPubkey, heirPubkey);

        CMutableTransaction mtx;  // dummy tx object
        // calculate total funds amount by adding all available inputs:
        int64_t inputs = Add1of2AddressInputs(mtx, fundingtxid, coinaddr, 0, 64);

        result.push_back(Pair("AvailableFund", ValueFromAmount(inputs))); // ValueFromAmount() function converts satoshis to coins representation
        result.push_back(Pair("InactivityTimeSetting", inactivityTime));
          
        uint64_t durationSec = 0;
        if (!hasHeirSpendingBegun) { // we do not need find duration if the spending already has begun
            int32_t numblocks;
            durationSec = CCduration(numblocks, latestFundingTxid);  
            // Note: when running cc heir contract on a private chain make sure there is at least one block mined after the block with the latest tx, 
            // for CCduration to return non-zero
        }

        result.push_back(Pair("HeirSpendingAllowed", (hasHeirSpendingBegun || durationSec > inactivityTime ? "true" : "false")));

        // adding owner current inactivity time:
        if (!hasHeirSpendingBegun && durationSec <= inactivityTime) {
            result.push_back(Pair("InactivityTimePassed", durationSec));
        }

        result.push_back(Pair("result", "success"));
    }
    else {
        result.push_back(Pair("result", "error"));
        result.push_back(Pair("error", "could not find heir cc plan for this txid (no initial tx)"));
    }
    return (result);
}


