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

#ifndef ZT_N_SWITCH_HPP
#define ZT_N_SWITCH_HPP

#include <map>
#include <set>
#include <vector>
#include <list>

#include "Constants.hpp"
#include "Mutex.hpp"
#include "MAC.hpp"
#include "NonCopyable.hpp"
#include "Packet.hpp"
#include "Utils.hpp"
#include "InetAddress.hpp"
#include "Topology.hpp"
#include "Array.hpp"
#include "Network.hpp"
#include "SharedPtr.hpp"
#include "IncomingPacket.hpp"

/* Ethernet frame types that might be relevant to us */
#define ZT_ETHERTYPE_IPV4 0x0800
#define ZT_ETHERTYPE_ARP 0x0806
#define ZT_ETHERTYPE_RARP 0x8035
#define ZT_ETHERTYPE_ATALK 0x809b
#define ZT_ETHERTYPE_AARP 0x80f3
#define ZT_ETHERTYPE_IPX_A 0x8137
#define ZT_ETHERTYPE_IPX_B 0x8138
#define ZT_ETHERTYPE_IPV6 0x86dd

namespace ZeroTier {

class RuntimeEnvironment;
class Peer;

/**
 * Core of the distributed Ethernet switch and protocol implementation
 *
 * This class is perhaps a bit misnamed, but it's basically where everything
 * meets. Transport-layer ZT packets come in here, as do virtual network
 * packets from tap devices, and this sends them where they need to go and
 * wraps/unwraps accordingly. It also handles queues and timeouts and such.
 */
class Switch : NonCopyable
{
public:
	Switch(const RuntimeEnvironment *renv);
	~Switch();

	/**
	 * Called when a packet is received from the real network
	 *
	 * @param fromAddr Internet IP address of origin
	 * @param data Packet data
	 * @param len Packet length
	 */
	void onRemotePacket(const InetAddress &fromAddr,const void *data,unsigned int len);

	/**
	 * Called when a packet comes from a local Ethernet tap
	 *
	 * @param network Which network's TAP did this packet come from?
	 * @param from Originating MAC address
	 * @param to Destination MAC address
	 * @param etherType Ethernet packet type
	 * @param vlanId VLAN ID or 0 if none
	 * @param data Ethernet payload
	 * @param len Frame length
	 */
	void onLocalEthernet(const SharedPtr<Network> &network,const MAC &from,const MAC &to,unsigned int etherType,unsigned int vlanId,const void *data,unsigned int len);

	/**
	 * Send a packet to a ZeroTier address (destination in packet)
	 *
	 * The packet must be fully composed with source and destination but not
	 * yet encrypted. If the destination peer is known the packet
	 * is sent immediately. Otherwise it is queued and a WHOIS is dispatched.
	 *
	 * The packet may be compressed. Compression isn't done here.
	 *
	 * Needless to say, the packet's source must be this node. Otherwise it
	 * won't be encrypted right. (This is not used for relaying.)
	 *
	 * The network ID should only be specified for frames and other actual
	 * network traffic. Other traffic such as controller requests and regular
	 * protocol messages should specify zero.
	 *
	 * @param packet Packet to send
	 * @param encrypt Encrypt packet payload? (always true except for HELLO)
	 * @param nwid Related network ID or 0 if message is not in-network traffic
	 */
	void send(const Packet &packet,bool encrypt,uint64_t nwid);

	/**
	 * Send RENDEZVOUS to two peers to permit them to directly connect
	 *
	 * This only works if both peers are known, with known working direct
	 * links to this peer. The best link for each peer is sent to the other.
	 *
	 * A rate limiter is in effect via the _lastUniteAttempt map. If force
	 * is true, a unite attempt is made even if one has been made less than
	 * ZT_MIN_UNITE_INTERVAL milliseconds ago.
	 *
	 * @param p1 One of two peers (order doesn't matter)
	 * @param p2 Second of pair
	 * @param force If true, send now regardless of interval
	 */
	bool unite(const Address &p1,const Address &p2,bool force);

	/**
	 * Attempt NAT traversal to peer at a given physical address
	 *
	 * @param peer Peer to contact
	 * @param atAddr Address of peer
	 */
	void rendezvous(const SharedPtr<Peer> &peer,const InetAddress &atAddr);

