/*
 * tase2_hmi_agent.c
 *
 * A persistent TASE.2/ICCP client that the SCADA HMI bridge drives. It holds a
 * single association to the FreeTASE2 server and turns simple line commands on
 * stdin into real ICCP services, emitting one JSON event per line on stdout.
 * This is what makes the HMI's interactions real protocol traffic on the wire
 * rather than a local fake: every operator action becomes an MMS read/write, and
 * Station B's view is fed by the Block 2 InformationReports this agent receives.
 *
 * The bridge runs two of these:
 *   - a "Station A" writer: WRITEF/WRITEI/OPERATE/READ (the local control centre)
 *   - a "Station B" subscriber: SUBSCRIBE, then it streams report events (the
 *     remote control centre / BA whose HMI shows whatever arrives)
 *
 * Commands (one per line on stdin):
 *   SUBSCRIBE                 define ds_hmi={tm1,tm2,ts1,ts2}, bind DSTransferSet01, enable RBE+integrity
 *   WRITEF <item> <float>     write a float Value (tm1, tm2)        e.g. WRITEF tm1 137.5
 *   WRITEI <item> <int>       write an integer Value (ts1, ts2)     e.g. WRITEI ts1 0
 *   OPERATE <cmd> <tag>       Block 5 operate dev1 (Command + Tag)  e.g. OPERATE 1 hmi-open
 *   READ <item>               read a point's Value                  e.g. READ tm1
 *   SNAPSHOT                  read Block 1 metadata + all points
 *   QUIT
 *
 * Events (one JSON object per line on stdout):
 *   {"ev":"online","host":"..","port":N,"domain":".."}
 *   {"ev":"snapshot","version":"..","features":"..","blt":"..","next_ts":"..",
 *    "tm1":..,"tm2":..,"ts1":..,"ts2":..}
 *   {"ev":"read","item":"tm1","value":..}
 *   {"ev":"write","item":"tm1$Value","value":..,"err":N}
 *   {"ev":"subscribed","dataset":"ds_hmi","transferset":"DSTransferSet01"}
 *   {"ev":"report","ts":"DSTransferSet01","time":"..","cond":N,"tm1":..,"tm2":..,"ts1":..,"ts2":..}
 *   {"ev":"error","msg":".."}
 *
 * usage: tase2_hmi_agent <host> <port> [domain]
 * GPL-3.0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>

#include "mms_client_connection.h"
#include "mms_value.h"
#include "linked_list.h"
#include "hal_thread.h"

static volatile int g_running = 1;
static const char*  g_dom = "TestDomain";

static void sigHandler(int s) { (void)s; g_running = 0; }

/* Print a JSON string value with the few escapes we can actually hit (quotes,
 * backslashes, control chars). Tags and object names are simple, but be safe. */
static void
emitJsonString(const char* s)
{
    putchar('"');
    for (const char* p = s; p && *p; p++) {
        unsigned char c = (unsigned char) *p;
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (c < 0x20)         { printf("\\u%04x", c); }
        else                        putchar(c);
    }
    putchar('"');
}

/* Read element 0 ("Value") of an indication-point structure as a JSON number. */
static void
emitPointValue(MmsValue* v)
{
    if (v == NULL) { printf("null"); return; }
    MmsValue* el = (MmsValue_getType(v) == MMS_STRUCTURE) ? MmsValue_getElement(v, 0) : v;
    if (el == NULL) { printf("null"); return; }
    switch (MmsValue_getType(el)) {
        case MMS_FLOAT:   printf("%.6g", MmsValue_toFloat(el)); break;
        case MMS_INTEGER: printf("%d", MmsValue_toInt32(el)); break;
        default:          printf("null"); break;
    }
}

static MmsValue*
readVar(MmsConnection con, const char* domain, const char* item)
{
    MmsError err;
    return MmsConnection_readVariable(con, &err, domain, item);
}

/* Block 2 report arriving: header is {name, time, conditions} then the data set
 * members in the order we defined ds_hmi (tm1, tm2, ts1, ts2). */
