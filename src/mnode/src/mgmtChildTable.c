/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"
#include "taosmsg.h"
#include "tast.h"
#include "textbuffer.h"
#include "tschemautil.h"
#include "tscompression.h"
#include "tskiplist.h"
#include "tsqlfunction.h"
#include "ttime.h"
#include "tstatus.h"
#include "tutil.h"
#include "mnode.h"
#include "mgmtAcct.h"
#include "mgmtChildTable.h"
#include "mgmtDb.h"
#include "mgmtDnodeInt.h"
#include "mgmtGrant.h"
#include "mgmtProfile.h"
#include "mgmtSuperTable.h"
#include "mgmtTable.h"
#include "mgmtVgroup.h"

void *tsChildTableSdb;
int32_t tsChildTableUpdateSize;
void *(*mgmtChildTableActionFp[SDB_MAX_ACTION_TYPES])(void *row, char *str, int32_t size, int32_t *ssize);

void *mgmtChildTableActionInsert(void *row, char *str, int32_t size, int32_t *ssize);
void *mgmtChildTableActionDelete(void *row, char *str, int32_t size, int32_t *ssize);
void *mgmtChildTableActionUpdate(void *row, char *str, int32_t size, int32_t *ssize);
void *mgmtChildTableActionEncode(void *row, char *str, int32_t size, int32_t *ssize);
void *mgmtChildTableActionDecode(void *row, char *str, int32_t size, int32_t *ssize);
void *mgmtChildTableActionReset(void *row, char *str, int32_t size, int32_t *ssize);
void *mgmtChildTableActionDestroy(void *row, char *str, int32_t size, int32_t *ssize);

static void mgmtDestroyChildTable(SChildTableObj *pTable) {
  free(pTable);
}

static void mgmtChildTableActionInit() {
  mgmtChildTableActionFp[SDB_TYPE_INSERT] = mgmtChildTableActionInsert;
  mgmtChildTableActionFp[SDB_TYPE_DELETE] = mgmtChildTableActionDelete;
  mgmtChildTableActionFp[SDB_TYPE_UPDATE] = mgmtChildTableActionUpdate;
  mgmtChildTableActionFp[SDB_TYPE_ENCODE] = mgmtChildTableActionEncode;
  mgmtChildTableActionFp[SDB_TYPE_DECODE] = mgmtChildTableActionDecode;
  mgmtChildTableActionFp[SDB_TYPE_RESET] = mgmtChildTableActionReset;
  mgmtChildTableActionFp[SDB_TYPE_DESTROY] = mgmtChildTableActionDestroy;
}

void *mgmtChildTableActionReset(void *row, char *str, int32_t size, int32_t *ssize) {
  SChildTableObj *pTable = (SChildTableObj *) row;
  memcpy(pTable, str, tsChildTableUpdateSize);
  return NULL;
}

void *mgmtChildTableActionDestroy(void *row, char *str, int32_t size, int32_t *ssize) {
  SChildTableObj *pTable = (SChildTableObj *)row;
  mgmtDestroyChildTable(pTable);
  return NULL;
}

void *mgmtChildTableActionInsert(void *row, char *str, int32_t size, int32_t *ssize) {
  SChildTableObj *pTable = (SChildTableObj *) row;

  SVgObj *pVgroup = mgmtGetVgroup(pTable->vgId);
  if (pVgroup == NULL) {
    mError("id:%s not in vgroup:%d", pTable->tableId, pTable->vgId);
    return NULL;
  }

  SDbObj *pDb = mgmtGetDb(pVgroup->dbName);
  if (pDb == NULL) {
    mError("vgroup:%d not in DB:%s", pVgroup->vgId, pVgroup->dbName);
    return NULL;
  }

  SAcctObj *pAcct = mgmtGetAcct(pDb->cfg.acct);
  if (pAcct == NULL) {
    mError("account not exists");
    return NULL;
  }

  if (!sdbMaster) {
    int32_t sid = taosAllocateId(pVgroup->idPool);
    if (sid != pTable->sid) {
      mError("sid:%d is not matched from the master:%d", sid, pTable->sid);
      return NULL;
    }
  }

  pTable->superTable = mgmtGetSuperTable(pTable->superTableId);
  mgmtAddTableIntoSuperTable(pTable->superTable);

  mgmtAddTimeSeries(pAcct, pTable->superTable->numOfColumns - 1);
  mgmtAddTableIntoDb(pDb);
  mgmtAddTableIntoVgroup(pVgroup, (STableInfo *) pTable);

  if (pVgroup->numOfTables >= pDb->cfg.maxSessions - 1 && pDb->numOfVgroups > 1) {
    mgmtMoveVgroupToTail(pDb, pVgroup);
  }

  return NULL;
}