	/**
	 * Request WHOIS on a given address
	 *
	 * @param addr Address to look up
	 */
	void requestWhois(const Address &addr);

	/**
	 * Cancel WHOIS for an address
	 *
	 * @param addr Address to cancel
	 */
	void cancelWhoisRequest(const Address &addr);

	/**
	 * Run any processes that are waiting for this peer's identity
	 *
	 * Called when we learn of a peer's identity from HELLO, OK(WHOIS), etc.
	 *
	 * @param peer New peer
	 */
	void doAnythingWaitingForPeer(const SharedPtr<Peer> &peer);

	/**
	 * Perform retries and other periodic timer tasks
	 *
	 * This can return a very long delay if there are no pending timer
	 * tasks. The caller should cap this comparatively vs. other values.
	 *
	 * @param now Current time
	 * @return Number of milliseconds until doTimerTasks() should be run again
	 */
	unsigned long doTimerTasks(uint64_t now);

private:
	void _handleRemotePacketFragment(const InetAddress &fromAddr,const void *data,unsigned int len);
	void _handleRemotePacketHead(const InetAddress &fromAddr,const void *data,unsigned int len);
	Address _sendWhoisRequest(const Address &addr,const Address *peersAlreadyConsulted,unsigned int numPeersAlreadyConsulted);
	bool _trySend(const Packet &packet,bool encrypt,uint64_t nwid);

	const RuntimeEnvironment *const RR;
	uint64_t _lastBeaconResponse;

	// Outsanding WHOIS requests and how many retries they've undergone
	struct WhoisRequest
	{
		uint64_t lastSent;
		Address peersConsulted[ZT_MAX_WHOIS_RETRIES]; // by retry
		unsigned int retries; // 0..ZT_MAX_WHOIS_RETRIES
	};
	std::map< Address,WhoisRequest > _outstandingWhoisRequests;
	Mutex _outstandingWhoisRequests_m;

	// Packet defragmentation queue -- comes before RX queue in path
	struct DefragQueueEntry
	{
		uint64_t creationTime;
		SharedPtr<IncomingPacket> frag0;
		Packet::Fragment frags[ZT_MAX_PACKET_FRAGMENTS - 1];
		unsigned int totalFragments; // 0 if only frag0 received, waiting for frags
		uint32_t haveFragments; // bit mask, LSB to MSB
	};
	std::map< uint64_t,DefragQueueEntry > _defragQueue;
	Mutex _defragQueue_m;

	// ZeroTier-layer RX queue of incoming packets in the process of being decoded
	std::vector< SharedPtr<IncomingPacket> > _rxQueue;
	Mutex _rxQueue_m;

	// ZeroTier-layer TX queue by destination ZeroTier address
	struct TXQueueEntry
	{
		TXQueueEntry() {}
		TXQueueEntry(uint64_t ct,const Packet &p,bool enc,uint64_t nw) :
			creationTime(ct),
			nwid(nw),
			packet(p),
			encrypt(enc) {}

		uint64_t creationTime;
		uint64_t nwid;
		Packet packet; // unencrypted/unMAC'd packet -- this is done at send time
		bool encrypt;
	};
	std::multimap< Address,TXQueueEntry > _txQueue;
	Mutex _txQueue_m;

	// Tracks sending of VERB_RENDEZVOUS to relaying peers
	std::map< Array< Address,2 >,uint64_t > _lastUniteAttempt; // key is always sorted in ascending order, for set-like behavior
	Mutex _lastUniteAttempt_m;

	// Active attempts to contact remote peers, including state of multi-phase NAT traversal
	struct ContactQueueEntry
	{
		ContactQueueEntry() {}
		ContactQueueEntry(const SharedPtr<Peer> &p,uint64_t ft,const InetAddress &a) :
			peer(p),
			fireAtTime(ft),
			inaddr(a),
			strategyIteration(0) {}

		SharedPtr<Peer> peer;
		uint64_t fireAtTime;
		InetAddress inaddr;
		unsigned int strategyIteration;
	};
	std::vector<ContactQueueEntry> _contactQueue;
	Mutex _contactQueue_m;
};

} // namespace ZeroTier

#endif
