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
#ifndef EQEMU_ENCRYPTION_INTERFACE_H
#define EQEMU_ENCRYPTION_INTERFACE_H
#ifdef WIN32

class EncryptionInterface
{
public:
	EncryptionInterface() { };
	~EncryptionInterface() { }

	virtual bool Loaded() = 0;

	virtual bool LoadCrypto(std::string name) = 0;

	virtual char* DecryptUsernamePassword(const char* encryptedBuffer, unsigned int bufferSize, int mode) = 0;

	virtual char* Encrypt(const char* buffer, unsigned int bufferSize, unsigned int &outSize) = 0;

	virtual void DeleteHeap(char* buffer) = 0;
};

#endif
#endif
