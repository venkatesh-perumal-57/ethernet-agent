/*
   If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2019 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#if !defined(WAN_MANAGER_UNIFICATION_ENABLED)
#if defined (FEATURE_RDKB_WAN_MANAGER) || defined (FEATURE_RDKB_WAN_AGENT)
/* ---- Include Files ---------------------------------------- */
#include "cosa_ethernet_manager.h"
#include "cosa_ethernet_apis.h"
#include <syscfg/syscfg.h>

/* ---- Global Constants -------------------------- */
#define LOOP_TIMEOUT 500000 // timeout in milliseconds. This is the state machine loop interval

/* ---- Global Types -------------------------- */
typedef enum
{
    STATE_EXIT = 0,
    STATE_ETH_DISCONNECTED,
    STATE_ETH_VALIDATING_LINK,
    STATE_ETH_VLAN_CONFIGURING,
    STATE_ETH_WAN_LINK_UP,
} ethSmState_t;

typedef enum
{
    ETH_IF_DOWN = 0,
    ETH_IF_UP,
    ETH_IF_WAN_DOWN,
    ETH_IF_WAN_UP,
    ETH_IF_WAN_VALIDATED
} ethInterfaceState_t;

static void *CcspEthManager_StateMachineThread();

/* ---- Global Variables ------------------------------------ */
ethSmState_t geCurrentState = STATE_EXIT;
ethInterfaceState_t geInterfaceState = ETH_IF_DOWN; // This stores the connection status of the physical Ethernet interface

fd_set readFdsMaster;
static fd_set errorFdsMaster;
int maxFd = 0;
static int CcspEthManager_StateMachineThread_start = FALSE;

PETH_SM_PRIVATE_INFO pstInfo = NULL;
/* ---- Private Function Prototypes -------------------------- */
/* STATES */
static ethSmState_t State_EthDisconnected(PETH_SM_PRIVATE_INFO pstInfo);    // waits for Ethernet to be physically connected.
static ethSmState_t State_EthValidatingLink(PETH_SM_PRIVATE_INFO pstInfo);  // waits for Ethernet Link to be validated
static ethSmState_t State_EthVLANConfiguring(PETH_SM_PRIVATE_INFO pstInfo); // waits for VLAN to be configure on Ethernet Link
static ethSmState_t State_EthWanLinkUp(PETH_SM_PRIVATE_INFO pstInfo);       // monitors the Ethernet interface
static ethSmState_t State_Exit(PETH_SM_PRIVATE_INFO pstInfo);       // monitors the Ethernet Exit state

/* TRANSITIONS */
static ethSmState_t Transition_Start(void);                                       // initiliases the state machine.
static ethSmState_t Transition_EthPhyInterfaceUp(PETH_SM_PRIVATE_INFO pstInfo);   // starts link validation process
static ethSmState_t Transition_EthWanLinkFound(PETH_SM_PRIVATE_INFO pstInfo);     // starts configuring vlan on link
static ethSmState_t Transition_EthWanLinkUp(PETH_SM_PRIVATE_INFO pstInfo);        // inform linkup event to higher layer agents
static ethSmState_t Transition_EthPhyInterfaceDown(PETH_SM_PRIVATE_INFO pstInfo); // inform higher layer agent to teardown wan interface
static ethSmState_t TransitionExit(PETH_SM_PRIVATE_INFO pstInfo);                 //Exit from state machine

static char *PrintEnumToString(const ethSmState_t state); // Convert enum state to string.

