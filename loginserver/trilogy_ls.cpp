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

// Minimal EQNetwork Login Server for Trilogy (EQ v29c/v30, "8-09-2001 14:25").
//
// EQNetwork wire format (all multi-byte values big-endian):
//   [HDR0][HDR1][SEQ(2)][optional ARSP(2)][optional ARQ(2)]
//   [optional ASQ_hi(1)+ASQ_lo(1)][opcode(2)][payload][CRC32(4)]
//
// HDR0 bit flags: a1_ARQ=0x02, a3_Fragment=0x08, a4_ASQ=0x10, a5_SEQStart=0x20
// HDR1 bit flags: b2_ARSP=0x04
//
// CRC32 covers bytes [0..size-5] (everything except the last 4 bytes).
// Stored big-endian: htonl(CRC32::Generate(buf, payload_len)).

// global_define.h includes <winsock2.h> + <windows.h> on Windows.
// It must be the very first header to avoid winsock1/winsock2 conflicts.
#include "../common/global_define.h"

#ifndef _WINDOWS
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR   (-1)
#  define closesocket    close
typedef int SOCKET;
#endif

#include "trilogy_ls.h"
#include "../common/crc32.h"
#include "../common/eqemu_logsys.h"
#include "login_server.h"
#include "encryption.h"
#include "world_server.h"

// Verant/Trilogy DES key: same value used as both key and IV.
// Source: EQClassic/LS/Login/EQCrypto.cpp
//   DES_cblock verant_key = { 19, 217, 19, 109, 208, 52, 21, 251 };
//   DES_cblock verant_iv  = verant_key; (initialized identically)
//   DES_ncbc_encrypt(..., &key_schedule, &verant_iv, enc);
#ifdef EQEMU_USE_OPENSSL
#  include <openssl/des.h>
#endif
#ifdef EQEMU_USE_MBEDTLS
#  include <mbedtls/des.h>
#endif

static bool trilogy_des_decrypt(const uint8_t* in, uint8_t* out, size_t len)
{
	static const uint8_t verant_key[8] = { 0x13, 0xD9, 0x13, 0x6D, 0xD0, 0x34, 0x15, 0xFB };
#ifdef EQEMU_USE_OPENSSL
	DES_key_schedule ks;
	DES_cblock key_cb, iv_cb;
	memcpy(&key_cb, verant_key, 8);
	memcpy(&iv_cb,  verant_key, 8);
	DES_set_key_unchecked(&key_cb, &ks);
	DES_ncbc_encrypt(in, out, (long)len, &ks, &iv_cb, DES_DECRYPT);
	return true;
#elif defined(EQEMU_USE_MBEDTLS)
	unsigned char key[8], iv[8];
	memcpy(key, verant_key, 8);
	memcpy(iv,  verant_key, 8);
	mbedtls_des_context ctx;
	mbedtls_des_init(&ctx);
	mbedtls_des_setkey_dec(&ctx, key);
	int r = mbedtls_des_crypt_cbc(&ctx, MBEDTLS_DES_DECRYPT, len, iv,
	                               in, out);
	mbedtls_des_free(&ctx);
	return r == 0;
#else
	(void)in; (void)out; (void)len;
	return false;
#endif
}

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

extern LoginServer server;

// LS opcode constants (host byte order; written to wire as htons)
static constexpr uint16_t LS_OP_LoginInfo           = 0x0100;
static constexpr uint16_t LS_OP_SessionId           = 0x0400;
static constexpr uint16_t LS_OP_AllFinish           = 0x0500;
static constexpr uint16_t LS_OP_ServerList          = 0x4600;
static constexpr uint16_t LS_OP_SessionKey          = 0x4700;
static constexpr uint16_t LS_OP_RequestServerStatus = 0x4800;
static constexpr uint16_t LS_OP_SendServerStatus    = 0x4A00;
static constexpr uint16_t LS_OP_LoginBanner         = 0x5200;
static constexpr uint16_t LS_OP_Version             = 0x5900;
static constexpr uint16_t LS_OP_Unknown8800         = 0x8800;
static constexpr uint16_t LS_OP_Unknown8E00         = 0x8E00;

