/*
 * tase2_client.c
 *
 * A small TASE.2/ICCP client built on libIEC61850's MMS client API. It runs a
 * full ICCP workflow against the server so a capture has real traffic in it: it
 * associates, does the Block 1 reads (TASE2_Version, Supported_Features,
 * Bilateral_Table_ID) and creates a data set, then binds that data set to a
 * transfer set and turns it on for Block 2 reporting, and finally operates the
 * Block 5 control point. After that it just listens for the reports coming back.
 *
 * No Python or FreeTase2 needed. Pairs with tase2_server.
 *
 * usage: tase2_client <host> <port> [domain] [seconds]
 * GPL-3.0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "mms_client_connection.h"
#include "mms_value.h"
#include "linked_list.h"
#include "hal_thread.h"

static volatile int g_running = 1;
static int          g_reportCount = 0;

static void sigHandler(int s) { (void)s; g_running = 0; }

static void
readAndPrint(MmsConnection con, const char* domain, const char* item)
{
    MmsError err;
    MmsValue* v = MmsConnection_readVariable(con, &err, domain, item);
    char buf[512];
    if (v) {
        MmsValue_printToBuffer(v, buf, sizeof(buf));
        printf("  read %-22s = %s\n", item, buf);
        MmsValue_delete(v);
    } else {
        printf("  read %-22s -> error %d\n", item, err);
    }
}

static void
writeComponent(MmsConnection con, const char* domain, const char* item, MmsValue* value)
{
    MmsError err;
    MmsConnection_writeVariable(con, &err, domain, item, value);
    printf("  write %-30s -> err %d\n", item, err);
    MmsValue_delete(value);
}

/* Unconfirmed InformationReport handler: this is the Block 2 report arriving. */
static void
reportHandler(void* parameter, char* domainName, char* variableListName,
              MmsValue* value, bool isVariableListName)
{
    g_reportCount++;
    char buf[1024];
    MmsValue_printToBuffer(value, buf, sizeof(buf));
    printf("[report #%d] %s%s = %s\n", g_reportCount,
           isVariableListName ? "list " : "var ",
           variableListName ? variableListName : (domainName ? domainName : "?"),
           buf);
}

int
main(int argc, char** argv)
{
    const char* host = (argc > 1) ? argv[1] : "127.0.0.1";
    int port         = (argc > 2) ? atoi(argv[2]) : 102;
    const char* dom  = (argc > 3) ? argv[3] : "TestDomain";
    int seconds      = (argc > 4) ? atoi(argv[4]) : 30;

    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    MmsConnection con = MmsConnection_create();
    MmsError err;

    printf("[client] connecting to %s:%d (domain %s)\n", host, port, dom);
    if (!MmsConnection_connect(con, &err, host, port)) {
        printf("[client] connect failed (err=%d)\n", err);
        MmsConnection_destroy(con);
        return 1;
    }
    printf("[client] associated.\n");

    /* Block 1: bilateral table negotiation */
    printf("[client] Block 1: bilateral-table negotiation\n");
    readAndPrint(con, NULL, "TASE2_Version");
    readAndPrint(con, NULL, "Supported_Features");
    readAndPrint(con, dom, "Bilateral_Table_ID");
    readAndPrint(con, dom, "Next_DSTransfer_Set");

    /* Block 1: create a data set (named variable list) */
    printf("[client] Block 1: creating data set 'ds_analog' = {tm1, tm2}\n");
    LinkedList dsVars = LinkedList_create();
    LinkedList_add(dsVars, MmsVariableAccessSpecification_create(strdup(dom), strdup("tm1")));
    LinkedList_add(dsVars, MmsVariableAccessSpecification_create(strdup(dom), strdup("tm2")));
    MmsConnection_deleteNamedVariableList(con, &err, dom, "ds_analog"); /* in case it exists */
    MmsConnection_defineNamedVariableList(con, &err, dom, "ds_analog", dsVars);
    printf("  defineNamedVariableList -> err %d\n", err);
    LinkedList_destroyDeep(dsVars,
        (LinkedListValueDeleteFunction) MmsVariableAccessSpecification_destroy);

    /* Block 2: bind data set to DSTransferSet01 and enable it */
    printf("[client] Block 2: configuring DSTransferSet01 (interval+integrity RBE)\n");
    writeComponent(con, dom, "DSTransferSet01$DataSetName", MmsValue_newVisibleString("ds_analog"));
    writeComponent(con, dom, "DSTransferSet01$Interval", MmsValue_newIntegerFromInt32(5));
    writeComponent(con, dom, "DSTransferSet01$DSConditionsRequested", MmsValue_newIntegerFromInt32(0x06));
    writeComponent(con, dom, "DSTransferSet01$RBE", MmsValue_newBoolean(true));
    writeComponent(con, dom, "DSTransferSet01$Status", MmsValue_newIntegerFromInt32(1));

    MmsConnection_setInformationReportHandler(con, reportHandler, NULL);

    /* Block 5: operate the device control point */
    printf("[client] Block 5: operate device control dev1\n");
    writeComponent(con, dom, "dev1$Tag", MmsValue_newVisibleString("client-op"));
    writeComponent(con, dom, "dev1$Command", MmsValue_newIntegerFromInt32(1));

    /* receive reports for the requested duration */
    printf("[client] listening for transfer-set reports for %ds (Ctrl+C to stop)\n", seconds);
    uint64_t deadline = Hal_getTimeInMs() + (uint64_t) seconds * 1000;
    while (g_running && Hal_getTimeInMs() < deadline) {
        MmsConnection_tick(con);     /* process incoming unsolicited reports */
        Thread_sleep(200);
    }

    printf("[client] received %d report(s); closing.\n", g_reportCount);
    MmsConnection_destroy(con);
    return 0;
}
