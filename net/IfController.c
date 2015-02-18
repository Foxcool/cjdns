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
#include "crypto/AddressCalc.h"
#include "crypto/CryptoAuth_pvt.h"
#include "interface/Iface.h"
#include "net/IfController.h"
#include "memory/Allocator.h"
#include "net/SwitchPinger.h"
#include "wire/PFChan.h"
#include "net/EventEmitter.h"
#include "util/Base32.h"
#include "util/Bits.h"
#include "util/events/Time.h"
#include "util/events/Timeout.h"
#include "util/Identity.h"
#include "util/version/Version.h"
#include "util/AddrTools.h"
#include "util/Defined.h"
#include "util/Checksum.h"
#include "util/Hex.h"
#include "wire/Error.h"
#include "wire/Message.h"

#include <stddef.h> // offsetof

/** After this number of milliseconds, a node will be regarded as unresponsive. */
#define UNRESPONSIVE_AFTER_MILLISECONDS (20*1024)

/**
 * After this number of milliseconds without a valid incoming message,
 * a peer is "lazy" and should be pinged.
 */
#define PING_AFTER_MILLISECONDS (3*1024)

/** How often to ping "lazy" peers, "unresponsive" peers are only pinged 20% of the time. */
#define PING_INTERVAL_MILLISECONDS 1024

/** The number of milliseconds to wait for a ping response. */
#define TIMEOUT_MILLISECONDS (2*1024)

/**
 * The number of seconds to wait before an unresponsive peer
 * making an incoming connection is forgotten.
 */
#define FORGET_AFTER_MILLISECONDS (256*1024)

/** Wait 32 seconds between sending beacon messages. */
#define BEACON_INTERVAL 32768


// ---------------- Map ----------------
#define Map_NAME EndpointsBySockaddr
#define Map_ENABLE_HANDLES
#define Map_KEY_TYPE struct Sockaddr*
#define Map_VALUE_TYPE struct Peer*
#define Map_USE_HASH
#define Map_USE_COMPARATOR
#include "util/Map.h"
static inline uint32_t Map_EndpointsBySockaddr_hash(struct Sockaddr** key)
{
    return Checksum_engine((uint8_t*) &(key[0][1]), key[0]->addrLen - Sockaddr_OVERHEAD);
}
static inline int Map_EndpointsBySockaddr_compare(struct Sockaddr** keyA, struct Sockaddr** keyB)
{
    return Bits_memcmp((uint8_t*) *keyA, (uint8_t*) *keyB, keyA[0]->addrLen);
}
// ---------------- EndMap ----------------

#define ArrayList_TYPE struct IfController_Iface_pvt
#define ArrayList_NAME OfIfaces
#include "util/ArrayList.h"

struct IfController_pvt;

struct IfController_Iface_pvt
{
    struct IfController_Iface pub;
    String* name;
    int beaconState;
    struct Map_EndpointsBySockaddr peerMap;
    struct IfController_pvt* ic;
    struct Iface* addrIface;
    struct Allocator* alloc;
    Identity
};

struct Peer
{
    /** The interface which is registered with the switch. */
    struct Iface switchIf;

    /** Between CryptoAuth and external, needed to add address to message. */
    struct Iface externalIf;

    /** The internal (wrapped by CryptoAuth) interface. */
    struct Iface* cryptoAuthIf;

    /** The interface which this peer belongs to. */
    struct IfController_Iface_pvt* ici;

    /** The address within the interface of this peer. */
    struct Sockaddr* lladdr;

    struct Address addr;

    /** Milliseconds since the epoch when the last *valid* message was received. */
    uint64_t timeOfLastMessage;

    /** Time when the last switch ping response was received from this node. */
    uint64_t timeOfLastPing;

    /** A counter to allow for 3/4 of all pings to be skipped when a node is definitely down. */
    uint32_t pingCount;

    /** The handle which can be used to look up this endpoint in the endpoint set. */
    uint32_t handle;

    /** True if we should forget about the peer if they do not respond. */
    bool isIncomingConnection;

    /**
     * If IfController_PeerState_UNAUTHENTICATED, no permanent state will be kept.
     * During transition from HANDSHAKE to ESTABLISHED, a check is done for a registeration of a
     * node which is already registered in a different switch slot, if there is one and the
     * handshake completes, it will be moved.
     */
    enum IfController_PeerState state;

    // traffic counters
    uint64_t bytesOut;
    uint64_t bytesIn;

    Identity
};

struct IfController_pvt
{
    /** Public functions and fields for this ifcontroller. */
    struct IfController pub;

    struct Allocator* const allocator;

    struct CryptoAuth* const ca;

    /** Switch for adding nodes when they are discovered. */
    struct SwitchCore* const switchCore;

    struct Random* const rand;

    struct Log* const logger;

    struct EventBase* const eventBase;

    /** For communicating with the Pathfinder. */
    struct Iface eventEmitterIf;

    /** After this number of milliseconds, a neoghbor will be regarded as unresponsive. */
    uint32_t unresponsiveAfterMilliseconds;

    /** The number of milliseconds to wait before pinging. */
    uint32_t pingAfterMilliseconds;

    /** The number of milliseconds to let a ping go before timing it out. */
    uint32_t timeoutMilliseconds;

    /** After this number of milliseconds, an incoming connection is forgotten entirely. */
    uint32_t forgetAfterMilliseconds;

    /** How often to send beacon messages (milliseconds). */
    uint32_t beaconInterval;

