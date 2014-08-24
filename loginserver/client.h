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
#ifndef EQEMU_CLIENT_H
#define EQEMU_CLIENT_H

#include "../common/debug.h"
#include "../common/opcodemgr.h"
#include "../common/eq_stream_type.h"
#include "../common/eq_stream_factory.h"
#ifndef WIN32
#include "eq_crypto_api.h"
#endif
#include <string>

enum ClientVersion
{
	cv_titanium,
	cv_sod
};

enum ClientStatus
{
	cs_not_sent_session_ready,
	cs_waiting_for_login,
	cs_logged_in
};

enum LoginMode
{
	lm_initial = 2,
	lm_from_world = 3
};

/**
* Client class, controls a single client and it's
* connection to the login server.
*/
class Client
{
public:
	/**
	* Constructor, sets our connection to c and version to v
	*/
	Client(EQStream *c, ClientVersion v);

	/**
	* Destructor.
	*/
	~Client() { }

	/**
	* Processes the client's connection and does various actions.
	*/
	virtual bool Process();

	/**
	* Sends our reply to session ready packet.
	*/
	virtual void Handle_SessionReady(const char* data, unsigned int size);

	/**
	* Verifies login and send a reply.
	*/
	virtual void Handle_Login(const char* data, unsigned int size);

	/**
	* Sends a packet to the requested server to see if the client is allowed or not.
	*/
	virtual void Handle_Play(const char* data);

	/**
	* Sends a server list packet to the client.
	*/
	virtual void SendServerListPacket();

	/**
	* Sends the input packet to the client and clears our play response states.
	*/
	virtual void SendPlayResponse(EQApplicationPacket *outapp);

	/**
	* Generates a random login key for the client during login.
	*/
	virtual void GenerateKey();

	/**
	* Gets the account id of this client.
	*/
	virtual unsigned int GetAccountID() const { return account_id; }

	/**
	* Gets the account name of this client.
	*/
	virtual std::string GetAccountName() const { return account_name; }

	/**
	* Gets the key generated at login for this client.
	*/
	virtual std::string GetKey() const { return key; }

	/**
	* Gets the server selected to be played on for this client.
	*/
	virtual unsigned int GetPlayServerID() const { return play_server_id; }

	/**
	* Gets the play sequence state for this client.
	*/
	virtual unsigned int GetPlaySequence() const { return play_sequence_id; }

	/**
	* Gets the connection for this client.
	*/
	virtual EQStream *GetConnection() { return connection; }
private:
	EQStream *connection;
	ClientVersion version;
	ClientStatus status;

	std::string account_name;
	unsigned int account_id;
	unsigned int play_server_id;
	unsigned int play_sequence_id;
	std::string key;
};

#endif