/* CosaEthManager_Start_StateMachine() */
void CosaEthManager_Start_StateMachine(PETH_SM_PRIVATE_INFO pstMPrivateInfo)
{
    pthread_t EthStateMachineThread;
    //PETH_SM_PRIVATE_INFO pstInfo = NULL;
    int iErrorCode = 0;

    if (pstMPrivateInfo == NULL)
    {
        CcspTraceError(("Invalid argument \n"));
        return;
    }

    //Allocate the memory and pass to the thread.
    pstInfo = (PETH_SM_PRIVATE_INFO)malloc(sizeof(ETH_SM_PRIVATE_INFO));
    if (NULL == pstInfo)
    {
        CcspTraceError(("%s %d Failed to allocate memory\n", __FUNCTION__, __LINE__));
        return;
    }

    //Copy buffer.
    memcpy(pstInfo, pstMPrivateInfo, sizeof(ETH_SM_PRIVATE_INFO));

    CcspTraceInfo(("%s %d - ETH State Machine Thread Interface Name: %s\n", __FUNCTION__, __LINE__, pstInfo->Name));

    if(CcspEthManager_StateMachineThread_start == TRUE){
        CcspTraceInfo(("%s %d - ETH State Machine Thread already running, not starting new\n", __FUNCTION__, __LINE__));
	return;
    }

    //ETH state machine thread
    iErrorCode = pthread_create(&EthStateMachineThread, NULL, &CcspEthManager_StateMachineThread, NULL);

    if (0 != iErrorCode)
    {
        CcspTraceInfo(("%s %d - Failed to start ETH State Machine Thread EC:%d\n", __FUNCTION__, __LINE__, iErrorCode));
    }
    else
    {
        CcspEthManager_StateMachineThread_start = TRUE;
        CcspTraceInfo(("%s %d - ETH State Machine Thread Started Successfully\n", __FUNCTION__, __LINE__));
    }
}

/* CcspEthManager_StateMachineThread() */
static void *CcspEthManager_StateMachineThread()
{
    // event handler
    int n = 0;
    fd_set readFds;
    fd_set errorFds;
    struct timeval tv;

    //pstInfo = (PETH_SM_PRIVATE_INFO)arg;
    if (pstInfo == NULL)
    {
        CcspTraceError(("%s %d Invalid Argument pstInfo = NULL \n", __FUNCTION__, __LINE__));
        pthread_exit(NULL);
    }

    //detach thread from caller stack
    pthread_detach(pthread_self());

    /* events : set up all the fd stuff for select */
    FD_ZERO(&readFdsMaster);
    FD_ZERO(&errorFdsMaster);

    // local variables
    bool bRunning = true;

    // initialise state machine
    geCurrentState = Transition_Start(); // do this first before anything else to init variables

    while (bRunning)
    {
        readFds = readFdsMaster;
        errorFds = errorFdsMaster;

        /* Wait up to 500 milliseconds */
        tv.tv_sec = 0;
        tv.tv_usec = LOOP_TIMEOUT;

        n = select(maxFd + 1, &readFds, NULL, &errorFds, &tv);
        if (n < 0)
        {
            /* interrupted by signal or something, continue */
            continue;
        }

        // process state
        switch (geCurrentState)
        {
        case STATE_ETH_DISCONNECTED:
        {
            geCurrentState = State_EthDisconnected(pstInfo);
            break;
        }

        case STATE_ETH_VALIDATING_LINK:
        {
            geCurrentState = State_EthValidatingLink(pstInfo);
            break;
        }

        case STATE_ETH_VLAN_CONFIGURING:
        {
            geCurrentState = State_EthVLANConfiguring(pstInfo);
            break;
        }

        case STATE_ETH_WAN_LINK_UP:
        {
            geCurrentState = State_EthWanLinkUp(pstInfo);
            break;
        }

        case STATE_EXIT:
        {
            geCurrentState = State_Exit(pstInfo);
            break;
        }

        default:
        {
            bRunning = false;
            if (NULL != pstInfo)
            {
                free(pstInfo);
                pstInfo = NULL;
            }

            CcspTraceInfo(("%s %d - Exit from state machine\n", __FUNCTION__, __LINE__));
        }
        }
    }

    CcspTraceInfo(("%s %d - Exit from state machine\n", __FUNCTION__, __LINE__));
    CcspEthManager_StateMachineThread_start = FALSE;
    if (NULL != pstInfo)
    {
         free(pstInfo);
         pstInfo = NULL;
    }

    pthread_exit(NULL);

    return NULL;
}

static ethSmState_t State_Exit(PETH_SM_PRIVATE_INFO pstInfo)
{

    COSA_DML_ETH_PORT_GLOBAL_CONFIG stGlobalInfo;

    memset(&stGlobalInfo, 0, sizeof(stGlobalInfo));

    if (ANSC_STATUS_SUCCESS != CosaDmlEthPortGetCopyOfGlobalInfoForGivenIfName(pstInfo->Name, &stGlobalInfo))
    {
        CcspTraceError(("Failed to get the global link status info \n"));
        return STATE_EXIT;
    }

    if (TRUE == stGlobalInfo.Upstream)
    {
        return STATE_ETH_DISCONNECTED;
    }

    return STATE_EXIT;
}