    /** The timeout event to use for pinging potentially unresponsive neighbors. */
    struct Timeout* const pingInterval;

    /** For pinging lazy/unresponsive nodes. */
    struct SwitchPinger* const switchPinger;

    struct ArrayList_OfIfaces* icis;

    /** A password which is generated per-startup and sent out in beacon messages. */
    uint8_t beaconPassword[Headers_Beacon_PASSWORD_LEN];

    struct Headers_Beacon beacon;

    Identity
};

static void sendPeer(struct IfController_pvt* ic,
                     uint32_t pathfinderId,
                     enum PFChan_Core ev,
                     struct Peer* peer,
                     struct Allocator* parentAlloc)
{
    struct Allocator* alloc = Allocator_child(parentAlloc);
    struct Message* msg = Message_new(PFChan_Node_SIZE, 512, alloc);
    struct PFChan_Node* node = (struct PFChan_Node*) msg->bytes;
    Bits_memcpyConst(node->ip6, peer->addr.ip6.bytes, 16);
    Bits_memcpyConst(node->publicKey, peer->addr.key, 32);
    node->path_be = Endian_hostToBigEndian64(peer->addr.path);
    node->metric_be = 0xffffffff;
    node->version_be = Endian_hostToBigEndian32(peer->addr.protocolVersion);
    Message_push32(msg, pathfinderId, NULL);
    Message_push32(msg, ev, NULL);
    Iface_send(&ic->eventEmitterIf, msg);
    Allocator_free(alloc);
}

static void onPingResponse(struct SwitchPinger_Response* resp, void* onResponseContext)
{
    if (SwitchPinger_Result_OK != resp->res) {
        return;
    }
    struct Peer* ep = Identity_check((struct Peer*) onResponseContext);
    struct IfController_pvt* ic = Identity_check(ep->ici->ic);

    ep->addr.protocolVersion = resp->version;

    if (Defined(Log_DEBUG)) {
        String* addr = Address_toString(&ep->addr, resp->ping->pingAlloc);
        if (!Version_isCompatible(Version_CURRENT_PROTOCOL, resp->version)) {
            Log_debug(ic->logger, "got switch pong from node [%s] with incompatible version",
                                  addr->bytes);
        } else if (ep->addr.path != resp->label) {
            uint8_t sl[20];
            AddrTools_printPath(sl, resp->label);
            Log_debug(ic->logger, "got switch pong from node [%s] mismatch label [%s]",
                                  addr->bytes, sl);
        } else {
            Log_debug(ic->logger, "got switch pong from node [%s]", addr->bytes);
        }
    }

    if (!Version_isCompatible(Version_CURRENT_PROTOCOL, resp->version)) {
        return;
    }

    if (ep->state == IfController_PeerState_ESTABLISHED) {
        sendPeer(ic, 0xffffffff, PFChan_Core_PEER, ep, ep->externalIf.allocator);
    }

    ep->timeOfLastPing = Time_currentTimeMilliseconds(ic->eventBase);

    if (Defined(Log_DEBUG)) {
        String* addr = Address_toString(&ep->addr, resp->ping->pingAlloc);
        Log_debug(ic->logger, "Received [%s] from lazy endpoint [%s]",
                  SwitchPinger_resultString(resp->res)->bytes, addr->bytes);
    }
}

/*
 * Send a ping packet to one of the endpoints.
 */
static void sendPing(struct Peer* ep)
{
    struct IfController_pvt* ic = Identity_check(ep->ici->ic);

    ep->pingCount++;

    struct SwitchPinger_Ping* ping =
        SwitchPinger_newPing(ep->addr.path,
                             String_CONST(""),
                             ic->timeoutMilliseconds,
                             onPingResponse,
                             ep->externalIf.allocator,
                             ic->switchPinger);

    if (Defined(Log_DEBUG)) {
        uint8_t key[56];
        Base32_encode(key, 56, CryptoAuth_getHerPublicKey(ep->cryptoAuthIf), 32);
        if (!ping) {
            Log_debug(ic->logger, "Failed to ping [%s.k], out of ping slots", key);
        } else {
            Log_debug(ic->logger, "SwitchPing [%s.k]", key);
        }
    }

    if (ping) {
        ping->onResponseContext = ep;
    }
}

