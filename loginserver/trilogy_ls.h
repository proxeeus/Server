/*  EQEmulator: Everquest Server Emulator
    Copyright (C) 2001-2024 EQEmulator Development Team (https://github.com/EQEmu/Server)

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

// Minimal EQNetwork Login Server for the Trilogy (EQ v29c/v30) client.
// Listens on UDP port 5998 and speaks the Verant EQNetwork protocol so
// the Trilogy client can reach the server list and proceed to port 9000.
// Real authentication happens at port 9000 via DES-block decryption.

#pragma once
#include <atomic>
#include <cstdint>
#include <ctime>
#include <map>
#include <thread>
#include <vector>

class TrilogyLoginServer {
public:
	void Start(uint16_t port = 5998);
	void Stop();

private:
	struct Session {
		uint16_t    gsq       = 0;     // server GSQ / SEQ counter
		uint16_t    arq       = 0;     // server ARQ counter
		uint8_t     asq_hi    = 1;     // ASQ high byte
		uint8_t     asq_lo    = 0;     // ASQ low byte
		uint16_t    cli_arq   = 0;     // client ARQ we owe an ARSP for
		bool        ack_due   = false; // true when ARSP must be included
		bool        sack_init = false; // true once SACK has been seeded
		bool        seq_sent  = false; // true once first SEQStart data pkt sent
		std::time_t last_pkt  = 0;
		// auth state (filled during LS_OP_LoginInfo)
		uint32_t    account_id   = 0;
		char        account_name[32] = {};
		char        key[16]          = {}; // 15-char session key + null
		uint32_t    client_ip        = 0;
		// set when client sends a reconnect IP (post-char-create re-auth)
		bool        reconnect_mode   = false;
	};

	void RunLoop(uint16_t port);

	void OnDatagram(int sock, uint32_t ip, uint16_t port_ne,
	                const uint8_t* buf, int len);

	void OnOpcode(int sock, uint32_t ip, uint16_t port_ne, Session& s,
	              uint16_t opcode, const uint8_t* payload, uint32_t plen);

	void SendApp(int sock, uint32_t ip, uint16_t port_ne, Session& s,
	             uint16_t opcode,
	             const uint8_t* payload = nullptr, uint32_t plen = 0);

	void SendAck(int sock, uint32_t ip, uint16_t port_ne, Session& s);

	std::vector<uint8_t> BuildServerList() const;

	static uint64_t SessionKey(uint32_t ip, uint16_t port_ne);

	std::map<uint64_t, Session> m_sessions;

	std::thread                 m_thread;
	std::atomic<bool>           m_running{false};
};
