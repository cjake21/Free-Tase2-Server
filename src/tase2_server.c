/*
 * tase2_server.c
 *
 * A small TASE.2/ICCP server built on libIEC61850's low-level MMS server API.
 *
 * libIEC61850 comes with an IEC 61850 server and an MMS client but no TASE.2
 * server, and the TASE.2 servers that do exist are commercial. This fills that
 * gap. It stands up a TASE.2/ICCP object model over the usual
 * TPKT/COTP/Session/Presentation/ACSE/MMS stack on TCP/102, so a TASE.2 client
 * can associate and run real ICCP services against it. Handy as a target for
 * SCADA/OT protocol testing and for checking IDS and parser tooling.
 *
 * It covers the common conformance blocks:
 *   Block 1  association, the VCC/ICC objects, data values, data sets and the
 *            transfer-set objects, and the bilateral table.
 *   Block 2  report-by-exception and integrity reporting of transfer sets, sent
 *            as unconfirmed MMS InformationReport PDUs.
 *   Block 5  a device control point you can select and operate.
 *
 * Objects live at two scopes. VMD scope (read with domain = NULL) holds
 * TASE2_Version and Supported_Features. The ICC domain holds everything else:
 * Bilateral_Table_ID, Next_DSTransfer_Set, the transfer-set status variables,
 * the indication points (tm1/tm2 RealQ, ts1/ts2 StateQ), the DSTransferSetNN
 * objects, and the dev1 control point.
 *
 * One thing to keep in mind: TASE.2 on the wire is just MMS (ISO 9506). There
 * is no separate TASE.2 PDU. What makes a capture TASE.2 is this object model
 * and the transfer-set / report behaviour layered on top of MMS. The indication
 * point and transfer-set value encodings here follow the common TASE.2
 * conventions rather than the full IEC 60870-6-802 type catalogue.
 *
 * Build it with the Makefile in this directory. libIEC61850 needs to be built
 * with CONFIG_MMS_SUPPORT_VMD_SCOPE_NAMED_VARIABLES=1. GPL-3.0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <math.h>

/* libIEC61850 public MMS headers */
#include "mms_server.h"
#include "mms_value.h"
#include "mms_type_spec.h"
#include "linked_list.h"
#include "hal_thread.h"
#include "tls_config.h"             /* TLS / Secure ICCP (optional, -T) */

/* libIEC61850 internal MMS server headers (not installed; included from the
 * source tree via -I in the Makefile). These expose the low-level model and
 * server lifecycle that IedServer is normally built on. */
#include "mms_device_model.h"        /* MmsDevice / MmsDomain / MmsVariableSpecification */
#include "mms_server_libinternal.h"  /* MmsServer lifecycle, handlers, value cache */
#include "mms_server_connection.h"   /* MmsServerConnection_sendInformationReport* */
#include "mms_named_variable_list.h" /* data set (named variable list) iteration */

/* Configuration */

#define TASE2_DEFAULT_PORT      102
#define TASE2_DEFAULT_DOMAIN    "TestDomain"
#define TASE2_DEFAULT_BLT_ID    "TestBilTab"
#define TASE2_VERSION_MAJOR     2000
#define TASE2_VERSION_MINOR     8
#define MAX_TRANSFER_SETS       8
#define SUPPORTED_FEATURES_BITS 16   /* CBB support bitstring */

/* DSConditions (report trigger) bit values per TASE.2 */
#define DSCOND_INTERVAL_TIMEOUT 0x01
#define DSCOND_INTEGRITY        0x02
#define DSCOND_OBJECT_CHANGE    0x04
#define DSCOND_OPERATOR_REQUEST 0x08
#define DSCOND_OTHER_EXTERNAL   0x10

typedef struct {
    const char* bindIp;        /* NULL = all interfaces */
    int         port;
    const char* domainName;
    const char* bltId;
    int         integritySeconds;
    int         tls;                /* -T : serve over TLS (Secure ICCP) */
    const char* certFile;           /* -C : server certificate (PEM) */
    const char* keyFile;            /* -K : server private key (PEM) */
    const char* caFile;             /* -A : CA used to validate client certs */
} Tase2Config;

/* Server state */

/* One DS Transfer Set the server tracks. The standard exposes these as MMS
 * named variables (a structure) the client writes to in order to enable
 * reporting of a named variable list (data set). */