static void iciPing(struct IfController_Iface_pvt* ici, struct IfController_pvt* ic)
{
    if (!ici->peerMap.count) { return; }
    uint64_t now = Time_currentTimeMilliseconds(ic->eventBase);

    // scan for endpoints have not sent anything recently.
    uint32_t startAt = Random_uint32(ic->rand) % ici->peerMap.count;
    for (uint32_t i = startAt, count = 0; count < ici->peerMap.count;) {
        i = (i + 1) % ici->peerMap.count;
        count++;

        struct Peer* ep = ici->peerMap.values[i];

        if (now < ep->timeOfLastMessage + ic->pingAfterMilliseconds) {
            if (now < ep->timeOfLastPing + ic->pingAfterMilliseconds) {
                // Possibly an out-of-date node which is mangling packets, don't ping too often
                // because it causes the RumorMill to be filled with this node over and over.
                continue;
            }
        }

        #ifdef Log_DEBUG
              uint8_t key[56];
              Base32_encode(key, 56, CryptoAuth_getHerPublicKey(ep->cryptoAuthIf), 32);
        #endif

        if (ep->isIncomingConnection && now > ep->timeOfLastMessage + ic->forgetAfterMilliseconds) {
            Log_debug(ic->logger, "Unresponsive peer [%s.k] has not responded in [%u] "
                                  "seconds, dropping connection",
                                  key, ic->forgetAfterMilliseconds / 1024);
            sendPeer(ic, 0xffffffff, PFChan_Core_PEER_GONE, ep, ep->externalIf.allocator);
            Allocator_free(ep->externalIf.allocator);
            continue;
        }

        bool unresponsive = (now > ep->timeOfLastMessage + ic->unresponsiveAfterMilliseconds);
        if (unresponsive) {
            // our link to the peer is broken...
            sendPeer(ic, 0xffffffff, PFChan_Core_PEER_GONE, ep, ep->externalIf.allocator);

            // Lets skip 87% of pings when they're really down.
            if (ep->pingCount % 8) {
                ep->pingCount++;
                continue;
            }

            ep->state = IfController_PeerState_UNRESPONSIVE;
        }

        Log_debug(ic->logger,
                  "Pinging %s peer [%s.k] lag [%u]",
                  (unresponsive ? "unresponsive" : "lazy"),
                  key,
                  (uint32_t)((now - ep->timeOfLastMessage) / 1024));

        sendPing(ep);

        // we only ping one node
        return;
    }
}

/**
 * Check the table for nodes which might need to be pinged, ping a node if necessary.
 * If a node has not responded in unresponsiveAfterMilliseconds then mark them as unresponsive
 * and if the connection is incoming and the node has not responded in forgetAfterMilliseconds
 * then drop them entirely.
 * This is called every PING_INTERVAL_MILLISECONDS but pingCallback is a misleading name.
 */
static void pingCallback(void* vic)
{
    struct IfController_pvt* ic = Identity_check((struct IfController_pvt*) vic);
    for (int i = 0; i < ic->icis->length; i++) {
        struct IfController_Iface_pvt* ici = ArrayList_OfIfaces_get(ic->icis, i);
        iciPing(ici, ic);
    }
}

/** If there's already an endpoint with the same public key, merge the new one with the old one. */
static void moveEndpointIfNeeded(struct Peer* ep)
{
    struct IfController_Iface_pvt* ici = ep->ici;
    Log_debug(ici->ic->logger, "Checking for old sessions to merge with.");
    for (uint32_t i = 0; i < ici->peerMap.count; i++) {
        struct Peer* thisEp = ici->peerMap.values[i];
        if (thisEp != ep && !Bits_memcmp(thisEp->addr.key, ep->addr.key, 32)) {
            Log_info(ici->ic->logger, "Moving endpoint to merge new session with old.");

            ep->addr.path = thisEp->addr.path;
            SwitchCore_swapInterfaces(&thisEp->switchIf, &ep->switchIf);
            Allocator_free(thisEp->externalIf.allocator);
            return;
        }
    }
}

// Incoming message which has passed through the cryptoauth and needs to be forwarded to the switch.
static uint8_t receivedAfterCryptoAuth(struct Message* msg, struct Iface* cryptoAuthIf)
{
    struct Peer* ep = Identity_check((struct Peer*) cryptoAuthIf->receiverContext);
    struct IfController_pvt* ic = Identity_check(ep->ici->ic);

    // nonce added by the CryptoAuth session.
    Message_pop(msg, NULL, 4, NULL);

    ep->bytesIn += msg->length;

    int caState = CryptoAuth_getState(cryptoAuthIf);
    if (ep->state < IfController_PeerState_ESTABLISHED) {
        // EP states track CryptoAuth states...
        ep->state = caState;

        uint8_t* hpk = CryptoAuth_getHerPublicKey(ep->cryptoAuthIf);
        Bits_memcpyConst(ep->addr.key, hpk, 32);
        Address_getPrefix(&ep->addr);

        if (caState == CryptoAuth_ESTABLISHED) {
            moveEndpointIfNeeded(ep);
            sendPeer(ic, 0xffffffff, PFChan_Core_PEER, ep, ep->externalIf.allocator);
        } else {
            // prevent some kinds of nasty things which could be done with packet replay.
            // This is checking the message switch header and will drop it unless the label
            // directs it to *this* router.
            if (msg->length < 8 || msg->bytes[7] != 1) {
                Log_info(ic->logger, "DROP message because CA is not established.");
                return Error_NONE;
            } else {
                // When a "server" gets a new connection from a "client" the router doesn't
                // know about that client so if the client sends a packet to the server, the
                // server will be unable to handle it until the client has sent inter-router
                // communication to the server. Here we will ping the client so when the
                // server gets the ping response, it will insert the client into its table
                // and know its version.

                // prevent DoS by limiting the number of times this can be called per second
                // limit it to 7, this will affect innocent packets but it doesn't matter much
                // since this is mostly just an optimization and for keeping the tests happy.
                if ((ep->pingCount + 1) % 7) {
                    sendPing(ep);
                }
            }
        }
    } else if (ep->state == IfController_PeerState_UNRESPONSIVE
        && caState == CryptoAuth_ESTABLISHED)
    {
        ep->state = IfController_PeerState_ESTABLISHED;
    } else {
        ep->timeOfLastMessage = Time_currentTimeMilliseconds(ic->eventBase);
    }

    Identity_check(ep);
    Assert_true(!(msg->capacity % 4));
    return Iface_send(&ep->switchIf, msg);
}

