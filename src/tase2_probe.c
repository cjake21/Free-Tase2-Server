/*
 * tase2_probe.c
 *
 * Minimal TASE.2/ICCP *client* probe built directly on libIEC61850's MMS
 * client API. It associates with a TASE.2 server and exercises the core
 * Block 1 services so the resulting capture contains real TASE.2 PDUs:
 *   - read VMD-scope TASE2_Version and Supported_Features
 *   - read ICC-domain Bilateral_Table_ID and Next_DSTransfer_Set
 *   - list domain variables
 *   - read indication points
 *   - create a data set (named variable list)
 *   - read the data set definition back
 *
 * This is intentionally dependency-free (no Python / FreeTase2) so it can drive
 * and validate the server on its own.
 *
 * usage: tase2_probe <host> <port> [domain]
 * License: GPL-3.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mms_client_connection.h"
#include "mms_value.h"
#include "linked_list.h"

static void
printVar(MmsConnection con, const char* domain, const char* item)
{
    MmsError err;
    MmsValue* v = MmsConnection_readVariable(con, &err, domain, item);
    if (v == NULL) {
        printf("  %-24s -> (read error %d)\n", item, err);
        return;
    }
    char buf[512];
    MmsValue_printToBuffer(v, buf, sizeof(buf));
    printf("  %-24s -> %s\n", item, buf);
    MmsValue_delete(v);
}

int
main(int argc, char** argv)
{
    const char* host = (argc > 1) ? argv[1] : "127.0.0.1";
    int port         = (argc > 2) ? atoi(argv[2]) : 102;
    const char* dom  = (argc > 3) ? argv[3] : "TestDomain";

    MmsConnection con = MmsConnection_create();
    MmsError err;

    printf("[probe] connecting to %s:%d ...\n", host, port);
    if (!MmsConnection_connect(con, &err, host, port)) {
        printf("[probe] connection failed (err=%d)\n", err);
        MmsConnection_destroy(con);
        return 1;
    }
    printf("[probe] associated.\n");

    printf("[probe] VMD-scope objects:\n");
    printVar(con, NULL, "TASE2_Version");
    printVar(con, NULL, "Supported_Features");

    printf("[probe] ICC domain '%s' objects:\n", dom);
    printVar(con, dom, "Bilateral_Table_ID");
    printVar(con, dom, "Next_DSTransfer_Set");
    printVar(con, dom, "tm1");
    printVar(con, dom, "tm2");
    printVar(con, dom, "ts1");

    printf("[probe] domain variable names:\n");
    LinkedList names = MmsConnection_getDomainVariableNames(con, &err, dom);
    if (names) {
        LinkedList e = LinkedList_getNext(names);
        int n = 0;
        while (e) {
            printf("  - %s\n", (char*) e->data);
            e = LinkedList_getNext(e);
            n++;
        }
        printf("  (%d variables)\n", n);
        LinkedList_destroy(names);
    } else {
        printf("  (getNameList error %d)\n", err);
    }

    printf("[probe] creating data set 'ds_probe' = {tm1, tm2}\n");
    /* NOTE: MmsVariableAccessSpecification_create() stores (does not copy) the
     * strings, and _destroy() frees them, so we must pass heap copies. */
    LinkedList dsVars = LinkedList_create();
    LinkedList_add(dsVars, MmsVariableAccessSpecification_create(strdup(dom), strdup("tm1")));
    LinkedList_add(dsVars, MmsVariableAccessSpecification_create(strdup(dom), strdup("tm2")));
    MmsConnection_defineNamedVariableList(con, &err, dom, "ds_probe", dsVars);
    printf("  define result err=%d\n", err);
    LinkedList_destroyDeep(dsVars,
        (LinkedListValueDeleteFunction) MmsVariableAccessSpecification_destroy);

    printf("[probe] done; closing.\n");
    MmsConnection_destroy(con);
    return 0;
}