typedef struct {
    char     name[32];          /* e.g. "DSTransferSet01" */
    char     dataSetName[129];  /* bound named variable list (data set) */
    bool     enabled;
    int      interval;          /* integrity period (s); 0 => use default */
    int      dsConditions;      /* requested condition mask */
    uint32_t reportsSent;
} TransferSet;

static MmsServer       g_server = NULL;
static MmsDevice*      g_device = NULL;
static MmsDomain*      g_domain = NULL;
static Tase2Config     g_cfg;
static volatile int    g_running = 1;

static Semaphore       g_lock;               /* guards the lists below */
static LinkedList      g_connections = NULL; /* <MmsServerConnection> */
static TransferSet     g_transferSets[MAX_TRANSFER_SETS];

/* live indication point values we mutate in the cache (held under model lock) */
static MmsValue* g_tm1 = NULL;  /* RealQ  { f, q } */
static MmsValue* g_tm2 = NULL;
static MmsValue* g_ts1 = NULL;  /* StateQ { s, q } */
static MmsValue* g_ts2 = NULL;
static MmsValue* g_tsTimeStamp = NULL;        /* Transfer_Set_Time_Stamp */
static MmsValue* g_dsConditionsDetected = NULL;

/* Small helpers for building MmsVariableSpecification type trees */

static MmsVariableSpecification*
specLeaf(const char* name, MmsType type, int sizeParam)
{
    MmsVariableSpecification* s =
        (MmsVariableSpecification*) calloc(1, sizeof(MmsVariableSpecification));
    s->name = name ? strdup(name) : NULL;
    s->type = type;
    switch (type) {
        case MMS_INTEGER:       s->typeSpec.integer = sizeParam; break;
        case MMS_UNSIGNED:      s->typeSpec.unsignedInteger = sizeParam; break;
        case MMS_BIT_STRING:    s->typeSpec.bitString = sizeParam; break;
        case MMS_VISIBLE_STRING:s->typeSpec.visibleString = sizeParam; break;
        case MMS_OCTET_STRING:  s->typeSpec.octetString = sizeParam; break;
        case MMS_BINARY_TIME:   s->typeSpec.binaryTime = sizeParam; break;
        case MMS_FLOAT:
            s->typeSpec.floatingpoint.exponentWidth = 8;
            s->typeSpec.floatingpoint.formatWidth = 32;
            break;
        default: break;
    }
    return s;
}

static MmsVariableSpecification*
specStruct(const char* name, int nElems)
{
    MmsVariableSpecification* s =
        (MmsVariableSpecification*) calloc(1, sizeof(MmsVariableSpecification));
    s->name = name ? strdup(name) : NULL;
    s->type = MMS_STRUCTURE;
    s->typeSpec.structure.elementCount = nElems;
    s->typeSpec.structure.elements =
        (MmsVariableSpecification**) calloc(nElems, sizeof(MmsVariableSpecification*));
    return s;
}

/* RealQ indication point: structure { Value: float, Flags: bitstring(8) } */
static MmsVariableSpecification*
specRealQ(const char* name)
{
    MmsVariableSpecification* s = specStruct(name, 2);
    s->typeSpec.structure.elements[0] = specLeaf("Value", MMS_FLOAT, 0);
    s->typeSpec.structure.elements[1] = specLeaf("Flags", MMS_BIT_STRING, 8);
    return s;
}

/* StateQ indication point: structure { Value: int(8), Flags: bitstring(8) } */
static MmsVariableSpecification*
specStateQ(const char* name)
{
    MmsVariableSpecification* s = specStruct(name, 2);
    s->typeSpec.structure.elements[0] = specLeaf("Value", MMS_INTEGER, 8);
    s->typeSpec.structure.elements[1] = specLeaf("Flags", MMS_BIT_STRING, 8);
    return s;
}

