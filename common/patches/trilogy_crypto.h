/*	EQEMu: Everquest Server Emulator

	Copyright (C) 2001-2016 EQEMu Development Team (http://eqemulator.net)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; version 2 of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY except by those people which sell it, which
	are required to give you total support for your newly bought product;
	without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

// Phase 2: Network & Stream Layer
//
// The Trilogy client DES-encrypts the first 40 bytes of the LoginInfo packet
// (the credential block) using a fixed Verant DES key with CBC mode.
// Decrypted layout of the 40-byte block:
//   char username[20]  (offset 0)
//   char password[20]  (offset 20)
//
// Verant key (from EQClassic LS/Login/EQCrypto.cpp, neorab's comment):
//   13 D9 13 6D D0 34 15 FB
//
// The EQStream session handshake (OP_SessionRequest / OP_SessionResponse /
// ACK/NAK sequence) is identical to all other EQEmu client versions; no
// changes to EQStream are required for Trilogy.  This file is the only
// Phase 2 code addition needed.

#ifndef COMMON_TRILOGY_CRYPTO_H
#define COMMON_TRILOGY_CRYPTO_H

#include "../types.h"

namespace Trilogy {

// Decrypt the 40-byte DES credential block from a Trilogy LoginInfo_Struct.
//
// Parameters:
//   crypt_in  — pointer to the raw 40-byte crypt[] field (encrypted)
//   plain_out — caller-supplied buffer of at least `size` bytes
//   size      — number of bytes to decrypt; must be a multiple of 8 and <= 40
//
// Returns true on success, false if OpenSSL DES support was not compiled in
// (EQEMU_USE_OPENSSL not defined).
//
// On success plain_out[0..19]  = null-terminated username
//            plain_out[20..39] = null-terminated password
bool DecryptLoginBlock(const uint8 *crypt_in, uint8 *plain_out, int size);

} /*Trilogy*/

#endif /*COMMON_TRILOGY_CRYPTO_H*/
