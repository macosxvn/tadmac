//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
//

/**
 * Version 1.0: point-to-point communication
 * Version 2.0: multi-hop communication
 * Version 3.0: multi-host (multi sender) communication
 */

#include "RicerLayer.h"
#include "tool/tool.h"

#include <cassert>

#include "FWMath.h"
#include "MacToPhyControlInfo.h"
#include "BaseArp.h"
#include "BaseConnectionManager.h"
#include "PhyUtils.h"
#include "MacPkt_m.h"
#include "MacToPhyInterface.h"
#include "BaseDecider.h"
#include "Decider802154Narrow.h"

Define_Module(RicerLayer)

/**
 * Initialize method of RicerLayer. Init all parameters, schedule timers.
 */
void RicerLayer::initialize(int stage) {
    BaseMacLayer::initialize(stage);

    if (stage == 0) {
        role = static_cast<ROLES>(hasPar("role") ? par("role") : 1);

        queueLength = hasPar("queueLength") ? par("queueLength") : 10;
        animation = hasPar("animation") ? par("animation") : true;
        bitrate = hasPar("bitrate") ? par("bitrate") : 250000;
        iwu = hasPar("iwu") ? par("iwu") : 0.05;

        // number of data slot: config par parameter in Destination or Relay, get by node index in Sources
        if (role == NODE_RECEIVER || role == NODE_TRANSMITER) {
            nbSlot = hasPar("nbSlot") ? par("nbSlot") : 2;
        } else {
            // do not count RECEIVER & RELAY index
            nbSlot = getNode()->getIndex() - 2;
        }

        txPower = hasPar("txPower") ? par("txPower") : 1.0;
        useMacAcks = hasPar("useMACAcks") ? par("useMACAcks") : false;
        maxTxAttempts = par("macMaxFrameRetries");
        stats = par("stats");
        nbTxDataPackets = 0;
        nbTxBeacons = 0;
        nbRxDataPackets = 0;
        nbRxBeacons = 0;
        nbMissedAcks = 0;
        nbRecvdAcks = 0;
        nbDroppedDataPackets = 0;
        nbTxAcks = 0;
        nbTxRelayData = 0;
        nbPacketError = 0;
        idleVec.setName("idle");

        txAttempts = 0;
        lastDataPktDestAddr = LAddress::L2BROADCAST;
        lastDataPktSrcAddr = LAddress::L2BROADCAST;
        forwardAddr.setAddress(hasPar("forwardAddr") ? par("forwardAddr") : "ff:ff:ff:ff:ff:ff");

        macState = INIT;

        // init the dropped packet info
        droppedPacket.setReason(DroppedPacket::NONE);
        nicId = getNic()->getId();
        WATCH(macState);

        // init some timer infomations
        checkInterval = PKG_WB_SIZE * 8 / bitrate;
        slotDuration = (PKG_WB_SIZE + PKG_DATA_SIZE + PKG_WB_SIZE + PKG_ACK_SIZE) * 8 / bitrate;
    }

    else if (stage == 1) {

        wakeup = new cMessage("wakeup");
        wakeup->setKind(Ricer_WAKE_UP);
        // force wake up to send data
        wakeup_data = new cMessage("wakeup_data");
        wakeup_data->setKind(Ricer_WAKE_UP_DATA);

        data_timeout = new cMessage("data_timeout");
        data_timeout->setKind(Ricer_DATA_TIMEOUT);
        data_timeout->setSchedulingPriority(100);

        data_tx_over = new cMessage("data_tx_over");
        data_tx_over->setKind(Ricer_DATA_TX_OVER);

        beacon_tx_over = new cMessage("beacon_tx_over");
        beacon_tx_over->setKind(Ricer_BEACON_TX_OVER);

        beacon_timeout = new cMessage("beacon_timeout");
        beacon_timeout->setKind(Ricer_BEACON_TIMEOUT);

        ack_tx_over = new cMessage("ack_tx_over");
        ack_tx_over->setKind(Ricer_ACK_TX_OVER);

        cca_timeout = new cMessage("cca_timeout");
        cca_timeout->setKind(Ricer_CCA_TIMEOUT);
        cca_timeout->setSchedulingPriority(100);

        ack_timeout = new cMessage("ack_timeout");
        ack_timeout->setKind(Ricer_ACK_TIMEOUT);

        start = new cMessage("Ricer_START");
        start->setKind(Ricer_START);

        scheduleAt(0.0, start);
    }
}