/* A DS Transfer Set object: structure of the standard TASE.2 attributes. */
static MmsVariableSpecification*
specTransferSet(const char* name)
{
    const char* fields[] = {
        "DataSetName", "StartTime", "Interval", "TLE", "BufferTime",
        "IntegrityCheck", "BlockData", "Critical", "RBE",
        "AllChangesReported", "Status", "EventCodeRequested", "DSConditionsRequested"
    };
    int n = (int)(sizeof(fields) / sizeof(fields[0]));
    MmsVariableSpecification* s = specStruct(name, n);
    s->typeSpec.structure.elements[0]  = specLeaf("DataSetName", MMS_VISIBLE_STRING, 129);
    s->typeSpec.structure.elements[1]  = specLeaf("StartTime", MMS_BINARY_TIME, 6);
    s->typeSpec.structure.elements[2]  = specLeaf("Interval", MMS_INTEGER, 32);
    s->typeSpec.structure.elements[3]  = specLeaf("TLE", MMS_INTEGER, 32);
    s->typeSpec.structure.elements[4]  = specLeaf("BufferTime", MMS_INTEGER, 32);
    s->typeSpec.structure.elements[5]  = specLeaf("IntegrityCheck", MMS_INTEGER, 32);
    s->typeSpec.structure.elements[6]  = specLeaf("BlockData", MMS_BOOLEAN, 0);
    s->typeSpec.structure.elements[7]  = specLeaf("Critical", MMS_BOOLEAN, 0);
    s->typeSpec.structure.elements[8]  = specLeaf("RBE", MMS_BOOLEAN, 0);
    s->typeSpec.structure.elements[9]  = specLeaf("AllChangesReported", MMS_BOOLEAN, 0);
    s->typeSpec.structure.elements[10] = specLeaf("Status", MMS_INTEGER, 8);
    s->typeSpec.structure.elements[11] = specLeaf("EventCodeRequested", MMS_INTEGER, 16);
    s->typeSpec.structure.elements[12] = specLeaf("DSConditionsRequested", MMS_INTEGER, 16);
    return s;
}

/* Build the MmsDevice model */

static void
buildModel(void)
{
    g_device = MmsDevice_create(NULL);     /* VMD (VCC) root, unnamed */
    g_domain = MmsDomain_create((char*) g_cfg.domainName); /* ICC domain */

    /* VMD-scope named variables */
    MmsVariableSpecification** vmdVars =
        (MmsVariableSpecification**) calloc(2, sizeof(MmsVariableSpecification*));
    /* TASE2_Version : structure { major: int16, minor: int16 } */
    MmsVariableSpecification* ver = specStruct("TASE2_Version", 2);
    ver->typeSpec.structure.elements[0] = specLeaf("major", MMS_INTEGER, 16);
    ver->typeSpec.structure.elements[1] = specLeaf("minor", MMS_INTEGER, 16);
    vmdVars[0] = ver;
    /* Supported_Features : bitstring of CBB flags */
    vmdVars[1] = specLeaf("Supported_Features", MMS_BIT_STRING, SUPPORTED_FEATURES_BITS);
    g_device->namedVariables = vmdVars;
    g_device->namedVariablesCount = 2;

    /* ICC domain named variables */
    int idx = 0;
    MmsVariableSpecification* d[32];
    d[idx++] = specLeaf("Bilateral_Table_ID", MMS_VISIBLE_STRING, 64);

    /* Next_DSTransfer_Set : structure where element[2] is the next free TS name
     * (matches what TASE.2 clients read to obtain a transfer-set object name). */
    MmsVariableSpecification* nextTs = specStruct("Next_DSTransfer_Set", 3);
    nextTs->typeSpec.structure.elements[0] = specLeaf("Available", MMS_INTEGER, 16);
    nextTs->typeSpec.structure.elements[1] = specLeaf("Max", MMS_INTEGER, 16);
    nextTs->typeSpec.structure.elements[2] = specLeaf("Name", MMS_VISIBLE_STRING, 32);
    d[idx++] = nextTs;

    /* transfer-set report status variables (read individually + sent in reports) */
    d[idx++] = specLeaf("Transfer_Set_Name", MMS_VISIBLE_STRING, 32);
    d[idx++] = specLeaf("Transfer_Set_Time_Stamp", MMS_BINARY_TIME, 6);
    d[idx++] = specLeaf("DSConditions_Detected", MMS_INTEGER, 16);
    d[idx++] = specLeaf("Event_Code_Detected", MMS_INTEGER, 16);
    d[idx++] = specLeaf("Transfer_Report_ACK", MMS_INTEGER, 16);
    d[idx++] = specLeaf("Transfer_Report_NACK", MMS_INTEGER, 16);

    /* indication points */
    d[idx++] = specRealQ("tm1");
    d[idx++] = specRealQ("tm2");
    d[idx++] = specStateQ("ts1");
    d[idx++] = specStateQ("ts2");

    /* DS Transfer Set objects */
    for (int i = 0; i < MAX_TRANSFER_SETS; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "DSTransferSet%02d", i + 1);
        d[idx++] = specTransferSet(nm);
        snprintf(g_transferSets[i].name, sizeof(g_transferSets[i].name), "%s", nm);
        g_transferSets[i].enabled = false;
        g_transferSets[i].dataSetName[0] = '\0';
    }

    /* Block 5: device control point (select-before-operate) */
    MmsVariableSpecification* dev = specStruct("dev1", 3);
    dev->typeSpec.structure.elements[0] = specLeaf("Command", MMS_INTEGER, 8);  /* operate value */
    dev->typeSpec.structure.elements[1] = specLeaf("Tag", MMS_VISIBLE_STRING, 32);
    dev->typeSpec.structure.elements[2] = specLeaf("Status", MMS_INTEGER, 8);
    d[idx++] = dev;

    MmsVariableSpecification** domainVars =
        (MmsVariableSpecification**) calloc(idx, sizeof(MmsVariableSpecification*));
    memcpy(domainVars, d, idx * sizeof(MmsVariableSpecification*));
    g_domain->namedVariables = domainVars;
    g_domain->namedVariablesCount = idx;

    MmsDomain** domains = (MmsDomain**) calloc(1, sizeof(MmsDomain*));
    domains[0] = g_domain;
    g_device->domains = domains;
    g_device->domainCount = 1;
}