void *mgmtChildTableActionDelete(void *row, char *str, int32_t size, int32_t *ssize) {
  SChildTableObj *pTable = (SChildTableObj *) row;
  if (pTable->vgId == 0) {
    return NULL;
  }

  SVgObj *pVgroup = mgmtGetVgroup(pTable->vgId);
  if (pVgroup == NULL) {
    mError("id:%s not in vgroup:%d", pTable->tableId, pTable->vgId);
    return NULL;
  }

  SDbObj *pDb = mgmtGetDb(pVgroup->dbName);
  if (pDb == NULL) {
    mError("vgroup:%d not in DB:%s", pVgroup->vgId, pVgroup->dbName);
    return NULL;
  }

  SAcctObj *pAcct = mgmtGetAcct(pDb->cfg.acct);
  if (pAcct == NULL) {
    mError("account not exists");
    return NULL;
  }

  mgmtRestoreTimeSeries(pAcct, pTable->superTable->numOfColumns - 1);
  mgmtRemoveTableFromDb(pDb);
  mgmtRemoveTableFromVgroup(pVgroup, (STableInfo *) pTable);

  mgmtRemoveTableFromSuperTable(pTable->superTable);

  if (pVgroup->numOfTables > 0) {
    mgmtMoveVgroupToHead(pDb, pVgroup);
  }

  return NULL;
}

void *mgmtChildTableActionUpdate(void *row, char *str, int32_t size, int32_t *ssize) {
  return mgmtChildTableActionReset(row, str, size, NULL);
}

void *mgmtChildTableActionEncode(void *row, char *str, int32_t size, int32_t *ssize) {
  SChildTableObj *pTable = (SChildTableObj *) row;
  assert(row != NULL && str != NULL);

  memcpy(str, pTable, tsChildTableUpdateSize);
  *ssize = tsChildTableUpdateSize;

  return NULL;
}

void *mgmtChildTableActionDecode(void *row, char *str, int32_t size, int32_t *ssize) {
  assert(str != NULL);

  SChildTableObj *pTable = (SChildTableObj *)calloc(sizeof(SChildTableObj), 1);
  if (pTable == NULL) return NULL;

  if (size < tsChildTableUpdateSize) {
    mgmtDestroyChildTable(pTable);
    return NULL;
  }
  memcpy(pTable, str, tsChildTableUpdateSize);

  return (void *)pTable;
}

void *mgmtChildTableAction(char action, void *row, char *str, int32_t size, int32_t *ssize) {
  if (mgmtChildTableActionFp[(uint8_t)action] != NULL) {
    return (*(mgmtChildTableActionFp[(uint8_t)action]))(row, str, size, ssize);
  }
  return NULL;
}