RicerLayer::~RicerLayer() {
    cancelAndDelete(wakeup);
    cancelAndDelete(wakeup_data);
    cancelAndDelete(data_timeout);
    cancelAndDelete(data_tx_over);
    cancelAndDelete(beacon_tx_over);
    cancelAndDelete(beacon_timeout);
    cancelAndDelete(ack_tx_over);
    cancelAndDelete(cca_timeout);
    cancelAndDelete(ack_timeout);
    cancelAndDelete(start);

    MacQueue::iterator it;
    for (it = macQueue.begin(); it != macQueue.end(); ++it) {
        delete (*it);
    }
    macQueue.clear();
}

void RicerLayer::finish() {
    BaseMacLayer::finish();

    // record stats
    if (stats) {
        recordScalar("nbTxRelayData", nbTxRelayData);
        recordScalar("nbTxDataPackets", nbTxDataPackets);
        recordScalar("nbTxBeacons", nbTxBeacons);
        recordScalar("nbRxDataPackets", nbRxDataPackets);
        recordScalar("nbRxBeacons", nbRxBeacons);
        recordScalar("nbMissedAcks", nbMissedAcks);
        recordScalar("nbRecvdAcks", nbRecvdAcks);
        recordScalar("nbTxAcks", nbTxAcks);
        recordScalar("nbDroppedDataPackets", nbDroppedDataPackets);
        recordScalar("nbPacketError", nbPacketError);
    }
}

/**
 * Check whether the queue is not full: if yes, print a warning and drop the
 * packet. Then initiate sending of the packet, if the node is sleeping. Do
 * nothing, if node is working.
 */
void RicerLayer::handleUpperMsg(cMessage *msg) {
    // store packet to queue
    addToQueue(msg);
    // force wakeup now
    if (macState == SLEEP) {
        // if scheduled to wakeup & send WB - cancel this event
        if (wakeup->isScheduled()) {
            cancelEvent(wakeup);
        }
        if (wakeup_data->isScheduled()) {
            cancelEvent(wakeup_data);
        }
        scheduleAt(simTime(), wakeup_data);
    }
}

/**
 * Send one short beacon packet immediately.
 */
void RicerLayer::sendBeacon() {
    MacPkt* beacon = new MacPkt();
    beacon->setSrcAddr(myMacAddr);
    beacon->setDestAddr(LAddress::L2BROADCAST);
    beacon->setKind(Ricer_BEACON);
    beacon->setName("Beacon");
    beacon->setBitLength(PKG_WB_SIZE * 8);

    //attach signal and send down
    attachSignal(beacon);
    sendDown(beacon);
    nbTxBeacons++;
}
/**
 * Send one short beacon packet immediately.
 */
void RicerLayer::sendMacAck() {
    MacPkt* ack = new MacPkt();
    ack->setSrcAddr(myMacAddr);
    ack->setDestAddr(lastDataPktSrcAddr);
    ack->setKind(Ricer_ACK);
    ack->setName("ACK");
    ack->setBitLength(PKG_ACK_SIZE * 8);

    //attach signal and send down
    attachSignal(ack);
    sendDown(ack);
    nbTxAcks++;
}

/**
 * Handle own messages:
 * Ricer_WAKEUP: wake up the node, check the channel for some time.
 * Ricer_CHECK_CHANNEL: if the channel is free, check whether there is something
 * in the queue and switch the radio to TX. When switched to TX, the node will
 * start sending beacons for a full slot duration. If the channel is busy,
 * stay awake to receive message. Schedule a timeout to handle false alarms.
 * Ricer_SEND_BEACONS: sending of beacons over. Next time the data packet
 * will be send out (single one).
 * Ricer_TIMEOUT_DATA: timeout the node after a false busy channel alarm. Go
 * back to sleep.
 */
