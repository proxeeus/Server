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

#ifdef EQEMU_POSTGRESQL_ENABLED
#include "database_postgresql.h"
#include "login_server.h"

DatabasePostgreSQL::DatabasePostgreSQL(std::string user, std::string pass, std::string host, std::string port, std::string name)
{
	ServiceLocator &service_loc = ServiceLocator::Get();
	db = nullptr;
	db = PQsetdbLogin(host.c_str(), port.c_str(), nullptr, nullptr, name.c_str(), user.c_str(), pass.c_str());
	if(!db)
	{
		service_loc.GetServerLog()->Log(log_database, "Failed to connect to PostgreSQL Database.");
	}

	if(PQstatus(db) != CONNECTION_OK)
	{
		service_loc.GetServerLog()->Log(log_database, "Failed to connect to PostgreSQL Database.");
		PQfinish(db);
		db = nullptr;
	}
}

DatabasePostgreSQL::~DatabasePostgreSQL()
{
	if(db)
	{
		PQfinish(db);
	}
}

bool DatabasePostgreSQL::GetLoginDataFromAccountName(std::string name, std::string &password, unsigned int &id)
{
	if(!db)
	{
		return false;
	}

	/**
	* PostgreSQL doesn't have automatic reconnection option like mysql
	* but it's easy to check and reconnect
	*/
	if(PQstatus(db) != CONNECTION_OK)
	{
		PQreset(db);
		if(PQstatus(db) != CONNECTION_OK)
		{
			return false;
		}
	}

	ServiceLocator &service_loc = ServiceLocator::Get();
	std::stringstream query(std::stringstream::in | std::stringstream::out);
	query << "SELECT LoginServerID, AccountPassword FROM " << service_loc.GetOptions()->GetAccountTable() << " WHERE AccountName = '";
	query << name;
	query << "'";

	PGresult *res = PQexec(db, query.str().c_str());

	char *error = PQresultErrorMessage(res);
	if(strlen(error) > 0)
	{
		service_loc.GetServerLog()->Log(log_database, "Database error in DatabasePostgreSQL::GetLoginDataFromAccountName(): %s", error);
		PQclear(res);
		return false;
	}

	if(PQntuples(res) > 0)
	{
		id = atoi(PQgetvalue(res, 0, 0));
		password = PQgetvalue(res, 0, 1);
		PQclear(res);
		return true;
	}

	PQclear(res);
	return false;
}

bool DatabasePostgreSQL::GetWorldRegistration(std::string long_name, std::string short_name, unsigned int &id, std::string &desc, unsigned int &list_id,
		unsigned int &trusted, std::string &list_desc, std::string &account, std::string &password)
{
	if(!db)
	{
		return false;
	}

	/**
	* PostgreSQL doesn't have automatic reconnection option like mysql
	* but it's easy to check and reconnect
	*/
	if(PQstatus(db) != CONNECTION_OK)
	{
		PQreset(db);
		if(PQstatus(db) != CONNECTION_OK)
		{
			return false;
		}
	}

	ServiceLocator &service_loc = ServiceLocator::Get();
	std::stringstream query(std::stringstream::in | std::stringstream::out);
	query << "SELECT WSR.ServerID, WSR.ServerTagDescription, WSR.ServerTrusted, SLT.ServerListTypeID, ";
	query << "SLT.ServerListTypeDescription, SAR.AccountName, SAR.AccountPassword FROM " << service_loc.GetOptions()->GetWorldRegistrationTable();
	query << " AS WSR JOIN " << service_loc.GetOptions()->GetWorldServerTypeTable() << " AS SLT ON WSR.ServerListTypeID = SLT.ServerListTypeID JOIN ";
	query << service_loc.GetOptions()->GetWorldAdminRegistrationTable() << " AS SAR ON WSR.ServerAdminID = SAR.ServerAdminID WHERE WSR.ServerShortName";
	query << " = '";
	query << short_name;
	query << "'";

	PGresult *res = PQexec(db, query.str().c_str());

	char *error = PQresultErrorMessage(res);
	if(strlen(error) > 0)
	{
		service_loc.GetServerLog()->Log(log_database, "Database error in DatabasePostgreSQL::GetWorldRegistration(): %s", error);
		PQclear(res);
		return false;
	}

	if(PQntuples(res) > 0)
	{
		id = atoi(PQgetvalue(res, 0, 0));
		desc = PQgetvalue(res, 0, 1);
		trusted = atoi(PQgetvalue(res, 0, 2));
		list_id = atoi(PQgetvalue(res, 0, 3));
		list_desc = PQgetvalue(res, 0, 4);
		account = PQgetvalue(res, 0, 5);
		password = PQgetvalue(res, 0, 6);

		PQclear(res);
		return true;
	}

	PQclear(res);
	return false;
}

void DatabasePostgreSQL::UpdateLSAccountData(unsigned int id, std::string ip_address)
{
	if(!db)
	{
		return;
	}

	/**
	* PostgreSQL doesn't have automatic reconnection option like mysql
	* but it's easy to check and reconnect
	*/
	if(PQstatus(db) != CONNECTION_OK)
	{
		PQreset(db);
		if(PQstatus(db) != CONNECTION_OK)
		{
			return;
		}
	}

	ServiceLocator &service_loc = ServiceLocator::Get();
	std::stringstream query(std::stringstream::in | std::stringstream::out);
	query << "UPDATE " << service_loc.GetOptions()->GetAccountTable() << " SET LastIPAddress = '";
	query << ip_address;
	query << "', LastLoginDate = current_date where LoginServerID = ";
	query << id;
	PGresult *res = PQexec(db, query.str().c_str());

	char *error = PQresultErrorMessage(res);
	if(strlen(error) > 0)
	{
		service_loc.GetServerLog()->Log(log_database, "Database error in DatabasePostgreSQL::GetLoginDataFromAccountName(): %s", error);
	}
	PQclear(res);
}

void DatabasePostgreSQL::UpdateWorldRegistration(unsigned int id, std::string long_name, std::string ip_address)
{
	if(!db)
	{
		return;
	}

	/**
	* PostgreSQL doesn't have automatic reconnection option like mysql
	* but it's easy to check and reconnect
	*/
	if(PQstatus(db) != CONNECTION_OK)
	{
		PQreset(db);
		if(PQstatus(db) != CONNECTION_OK)
		{
			return;
		}
	}

	ServiceLocator &service_loc = ServiceLocator::Get();
	std::stringstream query(std::stringstream::in | std::stringstream::out);
	query << "UPDATE " << service_loc.GetOptions()->GetWorldRegistrationTable() << " SET ServerLastLoginDate = current_date, ServerLastIPAddr = '";
	query << ip_address;
	query << "', ServerLongName = '";
	query << long_name;
	query << "' where ServerID = ";
	query << id;
	PGresult *res = PQexec(db, query.str().c_str());

	char *error = PQresultErrorMessage(res);
	if(strlen(error) > 0)
	{
		service_loc.GetServerLog()->Log(log_database, "Database error in DatabasePostgreSQL::GetLoginDataFromAccountName(): %s", error);
	}
	PQclear(res);
}

#endif