static void
reportHandler(void* parameter, char* domainName, char* variableListName,
              MmsValue* value, bool isVariableListName)
{
    static const char* names[4] = {"tm1", "tm2", "ts1", "ts2"};
    int n = MmsValue_getArraySize(value);

    printf("{\"ev\":\"report\",\"ts\":");
    emitJsonString(variableListName ? variableListName : "?");

    if (n >= 2) {
        char buf[64];
        MmsValue_printToBuffer(MmsValue_getElement(value, 1), buf, sizeof(buf));
        printf(",\"time\":");
        emitJsonString(buf);
    }
    if (n >= 3)
        printf(",\"cond\":%d", MmsValue_toInt32(MmsValue_getElement(value, 2)));

    /* members start at index 3, mapped to ds_hmi's fixed order */
    for (int i = 3; i < n && (i - 3) < 4; i++) {
        printf(",\"%s\":", names[i - 3]);
        emitPointValue(MmsValue_getElement(value, i));
    }
    printf("}\n");
    fflush(stdout);
}

static void
doSubscribe(MmsConnection con)
{
    MmsError err;

    LinkedList dsVars = LinkedList_create();
    LinkedList_add(dsVars, MmsVariableAccessSpecification_create(strdup(g_dom), strdup("tm1")));
    LinkedList_add(dsVars, MmsVariableAccessSpecification_create(strdup(g_dom), strdup("tm2")));
    LinkedList_add(dsVars, MmsVariableAccessSpecification_create(strdup(g_dom), strdup("ts1")));
    LinkedList_add(dsVars, MmsVariableAccessSpecification_create(strdup(g_dom), strdup("ts2")));
    MmsConnection_deleteNamedVariableList(con, &err, g_dom, "ds_hmi");
    MmsConnection_defineNamedVariableList(con, &err, g_dom, "ds_hmi", dsVars);
    LinkedList_destroyDeep(dsVars,
        (LinkedListValueDeleteFunction) MmsVariableAccessSpecification_destroy);

    MmsConnection_writeVariable(con, &err, g_dom, "DSTransferSet01$DataSetName",
                                MmsValue_newVisibleString("ds_hmi"));
    MmsConnection_writeVariable(con, &err, g_dom, "DSTransferSet01$Interval",
                                MmsValue_newIntegerFromInt32(5));
    MmsConnection_writeVariable(con, &err, g_dom, "DSTransferSet01$DSConditionsRequested",
                                MmsValue_newIntegerFromInt32(0x06));
    MmsConnection_writeVariable(con, &err, g_dom, "DSTransferSet01$RBE",
                                MmsValue_newBoolean(true));
    MmsConnection_setInformationReportHandler(con, reportHandler, NULL);
    MmsConnection_writeVariable(con, &err, g_dom, "DSTransferSet01$Status",
                                MmsValue_newIntegerFromInt32(1));

    printf("{\"ev\":\"subscribed\",\"dataset\":\"ds_hmi\",\"transferset\":\"DSTransferSet01\"}\n");
    fflush(stdout);
}

static void
doWrite(MmsConnection con, const char* item, MmsValue* v)
{
    MmsError err;
    char full[80];
    snprintf(full, sizeof(full), "%s$Value", item);
    MmsConnection_writeVariable(con, &err, g_dom, full, v);
    MmsValue_delete(v);
    printf("{\"ev\":\"write\",\"item\":");
    emitJsonString(full);
    printf(",\"err\":%d}\n", err);
    fflush(stdout);
}

static void
doOperate(MmsConnection con, int command, const char* tag)
{
    MmsError err;
    MmsConnection_writeVariable(con, &err, g_dom, "dev1$Tag", MmsValue_newVisibleString(tag));
    MmsConnection_writeVariable(con, &err, g_dom, "dev1$Command", MmsValue_newIntegerFromInt32(command));
    printf("{\"ev\":\"operate\",\"command\":%d,\"tag\":", command);
    emitJsonString(tag);
    printf(",\"err\":%d}\n", err);
    fflush(stdout);
}

static void
doRead(MmsConnection con, const char* item)
{
    MmsValue* v = readVar(con, g_dom, item);
    printf("{\"ev\":\"read\",\"item\":");
    emitJsonString(item);
    printf(",\"value\":");
    emitPointValue(v);
    printf("}\n");
    fflush(stdout);
    if (v) MmsValue_delete(v);
}

