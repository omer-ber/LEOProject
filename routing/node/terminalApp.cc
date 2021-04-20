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

#include "terminalApp.h"

Define_Module(TerminalApp);

/************************************************************************************************************************************/
/*---------------------------------------CODE FLOW IDEA-----------------------------------------------------------------------------*/
/************************************************************************************************************************************/
/* TerminalApp generates traffic (via self messages), and sends it to a satellite the terminal is connected to.
 * There are 2 different kinds of satellite - [main] and [sub]. The [main] satellite is the first satellite the terminal has connected
 *      to, and [sub] satellite is the second. When connected to sub satellite, the terminal would prioritize sending traffic to [sub]
 *      as it is more probable to stay around (Although it is possible for the [main] satellite to be the better option).
 * The self messages are of 3 different types:
 *      1. Create traffic (normal traffic for example)
 *      2. Update position - Keep track of the positions (=XY coordinates) of the 2 satellites and the terminal itself. Each update
 *                      leads to connection check (if satellites are still in disk).
 *      3. Initialize - INET's mobility module finishes initialization after this module, so self messages to finish
 *                      location initialization are necessary.
 *
 * The connection process (referred as 'handshake' in code) is as following:
 *      1. Connected satellites are chosen by minimum distance from terminal. [sub] satellites are chosen when [main] satellites
 *              are at distance of [thresholdRadius] from terminal.
 *      2. Send a [terminal_connect] message to a satellite. The satellite then begins looking for an open port to assign to terminal.
 *      3. The satellite returns [terminal_index_assign] message, which contains port index. If no port was found the assigned index is
 *              -1, and every self message of traffic creation is ignored until a satellite is found.
 * The disconnection method is different - send [terminal_disconnect] message and the satellite simply removes terminal from registry.
 *
 * When not connected to any satellite, the "update self position message" self message needs to be sent each update interval, to try
 *      and connect to any satellite to continue transmitting data to other terminals.
 * */

/************************************************************************************************************************************/
/*---------------------------------------PUBLIC FUNCTIONS---------------------------------------------------------------------------*/
/************************************************************************************************************************************/
TerminalApp::~TerminalApp(){
    delete indexList;
    delete numOfBurstMsg;
}

void TerminalApp::updatePosition(inet::IMobility* &mob, double &posX, double &posY){
    /* Find the (X,Y) position of the a module via mobility module.
     * Position is stored in given [posX] & [posY]
     * */

    posX = mob->getCurrentPosition().getX();
    posY = mob->getCurrentPosition().getY();
}

int TerminalApp::getConnectedSatelliteAddress(){
    /* Returns the address of the satellite connected to the terminal. If disconnected return -1.
     * */

    switch (isConnected){
        case connected:{
            // Only one is connected, that is the main
            return mainSatAddress;
        }
        case connectedToSub:{
            // Two are connected, return the sub satellite (More likely to be around when the packet is sent)
            return subSatAddress;
        }
        default:
            return -1;
    }
}

double TerminalApp::getDistanceFromSatellite(int satAddress){
    /* Get the distance from the terminal to [satAddress] satellite.
     *      This function reads [satAddress] satellite's display string, parsing its position from it
     *      and returns the distance from that satellite.
     * This function assumes that the terminal's position is up-to-date
     * */

    // Read position
    inet::IMobility *mob = check_and_cast<inet::IMobility*>(getParentModule()->getParentModule()->getSubmodule("rte", satAddress)->getSubmodule("mobility"));
    double satPosX, satPosY;

    // Parse the position to [satPosX] & [satPosY]
    updatePosition(mob, satPosX, satPosY);

    // Return distance from satellite
    return sqrt(pow(myPosX-satPosX,2)+pow(myPosY-satPosY,2));
}