int32_t mgmtInitChildTables() {
  void *pNode = NULL;
  void *pLastNode = NULL;
  SChildTableObj *pTable = NULL;

  mgmtChildTableActionInit();
  SChildTableObj tObj;
  tsChildTableUpdateSize = tObj.updateEnd - (int8_t *)&tObj;

  tsChildTableSdb = sdbOpenTable(tsMaxTables, tsChildTableUpdateSize,
                                 "ctables", SDB_KEYTYPE_STRING, tsMgmtDirectory, mgmtChildTableAction);
  if (tsChildTableSdb == NULL) {
    mError("failed to init child table data");
    return -1;
  }

  pNode = NULL;
  while (1) {
    pNode = sdbFetchRow(tsChildTableSdb, pNode, (void **)&pTable);
    if (pTable == NULL) {
      break;
    }

    SDbObj *pDb = mgmtGetDbByTableId(pTable->tableId);
    if (pDb == NULL) {
      mError("ctable:%s, failed to get db, discard it", pTable->tableId);
      sdbDeleteRow(tsChildTableSdb, pTable);
      pNode = pLastNode;
      continue;
    }

    SVgObj *pVgroup = mgmtGetVgroup(pTable->vgId);
    if (pVgroup == NULL) {
      mError("ctable:%s, failed to get vgroup:%d sid:%d, discard it", pTable->tableId, pTable->vgId, pTable->sid);
      pTable->vgId = 0;
      sdbDeleteRow(tsChildTableSdb, pTable);
      pNode = pLastNode;
      continue;
    }

    if (strcmp(pVgroup->dbName, pDb->name) != 0) {
      mError("ctable:%s, db:%s not match with vgroup:%d db:%s sid:%d, discard it",
             pTable->tableId, pDb->name, pTable->vgId, pVgroup->dbName, pTable->sid);
      pTable->vgId = 0;
      sdbDeleteRow(tsChildTableSdb, pTable);
      pNode = pLastNode;
      continue;
    }

    if (pVgroup->tableList == NULL) {
      mError("ctable:%s, vgroup:%d tableList is null", pTable->tableId, pTable->vgId);
      pTable->vgId = 0;
      sdbDeleteRow(tsChildTableSdb, pTable);
      pNode = pLastNode;
      continue;
    }

    pVgroup->tableList[pTable->sid] = (STableInfo*)pTable;
    taosIdPoolMarkStatus(pVgroup->idPool, pTable->sid, 1);

    SSuperTableObj *pSuperTable = mgmtGetSuperTable(pTable->superTableId);
    if (pSuperTable == NULL) {
      mError("ctable:%s, stable:%s not exist", pTable->tableId, pTable->superTableId);
      pTable->vgId = 0;
      sdbDeleteRow(tsChildTableSdb, pTable);
      pNode = pLastNode;
      continue;
    }

    pTable->superTable = pSuperTable;
    mgmtAddTableIntoSuperTable(pSuperTable);

    SAcctObj *pAcct = mgmtGetAcct(pDb->cfg.acct);
    mgmtAddTimeSeries(pAcct, pTable->superTable->numOfColumns - 1);
  }

  mTrace("child table is initialized");
  return 0;
}

void mgmtCleanUpChildTables() {
  sdbCloseTable(tsChildTableSdb);
}

static void *mgmtBuildCreateChildTableMsg(SChildTableObj *pTable, SVgObj *pVgroup, void *pTagData, int32_t tagDataLen) {
  int32_t totalCols = pTable->superTable->numOfColumns + pTable->superTable->numOfTags;
  int32_t contLen   = sizeof(SDCreateTableMsg) + totalCols * sizeof(SSchema) + tagDataLen;

  SDCreateTableMsg *pCreateTable = rpcMallocCont(contLen);
  if (pCreateTable == NULL) {
    return NULL;
  }

  memcpy(pCreateTable->tableId, pTable->tableId, TSDB_TABLE_ID_LEN);
  memcpy(pCreateTable->superTableId, pTable->superTable->tableId, TSDB_TABLE_ID_LEN);
  pCreateTable->tableType     = pTable->type;
  pCreateTable->numOfColumns  = htons(pTable->superTable->numOfColumns);
  pCreateTable->numOfTags     = htons(pTable->superTable->numOfTags);
  pCreateTable->sid           = htonl(pTable->sid);
  pCreateTable->sversion      = htonl(pTable->superTable->sversion);
  pCreateTable->tagDataLen    = htonl(tagDataLen);
  pCreateTable->sqlDataLen    = 0;
  pCreateTable->contLen       = htonl(contLen);
  pCreateTable->numOfVPeers   = htonl(pVgroup->numOfVnodes);
  pCreateTable->uid           = htobe64(pTable->uid);
  pCreateTable->superTableUid = htobe64(pTable->superTable->uid);
  pCreateTable->createdTime   = htobe64(pTable->createdTime);

  for (int i = 0; i < pVgroup->numOfVnodes; ++i) {
    pCreateTable->vpeerDesc[i].ip = htonl(pVgroup->vnodeGid[i].ip);
    pCreateTable->vpeerDesc[i].vnode = htonl(pVgroup->vnodeGid[i].vnode);
  }

  SSchema *pSchema = (SSchema *) pCreateTable->data;
  memcpy(pSchema, pTable->superTable->schema, totalCols * sizeof(SSchema));
  for (int32_t col = 0; col < totalCols; ++col) {
    pSchema->bytes = htons(pSchema->bytes);
    pSchema->colId = htons(pSchema->colId);
    pSchema++;
  }

  memcpy(pCreateTable + sizeof(SDCreateTableMsg) + totalCols * sizeof(SSchema), pTagData, tagDataLen);

  return pCreateTable;
}