// EQNetwork HDR byte 0 flags
static constexpr uint8_t HDR0_ARQ      = 0x02;
static constexpr uint8_t HDR0_FRAGMENT = 0x08;
static constexpr uint8_t HDR0_ASQ      = 0x10;
static constexpr uint8_t HDR0_SEQSTART = 0x20;
// EQNetwork HDR byte 1 flags
static constexpr uint8_t HDR1_ARSP     = 0x04;

// ============================================================

uint64_t TrilogyLoginServer::SessionKey(uint32_t ip, uint16_t port_ne)
{
	return (static_cast<uint64_t>(ip) << 16) | static_cast<uint64_t>(ntohs(port_ne));
}

void TrilogyLoginServer::Start(uint16_t port)
{
	if (m_running.exchange(true))
		return;
	m_thread = std::thread([this, port]() { RunLoop(port); });
	m_thread.detach();
}

void TrilogyLoginServer::Stop()
{
	m_running = false;
}

// ============================================================
// Background UDP receive loop
// ============================================================

void TrilogyLoginServer::RunLoop(uint16_t port)
{
	SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == INVALID_SOCKET) {
		LogError("[TrilogyLS] Failed to create UDP socket");
		m_running = false;
		return;
	}

	{
		int reuse = 1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
		           reinterpret_cast<const char*>(&reuse), sizeof(reuse));
	}

	sockaddr_in addr{};
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
		LogError("[TrilogyLS] Failed to bind UDP port {}", port);
		closesocket(sock);
		m_running = false;
		return;
	}

#ifdef _WINDOWS
	unsigned long nb = 1;
	ioctlsocket(sock, FIONBIO, &nb);
#else
	fcntl(sock, F_SETFL, O_NONBLOCK);
#endif

	LogInfo("[TrilogyLS] Login server listening on UDP port {}", port);

	uint8_t buf[2048];

	while (m_running) {
		// Drain all available datagrams before sleeping
		for (;;) {
			sockaddr_in from{};
#ifdef _WINDOWS
			int fromlen = sizeof(from);
			int n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
			                 reinterpret_cast<sockaddr*>(&from), &fromlen);
#else
			socklen_t fromlen = sizeof(from);
			int n = recvfrom(sock, buf, sizeof(buf), 0,
			                 reinterpret_cast<sockaddr*>(&from), &fromlen);
#endif
			if (n <= 0)
				break;
			LogNetcode("[TrilogyLS] Recv {} bytes from {}:{}", n,
			           inet_ntoa(from.sin_addr), ntohs(from.sin_port));
			OnDatagram(static_cast<int>(sock),
			           from.sin_addr.s_addr, from.sin_port, buf, n);
		}

		// Expire stale sessions (>60 s idle)
		std::time_t now = std::time(nullptr);
		for (auto it = m_sessions.begin(); it != m_sessions.end();) {
			if (now - it->second.last_pkt > 60)
				it = m_sessions.erase(it);
			else
				++it;
		}

#ifdef _WINDOWS
		Sleep(1);
#else
		usleep(1000);
#endif
	}

	closesocket(sock);
	LogInfo("[TrilogyLS] Login server stopped");
}

// ============================================================
// Incoming datagram parser
// ============================================================