int TerminalApp::getClosestSatellite(int ignoreIndex){
    /* Iterate over all satellites while ignoring the satellite index given in [ignoreIndex].
     * Returns the index of the closest satellite, or -1 if none are inside radius
     * */

    // Find closest satellite
    double minDistance = DBL_MAX;
    int satAddress = -1;
    for(int i = 0; i < numOfSatellites; i++){           // Iterate over all satellites
        if (i != ignoreIndex){                          // Ignore [ignoreIndex] satellite
            double dist = getDistanceFromSatellite(i);  // Calculate distance to satellite i

            // Save shortest distance (of a satellite inside the disk) and satellite index
            if (dist <= radius && dist < minDistance){
                minDistance = dist;
                satAddress = i;
            }
        }
    }

    return satAddress;
}

/************************************************************************************************************************************/
/*---------------------------------------PRIVATE FUNCTIONS--------------------------------------------------------------------------*/
/************************************************************************************************************************************/
void TerminalApp::initialize()
{
    // Initialize message pointers
    appMsg = nullptr;
    burstAppMsg = nullptr;
    pingAppMsg = nullptr;
    updateMyPositionMsg = nullptr;
    updateMainSatellitePositionMsg = nullptr;
    updateSubSatellitePositionMsg = nullptr;

    // Initialize parameters
    myAddress = par("address").intValue();
    numOfTerminals = par("numOfTerminals").intValue();
    numOfSatellites = getParentModule()->getParentModule()->par("num_of_hosts").intValue();
    radius = getParentModule()->par("radius").doubleValue();
    rate =  check_and_cast<cDatarateChannel*>(getParentModule()->getParentModule()->getSubmodule("rte", 0)->getSubmodule("queue", 0)->gate("line$o")->getTransmissionChannel())->getDatarate();
    thresholdRadius = radius * getParentModule()->par("frac").doubleValue();

    //// Regular App
    sendIATime = &par("sendIaTime");
    packetLengthBytes = &par("packetLength");
    /* Create an index array of possible targets.
     * The array is {0,1,...,numOfTerminals} without [myAddress].
     * When choosing a destination we take a random integer (name it
     *      'r') in range [0,numOfTerminals). That's the index
     *      in the above array. The destination itself is indexList[r]
     * There are numOfTerminals-1 elements in the array, meaning the
     *      index goes from 0 to numOfTerminals-2
     * In code: setDestAddr(indexList[intuniform(0, numOfTerminals-2)])
     * */
    indexList = new int[numOfTerminals-1];
    for(int i = 0, j = 0; i < numOfTerminals; i++){
        if (i != myAddress){
            indexList[j] = i;
            j++;
        }
    }

    //// Burst App
    burstNextInterval = &par("burst_next_interval");
    burstSize = &par("burst_size");
    /* Create a 'flow' array. Every time a message from a flow is sent,
     *      reduce the number saved in the array (at target index) by 1.
     *      Messages are sent after [burstNextInterval], and only if the
     *      relevant position in the array is not 0.
     * When the number hits 0, the next time the burst app is activated
     *      it only generates a random number (via burstSize) and does
     *      nothing else.
     * */
    numOfBurstMsg = new int[numOfTerminals];
    for(int i = 0, j = 0; i < numOfTerminals; i++){
        numOfBurstMsg[i] = 0;
    }

    //// Ping App
    pingTime = &par("pingTime");

    // Initialize statistics
    numSent = 0;                                           // Record with recordScalar()
    numReceived = 0;                                       // Record with recordScalar()
    latencyVector = new cOutVector[numOfTerminals];        // Record with .record() method
    for(int i = 0; i < numOfTerminals; i++){
        std::string latencyName = "latency with " + std::to_string(i);
        latencyVector[i].setName(latencyName.c_str());
        latencyVector[i].setUnit("ms");
    }
    hopCountVector.setName("Hop Count Vector");            // Record with .record() method
    hopCountHistogram.setName("Hop Count Histogram");      // First .collect() data, at finish use .record() method
    bytesSentSuccessfully = 0;
    totalBytesSent = 0;
    efficiency.setName("Efficiency");                      // Record with .record() method

    // Initialize position
    myMob = check_and_cast<inet::IMobility*>(getParentModule()->getSubmodule("mobility"));
    updateInterval = 0; //getParentModule()->getSubmodule("mobility")->par("updateInterval").doubleValue();
    scheduleAt(simTime(), new cMessage("Initialize self position", initializeMyPosition));

    // Initialize satellite databases
    isConnected = disconnected;
    resetSatelliteData(mainSatAddress, mainSatPosX, mainSatPosY, mainSatUpdateInterval, mainConnectionIndex, updateMainSatellitePositionMsg, mainSatMob);
    resetSatelliteData(subSatAddress, subSatPosX, subSatPosY, subSatUpdateInterval, subConnectionIndex, updateSubSatellitePositionMsg, subSatMob);

    // Initialize satellite connection - connect to closest satellite as main
    scheduleAt(simTime(), new cMessage("Initialize connection", initializeConnection));

    // Start regular app
    appMsg = new cMessage("Inter Arrival Time Self Message", interArrivalTime);
    scheduleAt(simTime() + sendIATime->doubleValue(), appMsg);

    // Start burst app
    burstAppMsg = new cMessage("Burst App Activation Self Message", burstAppProcess);
    scheduleAt(simTime() + 0.01, burstAppMsg);

    // Start ping app
    pingAppMsg = new cMessage("Ping App Self Message", pingAppProcess);
    scheduleAt(simTime() + pingTime->doubleValue(), pingAppMsg);
}