int32_t mgmtCreateChildTable(SCreateTableMsg *pCreate, int32_t contLen, SVgObj *pVgroup, int32_t sid,
                             SDCreateTableMsg **pDCreateOut, STableInfo **pTableOut) {
  int32_t numOfTables = sdbGetNumOfRows(tsChildTableSdb);
  if (numOfTables >= tsMaxTables) {
    mError("table:%s, numOfTables:%d exceed maxTables:%d", pCreate->tableId, numOfTables, tsMaxTables);
    return TSDB_CODE_TOO_MANY_TABLES;
  }

  char           *pTagData    = (char *) pCreate->schema;  // it is a tag key
  SSuperTableObj *pSuperTable = mgmtGetSuperTable(pTagData);
  if (pSuperTable == NULL) {
    mError("table:%s, corresponding super table does not exist", pCreate->tableId);
    return TSDB_CODE_INVALID_TABLE;
  }

  SChildTableObj *pTable = (SChildTableObj *) calloc(sizeof(SChildTableObj), 1);
  if (pTable == NULL) {
    mError("table:%s, failed to alloc memory", pCreate->tableId);
    return TSDB_CODE_SERV_OUT_OF_MEMORY;
  }
  strcpy(pTable->tableId, pCreate->tableId);
  strcpy(pTable->superTableId, pSuperTable->tableId);
  pTable->type        = TSDB_TABLE_TYPE_CHILD_TABLE;
  pTable->createdTime = taosGetTimestampMs();
  pTable->superTable  = pSuperTable;
  pTable->vgId        = pVgroup->vgId;
  pTable->sid         = sid;
  pTable->uid         = (((uint64_t) pTable->vgId) << 40) + ((((uint64_t) pTable->sid) & ((1ul << 24) - 1ul)) << 16) +
                        ((uint64_t) sdbGetVersion() & ((1ul << 16) - 1ul));

  if (sdbInsertRow(tsChildTableSdb, pTable, 0) < 0) {
    mError("table:%s, update sdb error", pCreate->tableId);
    return TSDB_CODE_SDB_ERROR;
  }

  pTagData += (TSDB_TABLE_ID_LEN + 1);
  int32_t tagDataLen = contLen - sizeof(SCreateTableMsg) - TSDB_TABLE_ID_LEN - 1;
  *pDCreateOut = mgmtBuildCreateChildTableMsg(pTable, pVgroup, pTagData, tagDataLen);
  if (*pDCreateOut == NULL) {
    mError("table:%s, failed to build create table message", pCreate->tableId);
    return TSDB_CODE_SERV_OUT_OF_MEMORY;
  }

  *pTableOut = (STableInfo *) pTable;

  mTrace("table:%s, create table in vgroup, vgroup:%d sid:%d vnode:%d uid:%" PRIu64 ,
         pTable->tableId, pVgroup->vgId, sid, pVgroup->vnodeGid[0].vnode, pTable->uid);

  return TSDB_CODE_SUCCESS;
}

int32_t mgmtDropChildTable(SDbObj *pDb, SChildTableObj *pTable) {
  SVgObj *pVgroup = mgmtGetVgroup(pTable->vgId);
  if (pVgroup == NULL) {
    mError("table:%s, failed to drop child table, vgroup not exist", pTable->tableId);
    return TSDB_CODE_OTHERS;
  }

  SDRemoveTableMsg *pRemove = rpcMallocCont(sizeof(SDRemoveTableMsg));
  if (pRemove == NULL) {
    mError("table:%s, failed to drop child table, no enough memory", pTable->tableId);
    return TSDB_CODE_SERV_OUT_OF_MEMORY;
  }

  strcpy(pRemove->tableId, pTable->tableId);
  pRemove->sid = htonl(pTable->sid);
  pRemove->uid = htobe64(pTable->uid);

  pRemove->numOfVPeers = htonl(pVgroup->numOfVnodes);
  for (int i = 0; i < pVgroup->numOfVnodes; ++i) {
    pRemove->vpeerDesc[i].ip = htonl(pVgroup->vnodeGid[i].ip);
    pRemove->vpeerDesc[i].vnode = htonl(pVgroup->vnodeGid[i].vnode);
  }

  SRpcIpSet ipSet = mgmtGetIpSetFromVgroup(pVgroup);
  mgmtSendRemoveTableMsg(pRemove, &ipSet, NULL);

  if (sdbDeleteRow(tsChildTableSdb, pTable) < 0) {
    mError("table:%s, update ctables sdb error", pTable->tableId);
    return TSDB_CODE_SDB_ERROR;
  }

  if (pVgroup->numOfTables <= 0) {
    mgmtDropVgroup(pDb, pVgroup);
  }

  return TSDB_CODE_SUCCESS;
}

