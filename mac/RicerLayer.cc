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

#include "RicerLayer.h"

#include <cassert>

#include "FWMath.h"
#include "MacToPhyControlInfo.h"
#include "BaseArp.h"
#include "BaseConnectionManager.h"
#include "PhyUtils.h"
#include "MacPkt_m.h"
#include "MacToPhyInterface.h"

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
        slotDuration = hasPar("slotDuration") ? par("slotDuration") : 0.09;
        bitrate = hasPar("bitrate") ? par("bitrate") : 15360;
        headerLength = hasPar("headerLength") ? par("headerLength") : 10;
        checkInterval = hasPar("checkInterval") ? par("checkInterval") : 0.005;
        dataduration = hasPar("dataduration") ? par("dataduration") : 0.01;
        initialization = hasPar("initialization") ? par("initialization") : 0.001;
        randInit = hasPar("randInit") ? par("randInit") : false;
        buzzduration = headerLength / bitrate;
//        dataduration = 20 * headerLength / bitrate;
        debugEV << "headerLength: " << headerLength << ", bitrate: " << bitrate << endl;

        txPower = hasPar("txPower") ? par("txPower") : 50;
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
        nbPacketDrop = 0;
        nbTxRelayData = 0;
        nbRxRelayData = 0;

        txAttempts = 0;
        lastDataPktDestAddr = LAddress::L2BROADCAST;
        lastDataPktSrcAddr = LAddress::L2BROADCAST;
        relayAddr.setAddress(hasPar("relayAddr") ? par("relayAddr") : "ff:ff:ff:ff:ff:ff");

        macState = INIT;

        // init the dropped packet info
        droppedPacket.setReason(DroppedPacket::NONE);
        nicId = getNic()->getId();
        WATCH(macState);
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

        wait_over = new cMessage("wait_over");
        wait_over->setKind(Ricer_WAIT_OVER);

        ack_tx_over = new cMessage("ack_tx_over");
        ack_tx_over->setKind(Ricer_ACK_TX_OVER);

        cca_timeout = new cMessage("cca_timeout");
        cca_timeout->setKind(Ricer_CCA_TIMEOUT);
        cca_timeout->setSchedulingPriority(100);

        ack_timeout = new cMessage("ack_timeout");
        ack_timeout->setKind(Ricer_ACK_TIMEOUT);

        start_Ricer = new cMessage("start_Ricer");
        start_Ricer->setKind(Ricer_START_Ricer);
        scheduleAt(0.0, start_Ricer);
    }
}

RicerLayer::~RicerLayer() {
    cancelAndDelete(wakeup);
    cancelAndDelete(wakeup_data);
    cancelAndDelete(data_timeout);
    cancelAndDelete(data_tx_over);
    cancelAndDelete(beacon_tx_over);
    cancelAndDelete(wait_over);
    cancelAndDelete(ack_tx_over);
    cancelAndDelete(cca_timeout);
    cancelAndDelete(ack_timeout);
    cancelAndDelete(start_Ricer);

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
        recordScalar("nbRxRelayData", nbRxRelayData);
        recordScalar("nbTxRelayData", nbTxRelayData);
        recordScalar("nbPacketDrop", nbPacketDrop);
        recordScalar("nbTxDataPackets", nbTxDataPackets);
        recordScalar("nbTxBeacons", nbTxBeacons);
        recordScalar("nbRxDataPackets", nbRxDataPackets);
        recordScalar("nbRxBeacons", nbRxBeacons);
        recordScalar("nbMissedAcks", nbMissedAcks);
        recordScalar("nbRecvdAcks", nbRecvdAcks);
        recordScalar("nbTxAcks", nbTxAcks);
        recordScalar("nbDroppedDataPackets", nbDroppedDataPackets);
        //recordScalar("timeSleep", timeSleep);
        //recordScalar("timeRX", timeRX);
        //recordScalar("timeTX", timeTX);
    }
}

/**
 * Check whether the queue is not full: if yes, print a warning and drop the
 * packet. Then initiate sending of the packet, if the node is sleeping. Do
 * nothing, if node is working.
 */
