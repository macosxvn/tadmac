package ftamac.modules.application;

import org.mixim.base.modules.IBaseApplLayer;

//****************************************************************************
// * file:        SensorApplLayer.ned
// *
// * author:      Amre El-Hoiydi, Jerome Rousselot
// *
// * copyright:   (C) 2007-2009 CSEM
// *
// *              This program is free software; you can redistribute it 
// *              and/or modify it under the terms of the GNU General Public 
// *              License as published by the Free Software Foundation; either
// *              version 2 of the License, or (at your option) any later 
// *              version.
// *              For further information see file COPYING 
// *              in the top level directory
// ***************************************************************************
// * part of:     framework implementation developed by tkn
// * description: Generate periodic traffic addressed to a sink
// *
// ***************************************************************************/

import org.mixim.base.modules.IBaseApplLayer;

// @brief Application layer to test lower layer implementations. 
// SensorApplLayer waits a time initializationTime, then starts sending
// nbPackets (of size headerLength) to host destAddr. The time intervals
// between two packets can follow three distributions: periodic (fixed),
// uniform or exponential.
//
// @ingroup application
// @author Jerome Rousselot, Amre El-Hoiydi
simple NormalApplLayer like IBaseApplLayer {
    parameters:
        bool debug = default(false); // debug switch
        bool stats = default(true);   // stats generation switch
        bool trace = default(true);  // activates detailed logging (per source latencies and vector logging). stats must be true.
        bool broadcastPackets = default(false); // send packets in broadcast mode
        double nbPackets = default(0);
        int destAddr = default(0);
        string trafficType = default("normal");  	          // Can be one of: normal, periodic or exponential, variable
        double trafficParam @unit(s) = default(1 s);			// the mean time between two packets
        double trafficStability @unit(s) = default(0.1 s);
        double initializationTime @unit(s) = default(1 s); // minimum time before generation of the first packet
		int headerLength @unit(byte) = default(2 byte);
        bool notAffectedByHostState = default(true);
        int nbChange = default(0);
        double runTime @unit(s) = default(1000s);
        string dataInput = default("");     // file data input - use the pre-generate data traffic
    gates:
        input lowerLayerIn; // from network layer
        input lowerControlIn;
        output lowerLayerOut; // to network layer
        output lowerControlOut;
}
