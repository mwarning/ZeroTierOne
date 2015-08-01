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

#ifndef ZT_MULTICASTER_HPP
#define ZT_MULTICASTER_HPP

#include <stdint.h>
#include <string.h>

#include <map>
#include <vector>
#include <list>

#include "Constants.hpp"
#include "Address.hpp"
#include "MAC.hpp"
#include "MulticastGroup.hpp"
#include "OutboundMulticast.hpp"
#include "Utils.hpp"
#include "Mutex.hpp"
#include "NonCopyable.hpp"

namespace ZeroTier {

class RuntimeEnvironment;
class CertificateOfMembership;
class Packet;

/**
 * Database of known multicast peers within a network
 */
class Multicaster : NonCopyable
{
private:
	struct MulticastGroupMember
	{
		MulticastGroupMember() {}
		MulticastGroupMember(const Address &a,uint64_t ts) : address(a),timestamp(ts) {}

		Address address;
		uint64_t timestamp; // time of last notification
	};

	struct MulticastGroupStatus
	{
		MulticastGroupStatus() : lastExplicitGather(0) {}

		uint64_t lastExplicitGather;
		std::vector<OutboundMulticast*> txQueue; // pending outbound multicasts
		std::vector<MulticastGroupMember> members; // members of this group
	};

public:
	Multicaster(const RuntimeEnvironment *renv);
	~Multicaster();

	/**
	 * Add or update a member in a multicast group
	 *
	 * @param now Current time
	 * @param nwid Network ID
	 * @param mg Multicast group
	 * @param member New member address
	 */
	inline void add(uint64_t now,uint64_t nwid,const MulticastGroup &mg,const Address &member)
	{
		Mutex::Lock _l(_groups_m);
		_add(now,nwid,mg,_groups[std::pair<uint64_t,MulticastGroup>(nwid,mg)],member);
	}

	/**
	 * Add multiple addresses from a binary array of 5-byte address fields
	 *
	 * It's up to the caller to check bounds on the array before calling this.
	 *
	 * @param now Current time
	 * @param nwid Network ID
	 * @param mg Multicast group
	 * @param addresses Raw binary addresses in big-endian format, as a series of 5-byte fields
	 * @param count Number of addresses
	 * @param totalKnown Total number of known addresses as reported by peer
	 */
	void addMultiple(uint64_t now,uint64_t nwid,const MulticastGroup &mg,const void *addresses,unsigned int count,unsigned int totalKnown);

	/**
	 * Remove a multicast group member (if present)
	 *
	 * @param nwid Network ID
	 * @param mg Multicast group
	 * @param member Member to unsubscribe
	 */
	void remove(uint64_t nwid,const MulticastGroup &mg,const Address &member);

	/**
	 * Append gather results to a packet by choosing registered multicast recipients at random
	 *
	 * This appends the following fields to the packet:
	 *   <[4] 32-bit total number of known members in this multicast group>
	 *   <[2] 16-bit number of members enumerated in this packet>
	 *   <[...] series of 5-byte ZeroTier addresses of enumerated members>
	 *
	 * If zero is returned, the first two fields will still have been appended.
	 *
	 * @param queryingPeer Peer asking for gather (to skip in results)
	 * @param nwid Network ID
	 * @param mg Multicast group
	 * @param appendTo Packet to append to
	 * @param limit Maximum number of 5-byte addresses to append
	 * @return Number of addresses appended
	 * @throws std::out_of_range Buffer overflow writing to packet
	 */
	unsigned int gather(const Address &queryingPeer,uint64_t nwid,const MulticastGroup &mg,Packet &appendTo,unsigned int limit) const;

	/**
	 * Get subscribers to a multicast group
	 *
	 * @param nwid Network ID
	 * @param mg Multicast group
	 */
	std::vector<Address> getMembers(uint64_t nwid,const MulticastGroup &mg,unsigned int limit) const;

	/**
	 * Send a multicast
	 *
	 * @param com Certificate of membership to include or NULL for none
	 * @param limit Multicast limit
	 * @param now Current time
	 * @param nwid Network ID
	 * @param alwaysSendTo Send to these peers first and even if not included in subscriber list
	 * @param mg Multicast group
	 * @param src Source Ethernet MAC address or NULL to skip in packet and compute from ZT address (non-bridged mode)
	 * @param etherType Ethernet frame type
	 * @param data Packet data
	 * @param len Length of packet data
	 */
	void send(
		const CertificateOfMembership *com,
		unsigned int limit,
		uint64_t now,
		uint64_t nwid,
		const std::vector<Address> &alwaysSendTo,
		const MulticastGroup &mg,
		const MAC &src,
		unsigned int etherType,
		const void *data,
		unsigned int len);

	/**
	 * Clean up and resort database
	 *
	 * @param RR Runtime environment
	 * @param now Current time
	 */
	void clean(uint64_t now);

	void printAll()  {
		std::map< std::pair<uint64_t,MulticastGroup>,MulticastGroupStatus >::const_iterator iter;
		for(iter = _groups.begin(); iter != _groups.end(); ++iter) {
			uint64_t nwid = iter->first.first;
			const MulticastGroup &mg = iter->first.second;
			const MulticastGroupStatus &mgs = iter->second;
			const std::vector<OutboundMulticast*> &txQueue = mgs.txQueue; // pending outbound multicasts
			const std::vector<MulticastGroupMember> &members = mgs.members;
			std::cout << "  nwid: "<< nwid << ", MulticastGroup: " << mg.toString() << std::endl;
			std::cout << "  txQueue.size(): "<< txQueue.size() << " (" << txQueue.capacity() << ")" << std::endl;
			std::cout << "  members.size(): " << members.size() << " (" << members.capacity() << "), mgs.lastExplicitGather: "<< mgs.lastExplicitGather << std::endl;

			/*std::cout << "    txQueue:" << std::endl;
			std::vector<OutboundMulticast*>::const_iterator iom;
			for(iom = txQueue.begin(); iom != txQueue.end(); ++iom) {
				std::cout << "      _alreadySentTo: " << (*iom)->_alreadySentTo.size() << std::endl;
			}*/
		}
		std::cout << "Multicaster::_groups.size(): " <<  _groups.size() << std::endl;
		std::cout << "Multicaster::free_oms.size(): "<< free_oms.size() << " (" << oms_allocated << ", " << oms_recycled << ")" << std::endl;
	}

	void putFreeOutboundMulticast(OutboundMulticast *om) {
		free_oms.push_back(om);
	}

	OutboundMulticast *getFreeOutboundMulticast() {
		if(free_oms.empty()) {
			oms_allocated++;
			return new OutboundMulticast();
		}
		oms_recycled++;
		OutboundMulticast *tmp = free_oms.back();
		free_oms.pop_back();
		return tmp;
	}

private:
	int oms_allocated;
	int oms_recycled;
	std::vector<OutboundMulticast*> free_oms;
	void _add(uint64_t now,uint64_t nwid,const MulticastGroup &mg,MulticastGroupStatus &gs,const Address &member);

	const RuntimeEnvironment *RR;
	std::map< std::pair<uint64_t,MulticastGroup>,MulticastGroupStatus > _groups;
	Mutex _groups_m;
};

} // namespace ZeroTier

#endif