void RicerLayer::handleSelfMsg(cMessage *msg) {
    switch (macState) {
    case INIT:
        if (msg->getKind() == Ricer_START) {
            debugEV << "State INIT, message Ricer_START, new state SLEEP"
                           << endl;
            changeDisplayColor(BLACK);
            phy->setRadioState(MiximRadio::SLEEP);
            macState = SLEEP;
            // if node is receiver or transmitter - wake up periodically to send beacon
            if (role == NODE_RECEIVER || role == NODE_TRANSMITER) {
                scheduleAt(simTime(), wakeup);
            }
            return;
        }
        break;
    case SLEEP:
        // wake up periodically to receive data
        if (msg->getKind() == Ricer_WAKE_UP) {
            debugEV << "State SLEEP, message Ricer_WAKEUP, new state CCA" << endl;
            scheduleAt(simTime() + checkInterval, cca_timeout);
            macState = CCA;
            phy->setRadioState(MiximRadio::RX);
            changeDisplayColor(GREEN);
            wakeupTime = simTime().dbl();
            return;
        }
        // wake up to send data
        if (msg->getKind() == Ricer_WAKE_UP_DATA) {
            debugEV << "State SLEEP, message Ricer_WAKE_UP_DATA, new state WAIT_BEACON" << endl;
            macState = WAIT_BEACON;
            scheduleAt(simTime() + slotDuration, beacon_timeout);

            phy->setRadioState(MiximRadio::RX);
            changeDisplayColor(GREEN);
            // store wakeupTime to calculate idle
            wakeupTime = simTime().dbl();
            return;
        }
        break;

    case WAIT_BEACON:
        // if didn't receive beacon
        if (msg->getKind() == Ricer_BEACON_TIMEOUT) {
            // if something in the queue, wakeup soon.
            debugEV << "State WAIT_BEACON, message BEACON_TIMEOUT, new state SLEEP" << endl;
            macState = SLEEP;
            scheduleAt(simTime() + dblrand() * slotDuration, wakeup_data);

            phy->setRadioState(MiximRadio::SLEEP);
            changeDisplayColor(BLACK);
            return;
        }
        // if receive beacon
        if (msg->getKind() == Ricer_BEACON) {
            // if error in receiving packet - do nothing
            if (packetError) {
                delete msg;
                return;
            }
            nbRxBeacons++;
            if (macQueue.size() > 0) {
                MacPkt *pkt = macQueue.front()->dup();
                lastDataPktDestAddr = pkt->getDestAddr();
                // Store last source address of packet data - will be used in case retransmiter
                lastDataPktSrcAddr = pkt->getSrcAddr();
                MacPkt* beacon = static_cast<MacPkt *>(msg);
                const LAddress::L2Type& src = beacon->getSrcAddr();
                // send data packet when receive beacon from only destination or relay host.
                if (src == lastDataPktDestAddr || src == forwardAddr) {
                    debugEV << "State WAITBEACON, message Ricer_BEACON received, new state SEND_DATA" << endl;
                    macState = SEND_DATA;

                    phy->setRadioState(MiximRadio::TX);
                    changeDisplayColor(YELLOW);

                    // cancel wait over event
                    cancelEvent(beacon_timeout);
                    txAttempts = 1;
                }
                beacon = NULL;
                pkt = NULL;
            }
            delete msg;
            return;
        }
        break;
    case CCA:
        if (msg->getKind() == Ricer_CCA_TIMEOUT) {
            debugEV << "State CCA, message CCA_TIMEOUT new state SEND_BEACON" << endl;
            // Check if channel is free
            if (phy->getChannelState().isIdle()) {
                macState = SEND_BEACON;
                phy->setRadioState(MiximRadio::TX);
                changeDisplayColor(YELLOW);
                return;
            } else if(ccaAttempts < 3) {
                ccaAttempts++;
                scheduleAt(simTime() + checkInterval, cca_timeout);
            } else {
                debugEV << "State CCA 3 time attemps ccs, new state SLEEP" << endl;
                while (wakeupTime + slotDuration < simTime().dbl()) {
                    wakeupTime+= slotDuration;
                }
                scheduleAt(wakeupTime + slotDuration, wakeup);
                macState = SLEEP;
                phy->setRadioState(MiximRadio::SLEEP);
                changeDisplayColor(BLACK);
            }
        }
        break;
    case WAIT_TX_BEACON_OVER:
        if (msg->getKind() == Ricer_BEACON_TX_OVER) {
            debugEV << "State WAIT_TX_BEACON_OVER, message Ricer_BEACON_TX_OVER new state WAIT_DATA" << endl;
            macState = WAIT_DATA;
            phy->setRadioState(MiximRadio::RX);
            changeDisplayColor(GREEN);
            // stay awake in nbSlot of slotDuration to wait data from receiver
            scheduleAt(simTime() + slotDuration * nbSlot, data_timeout);
            return;
        }
        break;
    case WAIT_TX_DATA_OVER:
        if (msg->getKind() == Ricer_DATA_TX_OVER) {
            debugEV << "State WAIT_TX_DATA_OVER, message Ricer_DATA_TX_OVER new state WAIT_ACK" << endl;
            macState = WAIT_ACK;
            phy->setRadioState(MiximRadio::RX);
            changeDisplayColor(GREEN);
            scheduleAt(simTime() + slotDuration, ack_timeout);
            return;
        }
        break;
    case WAIT_ACK:
        if (msg->getKind() == Ricer_ACK_TIMEOUT) {
            if (txAttempts < maxTxAttempts) {
                debugEV << "State WAIT_ACK, message Ricer_ACK_TIMEOUT, new state SEND_DATA" << endl;
                txAttempts++;
                macState = SEND_DATA;
                phy->setRadioState(MiximRadio::TX);
                changeDisplayColor(YELLOW);
            } else {
                delete macQueue.front();
                macQueue.pop_front();
                if (macQueue.size() > 0) {
                    debugEV << "State WAIT_ACK, message Ricer_ACK_TIMEOUT, new state SLEEP with schedule wakeup to send other packet" << endl;
                    scheduleAt(simTime() + dblrand() * slotDuration, wakeup_data);
                }
                else {
                    debugEV << "State WAIT_ACK, message Ricer_ACK_TIMEOUT, new state SLEEP without schedule wakeup" << endl;
                }
                macState = SLEEP;
                phy->setRadioState(MiximRadio::SLEEP);
                changeDisplayColor(BLACK);
                nbMissedAcks++;
            }
            return;
        }
        if (msg->getKind() == Ricer_ACK) {
            if (packetError) {
                delete msg;
                return;
            }
            debugEV << "State WAIT_ACK, message Ricer_ACK" << endl;
            MacPkt* mac = static_cast<MacPkt *>(msg);
            const LAddress::L2Type& src = mac->getSrcAddr();
            // the right ACK is received..
            debugEV << "We are waiting for ACK from : " << lastDataPktDestAddr
                           << ", and ACK came from : " << src << endl;
            debugEV << "New state SLEEP" << endl;
            nbRecvdAcks++;
            cancelEvent(ack_timeout);

            // remove data packet in queue
            delete macQueue.front();
            macQueue.pop_front();

            if (macQueue.size() > 0) {
                scheduleAt(simTime() + dblrand() * slotDuration, wakeup_data);
            } else if (role == NODE_TRANSMITER) {
                while (wakeupTime + slotDuration < simTime().dbl()) {
                    wakeupTime+= slotDuration;
                }
                scheduleAt(wakeupTime + slotDuration, wakeup);
            }
            macState = SLEEP;
            phy->setRadioState(MiximRadio::SLEEP);
            changeDisplayColor(BLACK);
            lastDataPktDestAddr = LAddress::L2BROADCAST;

            mac = NULL;
            delete msg;
            return;
        }
        break;
    case WAIT_DATA:
        if (msg->getKind() == Ricer_DATA) {
            // if receive data packet but this packet is error (channel noisy or conflict) - do nothing, wait other packet
            if (packetError) {
                delete msg;
                return;
            }

            dataPkt_prt_t mac = static_cast<dataPkt_prt_t>(msg);
            const LAddress::L2Type& dest = mac->getDestAddr();
            const LAddress::L2Type& src = mac->getSrcAddr();
            lastDataPktSrcAddr = src;

            // if current node is destination
            if (role == NODE_RECEIVER) {
                if (mac->getPacketsArraySize() > 0) {
                    nbRxDataPackets += mac->getPacketsArraySize();
                    for (int i = 0; i < mac->getPacketsArraySize(); i++) {
                        DATAPkt tmp = mac->getPackets(i);
                        sendUp(decapsMsg(&tmp));
                    }
                } else {
                    nbRxDataPackets++;
                    sendUp(decapsMsg(mac));
                }
            } else {
                nbRxDataPackets++;
                macQueue.push_back(mac);
            }
            delete msg;
            debugEV << "State WAIT_DATA, message Ricer_DATA, new state SEND_ACK" << endl;
            macState = SEND_ACK;
            phy->setRadioState(MiximRadio::TX);
            changeDisplayColor(YELLOW);
            mac = NULL;
            return;
        }
        if (msg->getKind() == Ricer_DATA_TIMEOUT) {
            debugEV << "State WAIT_DATA, message Ricer_DATA_TIMEOUT, new state SLEEP" << endl;
            while (wakeupTime + iwu < simTime().dbl()) {
                wakeupTime+= iwu;
            }
            scheduleAt(wakeupTime + iwu, wakeup);
            macState = SLEEP;
            phy->setRadioState(MiximRadio::SLEEP);
            changeDisplayColor(BLACK);
            return;
        }
        break;
    case WAIT_TX_ACK_OVER:
        if (msg->getKind() == Ricer_ACK_TX_OVER) {
            // if still have data in queue -> wait other beacon to send data
            if (macQueue.size() > 0) {
                scheduleAt(simTime(), wakeup_data);
                macState = SLEEP;
            } else { // no data to send -> sleep & wake up periodically to send beacon
                debugEV << "State WAIT_ACK_TX, message Ricer_ACK_TX_OVER, new state SLEEP" << endl;
                while (wakeupTime + slotDuration < simTime().dbl()) {
                    wakeupTime+= slotDuration;
                }
                scheduleAt(wakeupTime + slotDuration, wakeup);
                macState = SLEEP;
                phy->setRadioState(MiximRadio::SLEEP);
                changeDisplayColor(BLACK);
                lastDataPktSrcAddr = LAddress::L2BROADCAST;
            }
            return;
        }
        break;
    }
    debugEV << "Event type: " << msg->getKind() << " in state " << macState << " MiximRadio state " << phy->getRadioState() << endl;
    if (msg->getKind() == Ricer_BEACON || msg->getKind() == Ricer_DATA || msg->getKind() == Ricer_ACK || msg->getKind() == Ricer_DATA_AGG) {
        delete msg;
    }
}

