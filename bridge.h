/* Service indices into svc_table32 — MUST match fake64_bind_services order. */
#ifndef BRIDGE_H
#define BRIDGE_H

#define SVC_RAISETPL          0
#define SVC_RESTORETPL        1
#define SVC_ALLOCATEPAGES     2
#define SVC_FREEPAGES         3
#define SVC_GETMEMORYMAP      4
#define SVC_ALLOCATEPOOL      5
#define SVC_FREEPOOL          6
#define SVC_CREATEEVENT       7
#define SVC_SETTIMER          8
#define SVC_WAITFOREVENT      9
#define SVC_SIGNALEVENT      10
#define SVC_CLOSEEVENT       11
#define SVC_CHECKEVENT       12
#define SVC_INSTALLPROTOCOL  13
#define SVC_REINSTALLPROTO   14
#define SVC_UNINSTALLPROTO   15
#define SVC_HANDLEPROTOCOL   16
#define SVC_RESERVED         17
#define SVC_REGPROTONOTIFY   18
#define SVC_LOCATEHANDLE     19
#define SVC_LOCATEDEVICEPATH 20
#define SVC_INSTALLCONFIGTBL 21
#define SVC_LOADIMAGE        22
#define SVC_STARTIMAGE       23
#define SVC_EXIT             24
#define SVC_UNLOADIMAGE      25
#define SVC_EXITBOOTSERVICES 26
#define SVC_MONOTONIC        27
#define SVC_STALL            28
#define SVC_WATCHDOG         29
#define SVC_LOCATEPROTOCOL   30
#define SVC_MAX              31

#endif
