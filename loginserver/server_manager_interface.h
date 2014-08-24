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
#ifndef EQEMU_SERVER_MANAGER_INTERFACE_H
#define EQEMU_SERVER_MANAGER_INTERFACE_H

class ServerManagerInterface
{
public:
	ServerManagerInterface() { }
	~ServerManagerInterface() { }

	virtual void Process() = 0;

	virtual void SendUserToWorldRequest(unsigned int server_id, unsigned int client_account_id) = 0;

	virtual EQApplicationPacket *CreateServerListPacket(Client *c) = 0;

	virtual bool ServerExists(std::string l_name, std::string s_name, WorldServer *ignore = nullptr) = 0;

	virtual void DestroyServerByName(std::string l_name, std::string s_name, WorldServer *ignore = nullptr) = 0;
};

#endif
