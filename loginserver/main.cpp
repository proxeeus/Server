/*	EQEMu: Everquest Server Emulator
	Copyright (C) 2001-2010 EQEMu Development Team (http://eqemulator.net)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; version 2 of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY except by those people which sell it, which
	are required to give you total support for your newly bought product;
	without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE. See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/
#include "../common/debug.h"
#include "../common/types.h"
#include "../common/opcodemgr.h"
#include "../common/eq_stream_factory.h"
#include "../common/timer.h"
#include "../common/platform.h"
#include "../common/crash.h"
#include "login_server.h"
#include <time.h>
#include <stdlib.h>
#include <signal.h>
#include <string>
#include <sstream>

TimeoutManager timeout_manager;
template<> ServiceLocator* EQEmu::Singleton<ServiceLocator>::_inst = nullptr;

void CatchSignal(int sig_num)
{
	*(ServiceLocator::Get().GetServerRunning()) = false;
}

int main()
{
	RegisterExecutablePlatform(ExePlatformLogin);
	set_exception_handler();

	bool run_server = true;
	Options opts;

	ServiceLocator::Init();
	ServiceLocator &service_loc = ServiceLocator::Get();
	service_loc.SetServerRunning(&run_server);
	service_loc.SetOptions(&opts);

	//Create our error log, is of format login_<number>.log
	time_t current_time = time(nullptr);
	std::stringstream log_name(std::stringstream::in | std::stringstream::out);
#ifdef WIN32
	log_name << ".\\logs\\login_" << (unsigned int)current_time << ".log";
#else
	log_name << "./logs/login_" << (unsigned int)current_time << ".log";
#endif
	ErrorLog *log = new ErrorLog(log_name.str().c_str());
	service_loc.SetServerLog(log);
	log->Log(log_debug, "Logging System Init.");

	if(signal(SIGINT, CatchSignal) == SIG_ERR)	{
		log->Log(log_error, "Could not set signal handler");
		delete log;
		return 1;
	}
	if(signal(SIGTERM, CatchSignal) == SIG_ERR)	{
		log->Log(log_error, "Could not set signal handler");
		delete log;
		return 1;
	}
	
	//Create our subsystem and parse the ini file.
	Config *config = new Config();
	service_loc.SetConfig(config);
	log->Log(log_debug, "Config System Init.");
	config->Parse("login.ini");

	//Parse unregistered allowed option.
	if(config->GetVariable("options", "unregistered_allowed").compare("FALSE") == 0)
	{
		opts.AllowUnregistered(false);
	}

	//Parse trace option.
	if(config->GetVariable("options", "trace").compare("TRUE") == 0)
	{
		opts.Trace(true);
	}

	//Parse trace option.
	if(config->GetVariable("options", "world_trace").compare("TRUE") == 0)
	{
		opts.WorldTrace(true);
	}

	//Parse packet inc dump option.
	if(config->GetVariable("options", "dump_packets_in").compare("TRUE") == 0)
	{
		opts.DumpInPackets(true);
	}

	//Parse packet out dump option.
	if(config->GetVariable("options", "dump_packets_out").compare("TRUE") == 0)
	{
		opts.DumpOutPackets(true);
	}

	//Parse encryption mode option.
	std::string mode = config->GetVariable("security", "mode");
	if(mode.size() > 0)
	{
		opts.EncryptionMode(atoi(mode.c_str()));
	}

	//Parse local network option.
	std::string ln = config->GetVariable("options", "local_network");
	if(ln.size() > 0)
	{
		opts.LocalNetwork(ln);
	}

	//Parse reject duplicate servers option.
	if(config->GetVariable("options", "reject_duplicate_servers").compare("TRUE") == 0)
	{
		opts.RejectDuplicateServers(true);
	}

	//Parse account table option.
	ln = config->GetVariable("schema", "account_table");
	if(ln.size() > 0)
	{
		opts.AccountTable(ln);
	}

	//Parse world account table option.
	ln = config->GetVariable("schema", "world_registration_table");
	if(ln.size() > 0)
	{
		opts.WorldRegistrationTable(ln);
	}

	//Parse admin world account table option.
	ln = config->GetVariable("schema", "world_admin_registration_table");
	if(ln.size() > 0)
	{
		opts.WorldAdminRegistrationTable(ln);
	}

	//Parse world type table option.
	ln = config->GetVariable("schema", "world_server_type_table");
	if(ln.size() > 0)
	{
		opts.WorldServerTypeTable(ln);
	}

	//Create our DB from options.
	Database *db = nullptr;
	if(config->GetVariable("database", "subsystem").compare("MySQL") == 0)
	{
#ifdef EQEMU_MYSQL_ENABLED
		log->Log(log_debug, "MySQL Database Init.");
		db = (Database*)new DatabaseMySQL(
			config->GetVariable("database", "user"),
			config->GetVariable("database", "password"),
			config->GetVariable("database", "host"),
			config->GetVariable("database", "port"),
			config->GetVariable("database", "db"));
#endif
	}
	else if(config->GetVariable("database", "subsystem").compare("PostgreSQL") == 0)
	{
#ifdef EQEMU_POSTGRESQL_ENABLED
		log->Log(log_debug, "PostgreSQL Database Init.");
		db = (Database*)new DatabasePostgreSQL(
			config->GetVariable("database", "user"),
			config->GetVariable("database", "password"),
			config->GetVariable("database", "host"),
			config->GetVariable("database", "port"),
			config->GetVariable("database", "db"));
#endif
	}

	//Make sure our database got created okay, otherwise cleanup and exit.
	if(!db)
	{
		log->Log(log_error, "Database Initialization Failure.");
		log->Log(log_debug, "Config System Shutdown.");
		delete config;
		log->Log(log_debug, "Log System Shutdown.");
		delete log;
		return 1;
	}

	service_loc.SetDatabase(db);

#if WIN32
	//initialize our encryption.
	log->Log(log_debug, "Encryption Initialize.");
	Encryption *eq_crypto = new Encryption();
	if(eq_crypto->LoadCrypto(config->GetVariable("security", "plugin")))
	{
		log->Log(log_debug, "Encryption Loaded Successfully.");
	}
	else
	{
		//We can't run without encryption, cleanup and exit.
		log->Log(log_error, "Encryption Failed to Load.");
		delete eq_crypto;
		log->Log(log_debug, "Database System Shutdown.");
		delete db;
		log->Log(log_debug, "Config System Shutdown.");
		delete config;
		log->Log(log_debug, "Log System Shutdown.");
		delete log;
		return 1;
	}
	
	service_loc.SetEncryption(eq_crypto);
#endif

	//create our server manager.
	log->Log(log_debug, "Server Manager Initialize.");
	ServerManager *sm = new ServerManager();
	if(!sm)
	{
		//We can't run without a server manager, cleanup and exit.
		log->Log(log_error, "Server Manager Failed to Start.");
#ifdef WIN32
		log->Log(log_debug, "Encryption System Shutdown.");
		delete eq_crypto;
#endif
		log->Log(log_debug, "Database System Shutdown.");
		delete db;
		log->Log(log_debug, "Config System Shutdown.");
		delete config;
		log->Log(log_debug, "Log System Shutdown.");
		delete log;
		return 1;
	}
	
	service_loc.SetServerManager(sm);

	//create our client manager.
	log->Log(log_debug, "Client Manager Initialize.");
	ClientManager *cm = new ClientManager();
	if(!cm)
	{
		//We can't run without a client manager, cleanup and exit.
		log->Log(log_error, "Client Manager Failed to Start.");
		log->Log(log_debug, "Server Manager Shutdown.");
		delete sm;
#ifdef WIN32
		log->Log(log_debug, "Encryption System Shutdown.");
		delete eq_crypto;
#endif
		log->Log(log_debug, "Database System Shutdown.");
		delete db;
		log->Log(log_debug, "Config System Shutdown.");
		delete config;
		log->Log(log_debug, "Log System Shutdown.");
		delete log;
		return 1;
	}
	
	service_loc.SetClientManager(cm);

#ifdef WIN32
#ifdef UNICODE
	SetConsoleTitle(L"EQEmu Login Server");
#else
	SetConsoleTitle("EQEmu Login Server");
#endif
#endif

	log->Log(log_debug, "Server Started.");
	while(run_server)
	{
		Timer::SetCurrentTime();
		cm->Process();
		sm->Process();
		Sleep(10);
	}

	log->Log(log_debug, "Server Shutdown.");
	log->Log(log_debug, "Client Manager Shutdown.");
	delete cm;
	log->Log(log_debug, "Server Manager Shutdown.");
	delete sm;
#ifdef WIN32
	log->Log(log_debug, "Encryption System Shutdown.");
	delete eq_crypto;
#endif
	log->Log(log_debug, "Database System Shutdown.");
	delete db;
	log->Log(log_debug, "Config System Shutdown.");
	delete config;
	log->Log(log_debug, "Log System Shutdown.");
	delete log;
	return 0;
}

