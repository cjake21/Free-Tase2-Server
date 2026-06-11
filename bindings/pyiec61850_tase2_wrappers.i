/*
 * pyiec61850_tase2_wrappers.i
 *
 * SWIG %inline additions that give the stock libIEC61850 pyiec61850 binding the
 * handful of helper functions the FreeTase2 client expects but which upstream
 * pyiec61850 does not provide. scripts/01_clone_and_build.sh appends an
 * %include of this file to libiec61850/pyiec61850/iec61850.i before building
 * the binding.
 *
 * Provided:
 *   toMmsErrorP()                  -> MmsError*  (pass-by-pointer error slot)
 *   getMmsVASItemId(spec)          -> itemId of an MmsVariableAccessSpecification
 *   informationReportHandler_create() -> a printing MmsInformationReportHandler
 *   write_dataset(con, domain, ds_name, ts_name, buf, integrity, conditions)
 *                                  -> bind a data set to a DS transfer set and
 *                                     enable it (Block 2)
 *   MmsConnection_createWisop(...) -> MmsConnection with custom ISO parameters
 */
%inline %{
#include "mms_client_connection.h"
#include "iso_connection_parameters.h"
#include <stdlib.h>
#include <stdio.h>

static MmsError*
toMmsErrorP(void)
{
    return (MmsError*) calloc(1, sizeof(MmsError));
}

static char*
getMmsVASItemId(MmsVariableAccessSpecification* spec)
{
    return spec ? spec->itemId : NULL;
}

static void
_tase2_pyReportHandler(void* parameter, char* domainName, char* variableListName,
                       MmsValue* value, bool isVariableListName)
{
    char buf[1024];
    MmsValue_printToBuffer(value, buf, sizeof(buf));
    printf("[FreeTase2 report] %s = %s\n",
           variableListName ? variableListName : (domainName ? domainName : "?"), buf);
    fflush(stdout);
}

static MmsInformationReportHandler
informationReportHandler_create(void)
{
    return _tase2_pyReportHandler;
}

static bool
write_dataset(MmsConnection con, const char* domain, const char* ds_name,
              const char* ts_name, int buffer_time, int integrity_time,
              int all_changes_reported)
{
    MmsError err;
    char item[256];

    snprintf(item, sizeof(item), "%s$DataSetName", ts_name);
    MmsConnection_writeVariable(con, &err, domain, item, MmsValue_newVisibleString(ds_name));

    snprintf(item, sizeof(item), "%s$Interval", ts_name);
    MmsConnection_writeVariable(con, &err, domain, item, MmsValue_newIntegerFromInt32(integrity_time));

    snprintf(item, sizeof(item), "%s$BufferTime", ts_name);
    MmsConnection_writeVariable(con, &err, domain, item, MmsValue_newIntegerFromInt32(buffer_time));

    snprintf(item, sizeof(item), "%s$DSConditionsRequested", ts_name);
    MmsConnection_writeVariable(con, &err, domain, item, MmsValue_newIntegerFromInt32(all_changes_reported));

    snprintf(item, sizeof(item), "%s$Status", ts_name);
    MmsConnection_writeVariable(con, &err, domain, item, MmsValue_newIntegerFromInt32(1));

    return true;
}

static MmsConnection
MmsConnection_createWisop(const char* localApTitle, int localAeQual,
                          TSelector localT, SSelector localS, PSelector localP,
                          const char* remoteApTitle, int remoteAeQual,
                          TSelector remoteT, SSelector remoteS, PSelector remoteP)
{
    MmsConnection con = MmsConnection_create();
    IsoConnectionParameters params = MmsConnection_getIsoConnectionParameters(con);
    IsoConnectionParameters_setLocalAddresses(params, localP, localS, localT);
    IsoConnectionParameters_setRemoteAddresses(params, remoteP, remoteS, remoteT);
    IsoConnectionParameters_setLocalApTitle(params, localApTitle, localAeQual);
    IsoConnectionParameters_setRemoteApTitle(params, remoteApTitle, remoteAeQual);
    return con;
}
%}