void TerminalApp::handleMessage(cMessage *msg)
{
    if (msg->isSelfMessage()){
        // Self Message - Handle inner functionality
        switch (msg->getKind()){
            case interArrivalTime:{
                //// Send data out

                if (isConnected){
                    // Terminal is connected to a satellite - create new message

                    int satAddress = isConnected == connected? mainSatAddress : subSatAddress;  // Choose target satellite - if there are two give priority to sub
                    int connectionIndex = isConnected == connected? mainConnectionIndex : subConnectionIndex;

                    TerminalMsg *terMsg = new TerminalMsg("Regular Message", terminal);
                    terMsg->setSrcAddr(myAddress);
                    terMsg->setDestAddr(indexList[intuniform(0, numOfTerminals-2)]);
                    terMsg->setPacketType(terminal_message);
                    terMsg->setByteLength((int)packetLengthBytes->intValue());
                    terMsg->setReplyType(udpSingle);

                    /* Note - sendDirect(cMessage *msg, simtime_t propagationDelay, simtime_t duration, cModule *mod, const char *gateName, int index=-1)
                     *        is being used here.
                     * */
                    sendDirect(terMsg, 0, terMsg->getByteLength()/rate,getParentModule()->getParentModule()->getSubmodule("rte", satAddress),"terminalIn", connectionIndex);

                    // Update statistics
                    numSent++;
                    totalBytesSent += terMsg->getByteLength();
                }
                else{
                    EV << "Terminal " << myAddress << " is not connected to any satellite, retrying to send app data later" << endl;
                    if (hasGUI())
                        getParentModule()->bubble("Not connected!");
                }

                cancelEvent(appMsg);
                scheduleAt(simTime() + sendIATime->doubleValue(), appMsg);
                break;
            }
            case burstAppProcess:{
                //// Burst app wishes to send data out

                if (isConnected){
                    //// Terminal is connected to a satellite - create new message

                    int satAddress = isConnected == connected? mainSatAddress : subSatAddress;
                    int connectionIndex = isConnected == connected? mainConnectionIndex : subConnectionIndex;
                    for(int i = 0; i < numOfTerminals; i++){
                        if (i != myAddress){ // Do not send messages to self
                            if (numOfBurstMsg[i]){
                                //// Burst app has traffic for terminal i
                                TerminalMsg *terMsg = new TerminalMsg("Burst App Message", terminal);
                                terMsg->setSrcAddr(myAddress);
                                terMsg->setDestAddr(i);
                                terMsg->setPacketType(terminal_message);
                                terMsg->setByteLength((int)packetLengthBytes->intValue());
                                terMsg->setReplyType(udpFlow);
                                sendDirect(terMsg, 0, terMsg->getByteLength()/rate,getParentModule()->getParentModule()->getSubmodule("rte", satAddress),"terminalIn", connectionIndex);
                                numOfBurstMsg[i]--;

                                // Update statistics
                                numSent++;
                                totalBytesSent += terMsg->getByteLength();
                            }
                            else{
                                //// No traffic exists for terminal i

                                // Generate traffic
                                numOfBurstMsg[i] = (int)(burstSize->doubleValue());
                            }
                        }
                    }
                }
                else{
                    //// Terminal is not connected to a satellite
                    EV << "Terminal " << myAddress << " is not connected to any satellite, closing bursts..." << endl;

                    // Closing bursts
                    for(int i = 0, j = 0; i < numOfTerminals; i++){
                        numOfBurstMsg[i] = 0;
                    }

                    if (hasGUI())
                        getParentModule()->bubble("Not connected!");
                }

                cancelEvent(burstAppMsg);
                scheduleAt(simTime() + burstNextInterval->doubleValue(), burstAppMsg);
                break;
            }
            case pingAppProcess:{
                //// Time to send ping

                if (isConnected){
                    // Terminal is connected to a satellite - send ping

                    int satAddress = isConnected == connected? mainSatAddress : subSatAddress;
                    int connectionIndex = isConnected == connected? mainConnectionIndex : subConnectionIndex;
                    int dst = indexList[intuniform(0, numOfTerminals-2)];

                    std::string pingName = "Ping to " + std::to_string(dst) + " from " + std::to_string(myAddress);
                    TerminalMsg *terMsg = new TerminalMsg(pingName.c_str(), terminal);
                    terMsg->setSrcAddr(myAddress);
                    terMsg->setDestAddr(dst);
                    terMsg->setPacketType(terminal_message);
                    terMsg->setByteLength(PING_SIZE);
                    terMsg->setReplyType(ping);
                    sendDirect(terMsg, 0, terMsg->getByteLength()/rate,getParentModule()->getParentModule()->getSubmodule("rte", satAddress),"terminalIn", connectionIndex);

                    // Update statistics
                    numSent++;
                    totalBytesSent += terMsg->getByteLength();
                }
                else{
                    EV << "Terminal " << myAddress << " is not connected to any satellite, retrying to send ping later" << endl;
                    if (hasGUI())
                        getParentModule()->bubble("Not connected!");
                }

                cancelEvent(pingAppMsg);
                scheduleAt(simTime() + pingTime->doubleValue(), pingAppMsg);
                break;
            }
            case selfPositionUpdateTime:{
                //// Update self position & check satellite connection

                updatePosition(myMob, myPosX, myPosY);

                // Check connection, try to connect if found any satellite
                if (!isConnected){
                    int canConnect = findSatelliteToConnect(-1, main);
                    if (canConnect != -1){
                        // Do not try to reconnect again
                        cancelEvent(updateMyPositionMsg);
                        delete updateMyPositionMsg;
                        updateMyPositionMsg = nullptr;
                        return;
                    }
                }

                if (updateMyPositionMsg){
                    cancelEvent(updateMyPositionMsg);
                    scheduleAt(simTime() + updateInterval, updateMyPositionMsg);
                }
                break;
            }
            case mainSatellitePositionUpdateTime:{
                //// Update main satellite position & check connection

                double x = mainSatPosX, y = mainSatPosY;
                updatePosition(mainSatMob, mainSatPosX, mainSatPosY);
                if (x == mainSatPosX && y == mainSatPosY){
                    std::cout << "Noooooo" << endl;
                }

                if (checkConnection(main)){
                    // Satellite is still around the terminal

                    if (isConnected == connected){ // If no sub exists
                        // Check if distance is greater than threshold
                        if (getDistanceFromSatellite(mainSatAddress) >= thresholdRadius){

                            // The main satellite is outside of threshold - look for a new satellite to connect to
                            EV << "Terminal " << myAddress << " main satellite is outside threshold radius, looking for sub satellite" << endl;
                            findSatelliteToConnect(mainSatAddress, sub);
                        }
                    }
                }
                else{
                    // Terminal is too far away and is no longer connected

                    if (isConnected == connected){
                        // No sub - just disconnect
                        EV << "Main is out of range" << endl;
                        disconnectFromSatellite(main);

                        // Schedule self position update to reconnect
                        updateMyPositionMsg = new cMessage("update my position self message", selfPositionUpdateTime);
                        scheduleAt(simTime() + updateInterval, updateMyPositionMsg);
                    }
                    else{
                        // There is a sub satellite - it promotes to main
                        EV << "Main is out of range, upgrade sub to main" << endl;
                        disconnectFromSatellite(main);
                        upgradeSubToMain();

                        // Calculate exact time for position update + 0.01 and schedule position update
                        double remainingTime = getRemainingTime(mainSatUpdateInterval);
                        scheduleAt(simTime() + remainingTime, updateMainSatellitePositionMsg);

                        EV << "Terminal " << myAddress << " upgrade complete for satellite " << mainSatAddress << endl;
                        return;
                    }
                }

                // Send self message only if still connected
                if (updateMainSatellitePositionMsg){
                    cancelEvent(updateMainSatellitePositionMsg);
                    scheduleAt(simTime() + mainSatUpdateInterval, updateMainSatellitePositionMsg);
                }
                break;
            }
            case subSatellitePositionUpdateTime:{
                //// Update sub satellite position & check connection

                updatePosition(subSatMob, subSatPosX, subSatPosY);
                if (!checkConnection(sub)){
                    // Sub satellite is too far away - disconnect
                    disconnectFromSatellite(sub);
                }

                if (updateSubSatellitePositionMsg){
                    cancelEvent(updateSubSatellitePositionMsg);
                    scheduleAt(simTime() + subSatUpdateInterval, updateSubSatellitePositionMsg);
                }
                break;
            }
            case initializeMyPosition:{
                // Message to initialize original position of the terminal
                if (updateInterval){
                    updateMyPositionMsg = new cMessage("Update my position message", selfPositionUpdateTime);
                    scheduleAt(simTime() + updateInterval, updateMyPositionMsg);
                }
                updatePosition(myMob, myPosX, myPosY);

                if (!updateInterval){
                    updateInterval = getParentModule()->getParentModule()->getSubmodule("rte", 0)->getSubmodule("mobility")->par("updateInterval").doubleValue();
                }

                delete msg;
                break;
            }
            case initializeConnection:{
                // Try to connect to a satellite after initialization

                findSatelliteToConnect(-1, main);

                delete msg;
                break;
            }
        }
    }
    else{
        // Message from outside - Receive data

        if (!msg->getKind()){
            // Received legacy packet - error
            delete msg;
            std::cout << "Error! Received legacy packet!" << endl;
            endSimulation();
        }

        TerminalMsg *terMsg = check_and_cast<TerminalMsg*>(msg);
        switch (terMsg->getPacketType()){
            case terminal_message:{
                //// Received message from other terminal (isConnected != 0)
                // Record statistics
                simtime_t eed = simTime() - terMsg->getCreationTime();
                latencyVector[terMsg->getSrcAddr()].record(eed);
                hopCountVector.record(terMsg->getHopCount());
                hopCountHistogram.collect(terMsg->getHopCount());

                // Record efficiency - at source
                TerminalApp* src = check_and_cast<TerminalApp*>(getParentModule()->getParentModule()->getSubmodule("terminal", terMsg->getSrcAddr())->getSubmodule("app"));
                src->bytesSentSuccessfully += terMsg->getByteLength();
                src->numReceived++;
                src->efficiency.record((src->bytesSentSuccessfully)/((double)(src->totalBytesSent)));
                switch (terMsg->getReplyType()){
                    case udpSingle:{
                        if (hasGUI()){
                            getParentModule()->bubble("Received message!");
                        }
                        break;
                    }
                    case udpFlow:{
                        if (hasGUI()){
                            getParentModule()->bubble("Received burst message!");
                        }
                        break;
                    }
                    case ping:{
                        // Received ping, reply with pong.
                        EV << "Terminal " << myAddress << " received ping from terminal " << terMsg->getSrcAddr() << ". Preparing to send pong" << endl;
                        if (hasGUI())
                            getParentModule()->bubble("Ping!");

                        int satAddress = isConnected == connected? mainSatAddress : subSatAddress;
                        int connectionIndex = isConnected == connected? mainConnectionIndex : subConnectionIndex;
                        std::string pongName = "Pong to " + std::to_string(terMsg->getSrcAddr()) + " from " + std::to_string(myAddress);
                        TerminalMsg *terMsg = new TerminalMsg(pongName.c_str(), terminal);
                        terMsg->setSrcAddr(myAddress);
                        terMsg->setDestAddr(terMsg->getSrcAddr());
                        terMsg->setPacketType(terminal_message);
                        terMsg->setByteLength(PING_SIZE);
                        terMsg->setReplyType(pong);
                        sendDirect(terMsg, 0, terMsg->getByteLength()/rate,getParentModule()->getParentModule()->getSubmodule("rte", satAddress),"terminalIn", connectionIndex);

                        // Update statistics
                        numSent++;
                        totalBytesSent += terMsg->getByteLength();
                        break;
                    }
                    case pong:{
                        EV << "Terminal " << myAddress << " received pong from terminal " << terMsg->getSrcAddr() << "." << endl;
                        if (hasGUI())
                            getParentModule()->bubble("Pong!");
                        break;
                    }
                } // End of replyType switch-case


                break;
            }
            case terminal_index_assign:{
                //// Assign index from a satellite

                if (terMsg->getReplyType() != -1){
                    //Satellite found an open gate for me

                    EV << "Satellite " << terMsg->getSrcAddr() << " accepted terminal " << myAddress << endl;

                    switch (terMsg->getMode()){
                        case main:{
                            isConnected = connected;

                            mainConnectionIndex = terMsg->getReplyType();
                            if (!updateMainSatellitePositionMsg){
                                // First time connected to any satellite
                                updateMainSatellitePositionMsg = new cMessage("Main Satellite Position Update Self Message",mainSatellitePositionUpdateTime);
                            }

                            // Calculate exact time for position update + 0.01 and schedule position update
                            double remainingTime = getRemainingTime(mainSatUpdateInterval);
                            scheduleAt(simTime() + remainingTime, updateMainSatellitePositionMsg);
                            EV<< "Terminal " << myAddress << " connected to satellite " << terMsg->getSrcAddr() << " via " << mainConnectionIndex << endl;
                            break;
                        }
                        case sub:{
                            isConnected = connectedToSub;

                            subConnectionIndex = terMsg->getReplyType();
                            if (!updateSubSatellitePositionMsg){
                                // First time connected to any satellite
                                updateSubSatellitePositionMsg = new cMessage("Sub Satellite Position Update Self Message",subSatellitePositionUpdateTime);
                            }

                            // Calculate exact time for position update + 0.01 and schedule position update
                            double remainingTime = getRemainingTime(subSatUpdateInterval);
                            scheduleAt(simTime() + remainingTime, updateSubSatellitePositionMsg);

                            break;
                        }
                    }
                }
                else{
                    // Satellite didn't have an open slot

                    EV << "Satellite " << terMsg->getSrcAddr() << " rejected terminal " << myAddress << endl;

                    // Delete satellite data
                    switch (terMsg->getDestAddr()){
                        case main:{
                            resetSatelliteData(mainSatAddress, mainSatPosX, mainSatPosY, mainSatUpdateInterval, mainConnectionIndex, updateMainSatellitePositionMsg, mainSatMob);
                            break;
                        }
                        case sub:{
                            resetSatelliteData(subSatAddress, subSatPosX, subSatPosY, subSatUpdateInterval, subConnectionIndex, updateSubSatellitePositionMsg, subSatMob);
                            break;
                        }
                    }
                }
                break;
            }
            case terminal_list:
            case terminal_connect:
            case terminal_disconnect:
            default:
                // Received terminal message, that supposed to be packet (for satellites only)- error
                delete msg;
                std::cout << "Error! Received satellite's packet in TerminalMsg Format!" << endl;
                endSimulation();
        }

        delete msg;
    }
}

