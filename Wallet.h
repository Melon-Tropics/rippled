#ifndef __WALLET__
#define __WALLET__

#include "keystore.h"
#include "Serializer.h"
#include "Transaction.h"
#include <list>
#include <vector>

class NewcoinAddress;

/*
Keeps track of all the public/private keys you have created
*/
class LocalAccount
{
public:
	//CKey mKey;
	//std::string mHumanAddress;
	NewcoinAddress mAddress;
	CKey mPublicKey, mPrivateKey;
	int64 mAmount;
	uint32 mSeqNum;

	LocalAccount(bool);	// create a new local acount
	bool signRaw(Serializer::pointer);
	bool signRaw(Serializer::pointer, std::vector<unsigned char>& signature);
	bool checkSignRaw(Serializer::pointer, int signaturePosition=-1, int signedData=-1);
	CKey& peekPrivKey() { return mPrivateKey; }
	CKey& peekPubKey() { return mPublicKey; }

	uint160 getAddress(void) const { return mAddress.GetHash160(); }
};

class Wallet : public CBasicKeyStore
{
	std::list<LocalAccount> mYourAccounts;

	

	Transaction::pointer createTransaction(LocalAccount& fromAccount, uint160& destAddr, int64 amount);
	bool commitTransaction(Transaction::pointer trans);

	LocalAccount* consolidateAccountOfSize(int64 amount);

public:
	Wallet();
	void refreshAccounts();
	void load();

	int64 getBalance();

	// returns some human error str?
	std::string sendMoneyToAddress(uint160& destAddress, int64 amount);

	// you may need to update your balances
	void transactionChanged(Transaction::pointer trans);

};

#endif