/* Populate the value cache with initial values */

static MmsValue*
makeRealQ(float f)
{
    MmsValue* s = MmsValue_createEmptyStructure(2);
    MmsValue_setElement(s, 0, MmsValue_newFloat(f));
    MmsValue* q = MmsValue_newBitString(8);
    MmsValue_setElement(s, 1, q);
    return s;
}

static MmsValue*
makeStateQ(int st)
{
    MmsValue* s = MmsValue_createEmptyStructure(2);
    MmsValue_setElement(s, 0, MmsValue_newIntegerFromInt32(st));
    MmsValue_setElement(s, 1, MmsValue_newBitString(8));
    return s;
}

static void
populateCache(void)
{
    MmsDomain* vmd = (MmsDomain*) MmsServer_getDevice(g_server);

    /* TASE2_Version { major, minor } */
    MmsValue* ver = MmsValue_createEmptyStructure(2);
    MmsValue_setElement(ver, 0, MmsValue_newIntegerFromInt32(TASE2_VERSION_MAJOR));
    MmsValue_setElement(ver, 1, MmsValue_newIntegerFromInt32(TASE2_VERSION_MINOR));
    MmsServer_insertIntoCache(g_server, vmd, "TASE2_Version", ver);

    /* Supported_Features: set bits for Block1(0), Block2(1), Block5(4) */
    MmsValue* feat = MmsValue_newBitString(SUPPORTED_FEATURES_BITS);
    MmsValue_setBitStringBit(feat, 0, true);  /* Block 1 */
    MmsValue_setBitStringBit(feat, 1, true);  /* Block 2 */
    MmsValue_setBitStringBit(feat, 4, true);  /* Block 5 */
    MmsServer_insertIntoCache(g_server, vmd, "Supported_Features", feat);

    /* Bilateral_Table_ID */
    MmsServer_insertIntoCache(g_server, g_domain, "Bilateral_Table_ID",
                              MmsValue_newVisibleString(g_cfg.bltId));

    /* Next_DSTransfer_Set { available, max, name } */
    MmsValue* nextTs = MmsValue_createEmptyStructure(3);
    MmsValue_setElement(nextTs, 0, MmsValue_newIntegerFromInt32(MAX_TRANSFER_SETS));
    MmsValue_setElement(nextTs, 1, MmsValue_newIntegerFromInt32(MAX_TRANSFER_SETS));
    MmsValue_setElement(nextTs, 2, MmsValue_newVisibleString("DSTransferSet01"));
    MmsServer_insertIntoCache(g_server, g_domain, "Next_DSTransfer_Set", nextTs);

    /* transfer-set status vars */
    MmsServer_insertIntoCache(g_server, g_domain, "Transfer_Set_Name",
                              MmsValue_newVisibleString(""));
    g_tsTimeStamp = MmsValue_newBinaryTime(false);
    MmsServer_insertIntoCache(g_server, g_domain, "Transfer_Set_Time_Stamp", g_tsTimeStamp);
    g_dsConditionsDetected = MmsValue_newIntegerFromInt32(0);
    MmsServer_insertIntoCache(g_server, g_domain, "DSConditions_Detected", g_dsConditionsDetected);
    MmsServer_insertIntoCache(g_server, g_domain, "Event_Code_Detected",
                              MmsValue_newIntegerFromInt32(0));
    MmsServer_insertIntoCache(g_server, g_domain, "Transfer_Report_ACK",
                              MmsValue_newIntegerFromInt32(0));
    MmsServer_insertIntoCache(g_server, g_domain, "Transfer_Report_NACK",
                              MmsValue_newIntegerFromInt32(0));

    /* indication points (keep pointers for live updates) */
    g_tm1 = makeRealQ(11.0f);  MmsServer_insertIntoCache(g_server, g_domain, "tm1", g_tm1);
    g_tm2 = makeRealQ(22.0f);  MmsServer_insertIntoCache(g_server, g_domain, "tm2", g_tm2);
    g_ts1 = makeStateQ(1);     MmsServer_insertIntoCache(g_server, g_domain, "ts1", g_ts1);
    g_ts2 = makeStateQ(0);     MmsServer_insertIntoCache(g_server, g_domain, "ts2", g_ts2);

    /* DS Transfer Set objects (default values) */
    for (int i = 0; i < MAX_TRANSFER_SETS; i++) {
        MmsVariableSpecification* spec =
            MmsDomain_getNamedVariable(g_domain, g_transferSets[i].name);
        if (spec) {
            MmsValue* tsv = MmsValue_newDefaultValue(spec);
            MmsServer_insertIntoCache(g_server, g_domain, g_transferSets[i].name, tsv);
        }
    }

    /* device control point */
    MmsVariableSpecification* devSpec = MmsDomain_getNamedVariable(g_domain, "dev1");
    if (devSpec)
        MmsServer_insertIntoCache(g_server, g_domain, "dev1", MmsValue_newDefaultValue(devSpec));
}