void TerminalApp::finish(){
    recordScalar("Messages Sent in Total", numSent);
    recordScalar("Messages Sent Successfully", numReceived);
    recordScalar("Bytes Sent in Total", totalBytesSent);
    recordScalar("Bytes Sent Successfully", bytesSentSuccessfully);
    recordScalar("Average efficiency", bytesSentSuccessfully/(double)totalBytesSent);
    recordScalar("Average throughput [bps]", (8*bytesSentSuccessfully)/simTime().dbl());
    hopCountHistogram.record();
}

bool TerminalApp::checkConnection(int mode){
    /* Assuming the positions are up-to-date. Return whether the [mode] satellite is within disk or not
     * */

    switch (mode){
        case main:{
            return radius >= sqrt(pow(myPosX-mainSatPosX,2)+pow(myPosY-mainSatPosY,2));
        }
        case sub:{
            return radius >= sqrt(pow(myPosX-subSatPosX,2)+pow(myPosY-subSatPosY,2));
        }
        default:
            // Removes warning
            return false;
    }
}

void TerminalApp::resetSatelliteData(int &satAddress, double &satPosX, double &satPosY, double &satUpdateInterval, int &connectionIndex, cMessage *&updateMsg, inet::IMobility* &mob){
    /* Places default values inside given satellite database
     * */

    satAddress = -1;
    satPosX = -1;
    satPosY = -1;
    satUpdateInterval = -1;
    connectionIndex = -1;
    if (updateMsg)
        delete updateMsg;
    updateMsg = nullptr;
    mob = nullptr;
}

