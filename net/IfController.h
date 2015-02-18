/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef IfController_H
#define IfController_H

#include "benc/String.h"
#include "crypto/CryptoAuth.h"
#include "dht/Address.h"
#include "interface/Iface.h"
#include "memory/Allocator.h"
#include "switch/SwitchCore.h"
#include "net/SwitchPinger.h"
#include "net/EventEmitter.h"
#include "util/platform/Sockaddr.h"
#include "util/log/Log.h"
#include "util/Linker.h"
Linker_require("net/IfController.c")

#include <stdint.h>
#include <stdbool.h>

enum IfController_PeerState
{
    /**
     * In state >= NEW, a valid packet has been received but it could still be a replay.
     * Or it's an outgoing connection so we don't care about authentication.
     */
    IfController_PeerState_NEW = CryptoAuth_NEW,

    IfController_PeerState_HANDSHAKE1 = CryptoAuth_HANDSHAKE1,

    IfController_PeerState_HANDSHAKE2 = CryptoAuth_HANDSHAKE2,

    IfController_PeerState_HANDSHAKE3 = CryptoAuth_HANDSHAKE3,

    /** In state == ESTABLISHED, we know the node at the other end is authentic. */
    IfController_PeerState_ESTABLISHED = CryptoAuth_ESTABLISHED,

    /** If state == UNRESPONSIVE, the peer has not responded to pings in the required timeframe. */
    IfController_PeerState_UNRESPONSIVE = -1,

    /** If state is UNAUTHENTICATED, the other node has not sent a single valid packet. */
    IfController_PeerState_UNAUTHENTICATED = -2,
};
Assert_compileTime(CryptoAuth_STATE_COUNT == 5);

static inline char* IfController_stateString(enum IfController_PeerState ps)
{
    switch (ps) {
        case IfController_PeerState_NEW: return "NEW";
        case IfController_PeerState_HANDSHAKE1: return "HANDSHAKE1";
        case IfController_PeerState_HANDSHAKE2: return "HANDSHAKE2";
        case IfController_PeerState_HANDSHAKE3: return "HANDSHAKE3";
        case IfController_PeerState_ESTABLISHED: return "ESTABLISHED";
        case IfController_PeerState_UNRESPONSIVE: return "UNRESPONSIVE";
        case IfController_PeerState_UNAUTHENTICATED: return "UNAUTHENTICATED";
        default: return "INVALID";
    }
}

/**
 * Stats about a peer
 */
struct IfController_PeerStats
{
    struct Address addr;
    int state;
    uint64_t timeOfLastMessage;
    uint64_t bytesOut;
    uint64_t bytesIn;
    bool isIncomingConnection;
    String* user;

    /** Packet loss/duplication statistics. see: ReplayProtector */
    uint32_t duplicates;
    uint32_t lostPackets;
    uint32_t receivedOutOfRange;
};

struct IfController
{
    int unused;
};

struct IfController_Iface
{
    struct Iface addrIf;

    /** Interface number within IfController. */
    int ifNum;
};

/**
 * Register an Ethernet-like interface.
 * Ethernet-like means the interface is capable of sending messages to one or more nodes
 * and differentiates between them using an address.
 *
 * @param ifc the interface controller
 * @param name a string which will identify this interface
 * @param alloc an allocator, the interface will be removed when this is freed.
 */
struct IfController_Iface* IfController_newIface(struct IfController* ifc,
                                                 String* name,
                                                 struct Allocator* alloc);

/**
 * Add a new peer.
 * Called from the network interface when it is asked to make a connection or it autoconnects.
 * If the peer which is connected to becomes unresponsive, IC will *not* remove it but will
 * set it's state to UNRESPONSIVE and it is the job of the caller to remove the peer by freeing
 * the allocator which is provided with iface.
 *
 * @param ifc the interface controller.
 * @param interfaceNumber a number for the interface to use, see regIface.
 * @param herPublicKey the public key of the foreign node, NULL if unknown.
 * @param lladdr the link level address, must be the size given by the interface for interfaceNumber
 * @param password the password for authenticating with the other node.
 * @param alloc the peer will be dropped if this is freed.
 *
 * @return 0 if all goes well.
 *         IfController_bootstrapPeer_BAD_IFNUM if there is no such interface for this num.
 *         IfController_bootstrapPeer_OUT_OF_SPACE if there is no space to store the peer.
 *         IfController_bootstrapPeer_BAD_KEY the provided herPublicKey is not valid.
 *         IfController_bootstrapPeer_INTERNAL unspecified error.
 */
#define IfController_bootstrapPeer_BAD_IFNUM    -1
#define IfController_bootstrapPeer_BAD_KEY      -2
#define IfController_bootstrapPeer_OUT_OF_SPACE -3
#define IfController_bootstrapPeer_INTERNAL     -4
int IfController_bootstrapPeer(struct IfController* ifc,
                               int interfaceNumber,
                               uint8_t* herPublicKey,
                               const struct Sockaddr* lladdr,
                               String* password,
                               struct Allocator* alloc);

#define IfController_beaconState_newState_OFF    0
#define IfController_beaconState_newState_ACCEPT 1
#define IfController_beaconState_newState_SEND   2
#define IfController_beaconState_NO_SUCH_IFACE -1
#define IfController_beaconState_INVALID_STATE -2
int IfController_beaconState(struct IfController* ifc, int interfaceNumber, int newState);

/**
 * Disconnect a previously registered peer.
 *
 * @param ic the if controller
 * @param herPublicKey the public key of the foreign node
 * @retrun 0 if all goes well.
 *         IfController_disconnectPeer_NOTFOUND if no peer with herPublicKey is found.
 */
#define IfController_disconnectPeer_NOTFOUND -1
int IfController_disconnectPeer(struct IfController* ifController, uint8_t herPublicKey[32]);

/**
 * Get stats for the connected peers.
 *
 * @params ic the if controller
 * @params alloc the Allocator to use for the peerStats array in statsOut
 * @params statsOut pointer to the IfController_peerStats array
 * @return the number of IfController_peerStats in statsOut
 */
int IfController_getPeerStats(struct IfController* ic,
                              struct Allocator* alloc,
                              struct IfController_PeerStats** statsOut);

struct IfController* IfController_new(struct CryptoAuth* ca,
                                      struct SwitchCore* switchCore,
                                      struct Log* logger,
                                      struct EventBase* eventBase,
                                      struct SwitchPinger* switchPinger,
                                      struct Random* rand,
                                      struct Allocator* allocator,
                                      struct EventEmitter* ee);

#endif