/* Handlers */

static void
connectionHandler(void* parameter, MmsServerConnection connection,
                  MmsServerEvent event)
{
    if (event == MMS_SERVER_CONNECTION_TICK)
        return;
    char* peer = MmsServerConnection_getClientAddress(connection);
    Semaphore_wait(g_lock);
    if (event == MMS_SERVER_NEW_CONNECTION) {
        LinkedList_add(g_connections, connection);
        printf("[tase2] association from %s\n", peer ? peer : "?");
    } else if (event == MMS_SERVER_CONNECTION_CLOSED) {
        LinkedList_remove(g_connections, connection);
        printf("[tase2] association closed (%s)\n", peer ? peer : "?");
    }
    Semaphore_post(g_lock);
}

/* Write handler: device control (Block 5) and transfer-set enable (Block 2).
 * domain==NULL means VMD scope.
 *
 * Clients may address a structure member either as a component write
 * (componentId set) or as a flattened "Base$Member" itemId. We normalise both
 * into baseName + member. */
static MmsDataAccessError
writeHandler(void* parameter, MmsDomain* domain, const char* variableId,
             int arrayIdx, const char* componentId, MmsValue* value,
             MmsServerConnection connection)
{
    char baseName[64];
    const char* member = componentId;

    snprintf(baseName, sizeof(baseName), "%s", variableId);
    if (member == NULL) {
        char* dollar = strchr(baseName, '$');
        if (dollar) { *dollar = '\0'; member = dollar + 1; }
    }

    /* Block 5 device control: client operates dev1 -> log + accept */
    if (strcmp(baseName, "dev1") == 0) {
        printf("[tase2] device control operate on dev1.%s\n",
               member ? member : "(whole)");
        return DATA_ACCESS_ERROR_SUCCESS;
    }

    /* Block 2 transfer-set configuration: client writes DSTransferSetNN
     * attributes to bind a data set and enable reporting. */
    if (strncmp(baseName, "DSTransferSet", 13) == 0) {
        Semaphore_wait(g_lock);
        for (int i = 0; i < MAX_TRANSFER_SETS; i++) {
            if (strcmp(baseName, g_transferSets[i].name) != 0)
                continue;
            if (member && strcmp(member, "DataSetName") == 0 &&
                MmsValue_getType(value) == MMS_VISIBLE_STRING) {
                snprintf(g_transferSets[i].dataSetName,
                         sizeof(g_transferSets[i].dataSetName), "%s",
                         MmsValue_toString(value));
                printf("[tase2] %s bound to data set '%s'\n",
                       g_transferSets[i].name, g_transferSets[i].dataSetName);
            } else if (member && strcmp(member, "Status") == 0) {
                g_transferSets[i].enabled = (MmsValue_toInt32(value) != 0);
                printf("[tase2] %s %s\n", g_transferSets[i].name,
                       g_transferSets[i].enabled ? "ENABLED" : "disabled");
            } else if (member && strcmp(member, "Interval") == 0) {
                g_transferSets[i].interval = MmsValue_toInt32(value);
            } else if (member && strcmp(member, "DSConditionsRequested") == 0) {
                g_transferSets[i].dsConditions = MmsValue_toInt32(value);
            }
            break;
        }
        Semaphore_post(g_lock);
        return DATA_ACCESS_ERROR_SUCCESS;
    }

    return DATA_ACCESS_ERROR_SUCCESS; /* accept other writes */
}