int TerminalApp::findSatelliteToConnect(int ignoreIndex, int mode){
    /* This function tries to connect the closest satellite to terminal, ignoring [ignoreIndex].
     * Returns whether satellite address
     * */

    int satAddress = getClosestSatellite(ignoreIndex);

    if (satAddress != -1){
        // Start handshake with satellite
        connectToSatellite(satAddress, mode);
        return satAddress;
    }

    return -1;
}

void TerminalApp::connectToSatellite(int satAddress, int mode){
    /* Updates local database and send connection message to the satellite.
     * */

    switch (mode){
        case main:{
            mainSatAddress = satAddress;
            mainSatMob = check_and_cast<inet::IMobility*>(getParentModule()->getParentModule()->getSubmodule("rte", satAddress)->getSubmodule("mobility"));
            updatePosition(mainSatMob, mainSatPosX, mainSatPosY);
            mainSatUpdateInterval = getParentModule()->getParentModule()->getSubmodule("rte", satAddress)->getSubmodule("mobility")->par("updateInterval").doubleValue();
            break;
        }
        case sub:{
            subSatAddress = satAddress;
            subSatMob = check_and_cast<inet::IMobility*>(getParentModule()->getParentModule()->getSubmodule("rte", satAddress)->getSubmodule("mobility"));
            updatePosition(subSatMob, subSatPosX, subSatPosY);
            subSatUpdateInterval = getParentModule()->getParentModule()->getSubmodule("rte", satAddress)->getSubmodule("mobility")->par("updateInterval").doubleValue();
            break;
        }
    }

    // Ask to connect
    TerminalMsg *terMsg = new TerminalMsg("Connection Request", terminal);
    terMsg->setSrcAddr(myAddress);
    terMsg->setDestAddr(satAddress);
    terMsg->setPacketType(terminal_connect);
    terMsg->setMode(mode);
    terMsg->setByteLength(64); // Ethernet minimum

    cModule *satellite = getParentModule()->getParentModule()->getSubmodule("rte", satAddress);
    sendDirect(terMsg, 0, terMsg->getByteLength()/rate, satellite->gate("terminalIn", 0));

    EV << "Terminal " << myAddress << " attempted connection with satellite " << satAddress << endl;
}