static void
doSnapshot(MmsConnection con)
{
    char buf[128];
    printf("{\"ev\":\"snapshot\"");

    MmsValue* v = readVar(con, NULL, "TASE2_Version");
    if (v) { MmsValue_printToBuffer(v, buf, sizeof(buf)); printf(",\"version\":"); emitJsonString(buf); MmsValue_delete(v); }
    v = readVar(con, NULL, "Supported_Features");
    if (v) { MmsValue_printToBuffer(v, buf, sizeof(buf)); printf(",\"features\":"); emitJsonString(buf); MmsValue_delete(v); }
    v = readVar(con, g_dom, "Bilateral_Table_ID");
    if (v) { MmsValue_printToBuffer(v, buf, sizeof(buf)); printf(",\"blt\":"); emitJsonString(buf); MmsValue_delete(v); }
    v = readVar(con, g_dom, "Next_DSTransfer_Set");
    if (v) { MmsValue_printToBuffer(v, buf, sizeof(buf)); printf(",\"next_ts\":"); emitJsonString(buf); MmsValue_delete(v); }

    static const char* pts[4] = {"tm1", "tm2", "ts1", "ts2"};
    for (int i = 0; i < 4; i++) {
        v = readVar(con, g_dom, pts[i]);
        printf(",\"%s\":", pts[i]);
        emitPointValue(v);
        if (v) MmsValue_delete(v);
    }
    printf("}\n");
    fflush(stdout);
}

/* Trim trailing newline / CR in place. */
static void
chomp(char* s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

static void
handleCommand(MmsConnection con, char* line)
{
    char* cmd = strtok(line, " \t");
    if (cmd == NULL) return;

    if (!strcmp(cmd, "SUBSCRIBE")) {
        doSubscribe(con);
    } else if (!strcmp(cmd, "WRITEF")) {
        char* item = strtok(NULL, " \t");
        char* val  = strtok(NULL, " \t");
        if (item && val) doWrite(con, item, MmsValue_newFloat((float) atof(val)));
    } else if (!strcmp(cmd, "WRITEI")) {
        char* item = strtok(NULL, " \t");
        char* val  = strtok(NULL, " \t");
        if (item && val) doWrite(con, item, MmsValue_newIntegerFromInt32(atoi(val)));
    } else if (!strcmp(cmd, "OPERATE")) {
        char* val = strtok(NULL, " \t");
        char* tag = strtok(NULL, " \t");
        if (val) doOperate(con, atoi(val), tag ? tag : "hmi-op");
    } else if (!strcmp(cmd, "READ")) {
        char* item = strtok(NULL, " \t");
        if (item) doRead(con, item);
    } else if (!strcmp(cmd, "SNAPSHOT")) {
        doSnapshot(con);
    } else if (!strcmp(cmd, "QUIT")) {
        g_running = 0;
    }
}

int
main(int argc, char** argv)
{
    const char* host = (argc > 1) ? argv[1] : "127.0.0.1";
    int port         = (argc > 2) ? atoi(argv[2]) : 102;
    g_dom            = (argc > 3) ? argv[3] : "TestDomain";

    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);
    setvbuf(stdout, NULL, _IOLBF, 0);

    MmsConnection con = MmsConnection_create();
    MmsError err;

    if (!MmsConnection_connect(con, &err, host, port)) {
        printf("{\"ev\":\"error\",\"msg\":\"connect failed (err=%d)\"}\n", err);
        fflush(stdout);
        MmsConnection_destroy(con);
        return 1;
    }
    printf("{\"ev\":\"online\",\"host\":");
    emitJsonString(host);
    printf(",\"port\":%d,\"domain\":", port);
    emitJsonString(g_dom);
    printf("}\n");
    fflush(stdout);

    char line[256];
    while (g_running) {
        /* Pump the MMS stack so unsolicited reports are delivered promptly. */
        MmsConnection_tick(con);

        /* Wait briefly for a command, but stay responsive to reports. */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 150000 };
        int r = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            if (fgets(line, sizeof(line), stdin) == NULL) break; /* EOF: bridge closed */
            chomp(line);
            handleCommand(con, line);
        }
    }

    MmsConnection_destroy(con);
    return 0;
}