void TrilogyLoginServer::OnDatagram(int sock, uint32_t ip, uint16_t port_ne,
                                     const uint8_t* data, int size)
{
	// Minimum: HDR[2] + SEQ[2] + CRC[4] = 8 bytes
	if (size < 8)
		return;

	// Verify CRC32 (covers bytes [0 .. size-5])
	{
		uint32_t stored = ntohl(*reinterpret_cast<const uint32_t*>(data + size - 4));
		uint32_t calc   = CRC32::Generate(data, static_cast<uint32_t>(size - 4));
		if (stored != calc) {
			LogInfo("[TrilogyLS] CRC mismatch: stored=0x{:08X} calc=0x{:08X} hdr=0x{:02X}{:02X}",
			        stored, calc, data[0], data[1]);
			return;
		}
	}

	uint8_t hdr0 = data[0];
	uint8_t hdr1 = data[1];
	int o = 2;

	// SEQ (big-endian, not needed but consume it)
	if (o + 2 > size - 4) return;
	o += 2;

	// HDR1 optional fields
	if (hdr1 & HDR1_ARSP) { if (o + 2 > size - 4) return; o += 2; } // ARSP
	if (hdr1 & 0x08)       { if (o + 2 > size - 4) return; o += 2; } // b3
	if (hdr1 & 0x10)       { if (o + 1 > size - 4) return; o += 1; } // b4
	if (hdr1 & 0x20)       { if (o + 2 > size - 4) return; o += 2; } // b5
	if (hdr1 & 0x40)       { if (o + 4 > size - 4) return; o += 4; } // b6
	if (hdr1 & 0x80)       { if (o + 8 > size - 4) return; o += 8; } // b7

	// a1_ARQ
	uint16_t cli_arq = 0;
	bool has_arq = (hdr0 & HDR0_ARQ) != 0;
	if (has_arq) {
		if (o + 2 > size - 4) return;
		cli_arq = ntohs(*reinterpret_cast<const uint16_t*>(data + o));
		o += 2;
	}

	// a3_Fragment: skip 6-byte frag info (no reassembly for LS)
	if (hdr0 & HDR0_FRAGMENT) {
		if (o + 6 > size - 4) return;
		o += 6;
	}

	// a4_ASQ
	if (hdr0 & HDR0_ASQ) {
		if (o + 1 > size - 4) return;
		o += 1; // asq_high
		if (has_arq) {
			if (o + 1 > size - 4) return;
			o += 1; // asq_low
		}
	}

	// Closing-bits packet (a2 && a6): disconnect
	if ((hdr0 & 0x04) && (hdr0 & 0x40)) {
		m_sessions.erase(SessionKey(ip, port_ne));
		return;
	}

	// Look up or create session — only accept new sessions with SEQStart
	uint64_t key       = SessionKey(ip, port_ne);
	bool     is_new    = (m_sessions.find(key) == m_sessions.end());
	bool     seqstart  = (hdr0 & HDR0_SEQSTART) != 0;

	if (is_new && !seqstart)
		return;

	if (is_new)
		m_sessions[key] = Session{};

	Session& s  = m_sessions[key];
	s.last_pkt  = std::time(nullptr);

	if (has_arq) {
		s.cli_arq = cli_arq;
		s.ack_due = true;
	}

	// Extract opcode + payload (remaining payload minus CRC at end)
	int remaining = size - o - 4;
	if (remaining <= 0) {
		// Pure ACK from client — acknowledge if we owe one
		if (s.ack_due)
			SendAck(sock, ip, port_ne, s);
		return;
	}

	if (o + 2 > size - 4) return;
	uint16_t opcode = ntohs(*reinterpret_cast<const uint16_t*>(data + o));
	o += 2;

	const uint8_t* payload = data + o;
	uint32_t       plen    = static_cast<uint32_t>(size - o - 4);

	OnOpcode(sock, ip, port_ne, s, opcode, payload, plen);
}

// ============================================================
// LS opcode dispatch
// ============================================================