// This is directly called from SwitchCore, message is not encrypted.
static uint8_t sendFromSwitch(struct Message* msg, struct Iface* switchIf)
{
    struct Peer* ep = Identity_check((struct Peer*) switchIf);

    ep->bytesOut += msg->length;

    struct IfController_pvt* ic = Identity_check(ep->ici->ic);
    uint8_t ret;
    uint64_t now = Time_currentTimeMilliseconds(ic->eventBase);
    if (now - ep->timeOfLastMessage > ic->unresponsiveAfterMilliseconds) {
        // TODO(cjd): This is a hack because if the time of last message exceeds the
        //            unresponsive time, we need to send back an error and that means
        //            mangling the message which would otherwise be in the queue.
        struct Allocator* tempAlloc = Allocator_child(ic->allocator);
        struct Message* toSend = Message_clone(msg, tempAlloc);
        ret = Interface_sendMessage(ep->cryptoAuthIf, toSend);
        Allocator_free(tempAlloc);
    } else {
        ret = Interface_sendMessage(ep->cryptoAuthIf, msg);
    }

    // TODO(cjd): this is not quite right
    // We don't always trust the UDP interface to be accurate
    // short spurious failures and packet-backup should not cause us to treat a link as dead
    if (ret == Error_UNDELIVERABLE) {
        ret = 0;
    }

    // If this node is unresponsive then return an error.
    if (ret || now - ep->timeOfLastMessage > ic->unresponsiveAfterMilliseconds) {
        return ret ? ret : Error_UNDELIVERABLE;
    } else {
        /* Way way way too much noise
        Log_debug(ic->logger,  "Sending to neighbor, last message from this node was [%u] ms ago.",
                  (now - ep->timeOfLastMessage));
        */
    }

    return Error_NONE;
}

static int closeInterface(struct Allocator_OnFreeJob* job)
{
    struct Peer* toClose = Identity_check((struct Peer*) job->userData);
    struct IfController_pvt* ic = Identity_check(toClose->ici->ic);

    sendPeer(ic, 0xffffffff, PFChan_Core_PEER_GONE, toClose, toClose->externalIf.allocator);

    int index = Map_EndpointsBySockaddr_indexForHandle(toClose->handle, &toClose->ici->peerMap);
    Assert_true(index >= 0 && toClose->ici->peerMap.values[index] == toClose);
    Map_EndpointsBySockaddr_remove(index, &toClose->ici->peerMap);
    return 0;
}

static uint8_t sendAfterCryptoAuth(struct Message* msg, struct Iface* externalIf)
{
    struct Peer* ep =
        Identity_check((struct Peer*) &(
            ((uint8_t*)externalIf)[-offsetof(struct Peer, externalIf)]));

    Assert_true(!(((uintptr_t)msg->bytes) % 4) && "alignment fault");

    // push the lladdr...
    Message_push(msg, ep->lladdr, ep->lladdr->addrLen, NULL);

    // very noisy
    if (Defined(Log_DEBUG) && false) {
        char* printedAddr =
            Hex_print(&ep->lladdr[1], ep->lladdr->addrLen - Sockaddr_OVERHEAD, msg->alloc);
        Log_debug(ep->ici->ic->logger, "Outgoing message to [%s]", printedAddr);
    }

    Iface_send(&ep->ici->pub.addrIf, msg);
    return 0;
}

/**
 * Expects [ struct LLAddress ][ beacon ]
 */
