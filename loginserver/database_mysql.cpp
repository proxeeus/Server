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
#include "database.h"

#ifdef EQEMU_MYSQL_ENABLED
#include "database_mysql.h"
#include "login_server.h"

DatabaseMySQL::DatabaseMySQL(std::string user, std::string pass, std::string host, std::string port, std::string name)
{
	this->user = user;
	this->pass = pass;
	this->host = host;
	this->name = name;
	ServiceLocator &service_loc = ServiceLocator::Get();

	db = mysql_init(nullptr);
	if(db)
	{
		my_bool r = 1;
		mysql_options(db, MYSQL_OPT_RECONNECT, &r);
		if(!mysql_real_connect(db, host.c_str(), user.c_str(), pass.c_str(), name.c_str(), atoi(port.c_str()), nullptr, 0))
		{
			mysql_close(db);
			service_loc.GetServerLog()->Log(log_database, "Failed to connect to MySQL database. Error: %s", mysql_error(db));
			exit(1);
		}
	}
	else
	{
		service_loc.GetServerLog()->Log(log_database, "Failed to create db object in MySQL database.");
	}
}

DatabaseMySQL::~DatabaseMySQL()
{
	if(db)
	{
		mysql_close(db);
	}
}

bool DatabaseMySQL::GetLoginDataFromAccountName(std::string name, std::string &password, unsigned int &id)
{
	if(!db)
	{
		return false;
	}

	ServiceLocator &service_loc = ServiceLocator::Get();
	MYSQL_RES *res;
	MYSQL_ROW row;
	std::stringstream query(std::stringstream::in | std::stringstream::out);
	query << "SELECT LoginServerID, AccountPassword FROM " << service_loc.GetOptions()->GetAccountTable() << " WHERE AccountName = '";
	query << name;
	query << "'";

	if(mysql_query(db, query.str().c_str()) != 0)
	{
		service_loc.GetServerLog()->Log(log_database, "Mysql query failed: %s", query.str().c_str());
		return false;
	}

	res = mysql_use_result(db);

	if(res)
	{
		while((row = mysql_fetch_row(res)) != nullptr)
		{
			id = atoi(row[0]);
			password = row[1];
			mysql_free_result(res);
			return true;
		}
	}

	service_loc.GetServerLog()->Log(log_database, "Mysql query returned no result: %s", query.str().c_str());
	return false;
}

bool DatabaseMySQL::GetWorldRegistration(std::string long_name, std::string short_name, unsigned int &id, std::string &desc, unsigned int &list_id,
										 unsigned int &trusted, std::string &list_desc, std::string &account, std::string &password)
{
	if(!db)
	{
		return false;
	}

	ServiceLocator &service_loc = ServiceLocator::Get();
	MYSQL_RES *res;
	MYSQL_ROW row;
	char escaped_short_name[101];
	unsigned long length;
	length = mysql_real_escape_string(db, escaped_short_name, short_name.substr(0, 100).c_str(), short_name.substr(0, 100).length());
	escaped_short_name[length+1] = 0;
	std::stringstream query(std::stringstream::in | std::stringstream::out);
	query << "SELECT WSR.ServerID, WSR.ServerTagDescription, WSR.ServerTrusted, SLT.ServerListTypeID, ";
	query << "SLT.ServerListTypeDescription, WSR.ServerAdminID FROM " << service_loc.GetOptions()->GetWorldRegistrationTable();
	query << " AS WSR JOIN " << service_loc.GetOptions()->GetWorldServerTypeTable() << " AS SLT ON WSR.ServerListTypeID = SLT.ServerListTypeID";
	query << " WHERE WSR.ServerShortName = '";
	query << escaped_short_name;
	query << "'";

	if(mysql_query(db, query.str().c_str()) != 0)
	{
		service_loc.GetServerLog()->Log(log_database, "Mysql query failed: %s", query.str().c_str());
		return false;
	}

	res = mysql_use_result(db);
	if(res)
	{
		if((row = mysql_fetch_row(res)) != nullptr)
		{
			id = atoi(row[0]);
			desc = row[1];
			trusted = atoi(row[2]);
			list_id = atoi(row[3]);
			list_desc = row[4];
			int db_account_id = atoi(row[5]);
			mysql_free_result(res);

			if(db_account_id > 0)
			{
				std::stringstream query(std::stringstream::in | std::stringstream::out);
				query << "SELECT AccountName, AccountPassword FROM " << service_loc.GetOptions()->GetWorldAdminRegistrationTable();
				query << " WHERE ServerAdminID = " << db_account_id;

				if(mysql_query(db, query.str().c_str()) != 0)
				{
					service_loc.GetServerLog()->Log(log_database, "Mysql query failed: %s", query.str().c_str());
					return false;
				}

				res = mysql_use_result(db);
				if(res)
				{
					if((row = mysql_fetch_row(res)) != nullptr)
					{
						account = row[0];
						password = row[1];
						mysql_free_result(res);
						return true;
					}
				}

				service_loc.GetServerLog()->Log(log_database, "Mysql query returned no result: %s", query.str().c_str());
				return false;
			}
			return true;
		}
	}

	service_loc.GetServerLog()->Log(log_database, "Mysql query returned no result: %s", query.str().c_str());
	return false;
}