static ethSmState_t State_EthDisconnected(PETH_SM_PRIVATE_INFO pstInfo)
{

    COSA_DML_ETH_PORT_GLOBAL_CONFIG stGlobalInfo;

    memset(&stGlobalInfo, 0, sizeof(stGlobalInfo));

    if (ANSC_STATUS_SUCCESS != CosaDmlEthPortGetCopyOfGlobalInfoForGivenIfName(pstInfo->Name, &stGlobalInfo))
    {
        CcspTraceError(("Failed to get the global link status info \n"));
        return STATE_ETH_DISCONNECTED;
    }

    if (FALSE == stGlobalInfo.Upstream)
    {
        return TransitionExit(pstInfo);
    }
    else if ((ETH_LINK_STATUS_UP == stGlobalInfo.LinkStatus) && (ETH_WAN_DOWN == stGlobalInfo.WanStatus))
    {
        return Transition_EthPhyInterfaceUp(pstInfo);
    }
    else if ((ETH_LINK_STATUS_UP == stGlobalInfo.LinkStatus) && (ETH_WAN_UP == stGlobalInfo.WanStatus))
    {
        return Transition_EthWanLinkUp(pstInfo);
    }

    return STATE_ETH_DISCONNECTED;
}

static ethSmState_t State_EthValidatingLink(PETH_SM_PRIVATE_INFO pstInfo)
{
    COSA_DML_ETH_PORT_GLOBAL_CONFIG stGlobalInfo;

    memset(&stGlobalInfo, 0, sizeof(stGlobalInfo));

    if (ANSC_STATUS_SUCCESS != CosaDmlEthPortGetCopyOfGlobalInfoForGivenIfName(pstInfo->Name, &stGlobalInfo))
    {
        CcspTraceError(("Failed to get the global link status info \n"));
        return STATE_ETH_VALIDATING_LINK;
    }

    if ( (FALSE == stGlobalInfo.Upstream) || (ETH_LINK_STATUS_DOWN == stGlobalInfo.LinkStatus) )
    {
        return TransitionExit(pstInfo);
    }
    else if (TRUE == stGlobalInfo.WanValidated)
    {
        return Transition_EthWanLinkFound(pstInfo);
    }

    return STATE_ETH_VALIDATING_LINK;
}

static ethSmState_t State_EthVLANConfiguring(PETH_SM_PRIVATE_INFO pstInfo)
{
    COSA_DML_ETH_PORT_GLOBAL_CONFIG stGlobalInfo = {0};

    if (ANSC_STATUS_SUCCESS != CosaDmlEthPortGetCopyOfGlobalInfoForGivenIfName(pstInfo->Name, &stGlobalInfo))
    {
        CcspTraceError(("Failed to get the global link status info \n"));
        return STATE_ETH_VALIDATING_LINK;
    }

    if ( (FALSE == stGlobalInfo.Upstream) || (ETH_LINK_STATUS_DOWN == stGlobalInfo.LinkStatus))
    {
        return Transition_EthPhyInterfaceDown(pstInfo);
    }
    else if (ETH_WAN_UP == stGlobalInfo.WanStatus)
    {
        return Transition_EthWanLinkUp(pstInfo);
    }

    return STATE_ETH_VLAN_CONFIGURING;
}

static ethSmState_t State_EthWanLinkUp(PETH_SM_PRIVATE_INFO pstInfo)
{
    COSA_DML_ETH_PORT_GLOBAL_CONFIG stGlobalInfo;

    memset(&stGlobalInfo, 0, sizeof(stGlobalInfo));

    if (ANSC_STATUS_SUCCESS != CosaDmlEthPortGetCopyOfGlobalInfoForGivenIfName(pstInfo->Name, &stGlobalInfo))
    {
        CcspTraceError(("Failed to get the global link status info \n"));
        return STATE_ETH_VALIDATING_LINK;
    }

    if ((FALSE == stGlobalInfo.Upstream) || (ETH_WAN_DOWN == stGlobalInfo.WanStatus) || (ETH_LINK_STATUS_DOWN == stGlobalInfo.LinkStatus))
    {
        return Transition_EthPhyInterfaceDown(pstInfo);
    }

    return STATE_ETH_WAN_LINK_UP;
}