void TerminalApp::disconnectFromSatellite(int mode){
    /* Clears local database and send disconnection message to the satellite.
     * */

    int satAddress, gateIndex;
    switch (mode){
        case main:{
            satAddress = mainSatAddress;
            gateIndex = mainConnectionIndex;
            cancelEvent(updateMainSatellitePositionMsg);
            resetSatelliteData(mainSatAddress, mainSatPosX, mainSatPosY, mainSatUpdateInterval, mainConnectionIndex, updateMainSatellitePositionMsg, mainSatMob);
            break;
        }
        case sub:{
            satAddress = subSatAddress;
            gateIndex = subConnectionIndex;
            cancelEvent(updateSubSatellitePositionMsg);
            resetSatelliteData(subSatAddress, subSatPosX, subSatPosY, subSatUpdateInterval, subConnectionIndex, updateSubSatellitePositionMsg, subSatMob);
            break;
        }
    }

    // Send disconnection message
    TerminalMsg *terMsg = new TerminalMsg("Disconnection Request", terminal);
    terMsg->setSrcAddr(myAddress);
    terMsg->setDestAddr(satAddress);
    terMsg->setPacketType(terminal_disconnect);
    terMsg->setMode(mode);
    terMsg->setByteLength(0);

    cModule *satellite = getParentModule()->getParentModule()->getSubmodule("rte", satAddress);
    sendDirect(terMsg, satellite->gate("terminalIn", gateIndex));

    EV << "Terminal " << myAddress << " attempted disconnection from satellite " << satAddress << endl;

    isConnected--;
}