void DatabaseMySQL::UpdateLSAccountData(unsigned int id, std::string ip_address)
{
	if(!db)
	{
		return;
	}

	ServiceLocator &service_loc = ServiceLocator::Get();
	std::stringstream query(std::stringstream::in | std::stringstream::out);
	query << "UPDATE " << service_loc.GetOptions()->GetAccountTable() << " SET LastIPAddress = '";
	query << ip_address;
	query << "', LastLoginDate = now() where LoginServerID = ";
	query << id;

	if(mysql_query(db, query.str().c_str()) != 0)
	{
		service_loc.GetServerLog()->Log(log_database, "Mysql query failed: %s", query.str().c_str());
	}
}

void DatabaseMySQL::UpdateLSAccountInfo(unsigned int id, std::string name, std::string password, std::string email)
{
	if(!db)
	{
		return;
	}

	ServiceLocator &service_loc = ServiceLocator::Get();
	std::stringstream query(std::stringstream::in | std::stringstream::out);
	query << "REPLACE " << service_loc.GetOptions()->GetAccountTable() << " SET LoginServerID = ";
	query << id << ", AccountName = '" << name << "', AccountPassword = sha('";
	query << password << "'), AccountCreateDate = now(), AccountEmail = '" << email;
	query << "', LastIPAddress = '0.0.0.0', LastLoginDate = now()";

	if(mysql_query(db, query.str().c_str()) != 0)
	{
		service_loc.GetServerLog()->Log(log_database, "Mysql query failed: %s", query.str().c_str());
	}
}

void DatabaseMySQL::UpdateWorldRegistration(unsigned int id, std::string long_name, std::string ip_address)
{
	if(!db)
	{
		return;
	}

	ServiceLocator &service_loc = ServiceLocator::Get();
	char escaped_long_name[101];
	unsigned long length;
	length = mysql_real_escape_string(db, escaped_long_name, long_name.substr(0, 100).c_str(), long_name.substr(0, 100).length());
	escaped_long_name[length+1] = 0;
	std::stringstream query(std::stringstream::in | std::stringstream::out);
	query << "UPDATE " << service_loc.GetOptions()->GetWorldRegistrationTable() << " SET ServerLastLoginDate = now(), ServerLastIPAddr = '";
	query << ip_address;
	query << "', ServerLongName = '";
	query << escaped_long_name;
	query << "' WHERE ServerID = ";
	query << id;

	if(mysql_query(db, query.str().c_str()) != 0)
	{
		service_loc.GetServerLog()->Log(log_database, "Mysql query failed: %s", query.str().c_str());
	}
}

bool DatabaseMySQL::CreateWorldRegistration(std::string long_name, std::string short_name, unsigned int &id)
{
	if(!db)
	{
		return false;
	}

	ServiceLocator &service_loc = ServiceLocator::Get();
	MYSQL_RES *res;
	MYSQL_ROW row;
	char escaped_long_name[201];
	char escaped_short_name[101];
	unsigned long length;
	length = mysql_real_escape_string(db, escaped_long_name, long_name.substr(0, 100).c_str(), long_name.substr(0, 100).length());
	escaped_long_name[length+1] = 0;
	length = mysql_real_escape_string(db, escaped_short_name, short_name.substr(0, 100).c_str(), short_name.substr(0, 100).length());
	escaped_short_name[length+1] = 0;
	std::stringstream query(std::stringstream::in | std::stringstream::out);
	query << "SELECT max(ServerID) FROM " << service_loc.GetOptions()->GetWorldRegistrationTable();

	if(mysql_query(db, query.str().c_str()) != 0)
	{
		service_loc.GetServerLog()->Log(log_database, "Mysql query failed: %s", query.str().c_str());
		return false;
	}

	res = mysql_use_result(db);
	if(res)
	{
		if((row = mysql_fetch_row(res)) != nullptr)
		{
			id = atoi(row[0]) + 1;
			mysql_free_result(res);

			std::stringstream query(std::stringstream::in | std::stringstream::out);
			query << "INSERT INTO " << service_loc.GetOptions()->GetWorldRegistrationTable() << " SET ServerID = " << id;
			query << ", ServerLongName = '" << escaped_long_name << "', ServerShortName = '" << escaped_short_name;
			query << "', ServerListTypeID = 3, ServerAdminID = 0, ServerTrusted = 0, ServerTagDescription = ''";

			if(mysql_query(db, query.str().c_str()) != 0)
			{
				service_loc.GetServerLog()->Log(log_database, "Mysql query failed: %s", query.str().c_str());
				return false;
			}
			return true;
		}
	}
	service_loc.GetServerLog()->Log(log_database, "World registration did not exist in the database for %s %s", long_name.c_str(), short_name.c_str());
	return false;
}

#endif