/* reporting: send an unconfirmed InformationReport for each enabled transfer set */

static void
sendTransferSetReport(MmsServerConnection con, TransferSet* ts, int conditions)
{
    LinkedList values = LinkedList_create();

    /* Report header: Transfer_Set_Name, Time_Stamp, DSConditions_Detected */
    LinkedList_add(values, MmsValue_newVisibleString(ts->name));
    MmsValue* tstamp = MmsValue_newBinaryTime(false);
    MmsValue_setBinaryTime(tstamp, Hal_getTimeInMs());
    LinkedList_add(values, tstamp);
    LinkedList_add(values, MmsValue_newIntegerFromInt32(conditions));

    /* Data set member values: walk the named variable list (data set) the
     * client created and clone each member's current value from the cache. */
    if (ts->dataSetName[0] != '\0') {
        MmsNamedVariableList nvl =
            MmsDomain_getNamedVariableList(g_domain, ts->dataSetName);
        if (nvl) {
            LinkedList entries = MmsNamedVariableList_getVariableList(nvl);
            LinkedList e = LinkedList_getNext(entries);
            while (e) {
                MmsNamedVariableListEntry entry = (MmsNamedVariableListEntry) e->data;
                MmsDomain* dom = MmsNamedVariableListEntry_getDomain(entry);
                char* nm = MmsNamedVariableListEntry_getVariableName(entry);
                MmsValue* v = MmsServer_getValueFromCache(g_server, dom, nm);
                if (v) LinkedList_add(values, MmsValue_clone(v));
                e = LinkedList_getNext(e);
            }
        }
    }

    /* Unconfirmed PDU / InformationReport, VMD-specific itemId = transfer set */
    MmsServerConnection_sendInformationReportVMDSpecific(con, ts->name, values, false);
    LinkedList_destroyDeep(values, (LinkedListValueDeleteFunction) MmsValue_delete);
    ts->reportsSent++;
}

/* periodic work, run from the main loop */

static void
simulateValues(void)
{
    static double t = 0.0;
    t += 1.0;
    MmsServer_lockModel(g_server);
    if (g_tm1) MmsValue_setFloat(MmsValue_getElement(g_tm1, 0), (float)(11.0 + 5.0 * sin(t / 5.0)));
    if (g_tm2) MmsValue_setFloat(MmsValue_getElement(g_tm2, 0), (float)(22.0 + 3.0 * cos(t / 7.0)));
    if (g_ts1) MmsValue_setInt32(MmsValue_getElement(g_ts1, 0), ((int)t % 2));
    if (g_ts2) MmsValue_setInt32(MmsValue_getElement(g_ts2, 0), ((int)(t / 3) % 2));
    MmsServer_unlockModel(g_server);
}

static void
reportingTick(int integrityDue)
{
    Semaphore_wait(g_lock);
    for (int i = 0; i < MAX_TRANSFER_SETS; i++) {
        TransferSet* ts = &g_transferSets[i];
        if (!ts->enabled) continue;
        int cond = integrityDue ? DSCOND_INTEGRITY : DSCOND_OBJECT_CHANGE;

        LinkedList c = LinkedList_getNext(g_connections);
        MmsServer_lockModel(g_server);
        while (c) {
            sendTransferSetReport((MmsServerConnection) c->data, ts, cond);
            c = LinkedList_getNext(c);
        }
        MmsServer_unlockModel(g_server);
    }
    Semaphore_post(g_lock);
}

/* main */

static void sigHandler(int sig) { (void)sig; g_running = 0; }