static Iface_DEFUN handleBeacon(struct Message* msg, struct IfController_Iface_pvt* ici)
{
    struct IfController_pvt* ic = ici->ic;
    if (!ici->beaconState) {
        // accepting beacons disabled.
        Log_debug(ic->logger, "[%s] Dropping beacon because beaconing is disabled",
                  ici->name->bytes);
        return NULL;
    }

    if (msg->length < Headers_Beacon_SIZE) {
        Log_debug(ic->logger, "[%s] Dropping runt beacon", ici->name->bytes);
        return NULL;
    }

    struct Sockaddr* lladdrInmsg = (struct Sockaddr*) msg->bytes;

    // clear the bcast flag
    lladdrInmsg->flags = 0;

    Message_shift(msg, -lladdrInmsg->addrLen, NULL);

    struct Headers_Beacon beacon;
    Message_pop(msg, &beacon, Headers_Beacon_SIZE, NULL);

    if (Defined(Log_DEBUG)) {
        char* content = Hex_print(&beacon, Headers_Beacon_SIZE, msg->alloc);
        Log_debug(ici->ic->logger, "RECV BEACON CONTENT[%s]", content);
    }

    struct Address addr;
    Bits_memset(&addr, 0, sizeof(struct Address));
    Bits_memcpyConst(addr.key, beacon.publicKey, 32);
    addr.protocolVersion = Endian_bigEndianToHost32(beacon.version_be);
    Address_getPrefix(&addr);
    String* printedAddr = Address_toString(&addr, msg->alloc);

    if (addr.ip6.bytes[0] != 0xfc || !Bits_memcmp(ic->ca->publicKey, addr.key, 32)) {
        Log_debug(ic->logger, "handleBeacon invalid key [%s]", printedAddr->bytes);
        return NULL;
    }

    if (!Version_isCompatible(addr.protocolVersion, Version_CURRENT_PROTOCOL)) {
        if (Defined(Log_DEBUG)) {
            Log_debug(ic->logger, "[%s] DROP beacon from [%s] which was version [%d] "
                      "our version is [%d] making them incompatable", ici->name->bytes,
                      printedAddr->bytes, addr.protocolVersion, Version_CURRENT_PROTOCOL);
        }
        return NULL;
    }

    String* beaconPass = String_newBinary(beacon.password, Headers_Beacon_PASSWORD_LEN, msg->alloc);
    int epIndex = Map_EndpointsBySockaddr_indexForKey(&lladdrInmsg, &ici->peerMap);
    if (epIndex > -1) {
        // The password might have changed!
        struct Peer* ep = ici->peerMap.values[epIndex];
        CryptoAuth_setAuth(beaconPass, 1, ep->cryptoAuthIf);
        return NULL;
    }

    struct Allocator* epAlloc = Allocator_child(ici->alloc);
    struct Peer* ep = Allocator_calloc(epAlloc, sizeof(struct Peer), 1);
    struct Sockaddr* lladdr = Sockaddr_clone(lladdrInmsg, epAlloc);
    ep->ici = ici;
    ep->lladdr = lladdr;
    int setIndex = Map_EndpointsBySockaddr_put(&lladdr, &ep, &ici->peerMap);
    ep->handle = ici->peerMap.handles[setIndex];
    ep->isIncomingConnection = true;
    Bits_memcpyConst(&ep->addr, &addr, sizeof(struct Address));
    Identity_set(ep);
    Allocator_onFree(epAlloc, closeInterface, ep);

    ep->externalIf.sendMessage = sendAfterCryptoAuth;
    ep->externalIf.allocator = epAlloc;

    ep->cryptoAuthIf =
        CryptoAuth_wrapInterface(&ep->externalIf, beacon.publicKey, NULL, false, "outer", ic->ca);
    ep->cryptoAuthIf->receiveMessage = receivedAfterCryptoAuth;
    ep->cryptoAuthIf->receiverContext = ep;
    CryptoAuth_setAuth(beaconPass, 1, ep->cryptoAuthIf);

    ep->switchIf.sendMessage = sendFromSwitch;
    ep->switchIf.allocator = epAlloc;

    int ret = SwitchCore_addInterface(&ep->switchIf, 0, &ep->addr.path, ic->switchCore);
    if (ret == SwitchCore_addInterface_OUT_OF_SPACE) {
        Log_debug(ic->logger, "handleBeacon SwitchCore out of space");
        Allocator_free(epAlloc);
        return NULL;
    } else if (ret) {
        Log_debug(ic->logger, "handleBeacon SwitchCore something went wrong ret[%d]", ret);
        Allocator_free(epAlloc);
        return NULL;
    }

    // Update printedAddr since addr now contains path.
    printedAddr = Address_toString(&ep->addr, msg->alloc);

    // We want the node to immedietly be pinged but we don't want it to appear unresponsive because
    // the pinger will only ping every (PING_INTERVAL * 8) so we set timeOfLastMessage to
    // (now - pingAfterMilliseconds - 1) so it will be considered a "lazy node".
    ep->timeOfLastMessage =
        Time_currentTimeMilliseconds(ic->eventBase) - ic->pingAfterMilliseconds - 1;

    Log_info(ic->logger, "Added peer [%s] from beacon", printedAddr->bytes);

    // This should be safe because this is an outgoing request and we're sure the node will not
    // be relocated by moveEndpointIfNeeded()
    sendPeer(ic, 0xffffffff, PFChan_Core_PEER, ep, ep->externalIf.allocator);
    return NULL;
}

/**
 * Incoming message from someone we don't know, maybe someone responding to a beacon?
 * expects: [ struct LLAddress ][ content ]
 */
static Iface_DEFUN handleUnexpectedIncoming(struct Message* msg, struct IfController_Iface_pvt* ici)
{
    struct IfController_pvt* ic = ici->ic;

    struct Allocator* epAlloc = Allocator_child(ici->alloc);

    struct Sockaddr* lladdr = (struct Sockaddr*) msg->bytes;
    Message_shift(msg, -lladdr->addrLen, NULL);
    lladdr = Sockaddr_clone(lladdr, epAlloc);

    Assert_true(!((uintptr_t)msg->bytes % 4) && "alignment fault");

    struct Peer* ep = Allocator_calloc(epAlloc, sizeof(struct Peer), 1);
    ep->ici = ici;
    ep->lladdr = lladdr;
    Assert_true(Map_EndpointsBySockaddr_indexForKey(&lladdr, &ici->peerMap) == -1);
    int index = Map_EndpointsBySockaddr_put(&lladdr, &ep, &ici->peerMap);
    Assert_true(index >= 0);
    ep->handle = ici->peerMap.handles[index];
    Identity_set(ep);
    Allocator_onFree(epAlloc, closeInterface, ep);

    ep->state = IfController_PeerState_UNAUTHENTICATED;
    ep->isIncomingConnection = true;

    ep->externalIf.sendMessage = sendAfterCryptoAuth;
    ep->externalIf.allocator = epAlloc;

    ep->cryptoAuthIf =
        CryptoAuth_wrapInterface(&ep->externalIf, NULL, NULL, true, "outer", ic->ca);

    ep->cryptoAuthIf->receiveMessage = receivedAfterCryptoAuth;
    ep->cryptoAuthIf->receiverContext = ep;

    ep->switchIf.sendMessage = sendFromSwitch;
    ep->switchIf.allocator = epAlloc;

    int ret = SwitchCore_addInterface(&ep->switchIf, 0, &ep->addr.path, ic->switchCore);
    if (ret) {
        Allocator_free(epAlloc);
        return NULL;
    }

    // We want the node to immedietly be pinged but we don't want it to appear unresponsive because
    // the pinger will only ping every (PING_INTERVAL * 8) so we set timeOfLastMessage to
    // (now - pingAfterMilliseconds - 1) so it will be considered a "lazy node".
    ep->timeOfLastMessage =
        Time_currentTimeMilliseconds(ic->eventBase) - ic->pingAfterMilliseconds - 1;

    Log_info(ic->logger, "Adding peer with unknown key");

    if (Iface_send(&ep->externalIf, msg)) {
        // If the first message is a dud, drop all state for this peer.
        // probably some random crap that wandered in the socket.
        Allocator_free(epAlloc);
    }

    return NULL;
}