/**
 * Handle Ricer beacons and received data packets.
 */
void RicerLayer::handleLowerMsg(cMessage *msg) {
    // simply pass the massage as self message, to be processed by the FSM.
    handleSelfMsg(msg);
}

void RicerLayer::sendDataPacket() {
    nbTxDataPackets++;
    DATAPkt *pkt = new DATAPkt();
    // if relay node
    if (role == NODE_TRANSMITER) {
        pkt->setSrcAddr(myMacAddr);
        pkt->setDestAddr(forwardAddr);
        int nbPkt = macQueue.size();
        pkt->setPacketsArraySize(nbPkt);
        int idx = 0;
        // Aggregate all packets in queue & send to destination
        while (macQueue.front()) {
            dataPkt_prt_t tmp = macQueue.front()->dup();
            pkt->setPackets(idx, *tmp);
            // remove data packet in queue
            delete macQueue.front();
            macQueue.pop_front();
        }
    } else {
        pkt = macQueue.front()->dup();
    }
    attachSignal(pkt);
    lastDataPktDestAddr = pkt->getDestAddr();
    pkt->setKind(Ricer_DATA);
    pkt->setName("DATA");
    sendDown(pkt);
}

/**
 * Handle transmission over messages: either send another beacons or the data
 * packet itself.
 */