static void
parseArgs(int argc, char** argv)
{
    g_cfg.bindIp = NULL;
    g_cfg.port = TASE2_DEFAULT_PORT;
    g_cfg.domainName = TASE2_DEFAULT_DOMAIN;
    g_cfg.bltId = TASE2_DEFAULT_BLT_ID;
    g_cfg.integritySeconds = 30;
    g_cfg.tls = 0;
    g_cfg.certFile = NULL;
    g_cfg.keyFile = NULL;
    g_cfg.caFile = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) g_cfg.bindIp = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) g_cfg.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) g_cfg.domainName = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) g_cfg.bltId = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) g_cfg.integritySeconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-T")) g_cfg.tls = 1;
        else if (!strcmp(argv[i], "-C") && i + 1 < argc) g_cfg.certFile = argv[++i];
        else if (!strcmp(argv[i], "-K") && i + 1 < argc) g_cfg.keyFile = argv[++i];
        else if (!strcmp(argv[i], "-A") && i + 1 < argc) g_cfg.caFile = argv[++i];
        else if (!strcmp(argv[i], "-h")) {
            printf("usage: %s [-i bindIp] [-p port] [-d domain] [-b bltId] [-t integritySecs]\n"
                   "          [-T] [-C serverCert.pem] [-K serverKey.pem] [-A caCert.pem]\n", argv[0]);
            exit(0);
        }
    }
}

int
main(int argc, char** argv)
{
    parseArgs(argc, argv);
    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    g_lock = Semaphore_create(1);
    g_connections = LinkedList_create();

    buildModel();

    TLSConfiguration tlsConfig = NULL;
    if (g_cfg.tls) {
        tlsConfig = TLSConfiguration_create();
        if (g_cfg.certFile) TLSConfiguration_setOwnCertificateFromFile(tlsConfig, g_cfg.certFile);
        if (g_cfg.keyFile)  TLSConfiguration_setOwnKeyFromFile(tlsConfig, g_cfg.keyFile, NULL);
        if (g_cfg.caFile)   TLSConfiguration_addCACertificateFromFile(tlsConfig, g_cfg.caFile);
        /* if a CA is given, require + validate client certs (mutual TLS) */
        TLSConfiguration_setChainValidation(tlsConfig, g_cfg.caFile ? true : false);
        TLSConfiguration_setAllowOnlyKnownCertificates(tlsConfig, false);
        printf("[tase2] TLS / Secure ICCP enabled\n");
    }

    g_server = MmsServer_create(g_device, tlsConfig);
    MmsServer_setMaxConnections(g_server, 10);
    MmsServer_enableDynamicNamedVariableListService(g_server, true);
    MmsServer_setMaxDomainSpecificDataSets(g_server, 32);
    MmsServer_setMaxDataSetEntries(g_server, 64);
    MmsServer_setServerIdentity(g_server, "FreeTASE2", "tase2-server-sim", "0.1");
    MmsServer_installWriteHandler(g_server, writeHandler, NULL);
    MmsServer_installConnectionHandler(g_server, connectionHandler, NULL);

    populateCache();

    if (g_cfg.bindIp)
        MmsServer_setLocalIpAddress(g_server, g_cfg.bindIp);

    printf("[tase2] TASE.2/ICCP server starting: domain=%s blt=%s port=%d\n",
           g_cfg.domainName, g_cfg.bltId, g_cfg.port);
    printf("[tase2] VMD: TASE2_Version=%d-%d, Supported_Features=Block1,2,5\n",
           TASE2_VERSION_MAJOR, TASE2_VERSION_MINOR);

    /* libIEC61850 here is built single-threaded (CONFIG_MMS_SINGLE_THREADED=1),
     * so we drive the MMS stack and our periodic work from one loop. */
    MmsServer_startListeningThreadless(g_server, g_cfg.port);

    int tick = 0;
    uint64_t lastTick = Hal_getTimeInMs();
    while (g_running) {
        MmsServer_waitReady(g_server, 100);
        MmsServer_handleIncomingMessages(g_server);
        MmsServer_handleBackgroundTasks(g_server);

        uint64_t now = Hal_getTimeInMs();
        if (now - lastTick >= 1000) {
            lastTick = now;
            simulateValues();
            tick++;
            int integrityDue = (tick % g_cfg.integritySeconds) == 0;
            reportingTick(integrityDue);
        }
    }

    printf("\n[tase2] shutting down\n");
    MmsServer_destroy(g_server);
    Semaphore_destroy(g_lock);
    return 0;
}
