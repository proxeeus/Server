#ifndef EQEMU_LOGINSERVER_SERVICE_LOCATOR_H
#define EQEMU_LOGINSERVER_SERVICE_LOCATOR_H

#include "../common/singleton.h"

class ServiceLocator : public EQEmu::Singleton<ServiceLocator>
{
public:
	ServiceLocator() { };
	~ServiceLocator() { }
	
	void SetServerRunning(bool *v) { server_running = v; }
	bool *GetServerRunning() { return server_running; }

	void SetServerLog(ErrorLog *v) { server_log = v; }
	ErrorLog *GetServerLog() { return server_log; }

	void SetConfig(Config *v) { config = v; }
	Config *GetConfig() { return config; }

	void SetDatabase(Database *v) { db = v; }
	Database *GetDatabase() { return db; }

	void SetOptions(Options *v) { options = v; }
	Options *GetOptions() { return options; }

	void SetServerManager(ServerManager *v) { SM = v; }
	ServerManager *GetServerManager() { return SM; }

	void SetClientManager(ClientManager *v) { CM = v; }
	ClientManager *GetClientManager() { return CM; }

#ifdef WIN32
	void SetEncryption(Encryption *v) { eq_crypto = v; }
	Encryption *GetEncryption() { return eq_crypto; }
#endif

private:
	bool* server_running;
	ErrorLog *server_log;

	Config *config;
	Database *db;
	Options *options;
	ServerManager *SM;
	ClientManager *CM;

#ifdef WIN32
	Encryption *eq_crypto;
#endif
};

#endif
