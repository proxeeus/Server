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
#ifndef EQEMU_ERROR_LOG_INTERFACE_H
#define EQEMU_ERROR_LOG_INTERFACE_H

/**
 * Dictates the log type specified in ErrorLog for Log(...)
 */
enum eqLogType
{
	log_debug,
	log_error,
	log_database,
	log_network,
	log_network_trace,
	log_network_error,
	log_world,
	log_world_error,
	log_client,
	log_client_error,
	_log_largest_type
};

class ErrorLogInterface
{
public:
	ErrorLogInterface() { }
	virtual ~ErrorLogInterface() { }

	/**
	* Writes to the log system a variable message.
	*/
	virtual void Log(eqLogType type, const char *message, ...) = 0;

	/**
	* Writes to the log system a packet.
	*/
	virtual void LogPacket(eqLogType type, const char *data, size_t size) = 0;
};

#endif