static Iface_DEFUN handleIncomingFromWire(struct Iface* addrIf, struct Message* msg)
{
    struct IfController_Iface_pvt* ici =
        Identity_containerOf(addrIf, struct IfController_Iface_pvt, pub.addrIf);

    struct Sockaddr* lladdr = (struct Sockaddr*) msg->bytes;
    if (msg->length < Sockaddr_OVERHEAD || msg->length < lladdr->addrLen) {
        Log_debug(ici->ic->logger, "DROP runt");
        return NULL;
    }

    Assert_true(!((uintptr_t)msg->bytes % 4) && "alignment fault");
    Assert_true(!((uintptr_t)lladdr->addrLen % 4) && "alignment fault");

    // noisy
    if (Defined(Log_DEBUG) && false) {
        char* printedAddr = Hex_print(&lladdr[1], lladdr->addrLen - Sockaddr_OVERHEAD, msg->alloc);
        Log_debug(ici->ic->logger, "Incoming message from [%s]", printedAddr);
    }

    if (lladdr->flags & Sockaddr_flags_BCAST) {
        return handleBeacon(msg, ici);
    }

    int epIndex = Map_EndpointsBySockaddr_indexForKey(&lladdr, &ici->peerMap);
    if (epIndex == -1) {
        return handleUnexpectedIncoming(msg, ici);
    }

    struct Peer* ep = Identity_check((struct Peer*) ici->peerMap.values[epIndex]);
    Message_shift(msg, -lladdr->addrLen, NULL);
    Iface_send(&ep->externalIf, msg);
    return NULL;
}

struct IfController_Iface* IfController_newIface(struct IfController* ifc,
                                                 String* name,
                                                 struct Allocator* alloc)
{
    struct IfController_pvt* ic = Identity_check((struct IfController_pvt*) ifc);

    struct IfController_Iface_pvt* ici =
        Allocator_calloc(alloc, sizeof(struct IfController_Iface_pvt), 1);
    ici->name = String_clone(name, alloc);
    ici->peerMap.allocator = alloc;
    ici->ic = ic;
    ici->alloc = alloc;
    ici->pub.addrIf.send = handleIncomingFromWire;
    ici->pub.ifNum = ArrayList_OfIfaces_add(ic->icis, ici);

    Identity_set(ici);

    return &ici->pub;
}

static int freeAlloc(struct Allocator_OnFreeJob* job)
{
    struct Allocator* alloc = (struct Allocator*) job->userData;
    Allocator_free(alloc);
    return 0;
}

static void sendBeacon(struct IfController_Iface_pvt* ici, struct Allocator* tempAlloc)
{
    if (ici->beaconState < IfController_beaconState_newState_SEND) {
        Log_debug(ici->ic->logger, "sendBeacon(%s) -> beaconing disabled", ici->name->bytes);
        return;
    }

    Log_debug(ici->ic->logger, "sendBeacon(%s)", ici->name->bytes);

    struct Message* msg = Message_new(0, 128, tempAlloc);
    Message_push(msg, &ici->ic->beacon, Headers_Beacon_SIZE, NULL);

    if (Defined(Log_DEBUG)) {
        char* content = Hex_print(msg->bytes, msg->length, tempAlloc);
        Log_debug(ici->ic->logger, "SEND BEACON CONTENT[%s]", content);
    }

    struct Sockaddr sa = {
        .addrLen = Sockaddr_OVERHEAD,
        .flags = Sockaddr_flags_BCAST
    };
    Message_push(msg, &sa, Sockaddr_OVERHEAD, NULL);

    Iface_send(ici->pub.addrIf, msg);
}

static void beaconInterval(void* vIfController)
{
    struct IfController_pvt* ic =
        Identity_check((struct IfController_pvt*) vIfController);

    struct Allocator* alloc = Allocator_child(ic->allocator);
    for (int i = 0; i < ic->icis->length; i++) {
        struct IfController_Iface_pvt* ici = ArrayList_OfIfaces_get(ic->icis, i);
        sendBeacon(ici, alloc);
    }
    Allocator_free(alloc);

    Timeout_setTimeout(beaconInterval, ic, ic->beaconInterval, ic->eventBase, ic->allocator);
}

