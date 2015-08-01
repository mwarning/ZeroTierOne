/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2015  ZeroTier, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#include <algorithm>

#include "Constants.hpp"
#include "RuntimeEnvironment.hpp"
#include "SharedPtr.hpp"
#include "Multicaster.hpp"
#include "Topology.hpp"
#include "Switch.hpp"
#include "Packet.hpp"
#include "Peer.hpp"
#include "C25519.hpp"
#include "CertificateOfMembership.hpp"

namespace ZeroTier {

Multicaster::Multicaster(const RuntimeEnvironment *renv) :
	RR(renv)
{
}

Multicaster::~Multicaster()
{
}

void Multicaster::addMultiple(uint64_t now,uint64_t nwid,const MulticastGroup &mg,const void *addresses,unsigned int count,unsigned int totalKnown)
{
	const unsigned char *p = (const unsigned char *)addresses;
	const unsigned char *e = p + (5 * count);
	Mutex::Lock _l(_groups_m);

	if(_groups.size() > 1600)
		return;

	MulticastGroupStatus &gs = _groups.getGroup(nwid,mg);
	while (p != e) {
		_add(now,nwid,mg,gs,Address(p,5));
		p += 5;
	}
}

void Multicaster::remove(uint64_t nwid,const MulticastGroup &mg,const Address &member)
{
	Mutex::Lock _l(_groups_m);
	MGroups::iterator g(_groups.findGroup(nwid,mg));
	if (g != _groups.end()) {
		for(std::vector<MulticastGroupMember>::iterator m(g->members.begin());m!=g->members.end();++m) {
			if (m->address == member) {
				g->members.erase(m);
				break;
			}
		}
	}
}

unsigned int Multicaster::gather(const Address &queryingPeer,uint64_t nwid,const MulticastGroup &mg,Packet &appendTo,unsigned int limit) const
{
	unsigned char *p;
	unsigned int added = 0,i,k,rptr,totalKnown = 0;
	uint64_t a,picked[(ZT_PROTO_MAX_PACKET_LENGTH / 5) + 2];

	if (!limit)
		return 0;
	else if (limit > 0xffff)
		limit = 0xffff;

	const unsigned int totalAt = appendTo.size();
	appendTo.addSize(4); // sizeof(uint32_t)
	const unsigned int addedAt = appendTo.size();
	appendTo.addSize(2); // sizeof(uint16_t)

	{ // Return myself if I am a member of this group
		SharedPtr<Network> network(RR->node->network(nwid));
		if ((network)&&(network->subscribedToMulticastGroup(mg,true))) {
			RR->identity.address().appendTo(appendTo);
			++totalKnown;
			++added;
		}
	}

	Mutex::Lock _l(_groups_m);

	MGroups::const_iterator gs(_groups.findGroup(nwid,mg));
	if ((gs != _groups.end())&&(!gs->members.empty())) {
		totalKnown += (unsigned int)gs->members.size();

		// Members are returned in random order so that repeated gather queries
		// will return different subsets of a large multicast group.
		k = 0;
		while ((added < limit)&&(k < gs->members.size())&&((appendTo.size() + ZT_ADDRESS_LENGTH) <= ZT_UDP_DEFAULT_PAYLOAD_MTU)) {
			rptr = (unsigned int)RR->node->prng();

restart_member_scan:
			a = gs->members[rptr % (unsigned int)gs->members.size()].address.toInt();
			for(i=0;i<k;++i) {
				if (picked[i] == a) {
					++rptr;
					goto restart_member_scan;
				}
			}
			picked[k++] = a;

			if (queryingPeer.toInt() != a) { // do not return the peer that is making the request as a result
				p = (unsigned char *)appendTo.appendField(ZT_ADDRESS_LENGTH);
				*(p++) = (unsigned char)((a >> 32) & 0xff);
				*(p++) = (unsigned char)((a >> 24) & 0xff);
				*(p++) = (unsigned char)((a >> 16) & 0xff);
				*(p++) = (unsigned char)((a >> 8) & 0xff);
				*p = (unsigned char)(a & 0xff);
				++added;
			}
		}
	}

	appendTo.setAt(totalAt,(uint32_t)totalKnown);
	appendTo.setAt(addedAt,(uint16_t)added);

	//TRACE("..MC Multicaster::gather() attached %u of %u peers for %.16llx/%s (2)",n,(unsigned int)(gs->second.members.size() - skipped),nwid,mg.toString().c_str());

	return added;
}

std::vector<Address> Multicaster::getMembers(uint64_t nwid,const MulticastGroup &mg,unsigned int limit)
{
	std::vector<Address> ls;
	Mutex::Lock _l(_groups_m);
	MGroups::const_iterator gs(_groups.findGroup(nwid,mg));
	if (gs == _groups.end())
		return ls;
	for(std::vector<MulticastGroupMember>::reverse_iterator m(gs->members.rbegin());m!=gs->members.rend();++m) {
		ls.push_back(m->address);
		if (ls.size() >= limit)
			break;
	}
	return ls;
}

void Multicaster::send(
	const CertificateOfMembership *com,
	unsigned int limit,
	uint64_t now,
	uint64_t nwid,
	const std::vector<Address> &alwaysSendTo,
	const MulticastGroup &mg,
	const MAC &src,
	unsigned int etherType,
	const void *data,
	unsigned int len)
{
	unsigned long idxbuf[8194];
	unsigned long *indexes = idxbuf;

	Mutex::Lock _l(_groups_m);
	MulticastGroupStatus &gs = _groups.getGroup(nwid,mg);

	if (!gs.members.empty()) {
		// Allocate a memory buffer if group is monstrous
		if (gs.members.size() > (sizeof(idxbuf) / sizeof(unsigned long)))
			indexes = new unsigned long[gs.members.size()];

		// Generate a random permutation of member indexes
		for(unsigned long i=0;i<gs.members.size();++i)
			indexes[i] = i;
		for(unsigned long i=(unsigned long)gs.members.size()-1;i>0;--i) {
			unsigned long j = (unsigned long)RR->node->prng() % (i + 1);
			unsigned long tmp = indexes[j];
			indexes[j] = indexes[i];
			indexes[i] = tmp;
		}
	}

	if (gs.members.size() >= limit) {
		// Skip queue if we already have enough members to complete the send operation
		OutboundMulticast out;

		out.init(
			RR,
			now,
			nwid,
			com,
			limit,
			1, // we'll still gather a little from peers to keep multicast list fresh
			src,
			mg,
			etherType,
			data,
			len);

		unsigned int count = 0;

		for(std::vector<Address>::const_iterator ast(alwaysSendTo.begin());ast!=alwaysSendTo.end();++ast) {
			if (*ast != RR->identity.address()) {
				out.sendOnly(RR,*ast);
				if (++count >= limit)
					break;
			}
		}

		unsigned long idx = 0;
		while ((count < limit)&&(idx < gs.members.size())) {
			Address ma(gs.members[indexes[idx++]].address);
			if (std::find(alwaysSendTo.begin(),alwaysSendTo.end(),ma) == alwaysSendTo.end()) {
				out.sendOnly(RR,ma);
				++count;
			}
		}
	} else {
		unsigned int gatherLimit = (limit - (unsigned int)gs.members.size()) + 1;

		if ((now - gs.lastExplicitGather) >= ZT_MULTICAST_EXPLICIT_GATHER_DELAY) {
			gs.lastExplicitGather = now;
			SharedPtr<Peer> sn(RR->topology->getBestRoot());
			if (sn) {
				TRACE(">>MC upstream GATHER up to %u for group %.16llx/%s",gatherLimit,nwid,mg.toString().c_str());

				Packet outp(sn->address(),RR->identity.address(),Packet::VERB_MULTICAST_GATHER);
				outp.append(nwid);
				outp.append((uint8_t)0);
				mg.mac().appendTo(outp);
				outp.append((uint32_t)mg.adi());
				outp.append((uint32_t)gatherLimit);
				outp.armor(sn->key(),true);
				sn->send(RR,outp.data(),outp.size(),now);
			}
			gatherLimit = 0;
		}

		gs.txQueue.push_back(OutboundMulticast());
		OutboundMulticast &out = gs.txQueue.back();

		out.init(
			RR,
			now,
			nwid,
			com,
			limit,
			gatherLimit,
			src,
			mg,
			etherType,
			data,
			len);

		unsigned int count = 0;

		for(std::vector<Address>::const_iterator ast(alwaysSendTo.begin());ast!=alwaysSendTo.end();++ast) {
			if (*ast != RR->identity.address()) {
				out.sendAndLog(RR,*ast);
				if (++count >= limit)
					break;
			}
		}

		unsigned long idx = 0;
		while ((count < limit)&&(idx < gs.members.size())) {
			Address ma(gs.members[indexes[idx++]].address);
			if (std::find(alwaysSendTo.begin(),alwaysSendTo.end(),ma) == alwaysSendTo.end()) {
				out.sendAndLog(RR,ma);
				++count;
			}
		}
	}

	// Free allocated memory buffer if any
	if (indexes != idxbuf)
		delete [] indexes;
}

void Multicaster::clean(uint64_t now)
{
	Mutex::Lock _l(_groups_m);
	for(MGroups::iterator mm(_groups.begin());mm!=_groups.end();) {
		for(std::list<OutboundMulticast>::iterator tx(mm->txQueue.begin());tx!=mm->txQueue.end();) {
			if ((tx->expired(now))||(tx->atLimit()))
				mm->txQueue.erase(tx++);
			else ++tx;
		}

		unsigned long count = 0;
		{
			std::vector<MulticastGroupMember>::iterator reader(mm->members.begin());
			std::vector<MulticastGroupMember>::iterator writer(reader);
			while (reader != mm->members.end()) {
				if ((now - reader->timestamp) < ZT_MULTICAST_LIKE_EXPIRE) {
					*writer = *reader;
					++writer;
					++count;
				}
				++reader;
			}
		}

		if (count) {
			mm->members.resize(count);
			++mm;
		} else if (mm->txQueue.empty()) {
			_groups.eraseGroup(mm++);
		} else {
			mm->members.clear();
			++mm;
		}
	}
	_groups.compact();
}

void Multicaster::_add(uint64_t now,uint64_t nwid,const MulticastGroup &mg,MulticastGroupStatus &gs,const Address &member)
{
	// assumes _groups_m is locked

	if(_groups.size() > 1600)
		return;

	// Do not add self -- even if someone else returns it
	if (member == RR->identity.address())
		return;

	for(std::vector<MulticastGroupMember>::iterator m(gs.members.begin());m!=gs.members.end();++m) {
		if (m->address == member) {
			m->timestamp = now;
			return;
		}
	}

	gs.members.push_back(MulticastGroupMember(member,now));

	//TRACE("..MC %s joined multicast group %.16llx/%s via %s",member.toString().c_str(),nwid,mg.toString().c_str(),((learnedFrom) ? learnedFrom.toString().c_str() : "(direct)"));

	for(std::list<OutboundMulticast>::iterator tx(gs.txQueue.begin());tx!=gs.txQueue.end();) {
		if (tx->atLimit())
			gs.txQueue.erase(tx++);
		else {
			tx->sendIfNew(RR,member);
			if (tx->atLimit())
				gs.txQueue.erase(tx++);
			else ++tx;
		}
	}
}

} // namespace ZeroTier