double TerminalApp::getRemainingTime(double updateInterval, double epsilon){
    /* Calculate how much time is needed to wait until next update, and add [epsilon] to it.
     * Calculation is taking the fraction part of (current simulation time) / [updateInterval], and subtract it from [updateInterval]:
     *      remaining time = updateInterval - frac(simTime()/updateInterval)
     * To this time we add [epsilon] in order to get small delay
     *  */

    double currSimTime = simTime().dbl();
    return updateInterval + epsilon - updateInterval * (currSimTime/updateInterval - int(currSimTime/updateInterval));
}

void TerminalApp::upgradeSubToMain(void){
    /* Replace main satellite's database + message with sub satellite's database
     * */

    // Copy data from sub satellite to main
    mainSatAddress = subSatAddress;
    mainSatPosX = subSatPosX;
    mainSatPosY = subSatPosY;
    mainSatUpdateInterval = subSatUpdateInterval;
    mainConnectionIndex = subConnectionIndex;
    mainSatMob = subSatMob;

    // Cancel & delete both messages
    if (updateMainSatellitePositionMsg){
        cancelEvent(updateMainSatellitePositionMsg);
        delete updateMainSatellitePositionMsg;
        updateMainSatellitePositionMsg = nullptr;
    }
    cancelEvent(updateSubSatellitePositionMsg);
    delete updateSubSatellitePositionMsg;
    updateSubSatellitePositionMsg = nullptr;

    // Replace old main message with a new one (Ensures no segmentation fault occurs because of upgrade)
    updateMainSatellitePositionMsg = new cMessage("Main Satellite Position Update Self Message",mainSatellitePositionUpdateTime);

    // Clear sub satellite data
    resetSatelliteData(subSatAddress, subSatPosX, subSatPosY, subSatUpdateInterval, subConnectionIndex, updateSubSatellitePositionMsg, subSatMob);
}