int IfController_beaconState(struct IfController* ifc,
                                    int interfaceNumber,
                                    int newState)
{
    struct IfController_pvt* ic = Identity_check((struct IfController_pvt*) ifc);
    struct IfController_Iface_pvt* ici = ArrayList_OfIfaces_get(ic->icis, interfaceNumber);
    if (!ici) {
        return IfController_beaconState_NO_SUCH_IFACE;
    }
    char* val = NULL;
    switch (newState) {
        default: return IfController_beaconState_INVALID_STATE;
        case IfController_beaconState_newState_OFF: val = "OFF"; break;
        case IfController_beaconState_newState_ACCEPT: val = "ACCEPT"; break;
        case IfController_beaconState_newState_SEND: val = "SEND"; break;
    }
    Log_debug(ic->logger, "IfController_beaconState(%s, %s)", ici->name->bytes, val);
    ici->beaconState = newState;
    if (newState == IfController_beaconState_newState_SEND) {
        // Send out a beacon right away so we don't have to wait.
        struct Allocator* alloc = Allocator_child(ici->alloc);
        sendBeacon(ici, alloc);
        Allocator_free(alloc);
    }
    return 0;
}

int IfController_bootstrapPeer(struct IfController* ifc,
                                      int interfaceNumber,
                                      uint8_t* herPublicKey,
                                      const struct Sockaddr* lladdrParm,
                                      String* password,
                                      struct Allocator* alloc)
{
    struct IfController_pvt* ic =
        Identity_check((struct IfController_pvt*) ifc);

    Assert_true(herPublicKey);
    Assert_true(password);

    struct IfController_Iface_pvt* ici = ArrayList_OfIfaces_get(ic->icis, interfaceNumber);

    if (!ici) {
        return IfController_bootstrapPeer_BAD_IFNUM;
    }

    Log_debug(ic->logger, "bootstrapPeer total [%u]", ici->peerMap.count);

    uint8_t ip6[16];
    AddressCalc_addressForPublicKey(ip6, herPublicKey);
    if (!AddressCalc_validAddress(ip6) ||
        !Bits_memcmp(ic->ca->publicKey, herPublicKey, 32))
    {
        return IfController_bootstrapPeer_BAD_KEY;
    }

    struct Allocator* epAlloc = Allocator_child(ici->alloc);

    struct Sockaddr* lladdr = Sockaddr_clone(lladdrParm, epAlloc);

    struct Peer* ep = Allocator_calloc(epAlloc, sizeof(struct Peer), 1);
    int index = Map_EndpointsBySockaddr_put(&lladdr, &ep, &ici->peerMap);
    Assert_true(index >= 0);
    ep->handle = ici->peerMap.handles[index];
    ep->lladdr = lladdr;
    ep->ici = ici;
    ep->isIncomingConnection = false;
    Bits_memcpyConst(ep->addr.key, herPublicKey, 32);
    Address_getPrefix(&ep->addr);
    Identity_set(ep);
    Allocator_onFree(epAlloc, closeInterface, ep);
    Allocator_onFree(alloc, freeAlloc, epAlloc);

    ep->externalIf.sendMessage = sendAfterCryptoAuth;
    ep->externalIf.allocator = epAlloc;

    ep->cryptoAuthIf = CryptoAuth_wrapInterface(&ep->externalIf,
                                                herPublicKey,
                                                NULL,
                                                false,
                                                "outer",
                                                ic->ca);

    ep->cryptoAuthIf->receiveMessage = receivedAfterCryptoAuth;
    ep->cryptoAuthIf->receiverContext = ep;
    CryptoAuth_setAuth(password, 1, ep->cryptoAuthIf);

    ep->switchIf.sendMessage = sendFromSwitch;
    ep->switchIf.allocator = epAlloc;

    int ret = SwitchCore_addInterface(&ep->switchIf, 0, &ep->addr.path, ic->switchCore);
    if (ret) {
        Allocator_free(epAlloc);
        return (ret == SwitchCore_addInterface_OUT_OF_SPACE)
            ? IfController_bootstrapPeer_OUT_OF_SPACE
            : IfController_bootstrapPeer_INTERNAL;
    }

    // We want the node to immedietly be pinged but we don't want it to appear unresponsive because
    // the pinger will only ping every (PING_INTERVAL * 8) so we set timeOfLastMessage to
    // (now - pingAfterMilliseconds - 1) so it will be considered a "lazy node".
    ep->timeOfLastMessage =
        Time_currentTimeMilliseconds(ic->eventBase) - ic->pingAfterMilliseconds - 1;

    if (Defined(Log_INFO)) {
        struct Allocator* tempAlloc = Allocator_child(alloc);
        String* addrStr = Address_toString(&ep->addr, tempAlloc);
        Log_info(ic->logger, "Adding peer [%s]", addrStr->bytes);
        Allocator_free(tempAlloc);
    }

    // We can't just add the node directly to the routing table because we do not know
    // the version. We'll send it a switch ping and when it responds, we will know it's
    // key (if we don't already) and version number.
    sendPing(ep);

    return 0;
}

int IfController_getPeerStats(struct IfController* ifController,
                                     struct Allocator* alloc,
                                     struct IfController_PeerStats** statsOut)
{
    struct IfController_pvt* ic =
        Identity_check((struct IfController_pvt*) ifController);

    int count = 0;
    for (int i = 0; i < ic->icis->length; i++) {
        struct IfController_Iface_pvt* ici = ArrayList_OfIfaces_get(ic->icis, i);
        count += ici->peerMap.count;
    }

    struct IfController_PeerStats* stats =
        Allocator_calloc(alloc, sizeof(struct IfController_PeerStats), count);

