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

	void SetServerLog(ErrorLogInterface *v) { server_log = v; }
	ErrorLogInterface *GetServerLog() { return server_log; }

	void SetConfig(ConfigInterface *v) { config = v; }
	ConfigInterface *GetConfig() { return config; }

	void SetDatabase(Database *v) { db = v; }
	Database *GetDatabase() { return db; }

	void SetOptions(Options *v) { options = v; }
	Options *GetOptions() { return options; }

	void SetServerManager(ServerManagerInterface *v) { sm = v; }
	ServerManagerInterface *GetServerManager() { return sm; }

	void SetClientManager(ClientManagerInterface *v) { cm = v; }
	ClientManagerInterface *GetClientManager() { return cm; }

#ifdef WIN32
	void SetEncryption(EncryptionInterface *v) { eq_crypto = v; }
	EncryptionInterface *GetEncryption() { return eq_crypto; }
#endif

private:
	bool* server_running;
	ErrorLogInterface *server_log;
	ConfigInterface *config;
	
	Database *db;
	Options *options;
	ServerManagerInterface *sm;
	ClientManagerInterface *cm;

#ifdef WIN32
	EncryptionInterface *eq_crypto;
#endif
};

#endif