void* mgmtGetChildTable(char *tableId) {
  return sdbGetRow(tsChildTableSdb, tableId);
}

int32_t mgmtModifyChildTableTagValueByName(SChildTableObj *pTable, char *tagName, char *nContent) {
//  int32_t col = mgmtFindSuperTableTagIndex(pTable->superTable, tagName);
//  if (col < 0 || col > pTable->superTable->numOfTags) {
//    return TSDB_CODE_APP_ERROR;
//  }
//
//  //TODO send msg to dnode
//  mTrace("Succeed to modify tag column %d of table %s", col, pTable->tableId);
//  return TSDB_CODE_SUCCESS;

//  int32_t rowSize = 0;
//  SSchema *schema = (SSchema *)(pSuperTable->schema + (pSuperTable->numOfColumns + col) * sizeof(SSchema));
//
//  if (col == 0) {
//    pTable->isDirty = 1;
//    removeMeterFromMetricIndex(pSuperTable, pTable);
//  }
//  memcpy(pTable->pTagData + mgmtGetTagsLength(pMetric, col) + TSDB_TABLE_ID_LEN, nContent, schema->bytes);
//  if (col == 0) {
//    addMeterIntoMetricIndex(pMetric, pTable);
//  }
//
//  // Encode the string
//  int32_t   size = sizeof(STabObj) + TSDB_MAX_BYTES_PER_ROW + 1;
//  char *msg = (char *)malloc(size);
//  if (msg == NULL) {
//    mError("failed to allocate message memory while modify tag value");
//    return TSDB_CODE_APP_ERROR;
//  }
//  memset(msg, 0, size);
//
//  mgmtMeterActionEncode(pTable, msg, size, &rowSize);
//
//  int32_t ret = sdbUpdateRow(tsChildTableSdb, msg, rowSize, 1);  // Need callback function
//  tfree(msg);
//
//  if (pTable->isDirty) pTable->isDirty = 0;
//
//  if (ret < 0) {
//    mError("Failed to modify tag column %d of table %s", col, pTable->tableId);
//    return TSDB_CODE_APP_ERROR;
//  }
//
//  mTrace("Succeed to modify tag column %d of table %s", col, pTable->tableId);
//  return TSDB_CODE_SUCCESS;
  return 0;
}

int32_t mgmtGetChildTableMeta(SDbObj *pDb, SChildTableObj *pTable, STableMeta *pMeta, bool usePublicIp) {
  pMeta->uid          = htobe64(pTable->uid);
  pMeta->sid          = htonl(pTable->sid);
  pMeta->vgid         = htonl(pTable->vgId);
  pMeta->sversion     = htons(pTable->superTable->sversion);
  pMeta->precision    = pDb->cfg.precision;
  pMeta->numOfTags    = pTable->superTable->numOfTags;
  pMeta->numOfColumns = htons(pTable->superTable->numOfColumns);
  pMeta->tableType    = pTable->type;
  pMeta->contLen      = sizeof(STableMeta) + mgmtSetSchemaFromSuperTable(pMeta->schema, pTable->superTable);
  strcpy(pMeta->tableId, pTable->tableId);

  SVgObj *pVgroup = mgmtGetVgroup(pTable->vgId);
  if (pVgroup == NULL) {
    return TSDB_CODE_INVALID_TABLE;
  }
  for (int32_t i = 0; i < TSDB_VNODES_SUPPORT; ++i) {
    if (usePublicIp) {
      pMeta->vpeerDesc[i].ip    = pVgroup->vnodeGid[i].publicIp;
      pMeta->vpeerDesc[i].vnode = htonl(pVgroup->vnodeGid[i].vnode);
    } else {
      pMeta->vpeerDesc[i].ip    = pVgroup->vnodeGid[i].ip;
      pMeta->vpeerDesc[i].vnode = htonl(pVgroup->vnodeGid[i].vnode);
    }
  }
  pMeta->numOfVpeers = pVgroup->numOfVnodes;

  return TSDB_CODE_SUCCESS;
}