void TrilogyLoginServer::OnOpcode(int sock, uint32_t ip, uint16_t port_ne,
                                   Session& s, uint16_t opcode,
                                   const uint8_t* payload, uint32_t plen)
{
	LogNetcode("[TrilogyLS] rx opcode 0x{:04X} plen={}", opcode, plen);

	switch (opcode) {

	case LS_OP_Version: {
		// Client requests server version string
		const char* ver = "8-09-2001 14:25";
		SendApp(sock, ip, port_ne, s, LS_OP_Version,
		        reinterpret_cast<const uint8_t*>(ver),
		        static_cast<uint32_t>(strlen(ver) + 1));
		break;
	}

	case LS_OP_LoginBanner: {
		// Client requests login banner text
		const char* banner = "Welcome to EQEmu!";
		uint8_t     buf[256]{};
		buf[0] = 1; // display flag
		// buf[1..3] = 0
		size_t blen = strlen(banner);
		memcpy(buf + 4, banner, blen + 1);
		SendApp(sock, ip, port_ne, s, LS_OP_LoginBanner,
		        buf, static_cast<uint32_t>(4 + blen + 1));
		break;
	}

	case LS_OP_LoginInfo: {
		// Client sends 40-byte DES-CBC block with Verant key/IV: username[20]+password[20].
		if (plen >= 40) {
			uint8_t plain[40] = {};
			if (trilogy_des_decrypt(payload, plain, 40)) {
				char username[21] = {};
				char password[21] = {};
				strncpy(username, reinterpret_cast<const char*>(plain),      20);
				strncpy(password, reinterpret_cast<const char*>(plain + 20), 20);
				username[20] = '\0';
				password[20] = '\0';

				LogInfo("[TrilogyLS] Login attempt for user [{}]", username);

				unsigned int account_id = 0;
				std::string  stored_hash;
				if (server.db->GetLoginDataFromAccountInfo(username, "local", stored_hash, account_id)) {
					if (eqcrypt_verify_hash(username, password, stored_hash, server.options.GetEncryptionMode())) {
						s.account_id = account_id;
						strncpy(s.account_name, username, sizeof(s.account_name) - 1);
						s.client_ip  = ip;
						LogInfo("[TrilogyLS] Login success user [{}] id [{}]", username, account_id);
					} else {
						LogInfo("[TrilogyLS] Login failed (bad password) for user [{}]", username);
					}
				} else if (server.options.CanAutoCreateAccounts()) {
					account_id = server.db->CreateLoginAccount(username, password, "local");
					if (account_id > 0) {
						s.account_id = account_id;
						strncpy(s.account_name, username, sizeof(s.account_name) - 1);
						s.client_ip  = ip;
						LogInfo("[TrilogyLS] Created account user [{}] id [{}]", username, account_id);
					}
				} else {
					LogInfo("[TrilogyLS] Unknown account [{}] and auto-create disabled", username);
				}
			}
		}

		// session_id: "ls#N" (lowercase to match what the client sends to world)
		char sid[16] = {};
		if (s.account_id > 0)
			snprintf(sid, sizeof(sid), "ls#%u", s.account_id);
		else
			strncpy(sid, "ls#0", sizeof(sid)); // will fail world auth

		uint8_t buf[21]{};
		memcpy(buf,      sid,     strlen(sid));
		memcpy(buf + 10, "unused", 6);
		buf[17] = 4; buf[18] = 0; buf[19] = 0; buf[20] = 0;
		SendApp(sock, ip, port_ne, s, LS_OP_SessionId, buf, 21);
		break;
	}

	case LS_OP_RequestServerStatus: {
		// Client checks server availability — respond with empty status
		SendApp(sock, ip, port_ne, s, LS_OP_SendServerStatus);
		break;
	}

	case LS_OP_ServerList: {
		// Client requests the world server list
		auto list = BuildServerList();
		SendApp(sock, ip, port_ne, s, LS_OP_ServerList,
		        list.data(), static_cast<uint32_t>(list.size()));
		break;
	}

	case LS_OP_SessionKey: {
		// Generate 15-char alphanumeric session key
		char key_str[16] = {};
		for (int i = 0; i < 15; ++i) {
			int y = (rand() % 62) + 48;
			if (y > 57) y += 7;  // skip ":;<=>?@"
			if (y > 90) y += 6;  // skip "[\]^_`"
			key_str[i] = static_cast<char>(y);
		}
		strncpy(s.key, key_str, sizeof(s.key) - 1);

		// Pre-authorize this session with every connected world server
		if (server.server_manager && s.account_id > 0) {
			struct in_addr addr_in;
			addr_in.s_addr = s.client_ip;
			std::string ip_str = inet_ntoa(addr_in);
			for (auto& ws : server.server_manager->getWorldServers()) {
				if (ws->IsAuthorized()) {
					ws->SendClientAuth(ip_str, s.account_name, key_str, s.account_id, "local");
					LogInfo("[TrilogyLS] SendClientAuth to [{}] for user [{}] id [{}]",
					        ws->GetServerLongName(), s.account_name, s.account_id);
				}
			}
		}

		// key packet: [0x00][15 chars][0x00]
		uint8_t key[17]{};
		memcpy(key + 1, key_str, 15);
		SendApp(sock, ip, port_ne, s, LS_OP_SessionKey, key, 17);
		break;
	}

	case LS_OP_Unknown8800: {
		// Unknown opcode — client expects a 20-byte reply with pBuffer[0]=1
		uint8_t buf[20]{};
		buf[0] = 1;
		SendApp(sock, ip, port_ne, s, LS_OP_Unknown8800, buf, 20);
		break;
	}

	case LS_OP_Unknown8E00: {
		// Client re-requests session identity — send SessionId again
		char sid[16] = {};
		if (s.account_id > 0)
			snprintf(sid, sizeof(sid), "ls#%u", s.account_id);
		else
			strncpy(sid, "ls#0", sizeof(sid));
		uint8_t buf[21]{};
		memcpy(buf,      sid,     strlen(sid));
		memcpy(buf + 10, "unused", 6);
		buf[17] = 4;
		SendApp(sock, ip, port_ne, s, LS_OP_SessionId, buf, 21);
		break;
	}

	case LS_OP_AllFinish:
		// Client signalling logout — drop session
		m_sessions.erase(SessionKey(ip, port_ne));
		break;

	default:
		// Unknown opcode: still need to ack if client requested it
		if (s.ack_due)
			SendAck(sock, ip, port_ne, s);
		break;
	}
}