void RicerLayer::handleUpperMsg(cMessage *msg) {
    bool pktAdded = addToQueue(msg);
    if (!pktAdded)
        return;
    // force wakeup now
    if (macState == SLEEP) {
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
    beacon->setBitLength(headerLength);

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
    ack->setBitLength(headerLength);

    //attach signal and send down
    attachSignal(ack);
    sendDown(ack);
    nbTxAcks++;
    //endSimulation();
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
        if (msg->getKind() == Ricer_START_Ricer) {
            debugEV << "State INIT, message Ricer_START, new state SLEEP"
                           << endl;
            changeDisplayColor(BLACK);
            phy->setRadioState(MiximRadio::SLEEP);
            macState = SLEEP;
            // if node is receiver or transmitter - wake up periodically to send beacon
            if (role == NODE_RECEIVER || role == NODE_TRANSMITER) {
                if (randInit) {
                    double init = dblrand() * slotDuration;
                    std::cout<< init << std::endl;
                    scheduleAt(simTime() + init + initialization, wakeup);
                } else {
                    scheduleAt(simTime() + initialization, wakeup);
                }
            }
            return;
        }
        break;
    case SLEEP:
        // wake up periodically to receive data
        if (msg->getKind() == Ricer_WAKE_UP) {
            debugEV << "State SLEEP, message Ricer_WAKEUP, new state CCA" << endl;
            scheduleAt(simTime() + buzzduration, cca_timeout);
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
            scheduleAt(simTime() + slotDuration, wait_over);

            phy->setRadioState(MiximRadio::RX);
            changeDisplayColor(GREEN);
            return;
        }
        break;

    case WAIT_BEACON:
        // if didn't receive beacon
        if (msg->getKind() == Ricer_WAIT_OVER) {
            // if something in the queue, wakeup soon.
            debugEV << "State WAIT_BEACON, message WAIT_OVER, new state SLEEP" << endl;
            macState = SLEEP;
            scheduleAt(simTime() + dblrand() * slotDuration, wakeup_data);

            phy->setRadioState(MiximRadio::SLEEP);
            changeDisplayColor(BLACK);
            return;
        }
        // if receive beacon
        if (msg->getKind() == Ricer_BEACON) {
            nbRxBeacons++;
            if (macQueue.size() > 0) {
                MacPkt *pkt = macQueue.front()->dup();
                lastDataPktDestAddr = pkt->getDestAddr();
                // Store last source address of packet data - will be used in case retransmiter
                lastDataPktSrcAddr = pkt->getSrcAddr();
                MacPkt* beacon = static_cast<MacPkt *>(msg);
                const LAddress::L2Type& src = beacon->getSrcAddr();
                // send data packet when receive beacon from only destination or relay host.
                if (src == lastDataPktDestAddr || src == relayAddr) {
                    debugEV << "State WAITBEACON, message Ricer_BEACON received, new state SEND_DATA" << endl;
                    macState = SEND_DATA;

                    phy->setRadioState(MiximRadio::TX);
                    changeDisplayColor(YELLOW);

                    // cancel wait over event
                    cancelEvent(wait_over);
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
            if (phy->getChannelState().isIdle()) {
                macState = SEND_BEACON;
                phy->setRadioState(MiximRadio::TX);
                changeDisplayColor(YELLOW);
                return;
            } else if(ccaAttempts < 3) {
                ccaAttempts++;
                scheduleAt(simTime() + buzzduration, cca_timeout);
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
            scheduleAt(simTime() + dataduration, data_timeout);
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
            cancelEvent(data_timeout);
            nbRxDataPackets++;
            MacPkt* mac = static_cast<MacPkt *>(msg);
            const LAddress::L2Type& dest = mac->getDestAddr();
            const LAddress::L2Type& src = mac->getSrcAddr();
            if (dest == myMacAddr) {
                sendUp(decapsMsg(mac));
                lastDataPktSrcAddr = src;
            } else {
                // if can push this packet to queue
                if (macQueue.size() < queueLength) {
                    macQueue.push_back(mac);
                } else {
                    delete msg;
                }
            }
            debugEV << "State WAIT_DATA, message Ricer_DATA, new state SEND_ACK" << endl;
            macState = SEND_ACK;
            phy->setRadioState(MiximRadio::TX);
            changeDisplayColor(YELLOW);
            mac = NULL;
            return;
        }
        if (msg->getKind() == Ricer_DATA_TIMEOUT) {
            debugEV << "State WAIT_DATA, message Ricer_DATA_TIMEOUT, new state SLEEP" << endl;
            while (wakeupTime + slotDuration < simTime().dbl()) {
                wakeupTime+= slotDuration;
            }
            scheduleAt(wakeupTime + slotDuration, wakeup);
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
    if (msg->getKind() == Ricer_BEACON || msg->getKind() == Ricer_DATA || msg->getKind() == Ricer_ACK) {
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
    MacPkt *pkt = macQueue.front()->dup();
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
    }
    // MiximRadio switching (to RX or TX) ir over, ignore switching to SLEEP.
    else if (msg->getKind() == MacToPhyInterface::RADIO_SWITCHING_OVER) {
        // we just switched to TX after CCA, so simply send the first
        // sendPremable self message
        if ((macState == SEND_BEACON)
                && (phy->getRadioState() == MiximRadio::TX)) {
            macState = WAIT_TX_BEACON_OVER;
            sendBeacon();
        }
        if ((macState == SEND_ACK)
                && (phy->getRadioState() == MiximRadio::TX)) {
            macState = WAIT_TX_ACK_OVER;
            sendMacAck();
        }
        // Switching radio to send data
        if ((macState == SEND_DATA)
                && (phy->getRadioState() == MiximRadio::TX)) {
            macState = WAIT_TX_DATA_OVER;
            sendDataPacket();
        }
    } else {
        std::cout << "control message with wrong kind -- deleting\n";
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
        debugEV << "New packet arrived, but queue is FULL, so new packet is"
                " deleted\n";
        msg->setName("MAC ERROR");
        msg->setKind(PACKET_DROPPED);
        sendControlUp(msg);
        droppedPacket.setReason(DroppedPacket::QUEUE);
        emit(BaseLayer::catDroppedPacketSignal, &droppedPacket);
        nbDroppedDataPackets++;

        return false;
    }

    MacPkt *macPkt = new MacPkt(msg->getName());
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
