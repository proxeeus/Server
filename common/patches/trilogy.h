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

#ifndef COMMON_TRILOGY_H
#define COMMON_TRILOGY_H

#include "../struct_strategy.h"

class EQStreamIdentifier;

namespace Trilogy
{

	// Public interface — called from patches.cpp
	extern void Register(EQStreamIdentifier &into);
	extern void Reload();

	// Strategy class — inherits StructStrategy, registered as the
	// encoder/decoder for all streams identified as Trilogy clients.
	class Strategy : public StructStrategy {
	public:
		Strategy();

	protected:
		virtual std::string Describe() const;
		virtual const EQ::versions::ClientVersion ClientVersion() const;

		// Declares Encode_*/Decode_* methods listed in trilogy_ops.h
		#include "ss_declare.h"
		#include "trilogy_ops.h"
	};

} /*Trilogy*/

#endif /*COMMON_TRILOGY_H*/