// ============================================================
// Packet builders
// ============================================================

void TrilogyLoginServer::SendApp(int sock, uint32_t ip, uint16_t port_ne,
                                  Session& s, uint16_t opcode,
                                  const uint8_t* payload, uint32_t plen)
{
	// Seed SACK on the first outgoing packet from this session
	if (!s.sack_init) {
		s.sack_init = true;
		s.gsq    = 1;
		s.arq    = static_cast<uint16_t>(rand() % 0x3FFF);
		s.asq_hi = 1;
		s.asq_lo = 0;
	}

	// SEQStart only on the very first data packet
	bool first = !s.seq_sent;
	s.seq_sent = true;

	uint8_t buf[4096];
	int     o = 0;

	// HDR
	uint8_t hdr0 = HDR0_ARQ | HDR0_ASQ | (first ? HDR0_SEQSTART : 0u);
	uint8_t hdr1 = s.ack_due ? HDR1_ARSP : 0u;
	buf[o++] = hdr0;
	buf[o++] = hdr1;

	// SEQ (big-endian)
	{
		uint16_t seq = htons(s.gsq++);
		memcpy(buf + o, &seq, 2);
		o += 2;
	}

	// ARSP: include client's ARQ value if we owe an acknowledgment
	if (s.ack_due) {
		uint16_t arsp = htons(s.cli_arq);
		memcpy(buf + o, &arsp, 2);
		o += 2;
		s.ack_due = false;
	}

	// ARQ (our sequence request — client uses this to ack us)
	{
		uint16_t arq = htons(s.arq++);
		memcpy(buf + o, &arq, 2);
		o += 2;
	}

	// ASQ: both bytes present when a1_ARQ && a4_ASQ are set
	buf[o++] = s.asq_hi;
	buf[o++] = s.asq_lo++;

	// Opcode (big-endian)
	{
		uint16_t op = htons(opcode);
		memcpy(buf + o, &op, 2);
		o += 2;
	}

	// Payload
	if (plen > 0 && payload != nullptr) {
		if (static_cast<size_t>(o) + plen > sizeof(buf) - 4)
			return; // packet too large (shouldn't happen for LS)
		memcpy(buf + o, payload, plen);
		o += static_cast<int>(plen);
	}

	// CRC32 (big-endian, covers bytes [0..o-1])
	{
		uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
		memcpy(buf + o, &crc, 4);
		o += 4;
	}

	sockaddr_in to{};
	to.sin_family      = AF_INET;
	to.sin_addr.s_addr = ip;
	to.sin_port        = port_ne;

	sendto(static_cast<SOCKET>(sock),
	       reinterpret_cast<const char*>(buf), o, 0,
	       reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

void TrilogyLoginServer::SendAck(int sock, uint32_t ip, uint16_t port_ne,
                                  Session& s)
{
	// Seed SACK if this is somehow our very first packet
	if (!s.sack_init) {
		s.sack_init = true;
		s.gsq    = 1;
		s.arq    = static_cast<uint16_t>(rand() % 0x3FFF);
		s.asq_hi = 1;
		s.asq_lo = 0;
	}

	uint8_t buf[16];
	int     o = 0;

	// HDR: only ARSP set (no ARQ, no ASQ, no SEQStart)
	buf[o++] = 0x00;    // HDR0
	buf[o++] = HDR1_ARSP; // HDR1

	// SEQ
	{
		uint16_t seq = htons(s.gsq++);
		memcpy(buf + o, &seq, 2);
		o += 2;
	}

	// ARSP value = client's ARQ
	{
		uint16_t arsp = htons(s.cli_arq);
		memcpy(buf + o, &arsp, 2);
		o += 2;
	}
	s.ack_due = false;

	// CRC32
	{
		uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
		memcpy(buf + o, &crc, 4);
		o += 4;
	}

	sockaddr_in to{};
	to.sin_family      = AF_INET;
	to.sin_addr.s_addr = ip;
	to.sin_port        = port_ne;

	sendto(static_cast<SOCKET>(sock),
	       reinterpret_cast<const char*>(buf), o, 0,
	       reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// ============================================================
// Server list packet builder
// ============================================================

std::vector<uint8_t> TrilogyLoginServer::BuildServerList() const
{
	// Use the first authorized world server connected to this login server.
	// Fall back to sensible defaults if none are connected yet.
	std::string name = "EQEmu";
	std::string addr = "127.0.0.1";

	if (server.server_manager) {
		for (auto& ws : server.server_manager->getWorldServers()) {
			if (ws->IsAuthorized()) {
				const std::string& wname = ws->GetServerLongName();
				const std::string& waddr = ws->GetRemoteIP();
				if (!wname.empty()) name = wname;
				if (!waddr.empty()) addr = waddr;
				break;
			}
		}
	}

	// Binary layout:
	//   ServerList_Struct header (4 bytes)
	//   [server name \0][server addr \0][ServerListServerFlags (13 bytes)]
	//   ServerListEndFlags (25 bytes)
	//   chatserver string \0

	const size_t flags_size = 13; // greenname(1) + usercount(4) + unknown[8]
	const size_t end_size   = 25; // admin(4) + zeroes_a[8] + kunark(1) + velious(1) + zeroes_b[11]
	size_t total = 4
	             + name.size() + 1
	             + addr.size() + 1
	             + flags_size
	             + end_size
	             + 1; // chatserver null terminator

	std::vector<uint8_t> out(total, 0);
	size_t o = 0;

	// ServerList_Struct header
	out[o++] = 1;     // numservers = 1
	out[o++] = 0;     // unknown1
	out[o++] = 0;     // unknown2
	out[o++] = 0xFF;  // showusercount (0xFF = show real count, not just "UP")

	// Server name
	memcpy(out.data() + o, name.c_str(), name.size() + 1);
	o += name.size() + 1;

	// Server address
	memcpy(out.data() + o, addr.c_str(), addr.size() + 1);
	o += addr.size() + 1;

	// ServerListServerFlags_Struct: greenname(1) + usercount(4, LE) + unknown[8]
	out[o++] = 0;    // greenname = 0 (white name)
	// usercount[4] = 0 (already zero from vector init)
	o += 4;
	// unknown[8] = 0
	o += 8;

	// ServerListEndFlags_Struct: admin(4) + zeroes_a[8] + kunark(1) + velious(1) + zeroes_b[11]
	// admin[4] = 0, zeroes_a[8] = 0 (already zeroed)
	o += 4 + 8;
	out[o++] = 1;  // kunark = 1
	out[o++] = 1;  // velious = 1
	// zeroes_b[11] = 0; chatserver\0 = 0 (vector was zero-initialized)

	return out;
}