    int xcount = 0;
    for (int j = 0; j < ic->icis->length; j++) {
        struct IfController_Iface_pvt* ici = ArrayList_OfIfaces_get(ic->icis, j);
        for (int i = 0; i < (int)ici->peerMap.count; i++) {
            struct Peer* peer = Identity_check((struct Peer*) ici->peerMap.values[i]);
            struct IfController_PeerStats* s = &stats[xcount];
            xcount++;
            Bits_memcpyConst(&s->addr, &peer->addr, sizeof(struct Address));
            s->bytesOut = peer->bytesOut;
            s->bytesIn = peer->bytesIn;
            s->timeOfLastMessage = peer->timeOfLastMessage;
            s->state = peer->state;
            s->isIncomingConnection = peer->isIncomingConnection;
            s->user = NULL;
            String* user = CryptoAuth_getUser(peer->cryptoAuthIf);
            if (user) {
                s->user = String_clone(user, alloc);
            }
            struct ReplayProtector* rp = CryptoAuth_getReplayProtector(peer->cryptoAuthIf);
            s->duplicates = rp->duplicates;
            s->lostPackets = rp->lostPackets;
            s->receivedOutOfRange = rp->receivedOutOfRange;
        }
    }

    Assert_true(xcount == count);

    *statsOut = stats;
    return count;
}

int IfController_disconnectPeer(struct IfController* ifController,
                                       uint8_t herPublicKey[32])
{
    struct IfController_pvt* ic =
        Identity_check((struct IfController_pvt*) ifController);

    for (int j = 0; j < ic->icis->length; j++) {
        struct IfController_Iface_pvt* ici = ArrayList_OfIfaces_get(ic->icis, j);
        for (int i = 0; i < (int)ici->peerMap.count; i++) {
            struct Peer* peer = ici->peerMap.values[i];
            if (!Bits_memcmp(herPublicKey, CryptoAuth_getHerPublicKey(peer->cryptoAuthIf), 32)) {
                Allocator_free(peer->externalIf.allocator);
                return 0;
            }
        }
    }
    return IfController_disconnectPeer_NOTFOUND;
}

static Iface_DEFUN incomingFromEventEmitterIf(struct Iface* eventEmitterIf, struct Message* msg)
{
    struct IfController_pvt* ic =
         Identity_containerOf(eventEmitterIf, struct IfController_pvt, eventEmitterIf);
    Assert_true(Message_pop32(msg, NULL) == PFChan_Pathfinder_PEERS);
    uint32_t pathfinderId = Message_pop32(msg, NULL);
    Assert_true(!msg->length);

    for (int j = 0; j < ic->icis->length; j++) {
        struct IfController_Iface_pvt* ici = ArrayList_OfIfaces_get(ic->icis, j);
        for (int i = 0; i < (int)ici->peerMap.count; i++) {
            struct Peer* peer = Identity_check((struct Peer*) ici->peerMap.values[i]);
            if (peer->state != IfController_PeerState_ESTABLISHED) { continue; }
            sendPeer(ic, pathfinderId, PFChan_Core_PEER, peer, msg->alloc);
        }
    }
    return NULL;
}

struct IfController* IfController_new(struct CryptoAuth* ca,
                                                    struct SwitchCore* switchCore,
                                                    struct Log* logger,
                                                    struct EventBase* eventBase,
                                                    struct SwitchPinger* switchPinger,
                                                    struct Random* rand,
                                                    struct Allocator* allocator,
                                                    struct EventEmitter* ee)
{
    struct IfController_pvt* out =
        Allocator_malloc(allocator, sizeof(struct IfController_pvt));
    Bits_memcpyConst(out, (&(struct IfController_pvt) {
        .allocator = allocator,
        .ca = ca,
        .rand = rand,
        .switchCore = switchCore,
        .logger = logger,
        .eventBase = eventBase,
        .switchPinger = switchPinger,
        .unresponsiveAfterMilliseconds = UNRESPONSIVE_AFTER_MILLISECONDS,
        .pingAfterMilliseconds = PING_AFTER_MILLISECONDS,
        .timeoutMilliseconds = TIMEOUT_MILLISECONDS,
        .forgetAfterMilliseconds = FORGET_AFTER_MILLISECONDS,
        .beaconInterval = BEACON_INTERVAL,

        .pingInterval = (switchPinger)
            ? Timeout_setInterval(pingCallback,
                                  out,
                                  PING_INTERVAL_MILLISECONDS,
                                  eventBase,
                                  allocator)
            : NULL

    }), sizeof(struct IfController_pvt));
    Identity_set(out);

    out->icis = ArrayList_OfIfaces_new(allocator);

    out->eventEmitterIf.send = incomingFromEventEmitterIf;
    EventEmitter_regCore(ee, &out->eventEmitterIf, PFChan_Pathfinder_PEERS);

    // Add the beaconing password.
    Random_bytes(rand, out->beacon.password, Headers_Beacon_PASSWORD_LEN);
    String strPass = { .bytes=(char*)out->beacon.password, .len=Headers_Beacon_PASSWORD_LEN };
    int ret = CryptoAuth_addUser(&strPass, 1, String_CONST("Local Peers"), ca);
    if (ret) {
        Log_warn(logger, "CryptoAuth_addUser() returned [%d]", ret);
    }
    Bits_memcpyConst(out->beacon.publicKey, ca->publicKey, 32);
    out->beacon.version_be = Endian_hostToBigEndian32(Version_CURRENT_PROTOCOL);

    Timeout_setTimeout(beaconInterval, out, BEACON_INTERVAL, eventBase, allocator);

    return &out->pub;
}