static ethSmState_t Transition_Start()
{
    CcspTraceInfo(("[%s] State = [%s]\n", __FUNCTION__, PrintEnumToString(geCurrentState)));
    return STATE_EXIT;
}

static ethSmState_t Transition_EthPhyInterfaceUp(PETH_SM_PRIVATE_INFO pstInfo)
{
    UNREFERENCED_PARAMETER(pstInfo);
    CcspTraceInfo(("[%s] State from [%s] to [%s] \n", __FUNCTION__, PrintEnumToString(geCurrentState), PrintEnumToString(STATE_ETH_VALIDATING_LINK)));
    return STATE_ETH_VALIDATING_LINK;
}

static ethSmState_t Transition_EthWanLinkFound(PETH_SM_PRIVATE_INFO pstInfo)
{
    CcspTraceInfo(("[%s] State from [%s] to [%s]\n", __FUNCTION__, PrintEnumToString(geCurrentState), PrintEnumToString(STATE_ETH_VLAN_CONFIGURING)));
    COSA_DML_ETH_PORT_GLOBAL_CONFIG stGlobalInfo;

    memset(&stGlobalInfo, 0, sizeof(stGlobalInfo));

    if (ANSC_STATUS_SUCCESS != CosaDmlEthPortGetCopyOfGlobalInfoForGivenIfName(pstInfo->Name, &stGlobalInfo))
    {
        CcspTraceError(("Failed to get the global link status info \n"));
        return STATE_ETH_VALIDATING_LINK;
    }

    /**
     * Notify VLAN Agent to create Ethernet.Link and Enable it.
     * Eg: L2 Interface ->eth3  , l3 Interface -> erouter0
     */
#if defined(FEATURE_RDKB_WAN_AGENT)
    if (ANSC_STATUS_SUCCESS != CosaDmlEthCreateEthLink(pstInfo->Name, stGlobalInfo.Path))
#elif defined(FEATURE_RDKB_WAN_MANAGER)
    #if defined(_PLATFORM_RASPBERRYPI_) || defined(_PLATFORM_TURRIS_) || defined(_PLATFORM_BANANAPI_R4_) || defined(_COSA_QCA_ARM_)
    CHAR wanPhyName[20] = {0},out_value[20] = {0};
    if (!syscfg_get(NULL, "wan_physical_ifname", out_value, sizeof(out_value)))
    {
           strcpy(wanPhyName, out_value);
    }

    if (!strncmp(wanPhyName, "erouter0", sizeof(wanPhyName)))
    {
            CosaDmlEthPortSetWanStatus(pstInfo->Name,ETH_WAN_UP);
            return Transition_EthWanLinkUp(pstInfo);
    }
    #endif
    if (ANSC_STATUS_SUCCESS != CosaDmlEthCreateEthLink(pstInfo->Name, stGlobalInfo.LowerLayers))
#endif
    {
        CcspTraceError(("%s Failed to create Ethernet link \n", __FUNCTION__));
    }

    /**
     * TODO: Move base interface to the LAN bridge.
     */

    return STATE_ETH_VLAN_CONFIGURING;
}

static ethSmState_t Transition_EthWanLinkUp(PETH_SM_PRIVATE_INFO pstInfo)
{
    CcspTraceInfo(("[%s] State from [%s] to [%s]\n", __FUNCTION__, PrintEnumToString(geCurrentState), PrintEnumToString(STATE_ETH_WAN_LINK_UP)));
    /*
    *   Notify to WANAgent for Up event
    */
#if defined (FEATURE_RDKB_WAN_AGENT)
    if (ANSC_STATUS_SUCCESS != CosaDmlEthSetWanStatusForWanAgent(pstInfo->Name, "Up"))
#elif defined(FEATURE_RDKB_WAN_MANAGER)
    if (ANSC_STATUS_SUCCESS != CosaDmlEthSetWanInterfaceNameForWanManager(pstInfo->Name, WAN_INTERFACE_NAME))
    {
        CcspTraceError(("%s Failed to set LinkUp to WAN\n", __FUNCTION__));
    }
    if (ANSC_STATUS_SUCCESS != CosaDmlEthSetWanLinkStatusForWanManager(pstInfo->Name, "Up"))
#endif
    {
        CcspTraceError(("%s Failed to set LinkUp to WAN\n", __FUNCTION__));
    }
    CcspTraceInfo(("[%s] State = [%s]\n", __FUNCTION__, PrintEnumToString(STATE_ETH_WAN_LINK_UP)));
    return STATE_ETH_WAN_LINK_UP;
}

