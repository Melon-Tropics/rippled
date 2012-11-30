
#include "Application.h"
#include "Config.h"
#include "PeerDoor.h"
#include "RPCDoor.h"
#include "BitcoinUtil.h"
#include "key.h"
#include "utils.h"
#include "TaggedCache.h"
#include "Log.h"

#include "../database/SqliteDatabase.h"

#include <iostream>

#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>

SETUP_LOG();
LogPartition TaggedCachePartition("TaggedCache");
Application* theApp = NULL;

DatabaseCon::DatabaseCon(const std::string& strName, const char *initStrings[], int initCount)
{
	boost::filesystem::path	pPath	= theConfig.DATA_DIR / strName;

	mDatabase = new SqliteDatabase(pPath.string().c_str());
	mDatabase->connect();
	for(int i = 0; i < initCount; ++i)
		mDatabase->executeSQL(initStrings[i], true);
}

DatabaseCon::~DatabaseCon()
{
	mDatabase->disconnect();
	delete mDatabase;
}

Application::Application() :
	mIOWork(mIOService), mAuxWork(mAuxService), mUNL(mIOService), mNetOps(mIOService, &mLedgerMaster),
	mTempNodeCache("NodeCache", 16384, 90), mHashedObjectStore(16384, 300),
	mSNTPClient(mAuxService), mRPCHandler(&mNetOps), 
	mRpcDB(NULL), mTxnDB(NULL), mLedgerDB(NULL), mWalletDB(NULL), mHashNodeDB(NULL), mNetNodeDB(NULL),
	mConnectionPool(mIOService), mPeerDoor(NULL), mRPCDoor(NULL), mWSPublicDoor(NULL), mWSPrivateDoor(NULL),
	mSweepTimer(mAuxService)
{
	RAND_bytes(mNonce256.begin(), mNonce256.size());
	RAND_bytes(reinterpret_cast<unsigned char *>(&mNonceST), sizeof(mNonceST));
	mJobQueue.setThreadCount();
	mSweepTimer.expires_from_now(boost::posix_time::seconds(60));
	mSweepTimer.async_wait(boost::bind(&Application::sweep, this));
}

extern const char *RpcDBInit[], *TxnDBInit[], *LedgerDBInit[], *WalletDBInit[], *HashNodeDBInit[], *NetNodeDBInit[];
extern int RpcDBCount, TxnDBCount, LedgerDBCount, WalletDBCount, HashNodeDBCount, NetNodeDBCount;

void Application::stop()
{
	mIOService.stop();
	mJobQueue.shutdown();
	mHashedObjectStore.bulkWrite();
	mValidations.flush();
	mAuxService.stop();

	cLog(lsINFO) << "Stopped: " << mIOService.stopped();
}

static void InitDB(DatabaseCon** dbCon, const char *fileName, const char *dbInit[], int dbCount)
{
	*dbCon = new DatabaseCon(fileName, dbInit, dbCount);
}

volatile bool doShutdown = false;

#ifdef SIGINT
void sigIntHandler(int)
{
	doShutdown = true;
}
#endif

void Application::run()
{
#ifndef WIN32
#ifdef SIGINT
	if (!config.RUN_STANDALONE)
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = sigIntHandler;
		sigaction(SIGINT, &sa, NULL);
	}
#endif
#endif

	assert(mTxnDB == NULL);
	if (!theConfig.DEBUG_LOGFILE.empty())
	{ // Let DEBUG messages go to the file but only WARNING or higher to regular output (unless verbose)
		Log::setLogFile(theConfig.DEBUG_LOGFILE);
		if (Log::getMinSeverity() > lsDEBUG)
			LogPartition::setSeverity(lsDEBUG);
	}

	boost::thread auxThread(boost::bind(&boost::asio::io_service::run, &mAuxService));
	auxThread.detach();


	if (!theConfig.RUN_STANDALONE)
		mSNTPClient.init(theConfig.SNTP_SERVERS);

	//
	// Construct databases.
	//
	boost::thread t1(boost::bind(&InitDB, &mRpcDB, "rpc.db", RpcDBInit, RpcDBCount));
	boost::thread t2(boost::bind(&InitDB, &mTxnDB, "transaction.db", TxnDBInit, TxnDBCount));
	boost::thread t3(boost::bind(&InitDB, &mLedgerDB, "ledger.db", LedgerDBInit, LedgerDBCount));
	boost::thread t4(boost::bind(&InitDB, &mWalletDB, "wallet.db", WalletDBInit, WalletDBCount));
	boost::thread t5(boost::bind(&InitDB, &mHashNodeDB, "hashnode.db", HashNodeDBInit, HashNodeDBCount));
	boost::thread t6(boost::bind(&InitDB, &mNetNodeDB, "netnode.db", NetNodeDBInit, NetNodeDBCount));
	t1.join(); t2.join(); t3.join(); t4.join(); t5.join(); t6.join();

	if (theConfig.START_UP == Config::FRESH)
	{
		cLog(lsINFO) << "Starting new Ledger";
		startNewLedger();
	}
	else if (theConfig.START_UP == Config::LOAD)
	{
		cLog(lsINFO) << "Loading Old Ledger";
		loadOldLedger();
	}
	else if (theConfig.START_UP == Config::NETWORK)
	{ // This should probably become the default once we have a stable network
		if (!theConfig.RUN_STANDALONE)
			mNetOps.needNetworkLedger();
		startNewLedger();
	}
	else
		startNewLedger();

	//
	// Begin validation and ip maintenance.
	// - Wallet maintains local information: including identity and network connection persistence information.
	//
	mWallet.start();

	//
	// Set up UNL.
	//
	if (!theConfig.RUN_STANDALONE)
		getUNL().nodeBootstrap();


	//
	// Allow peer connections.
	//
	if (!theConfig.RUN_STANDALONE && !theConfig.PEER_IP.empty() && theConfig.PEER_PORT)
	{
		mPeerDoor = new PeerDoor(mIOService);
	}
	else
	{
		cLog(lsINFO) << "Peer interface: disabled";
	}

	//
	// Allow RPC connections.
	//
	if (!theConfig.RPC_IP.empty() && theConfig.RPC_PORT)
	{
		mRPCDoor = new RPCDoor(mIOService);
	}
	else
	{
		cLog(lsINFO) << "RPC interface: disabled";
	}

	//
	// Allow private WS connections.
	//
	if (!theConfig.WEBSOCKET_IP.empty() && theConfig.WEBSOCKET_PORT)
	{
		mWSPrivateDoor	= WSDoor::createWSDoor(theConfig.WEBSOCKET_IP, theConfig.WEBSOCKET_PORT, false);
	}
	else
	{
		cLog(lsINFO) << "WS private interface: disabled";
	}

	//
	// Allow public WS connections.
	//
	if (!theConfig.WEBSOCKET_PUBLIC_IP.empty() && theConfig.WEBSOCKET_PUBLIC_PORT)
	{
		mWSPublicDoor	= WSDoor::createWSDoor(theConfig.WEBSOCKET_PUBLIC_IP, theConfig.WEBSOCKET_PUBLIC_PORT, true);
	}
	else
	{
		cLog(lsINFO) << "WS public interface: disabled";
	}

	//
	// Begin connecting to network.
	//
	if (!theConfig.RUN_STANDALONE)
		mConnectionPool.start();


	if (theConfig.RUN_STANDALONE)
	{
		cLog(lsWARNING) << "Running in standalone mode";
		mNetOps.setStandAlone();
	}
	else
		mNetOps.setStateTimer();

	mIOService.run(); // This blocks

	if (mWSPublicDoor)
		mWSPublicDoor->stop();

	if (mWSPrivateDoor)
		mWSPrivateDoor->stop();

	cLog(lsINFO) << "Done.";
}