void RicerLayer::handleLowerControl(cMessage *msg) {
    // Transmission of one packet is over
    if (msg->getKind() == MacToPhyInterface::TX_OVER) {
        if (macState == WAIT_TX_DATA_OVER) {
            scheduleAt(simTime(), data_tx_over);
        }
        if (macState == WAIT_TX_BEACON_OVER) {
            scheduleAt(simTime(), beacon_tx_over);
        }
        if (macState == WAIT_TX_ACK_OVER) {
            scheduleAt(simTime(), ack_tx_over);
        }
        packetError = false;
    }
    // MiximRadio switching (to RX or TX) is over, ignore switching to SLEEP.
    else if (msg->getKind() == MacToPhyInterface::RADIO_SWITCHING_OVER) {
        if ((macState == SEND_BEACON) && (phy->getRadioState() == MiximRadio::TX)) {
            macState = WAIT_TX_BEACON_OVER;
            sendBeacon();
        }
        if ((macState == SEND_ACK) && (phy->getRadioState() == MiximRadio::TX)) {
            macState = WAIT_TX_ACK_OVER;
            sendMacAck();
        }
        if ((macState == SEND_DATA) && (phy->getRadioState() == MiximRadio::TX)) {
            macState = WAIT_TX_DATA_OVER;
            sendDataPacket();
        }
        packetError = false;
    } else {
        if (msg->getKind() == BaseDecider::PACKET_DROPPED) {
            packetError = true;
            nbPacketError++;
        } else if (msg->getKind() == Decider802154Narrow::RECEPTION_STARTED) {

        } else {
            std::cout << "control message with wrong kind (" << msg->getKind() << ") -- deleting\n";
        }
    }
    delete msg;
}

