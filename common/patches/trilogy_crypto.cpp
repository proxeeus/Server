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

// Phase 2: Network & Stream Layer — Trilogy DES credential decryption.
//
// Protocol analysis summary (Task 2.1 / 2.2 / 2.3):
//
//   Session layer:  Identical Daybreak UDP protocol (OP_SessionRequest /
//                   OP_SessionResponse, ARQ/ASQ, CRC32).  No EQStream
//                   changes needed.
//
//   CRC32 table:    EQClassic LS/common/crc32.cpp uses the same standard
//                   IEEE CRC32 polynomial table as Server/common/crc32.cpp.
//                   Verified by comparing all 256 entries — byte-for-byte
//                   identical.  No EQStream changes needed.
//
//   Compression:    zlib deflate (flag 0x01) and XOR encoding (flag 0x04)
//                   are unchanged from post-Trilogy clients.  No EQStream
//                   changes needed.
//
//   Stream signature non-collision (Task 2.2):
//     World: Trilogy  LoginInfo_Struct  =  40 bytes  (opcode 0x5818)
//            Titanium LoginInfo_Struct  = 488 bytes  (same internal opcode)
//            → size difference is the discriminator; no collision.
//     Zone:  Trilogy  ClientZoneEntry_Struct = 30 bytes  (opcode 0x2a20)
//            Titanium ClientZoneEntry_Struct = 68 bytes  (same internal opcode)
//            → size difference is the discriminator; no collision.
//
//   DES encryption (Task 2.1 / Phase 3 call-site):
//     The first 40 bytes of OP_SendLoginInfo are DES-CBC encrypted with the
//     Verant key.  This is the *only* non-standard crypto in the Trilogy
//     protocol — all other packets use the standard EQEmu XOR encoding.
//     DecryptLoginBlock() is the sole entry point; the call-site lives in
//     world/client.cpp::HandleSendLoginInfoPacket() (added in Phase 3).

#include "../global_define.h"
#include "trilogy_crypto.h"

#ifdef EQEMU_USE_OPENSSL
#include <openssl/des.h>
#include <string.h>
#endif

namespace Trilogy {

// Verant DES key — source: EQClassic LS/Login/EQCrypto.cpp (neorab comment):
//   "verant's DES key in our client: 13 D9 13 6D D0 34 15 FB"
static const unsigned char s_verant_key[8] = {
	0x13, 0xD9, 0x13, 0x6D, 0xD0, 0x34, 0x15, 0xFB
};

bool DecryptLoginBlock(const uint8 *crypt_in, uint8 *plain_out, int size)
{
#ifdef EQEMU_USE_OPENSSL
	DES_cblock key;
	DES_key_schedule ks;

	memcpy(&key, s_verant_key, 8);
	DES_key_sched(&key, &ks);

	// IV is the same as the key, per EQClassic EQCrypto::DoDESCrypt():
	//   DES_cblock verant_iv = { 19, 217, 19, 109, 208, 52, 21, 251 };
	DES_cblock iv;
	memcpy(&iv, s_verant_key, 8);

	DES_ncbc_encrypt(crypt_in, plain_out, size, &ks, &iv, DES_DECRYPT);
	return true;
#else
	(void)crypt_in;
	(void)plain_out;
	(void)size;
	return false;
#endif
}

} /*Trilogy*/