void Application::sweep()
{
	mMasterTransaction.sweep();
	mHashedObjectStore.sweep();
	mLedgerMaster.sweep();
	mTempNodeCache.sweep();
	mValidations.sweep();
	mSweepTimer.expires_from_now(boost::posix_time::seconds(60));
	mSweepTimer.async_wait(boost::bind(&Application::sweep, this));
}

Application::~Application()
{
	delete mTxnDB;
	delete mLedgerDB;
	delete mWalletDB;
	delete mHashNodeDB;
	delete mNetNodeDB;
}

void Application::startNewLedger()
{
	// New stuff.
	RippleAddress	rootSeedMaster		= RippleAddress::createSeedGeneric("masterpassphrase");
	RippleAddress	rootGeneratorMaster	= RippleAddress::createGeneratorPublic(rootSeedMaster);
	RippleAddress	rootAddress			= RippleAddress::createAccountPublic(rootGeneratorMaster, 0);

	// Print enough information to be able to claim root account.
	cLog(lsINFO) << "Root master seed: " << rootSeedMaster.humanSeed();
	cLog(lsINFO) << "Root account: " << rootAddress.humanAccountID();

	{
		Ledger::pointer firstLedger = boost::make_shared<Ledger>(rootAddress, SYSTEM_CURRENCY_START);
		assert(!!firstLedger->getAccountState(rootAddress));
		firstLedger->updateHash();
		firstLedger->setClosed();
		firstLedger->setAccepted();
		mLedgerMaster.pushLedger(firstLedger);

		Ledger::pointer secondLedger = boost::make_shared<Ledger>(true, boost::ref(*firstLedger));
		secondLedger->setClosed();
		secondLedger->setAccepted();
		mLedgerMaster.pushLedger(secondLedger, boost::make_shared<Ledger>(true, boost::ref(*secondLedger)), false);
		assert(!!secondLedger->getAccountState(rootAddress));
		mNetOps.setLastCloseTime(secondLedger->getCloseTimeNC());
	}
}

void Application::loadOldLedger()
{
	try
	{
		Ledger::pointer lastLedger = Ledger::getLastFullLedger();

		if (!lastLedger)
		{
			std::cout << "No Ledger found?" << std::endl;
			exit(-1);
		}
		lastLedger->setClosed();

		cLog(lsINFO) << "Loading ledger " << lastLedger->getHash() << " seq:" << lastLedger->getLedgerSeq();

		if (lastLedger->getAccountHash().isZero())
		{
			cLog(lsFATAL) << "Ledger is empty.";
			assert(false);
			exit(-1);
		}

		if (!lastLedger->walkLedger())
		{
			cLog(lsFATAL) << "Ledger is missing nodes.";
			exit(-1);
		}

		if (!lastLedger->assertSane())
		{
			cLog(lsFATAL) << "Ledger is not sane.";
			exit(-1);
		}
		mLedgerMaster.setLedgerRangePresent(0, lastLedger->getLedgerSeq());

		Ledger::pointer openLedger = boost::make_shared<Ledger>(false, boost::ref(*lastLedger));
		mLedgerMaster.switchLedgers(lastLedger, openLedger);
		mNetOps.setLastCloseTime(lastLedger->getCloseTimeNC());
	}
	catch (SHAMapMissingNode& mn)
	{
		cLog(lsFATAL) << "Cannot load ledger. " << mn;
		exit(-1);
	}
}
// vim:ts=4