static ethSmState_t Transition_EthPhyInterfaceDown(PETH_SM_PRIVATE_INFO pstInfo)
{
    COSA_DML_ETH_PORT_GLOBAL_CONFIG stGlobalInfo;

    CcspTraceInfo(("[%s] State from [%s] to [%s]\n", __FUNCTION__, PrintEnumToString(geCurrentState), PrintEnumToString(STATE_ETH_DISCONNECTED)));

    memset(&stGlobalInfo, 0, sizeof(stGlobalInfo));

    if (ANSC_STATUS_SUCCESS != CosaDmlEthPortGetCopyOfGlobalInfoForGivenIfName(pstInfo->Name, &stGlobalInfo))
    {
        CcspTraceError(("Failed to get the global link status info \n"));
        return STATE_ETH_VALIDATING_LINK;
    }

#if defined (FEATURE_RDKB_WAN_MANAGER)
    CosaDmlEthPortSetWanStatus(pstInfo->Name,ETH_WAN_DOWN);
#endif //FEATURE_RDKB_WAN_MANAGER

    /**
     * Notify VLANAgent to delete Ethernet.Link.
     */
#if defined (FEATURE_RDKB_WAN_AGENT)
    if (ANSC_STATUS_SUCCESS != CosaDmlEthDeleteEthLink(pstInfo->Name,stGlobalInfo.Path))
#elif defined(FEATURE_RDKB_WAN_MANAGER)
    if (ANSC_STATUS_SUCCESS != CosaDmlEthDeleteEthLink(pstInfo->Name,stGlobalInfo.LowerLayers))
#endif
    {
        CcspTraceError(("%s Failed to delete eth link", __FUNCTION__));
    }
    /**
     * Notify WANAgent about the wan state.
     */
#if defined(FEATURE_RDKB_WAN_AGENT)
    if (ANSC_STATUS_SUCCESS != CosaDmlEthSetWanStatusForWanAgent(pstInfo->Name, "Down"))
#elif defined (FEATURE_RDKB_WAN_MANAGER)
    if (ANSC_STATUS_SUCCESS != CosaDmlEthSetWanLinkStatusForWanManager(pstInfo->Name, "Down"))
#endif
    {
        CcspTraceError(("%s Failed to set LinkDown to WAN\n", __FUNCTION__));
    }
    /**
     * TODO: Move base  interface into LAN bridge.
     */

    return STATE_EXIT;
}

static ethSmState_t TransitionExit(PETH_SM_PRIVATE_INFO pstInfo)
{
    UNREFERENCED_PARAMETER(pstInfo);

#if defined(FEATURE_RDKB_WAN_MANAGER)
    if (ANSC_STATUS_SUCCESS != CosaDmlEthSetWanLinkStatusForWanManager(pstInfo->Name, "Down"))
    {
        CcspTraceError(("%s Failed to set LinkStatus to WAN \n", __FUNCTION__));
    }
#endif

    CcspTraceInfo(("[%s] State from [%s] to [%s]\n", __FUNCTION__, PrintEnumToString(geCurrentState), PrintEnumToString(STATE_EXIT)));
    /*
     *  1. Exit fro state machine
     */

    return STATE_EXIT;
}

static char *PrintEnumToString(const ethSmState_t state)
{
    switch (state)
    {
    case STATE_ETH_DISCONNECTED:
        return "STATE_ETH_DISCONNECTED";
        break;
    case STATE_ETH_VALIDATING_LINK:
        return "STATE_ETH_VALIDATING_LINK";
        break;
    case STATE_ETH_VLAN_CONFIGURING:
        return "STATE_ETH_VLAN_CONFIGURING";
        break;
    case STATE_ETH_WAN_LINK_UP:
        return "STATE_ETH_WAN_LINK_UP";
        break;
    case STATE_EXIT:
        return "STATE_EXIT";
        break;
    default:
        return "Invalid state";
        break;
    }
}
#endif //#if defined (FEATURE_RDKB_WAN_MANAGER) || defined (FEATURE_RDKB_WAN_AGENT)
#endif //WAN_MANAGER_UNIFICATION_ENABLED