/**
 * Encapsulates the received network-layer packet into a MacPkt and set all
 * needed header fields.
 */
bool RicerLayer::addToQueue(cMessage *msg) {
    if (macQueue.size() >= queueLength) {
        // queue is full, message has to be deleted
        debugEV << "New packet arrived, but queue is FULL, so new packet is deleted\n";
        msg->setName("MAC ERROR");
        msg->setKind(PACKET_DROPPED);
        sendControlUp(msg);
        droppedPacket.setReason(DroppedPacket::QUEUE);
        emit(BaseLayer::catDroppedPacketSignal, &droppedPacket);
        nbDroppedDataPackets++;
        return false;
    }

    DATAPkt *macPkt = new DATAPkt(msg->getName());
    macPkt->setBitLength(headerLength);
    cObject * const cInfo = msg->removeControlInfo();
    //EV<<"CSMA received a message from upper layer, name is "
    //  << msg->getName() <<", CInfo removed, mac addr="
    //  << cInfo->getNextHopMac()<<endl;
    macPkt->setDestAddr(getUpperDestinationFromControlInfo(cInfo));
    delete cInfo;
    macPkt->setSrcAddr(myMacAddr);

    assert(static_cast<cPacket*>(msg));
    macPkt->encapsulate(static_cast<cPacket*>(msg));

    macQueue.push_back(macPkt);
    debugEV << "Max queue length: " << queueLength << ", packet put in queue"
            "\n  queue size: " << macQueue.size() << " macState: " << macState
                   << endl;

    return true;
}

void RicerLayer::attachSignal(MacPkt *macPkt) {
    //calc signal duration

    simtime_t duration = macPkt->getBitLength() / bitrate;
//	debugEV <<"length" <<macPkt->getName()<<endl;
    //create and initialize control info with new signal
    setDownControlInfo(macPkt, createSignal(simTime(), duration, txPower, bitrate));
}

/**
 * Change the color of the node for animation purposes.
 */

void RicerLayer::changeDisplayColor(Ricer_COLORS color) {
    if (!animation)
        return;
    cDisplayString& dispStr = findHost()->getDisplayString();
    //b=40,40,rect,black,black,2"
    if (color == GREEN)
        dispStr.setTagArg("b", 3, "green");
    //dispStr.parse("b=40,40,rect,green,green,2");
    if (color == BLUE)
        dispStr.setTagArg("b", 3, "blue");
    //dispStr.parse("b=40,40,rect,blue,blue,2");
    if (color == RED)
        dispStr.setTagArg("b", 3, "red");
    //dispStr.parse("b=40,40,rect,red,red,2");
    if (color == BLACK)
        dispStr.setTagArg("b", 3, "black");
    //dispStr.parse("b=40,40,rect,black,black,2");
    if (color == YELLOW)
        dispStr.setTagArg("b", 3, "yellow");
    //dispStr.parse("b=40,40,rect,yellow,yellow,2");
}
