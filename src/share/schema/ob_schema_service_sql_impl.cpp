/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SHARE_SCHEMA
#include "ob_schema_service_sql_impl.h"
#include "lib/utility/utility.h"
#include "lib/thread_local/ob_tsi_factory.h"
#include "lib/mysqlclient/ob_mysql_result.h"
#include "lib/mysqlclient/ob_mysql_connection.h"
#include "lib/mysqlclient/ob_mysql_statement.h"
#include "lib/mysqlclient/ob_mysql_connection_pool.h"
#include "lib/string/ob_sql_string.h"
#include "lib/oblog/ob_log.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/time/ob_time_utility.h"
#include "lib/container/ob_array_iterator.h"
#include "lib/mysqlclient/ob_mysql_proxy.h"
#include "lib/charset/ob_dtoa.h"
#include "share/config/ob_server_config.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/ob_global_stat_proxy.h"
#include "share/system_variable/ob_system_variable.h"
// TODO, move basic strcuts to ob_schema_struct.h
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_utils.h"
#include "share/schema/ob_schema_mgr.h"
#include "share/schema/ob_schema_retrieve_utils.h"
#include "share/ob_cluster_role.h"
#include "share/ob_get_compat_mode.h"
#include "observer/ob_sql_client_decorator.h"
#include "observer/ob_server_struct.h"
#include "share/schema/ob_server_schema_service.h"
#include "sql/ob_sql_utils.h"

#define COMMON_SQL              "SELECT * FROM %s"
#define COMMON_SQL_WITH_TENANT  "SELECT * FROM %s WHERE tenant_id = %lu"

#define FETCH_ALL_DDL_OPERATION_SQL         "SELECT * FROM %s"
#define FETCH_ALL_DDL_OPERATION_SQL_WITH_VERSION_RANGE                  \
    FETCH_ALL_DDL_OPERATION_SQL" WHERE schema_version > %lu AND schema_version <= %lu"
#define FETCH_ALL_SYS_VARIABLE_HISTORY_SQL  "SELECT * FROM %s WHERE tenant_id = %lu and schema_version <= %ld"

#define FETCH_ALL_TENANT_HISTORY_SQL        COMMON_SQL

#define FETCH_ALL_TABLE_SQL                     COMMON_SQL_WITH_TENANT
#define FETCH_ALL_TABLE_HISTORY_SQL             COMMON_SQL_WITH_TENANT
#define FETCH_ALL_COLUMN_SQL                    COMMON_SQL_WITH_TENANT
#define FETCH_ALL_COLUMN_HISTORY_SQL            COMMON_SQL_WITH_TENANT
#define FETCH_ALL_CONSTRAINT_SQL                COMMON_SQL_WITH_TENANT
#define FETCH_ALL_CONSTRAINT_HISTORY_SQL        COMMON_SQL_WITH_TENANT
#define FETCH_ALL_DATABASE_HISTORY_SQL          COMMON_SQL_WITH_TENANT
#define FETCH_ALL_TABLEGROUP_HISTORY_SQL2       COMMON_SQL_WITH_TENANT
#define FETCH_ALL_TABLEGROUP_HISTORY_SQL        COMMON_SQL_WITH_TENANT

#define FETCH_ALL_USER_HISTORY_SQL              COMMON_SQL_WITH_TENANT
#define FETCH_ALL_DB_PRIV_HISTORY_SQL           COMMON_SQL_WITH_TENANT
#define FETCH_ALL_SYS_PRIV_HISTORY_SQL          COMMON_SQL_WITH_TENANT
#define FETCH_ALL_TABLE_PRIV_HISTORY_SQL        COMMON_SQL_WITH_TENANT
#define FETCH_ALL_OBJ_PRIV_HISTORY_SQL          COMMON_SQL_WITH_TENANT
#define FETCH_ALL_OUTLINE_HISTORY_SQL           COMMON_SQL_WITH_TENANT
#define FETCH_ALL_SYNONYM_HISTORY_SQL           COMMON_SQL_WITH_TENANT
#define FETCH_ALL_FUNC_HISTORY_SQL              COMMON_SQL_WITH_TENANT
#define FETCH_ALL_ROLE_GRANTEE_MAP_HISTORY_SQL  COMMON_SQL_WITH_TENANT

#define FETCH_ALL_RECYCLEBIN_SQL "SELECT * FROM %s " \
    "WHERE tenant_id = %lu and object_name = '%.*s' and type = %d "

#define FETCH_EXPIRE_ALL_RECYCLEBIN_SQL "SELECT * FROM %s " \
    "WHERE tenant_id = %lu and time_to_usec(gmt_create) < %ld order by gmt_create"

#define FETCH_EXPIRE_SYS_ALL_RECYCLEBIN_SQL "SELECT * FROM %s " \
    "WHERE (tenant_id = %lu or TYPE = 7) and time_to_usec(gmt_create) < %ld order by gmt_create"

#define FETCH_ALL_RECYCLEBIN_SQL_WITH_CONDITION COMMON_SQL_WITH_TENANT

#define FETCH_ALL_PART_SQL                COMMON_SQL_WITH_TENANT
#define FETCH_ALL_PART_HISTORY_SQL        COMMON_SQL_WITH_TENANT
#define FETCH_ALL_SUBPART_SQL             COMMON_SQL_WITH_TENANT " and part_id = 0"
#define FETCH_ALL_SUBPART_HISTORY_SQL     COMMON_SQL_WITH_TENANT " and part_id = 0"
#define FETCH_ALL_DEF_SUBPART_SQL         COMMON_SQL_WITH_TENANT
#define FETCH_ALL_DEF_SUBPART_HISTORY_SQL COMMON_SQL_WITH_TENANT
#define FETCH_ALL_SUB_PART_SQL             COMMON_SQL_WITH_TENANT
#define FETCH_ALL_SUB_PART_HISTORY_SQL     COMMON_SQL_WITH_TENANT
#define FETCH_ALL_REPLICATION_GROUP_HISTORY_SQL  COMMON_SQL_WITH_TENANT

#define FETCH_ALL_PACKAGE_HISTORY_SQL                     COMMON_SQL_WITH_TENANT
#define FETCH_ALL_ROUTINE_HISTORY_SQL                     COMMON_SQL_WITH_TENANT
#define FETCH_ALL_ROUTINE_PARAM_HISTORY_SQL               COMMON_SQL_WITH_TENANT
#define FETCH_ALL_TRIGGER_HISTORY_SQL                     COMMON_SQL_WITH_TENANT
#define FETCH_ALL_TRIGGER_ID_HISTORY_SQL                  "SELECT trigger_id, is_deleted FROM %s WHERE tenant_id = %lu"
#define FETCH_ALL_SEQUENCE_OBJECT_HISTORY_SQL             COMMON_SQL_WITH_TENANT
#define FETCH_ALL_KEYSTORE_HISTORY_SQL                    COMMON_SQL_WITH_TENANT
#define FETCH_ALL_LABEL_SE_POLICY_OBJECT_HISTORY_SQL      COMMON_SQL_WITH_TENANT
#define FETCH_ALL_LABEL_SE_COMPONENT_OBJECT_HISTORY_SQL   COMMON_SQL_WITH_TENANT
#define FETCH_ALL_LABEL_SE_LABEL_OBJECT_HISTORY_SQL       COMMON_SQL_WITH_TENANT
#define FETCH_ALL_LABEL_SE_USER_LEVEL_OBJECT_HISTORY_SQL  COMMON_SQL_WITH_TENANT
#define FETCH_ALL_TABLESPACE_HISTORY_SQL                  COMMON_SQL_WITH_TENANT
#define FETCH_ALL_PROFILE_HISTORY_SQL                     COMMON_SQL_WITH_TENANT
#define FETCH_ALL_SECURITY_AUDIT_HISTORY_SQL              COMMON_SQL_WITH_TENANT

#define FETCH_ALL_TYPE_HISTORY_SQL                        COMMON_SQL_WITH_TENANT
#define FETCH_ALL_TYPE_ATTR_HISTORY_SQL                   COMMON_SQL_WITH_TENANT
#define FETCH_ALL_COLL_TYPE_HISTORY_SQL                   COMMON_SQL_WITH_TENANT
#define FETCH_ALL_OBJECT_TYPE_HISTORY_SQL                 COMMON_SQL_WITH_TENANT

#define FETCH_ALL_DBLINK_HISTORY_SQL                      COMMON_SQL_WITH_TENANT
#define FETCH_ALL_DIRECTORY_HISTORY_SQL                   COMMON_SQL_WITH_TENANT
#define FETCH_ALL_TENANT_CONTEXT_HISTORY_SQL              COMMON_SQL_WITH_TENANT
#define FETCH_ALL_TENANT_MOCK_FK_PARENT_TABLE_HISTORY_SQL COMMON_SQL_WITH_TENANT

// foreign key begin
#define FETCH_ALL_FOREIGN_KEY_SQL \
  "SELECT * FROM %s WHERE tenant_id = %lu"

#define FETCH_ALL_FOREIGN_KEY_HISTORY_SQL \
  "SELECT * FROM %s WHERE tenant_id = %lu"

#define FETCH_ALL_FOREIGN_KEY_COLUMN_SQL \
  "SELECT * FROM %s WHERE tenant_id = %lu"

#define FETCH_ALL_FOREIGN_KEY_COLUMN_HISTORY_SQL \
  "SELECT * FROM %s WHERE tenant_id = %lu"

#define FETCH_ALL_TENANT_CONSTRAINT_COLUMN_HISTORY_SQL \
  "SELECT * FROM %s WHERE tenant_id = %lu"

#define FETCH_TABLE_ID_AND_NAME_FROM_ALL_FOREIGN_KEY_SQL \
  "SELECT is_deleted, foreign_key_id, child_table_id, foreign_key_name FROM %s WHERE tenant_id = %lu"
// foreign key end

#define FETCH_TABLE_ID_AND_CST_NAME_FROM_ALL_CONSTRAINT_HISTORY_SQL \
  "SELECT * FROM %s WHERE tenant_id = %lu"

#define FETCH_RECYCLE_TABLE_OBJECT \
    "SELECT table_name, database_id, tablegroup_id FROM %s \
     WHERE tenant_id = %lu and table_id = %lu and schema_version <= %ld and \
           table_name is not null and table_name != "" and table_name != %s \
     ORDER BY SCHEMA_VERSION DESC LIMIT 1"

#define FETCH_RECYCLE_DATABASE_OBJECT \
    "SELECT database_name, default_tablegroup_id FROM %s \
     WHERE tenant_id = %lu and database_id = %lu and schema_version <= %ld and \
           database_name is not null and database_name != "" and database_name != %s \
     ORDER BY SCHEMA_VERSION DESC LIMIT 1"

#define FETCH_ENCRYPT_INFO_FROM_ALL_TENANT_TABLESAPCE_SQL\
 "SELECT is_deleted, tablespace_id, master_key_id, encrypt_key FROM %s WHERE tenant_id = %lu"

#define DEFINE_SQL_CLIENT_RETRY_WEAK(sql_client)    \
  ObSQLClientRetryWeak sql_client_retry_weak(&sql_client);

#define DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp)    \
        bool check_sys_variable = true; \
        DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_PARAMETER(sql_client, snapshot_timestamp, check_sys_variable)

#define DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_PARAMETER(sql_client, snapshot_timestamp, check_sys_variable)    \
  if (g_liboblog_mode_) {                                               \
    /* To avoid error when liboblog use mysql connection and */         \
    /* set non-existed sys variable, check_sys_variable is reset to true. */ \
    check_sys_variable = true;                                          \
  }                                                                     \
  ObSQLClientRetryWeak sql_client_retry_weak(&sql_client, false, \
                                             snapshot_timestamp, check_sys_variable);
namespace oceanbase
{
namespace share
{
namespace schema
{
using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::common::sqlclient;
using namespace oceanbase::sql;

template<typename T>
int ObSchemaServiceSQLImpl::retrieve_schema_version(T &result, int64_t &schema_version)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(result.next())) {
    if (ret == OB_ITER_END) { //no record
      ret = OB_EMPTY_RESULT;
      LOG_WARN("select max(schema_version) return no row", K(ret));
    } else {
      LOG_WARN("fail to get schema version. iter quit. ", K(ret));
    }
  } else {
    EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "version", schema_version, uint64_t);
    // for debug purpose:
    // https://aone.alibaba-inc.com/issue/11260176?from=ak&
    int32_t myport = 0;
    char svr_ip[OB_IP_STR_BUFF] = "";
    int64_t tmp_real_str_len = 0;
    UNUSED(tmp_real_str_len);
    EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(result, "myip", svr_ip, OB_IP_STR_BUFF, tmp_real_str_len);
    EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "myport", myport, int32_t);
    // end debug
    if (OB_FAIL(ret)) {
      LOG_WARN("fail to get schema_version: ", K(ret));
    } else {
      LOG_TRACE("retrieve schema, ", "newest schema_version", schema_version, "host", static_cast<const char*>(svr_ip), K(myport));
      //check if this is only one
      if (OB_ITER_END != (ret = result.next())) {
        LOG_WARN("fail to get all table schema. iter quit. ", K(ret));
        ret = OB_ERR_UNEXPECTED;
      } else {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

ObSchemaServiceSQLImpl::ObSchemaServiceSQLImpl()
    : mysql_proxy_(NULL),
      dblink_proxy_(NULL),
      mutex_(),
      last_operation_schema_version_(OB_INVALID_VERSION),
      tenant_service_(*this),
      database_service_(*this),
      table_service_(*this),
      tablegroup_service_(*this),
      user_service_(*this),
      priv_service_(*this),
      outline_service_(*this),
      routine_service_(*this),
      trigger_service_(*this),
      refreshed_schema_version_(OB_INVALID_VERSION),
      gen_schema_version_(OB_INVALID_VERSION),
      config_(NULL),
      is_inited_(false),
      synonym_service_(*this),
      udf_service_(*this),
      udt_service_(*this),
      sequence_service_(*this),
      keystore_service_(*this),
      label_se_policy_service_(*this),
      tablespace_service_(*this),
      profile_service_(*this),
      audit_service_(*this),
      rw_lock_(),
      last_operation_tenant_id_(OB_INVALID_TENANT_ID),
      sequence_id_(OB_INVALID_ID),
      schema_info_(),
      sys_variable_service_(*this),
      dblink_service_(*this),
      directory_service_(*this),
      context_service_(*this),
      cluster_schema_status_(ObClusterSchemaStatus::NORMAL_STATUS),
      gen_schema_version_map_(),
      schema_service_(NULL)
{
}

ObSchemaServiceSQLImpl::~ObSchemaServiceSQLImpl()
{
  is_inited_ = false;
}

bool ObSchemaServiceSQLImpl::check_inner_stat()
{
  bool ret = true;
  if (IS_NOT_INIT) {
    LOG_ERROR("not init");
    ret = false;
  }
  return ret;
}

int ObSchemaServiceSQLImpl::init(
    ObMySQLProxy *sql_proxy,
    ObDbLinkProxy *dblink_proxy,
    const ObServerSchemaService *schema_service)
{
  int ret = OB_SUCCESS;
  lib::ObMutexGuard guard(mutex_);
  if (OB_ISNULL(sql_proxy)
      || OB_ISNULL(schema_service)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ptr is null", K(ret), KP(sql_proxy), KP(dblink_proxy), KP(schema_service));
  } else if (OB_FAIL(gen_schema_version_map_.create(TENANT_MAP_BUCKET_NUM,
              ObModIds::OB_GEN_SCHEMA_VERSION_MAP, ObModIds::OB_GEN_SCHEMA_VERSION_MAP))) {
  } else {
    if (OB_ISNULL(dblink_proxy)) {
      LOG_WARN("dblink proxy is null");
    }
    mysql_proxy_ = sql_proxy;
    dblink_proxy_ = dblink_proxy;
    schema_service_ = schema_service;
    table_service_.init(mysql_proxy_);
    refreshed_schema_version_ = OB_CORE_SCHEMA_VERSION;
    is_inited_ = true;
  }
  return ret;
}

void ObSchemaServiceSQLImpl::set_refreshed_schema_version(const int64_t schema_version)
{
  refreshed_schema_version_ = std::max(schema_version, refreshed_schema_version_ + 1);
}

int ObSchemaServiceSQLImpl::get_all_core_table_schema(ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObInnerTableSchema::all_core_table_schema(table_schema))) {
    LOG_WARN("all_core_table_schema failed", K(ret));
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_core_table_schemas(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    ObArray<ObTableSchema> &core_schemas)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail", KR(ret), K(schema_status));
  } else if (OB_FAIL(get_core_table_priorities(sql_client, schema_status, core_schemas))) {
    LOG_WARN("get_core_table_priorities failed", KR(ret), K(schema_status));
  } else if (core_schemas.count() > 0) {
    if (OB_FAIL(get_core_table_columns(sql_client, schema_status, core_schemas))) {
      LOG_WARN("get_core_table_columns failed", KR(ret), K(schema_status));
    } else {
      // mock partition array
      for (int64_t i = 0; OB_SUCC(ret) && i < core_schemas.count(); i++) {
        ObTableSchema &core_schema = core_schemas.at(i);
        if (OB_FAIL(try_mock_partition_array(core_schema))) {
          LOG_WARN("fail to mock partition array", KR(ret), K(core_schema));
        }
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_sys_table_schemas(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    const ObIArray<uint64_t> &table_ids,
    ObIAllocator &allocator,
    ObArray<ObTableSchema *> &sys_schemas)
{
  int ret = OB_SUCCESS;
  ObArray<uint64_t> sys_table_ids;
  if (!check_inner_stat()) {
     ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail", KR(ret), K(schema_status));
  } else if (table_ids.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("table_ids is empty", KR(ret), K(schema_status));
  } else if (OB_FAIL(sys_table_ids.assign(table_ids))) {
    LOG_WARN("assign failed", KR(ret), K(schema_status));
  } else {
    // sys table schema get newest version
    const int64_t schema_version = INT64_MAX;
    if (OB_FAIL(get_batch_table_schema(schema_status, schema_version,
        sys_table_ids, sql_client, allocator, sys_schemas))) {
      LOG_WARN("get_batch_table_schema failed", KR(ret), K(schema_status),
               K(schema_version), K(table_ids));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_table_schema(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    ObArray<uint64_t> &table_ids,
    ObISQLClient &sql_client,
    ObIAllocator &allocator,
    ObArray<ObTableSchema *> &table_schema_array)
{
  int ret = OB_SUCCESS;
  LOG_DEBUG("fetch batch table schema begin.");

  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  } else if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid schema_version", K(schema_version), K(ret));
  } else {
    std::sort(table_ids.begin(), table_ids.end(), std::greater<uint64_t>());
    // get not core table schemas from __all_table and __all_column
    if (OB_FAIL(get_not_core_table_schemas(schema_status, schema_version, table_ids,
                                           sql_client, allocator, table_schema_array))) {
      LOG_WARN("get_not_core_table_schemas failed", K(schema_version),
               K(table_ids), K(ret));
    }
  }

  LOG_INFO("get batch table schema finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::get_new_schema_version(uint64_t tenant_id, int64_t &schema_version)
{
  int ret = OB_SUCCESS;
  schema_version = OB_INVALID_VERSION;
  if (OB_INVALID_TENANT_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", K(ret), K(tenant_id));
  } else if (OB_SYS_TENANT_ID == tenant_id) {
    schema_version = gen_schema_version_;
  } else if (OB_FAIL(gen_schema_version_map_.get_refactored(tenant_id, schema_version))) {
    LOG_WARN("fail to get new schema_version", K(ret));
  }
  return ret;
}

int ObSchemaServiceSQLImpl::gen_new_schema_version(
    uint64_t tenant_id,
    int64_t refreshed_schema_version,
    int64_t &schema_version)
{
  int ret = OB_SUCCESS;
  schema_version = OB_INVALID_VERSION;
  if (OB_INVALID_TENANT_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", K(ret), K(tenant_id));
  } else {
    if (is_sys_tenant(tenant_id)) {
      if (OB_FAIL(gen_leader_sys_schema_version(tenant_id, schema_version))) {
          LOG_WARN("failed to gen leader sys tenant_id schema version", K(ret), K(tenant_id));
      }
    } else {
      // normal tenant
      if (OB_FAIL(gen_leader_normal_schema_version(tenant_id, refreshed_schema_version, schema_version))) {
        LOG_WARN("failed to gen leader normal schema version", K(ret), K(tenant_id), K(refreshed_schema_version));
      }

    }
  }
  if (OB_FAIL(ret)) {
  } else {
    LOG_INFO("new schema version", K(schema_version), "this", OB_P(this));
  }
  return ret;
}


// generate new schema version for ddl on primary cluster's system tenant
int ObSchemaServiceSQLImpl::gen_leader_sys_schema_version(const int64_t tenant_id, int64_t &schema_version)
{
  int ret = OB_SUCCESS;
  UNUSED(tenant_id);
  schema_version = OB_INVALID_VERSION;
  // generate new schema version by following factors:
  // 1. lasted schema version(refreshed_schema_version_, gen_schema_version_)
  // 2. next_new_schema_version_(for standby cluster's system tenant's ddl execution)
  // 3. rs local timestamp
  schema_version = std::max(refreshed_schema_version_, gen_schema_version_);
  schema_version = std::max(schema_version + SYS_SCHEMA_VERSION_INC_STEP,
      ObTimeUtility::current_time());
  /* format version */
  schema_version /= SCHEMA_VERSION_INC_STEP;
  schema_version *= SCHEMA_VERSION_INC_STEP;
  gen_schema_version_ = schema_version;
  return ret;
}
// generate new schema version for ddl on primary cluster's normal tenant
int ObSchemaServiceSQLImpl::gen_leader_normal_schema_version(const uint64_t tenant_id,
                                       const int64_t refreshed_schema_version,
                                       int64_t &schema_version)
{
  int ret = OB_SUCCESS;
  schema_version = OB_INVALID_VERSION;
  if (OB_SYS_TENANT_ID == tenant_id) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tenant id is invalid", K(ret), K(tenant_id));
  } else {
    // generate new schema version by following factors:
    // 1. lasted schema version(tenant's refreshed_schema_version, gen_schema_version_)
    // 2. next_new_schema_version_(for standby cluster's system tenant's ddl execution)
    // 3. rs local timestamp
    int64_t gen_schema_version = OB_INVALID_VERSION;
    if (OB_FAIL(gen_schema_version_map_.get_refactored(tenant_id, gen_schema_version))) {
      if (OB_HASH_NOT_EXIST == ret) {
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("fail to get gen schema version", K(ret), K(tenant_id));
      }
    }
    if (OB_SUCC(ret)) {
      schema_version = std::max(refreshed_schema_version, gen_schema_version);
      schema_version = std::max(schema_version + SCHEMA_VERSION_INC_STEP,
          ObTimeUtility::current_time());
      /* format version */
      schema_version /= SCHEMA_VERSION_INC_STEP;
      schema_version *= SCHEMA_VERSION_INC_STEP;
      bool overwrite = true;
      if (OB_FAIL(gen_schema_version_map_.set_refactored(tenant_id, schema_version, overwrite))) {
        LOG_WARN("fail to set gen schema version", K(ret), K(tenant_id), K(schema_version));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_core_table_priorities(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    ObArray<ObTableSchema> &core_schemas)
{
  int ret = OB_SUCCESS;
  ObArray<ObTableSchema *> temp_table_schema_ptrs;
  ObArray<ObTableSchema> temp_table_schemas;
  core_schemas.reset();
  const char *table_name = NULL;
  const uint64_t tenant_id = schema_status.tenant_id_;
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail", KR(ret), K(schema_status));
  } else if (OB_FAIL(ObSchemaUtils::get_all_table_name(tenant_id,
                                                       table_name,
                                                       schema_service_))) {
    LOG_WARN("fail to get all table name", KR(ret), K(schema_status));
  } else {
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    bool check_sys_variable = false;  // to avoid cyclic dependence
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_PARAMETER(sql_client, snapshot_timestamp, check_sys_variable);
    ObCoreTableProxy core_kv(table_name, sql_client_retry_weak, tenant_id);
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(core_kv.load())) {
      LOG_WARN("core_kv load failed", KR(ret), K(schema_status));
    } else {
      ObTableSchema core_schema;
      while (OB_SUCC(ret)) {
        const ObCoreTableProxy::Row *priority_row = NULL;
        if (OB_FAIL(core_kv.next())) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
            break;
          } else {
            LOG_WARN("core_kv next failed", KR(ret), K(schema_status));
          }
        } else if (OB_FAIL(core_kv.get_cur_row(priority_row))) {
          LOG_WARN("get current row failed", KR(ret), K(schema_status));
        } else if (NULL == priority_row) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("NULL row", KR(ret), K(schema_status));
        } else {
          core_schema.reset();
          const bool check_deleted = false;
          bool is_deleted = false;
          if (OB_FAIL(ObSchemaRetrieveUtils::fill_table_schema(
              tenant_id, check_deleted, *priority_row, core_schema, is_deleted))) {
            LOG_WARN("fill_table_schema failed", KR(ret), K(schema_status), K(check_deleted), KPC(priority_row));
          } else if (is_deleted) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("core table don't read history table, "
                     "impossible to have is_deleted set", KR(ret), K(schema_status));
          } else if (OB_ALL_CORE_TABLE_TID == core_schema.get_table_id()) {
            // __all_core_table's schema in inner_table only used for sys views, won't be used by schema.
          } else if (OB_FAIL(temp_table_schemas.push_back(core_schema))) {
            LOG_WARN("push_back failed", KR(ret), K(schema_status));
          }
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < temp_table_schemas.count(); ++i) {
        if (OB_FAIL(temp_table_schema_ptrs.push_back(&temp_table_schemas.at(i)))) {
          LOG_WARN("fail to push back table", KR(ret), K(schema_status), K(i));
        }
      }
      if (OB_SUCC(ret)) {
        std::sort(temp_table_schema_ptrs.begin(), temp_table_schema_ptrs.end(), cmp_table_id);
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < temp_table_schema_ptrs.count(); ++i) {
        if (OB_FAIL(core_schemas.push_back(*(temp_table_schema_ptrs.at(i))))) {
          LOG_WARN("fail to push back table", KR(ret), K(schema_status), K(i));
        }
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_core_table_columns(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    ObArray<ObTableSchema> &core_schemas)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  bool check_sys_variable = false;  // to avoid cyclic dependence
  DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_PARAMETER(sql_client, snapshot_timestamp, check_sys_variable);
  ObCoreTableProxy core_kv(OB_ALL_COLUMN_TNAME, sql_client_retry_weak, tenant_id);
  if (OB_FAIL(core_kv.load())) {
    LOG_WARN("core_kv load failed", KR(ret), K(schema_status));
  } else {
    ObColumnSchemaV2 column_schema;
    while (OB_SUCC(ret)) {
      const ObCoreTableProxy::Row *column_row = NULL;
      if (OB_FAIL(core_kv.next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          break;
        } else {
          LOG_WARN("core_kv next failed", KR(ret), K(schema_status));
        }
      } else if (OB_FAIL(core_kv.get_cur_row(column_row))) {
        LOG_WARN("get current row failed", KR(ret), K(schema_status));
      } else if (NULL == column_row) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("NULL row", KR(ret), K(schema_status));
      } else {
        const bool check_deleted = false;
        bool is_deleted = false;
        if (OB_FAIL(ObSchemaRetrieveUtils::fill_column_schema(
                    tenant_id, check_deleted, *column_row, column_schema, is_deleted))) {
          LOG_WARN("fill_column_schema failed", KR(ret),
                   K(schema_status), K(check_deleted), KPC(column_row));
        } else if (is_deleted) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("core table don't read history table, "
                   "impossible to have is_deleted set", KR(ret), K(schema_status));
        } else {
          FOREACH_CNT_X(core_schema, core_schemas, OB_SUCCESS == ret) {
            if (column_schema.get_table_id() == core_schema->get_table_id()) {
              if (OB_FAIL(core_schema->add_column(column_schema))) {
                LOG_WARN("push_back failed", KR(ret), K(schema_status));
              }
              break;
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      FOREACH_CNT_X(core_schema, core_schemas, OB_SUCCESS == ret) {
        if (core_schema->get_column_count() <= 0) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("not column of table exists", KR(ret), KPC(core_schema));
        }
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_not_core_table_schemas(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version, const ObArray<uint64_t> &table_ids,
    ObISQLClient &sql_client, ObIAllocator &allocator,
    ObArray<ObTableSchema *> &not_core_schemas)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail, ", K(ret));
  } else if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid schema_version", K(schema_version), K(ret));
  } else {
    // split query to tenant space && split big query
    int64_t begin = 0;
    int64_t end = 0;
    while (OB_SUCCESS == ret && end < table_ids.count()) {
      while (OB_SUCCESS == ret && end < table_ids.count()
             && end - begin < MAX_IN_QUERY_PER_TIME) {
        end++;
      }
      if (OB_FAIL(fetch_all_table_info(schema_status, schema_version, tenant_id, sql_client, allocator,
                                       not_core_schemas, &table_ids.at(begin), end - begin))) {
        LOG_WARN("fetch all table info failed", K(schema_version), K(schema_status), K(tenant_id), K(ret));
      } else if (OB_FAIL(fetch_all_column_info(schema_status, schema_version, tenant_id, sql_client,
                                               not_core_schemas, &table_ids.at(begin), end - begin))) {
        LOG_WARN("fetch all column info failed", K(schema_version), K(schema_status), K(tenant_id), K(ret));
      } else if (OB_FAIL(fetch_all_partition_info(schema_status, schema_version, tenant_id, sql_client,
                                                  not_core_schemas, &table_ids.at(begin), end - begin))) {
        LOG_WARN("Failed to fetch all partition info", K(ret), K(schema_version), K(schema_status), K(tenant_id));
      } else if (OB_FAIL(fetch_all_constraint_info_ignore_inner_table(schema_status, schema_version, tenant_id,
                 sql_client, not_core_schemas, &table_ids.at(begin), end - begin))) {
        LOG_WARN("fetch all constraints info failed", K(schema_version), K(schema_status), K(tenant_id), K(ret));
        // For liboblog compatibility, we should ingore error when table is not exist.
        if (-ER_NO_SUCH_TABLE == ret && ObSchemaService::g_liboblog_mode_) {
          LOG_WARN("liboblog mode, ignore all constraint info schema table NOT EXIST error",
              K(ret), K(schema_version), K(schema_status));
        }
      }
      begin = end;
    }
    if (FAILEDx(sort_tables_partition_info(not_core_schemas))) {
      LOG_WARN("fail to sort tables partition info", KR(ret), K(tenant_id));
    }
    if (OB_SUCC(ret)) {
      int64_t tenant_id = OB_INVALID_TENANT_ID;
      if (not_core_schemas.count() <= 0) {
        /*do nothing*/
      } else if (FALSE_IT(tenant_id = not_core_schemas.at(0)->get_tenant_id())) {
      } else if (OB_FAIL(fetch_all_encrypt_info(schema_status, schema_version, tenant_id, sql_client,
                 not_core_schemas, not_core_schemas.count()))) {
        LOG_WARN("fail to fetch all encrypt info", K(ret));
      }
    }
  }
  return ret;
}

/*
int ObSchemaServiceSQLImpl::insert_sys_param(
    const ObSysParam &sys_param,
    common::ObISQLClient *sql_client)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql_string;
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail, ", K(ret));
  } else if (OB_ISNULL(sql_client)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("sql_client is NULL, ", K(ret));
  } else if (!sys_param.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("sys param is invalid", K(ret));
  } else if (OB_SUCCESS
           != (ret =
               sql_string.append_fmt(
                   "INSERT INTO %s (TENANT_ID, ZONE, NAME, DATA_TYPE, VALUE, MIN_VAL, MAX_VAL, INFO, FLAGS, gmt_modified) "
                   "values (%lu, '%s', '%s', %ld, '%s', '%s', '%s', '%s', %ld, now(6))",
                   OB_ALL_SYS_VARIABLE_TNAME,
                   sys_param.tenant_id_,
                   sys_param.zone_.ptr(),
                   sys_param.name_,
                   sys_param.data_type_,
                   sys_param.value_,
                   sys_param.min_val_,
                   sys_param.max_val_,
                   sys_param.info_,
                   sys_param.flags_))) {
    LOG_WARN("sql string append format string failed, ", K(ret));
  } else if (OB_FAIL(sql_client->write(sql_string.ptr(), affected_rows))) {
    LOG_WARN("execute sql failed,", "sql", sql_string.ptr(), K(ret));
  } else if (1 != affected_rows) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("affected_rows expect to 1, ", K(affected_rows), K(ret));
  }
  return ret;
}
*/

int ObSchemaServiceSQLImpl::get_core_version(
    common::ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    int64_t &core_schema_version)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    core_schema_version = 0;
    const uint64_t tenant_id = schema_status.tenant_id_;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    bool check_sys_variable = false;  // to avoid cyclic dependence
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_PARAMETER(sql_client, snapshot_timestamp, check_sys_variable);
    ObGlobalStatProxy proxy(sql_client_retry_weak, tenant_id);
    if (OB_FAIL(proxy.get_core_schema_version(core_schema_version))) {
      LOG_WARN("get_core_schema_version failed", K(ret));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_baseline_schema_version(
    common::ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    int64_t &baseline_schema_version)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret), K(schema_status));
  } else {
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    bool check_sys_variable = false;
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_PARAMETER(sql_client, snapshot_timestamp, check_sys_variable);
    ObGlobalStatProxy proxy(sql_client_retry_weak, tenant_id);
    if (OB_FAIL(proxy.get_baseline_schema_version(baseline_schema_version))) {
      LOG_WARN("get_baseline_schema_version failed", KR(ret), K(schema_status));
    }
  }
  return ret;
}

// for ddl, using strong read
int ObSchemaServiceSQLImpl::get_table_schema_from_inner_table(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t table_id,
    ObISQLClient &sql_client,
    ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  table_schema.reset();
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail", KR(ret), K(schema_status));
  } else {
    if (is_core_table(table_id)) {
      ObArray<ObTableSchema> core_schemas;
      if (OB_FAIL(get_core_table_schemas(sql_client, schema_status, core_schemas))) {
        LOG_WARN("get core table schemas failed", KR(ret), K(schema_status));
      } else {
        const ObTableSchema *dst_schema = NULL;
        FOREACH_CNT_X(core_schema, core_schemas, OB_SUCCESS == ret) {
          if (table_id == core_schema->get_table_id()) {
            dst_schema = core_schema;
            break;
          }
        }
        if (NULL == dst_schema) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("no row", K(table_id), KR(ret));
        } else if (OB_FAIL(table_schema.assign(*dst_schema))){
          LOG_WARN("fail to assign schema", KR(ret), K(schema_status));
        }
      }
    } else {
      // set schema_version to get newest table_schema
      int64_t schema_version = INT64_MAX - 1;
      ObArray<uint64_t> table_ids;
      ObArray<ObTableSchema *> tables;
      ObArenaAllocator allocator(ObModIds::OB_SCHEMA);
      if (OB_FAIL(table_ids.push_back(table_id))) {
        LOG_WARN("push table_id to array failed", KR(ret), K(schema_status));
      } else if (OB_FAIL(get_batch_table_schema(schema_status, schema_version,
                                                table_ids, sql_client, allocator, tables))) {
        LOG_WARN("get table schema failed", KR(ret), K(schema_version), K(table_id), K(schema_status));
      } else if (tables.count() <= 0) {
        ret = OB_TABLE_NOT_EXIST;
        LOG_WARN("table array should not be empty", KR(ret), K(schema_status));
      } else if (OB_FAIL(table_schema.assign(*tables.at(0)))){
        LOG_WARN("fail to assign schema", KR(ret), K(schema_status));
      }
    }
  }
  return ret;
}

// get mock fk parent table schema of a single mock fk parent table
int ObSchemaServiceSQLImpl::get_mock_fk_parent_table_schema_from_inner_table(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t table_id,
    common::ObISQLClient &sql_client,
    ObMockFKParentTableSchema &mock_fk_parent_table_schema)
{
  int ret = OB_SUCCESS;
  mock_fk_parent_table_schema.reset();
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail", KR(ret), K(schema_status));
  } else {
    // set schema_version to get newest table_schema
    int64_t schema_version = INT64_MAX - 1;
    ObArray<uint64_t> mock_fk_parent_table_ids;
    ObArray<ObMockFKParentTableSchema> mock_fk_parent_tables;
    if (OB_FAIL(mock_fk_parent_table_ids.push_back(table_id))) {
      LOG_WARN("push table_id to array failed", KR(ret), K(schema_status));
    } else if (OB_FAIL(get_batch_mock_fk_parent_tables(schema_status, schema_version,
        mock_fk_parent_table_ids, sql_client, mock_fk_parent_tables))) {
      LOG_WARN("get table schema failed", KR(ret), K(schema_version), K(mock_fk_parent_table_ids), K(schema_status));
    } else if (mock_fk_parent_tables.count() <= 0) {
      ret = OB_TABLE_NOT_EXIST;
      LOG_WARN("table array should not be empty", KR(ret), K(schema_status));
    } else if (OB_FAIL(mock_fk_parent_table_schema.assign(mock_fk_parent_tables.at(0)))){
      LOG_WARN("fail to assign schema", KR(ret), K(schema_status));
    }
  }
  return ret;
}

#define FETCH_ALL_TENANT_HISTORY_SQL3           "SELECT * from %s where 1 = 1"
#define FETCH_ALL_TABLE_HISTORY_SQL3            COMMON_SQL_WITH_TENANT
#define FETCH_ALL_TABLE_HISTORY_FULL_SCHEMA     "SELECT /*+ leading(b a) use_nl(b a) %s*/ a.* FROM %s AS a JOIN "\
                                                "(SELECT tenant_id, table_id, MAX(schema_version) AS schema_version FROM %s "\
                                                "WHERE tenant_id = %lu AND schema_version <= %ld GROUP BY tenant_id, table_id) AS b "\
                                                "ON a.tenant_id = b.tenant_id AND a.table_id = b.table_id AND a.schema_version = b.schema_version "\
                                                "WHERE a.is_deleted = 0 and a.table_id != %lu"

int ObSchemaServiceSQLImpl::get_all_tenants(ObISQLClient &client,
                                            const int64_t schema_version,
                                            ObIArray<ObSimpleTenantSchema> &schema_array)
{
  int ret = OB_SUCCESS;
  schema_array.reset();

  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  } else if (OB_FAIL(fetch_tenants(client, schema_version, schema_array))) {
    LOG_WARN("fetch tenants failed", K(ret));
  }

  return ret;
}

int ObSchemaServiceSQLImpl::get_sys_variable(
    ObISQLClient &client,
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const int64_t schema_version,
    ObSimpleSysVariableSchema &sys_variable)
{
  int ret = OB_SUCCESS;
  int64_t fetch_version = OB_INVALID_VERSION;
  sys_variable.reset();
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail", K(ret));
  } else if (OB_FAIL(fetch_sys_variable_version(client, schema_status, tenant_id, schema_version, fetch_version))) {
    LOG_WARN("fetch sys variable version failed", K(ret));
  } else if (OB_FAIL(fetch_sys_variable(client, schema_status, tenant_id, fetch_version, sys_variable))) {
    LOG_WARN("fail to fetch sys variable", K(ret), K(tenant_id), K(schema_version));
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_sys_variable(
    ObISQLClient &client,
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const int64_t schema_version,
    ObSimpleSysVariableSchema &sys_variable)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator(ObModIds::OB_TEMP_VARIABLES);
  sys_variable.reset();
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail", K(ret));
  } else if (OB_INVALID_TENANT_ID == tenant_id
             || OB_INVALID_VERSION == schema_version) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id), K(schema_version));
  } else {
    ObObj var_lower_case;
    int64_t var_value = OB_INVALID_ID;
    ObString lower_case_name(OB_SV_LOWER_CASE_TABLE_NAMES);
    if (OB_FAIL(get_tenant_system_variable(schema_status, tenant_id, allocator,
        client, schema_version, lower_case_name, var_lower_case))) {
      //TODO:(here should handle deleted tenant, just let it go temporarily for xiuming'test)
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (is_sys_tenant(sys_variable.get_tenant_id())
            || is_meta_tenant(sys_variable.get_tenant_id())) {
          sys_variable.set_name_case_mode(OB_ORIGIN_AND_INSENSITIVE);
        } else {
          sys_variable.set_name_case_mode(OB_LOWERCASE_AND_INSENSITIVE);
        }
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("failed to get_tenant_system_variable", K(lower_case_name), K(ret));
      }
    } else if (OB_FAIL(var_lower_case.get_int(var_value))) {
      LOG_WARN("failed to get int", K(var_lower_case), K(ret));
    } else if (var_value <= OB_NAME_CASE_INVALID || var_value >= OB_NAME_CASE_MAX) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid var value", K(var_value), K(ret));
    } else {
      ObNameCaseMode case_mode = OB_NAME_CASE_INVALID;
      case_mode = static_cast<ObNameCaseMode>(var_value);
      sys_variable.set_name_case_mode(case_mode);
    }
    if (OB_SUCC(ret)) {
      ObString read_only_name(OB_SV_READ_ONLY);
      ObObj var_read_only;
      if (OB_FAIL(get_tenant_system_variable(schema_status, tenant_id, allocator,
          client, schema_version, read_only_name, var_read_only))) {
        if (OB_ENTRY_NOT_EXIST == ret) {
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("failed to get tenant system variable", K(ret), K(read_only_name));
        }
      } else if (OB_FAIL(var_read_only.get_int(var_value))) {
        LOG_WARN("failed to get int", K(ret), K(var_read_only));
      } else {
        sys_variable.set_read_only(0 != var_value);
      }
    }
    if (OB_SUCC(ret)) {
      sys_variable.set_tenant_id(tenant_id);
      sys_variable.set_schema_version(schema_version);
    }
  }
  return ret;
}

//can only use to get variables of non-datetime type
int ObSchemaServiceSQLImpl::get_tenant_system_variable(const ObRefreshSchemaStatus &schema_status,
                                                       uint64_t tenant_id,
                                                       ObIAllocator &allocator,
                                                       ObISQLClient &sql_client,
                                                       int64_t schema_version,
                                                       ObString &var_name,
                                                       ObObj &out_var_obj)
{
  int ret = OB_SUCCESS;
  bool try_sys_variable = false;
  sqlclient::ObMySQLResult *result = NULL;
  ObSqlString sql;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  bool check_sys_variable = false;
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_PARAMETER(sql_client, snapshot_timestamp, check_sys_variable);
  if (OB_INVALID_ID == tenant_id || var_name.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id or var_name", K(tenant_id), K(var_name), K(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      if (OB_FAIL(sql.assign_fmt("SELECT data_type, value, is_deleted"
                                 " FROM %s where tenant_id = %lu and name = '%.*s' and schema_version <= %ld",
                                 OB_ALL_SYS_VARIABLE_HISTORY_TNAME,
                                 fill_extract_tenant_id(schema_status, tenant_id),
                                 var_name.length(), var_name.ptr(), schema_version))) {
        LOG_WARN("append sql failed", K(tenant_id), K(var_name), K(ret));
      } else if (OB_FAIL(sql.append(" order by schema_version desc;"))) {
        LOG_WARN("append sql failed", K(tenant_id), K(var_name), K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        // for liboblog compatibility
        if (ret == -1146 && ObSchemaService::g_liboblog_mode_) {
          ret = OB_SUCCESS;
          try_sys_variable = true;
        } else {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        }
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get result.", K(tenant_id), K(var_name), K(ret));
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret && ObSchemaService::g_liboblog_mode_) {
          ret = OB_SUCCESS;
          try_sys_variable = true;
        } else {
          LOG_WARN("fail to get system variable", K(tenant_id), K(var_name), K(ret));
        }
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_system_variable_obj(tenant_id, *result, allocator, out_var_obj))) {
        LOG_WARN("fail to retrieve system variable obj", K(ret), K(tenant_id), K(var_name));
      }
    }
  }

  if (OB_SUCC(ret) && try_sys_variable) {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sql.reuse();
      // While cluster is in upgradation, __all_sys_variable_history may not be modified yet,
      // so we try to use __all_sys_variable to fetch system variable schema.
      if (OB_FAIL(sql.assign_fmt("SELECT data_type, value, 0 as is_deleted FROM %s where tenant_id = %lu and name = '%.*s';",
                                OB_ALL_SYS_VARIABLE_TNAME,
                                fill_extract_tenant_id(schema_status, tenant_id),
                                var_name.length(), var_name.ptr()))) {
        LOG_WARN("append sql failed", K(tenant_id), K(var_name), K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get result.", K(tenant_id), K(var_name), K(ret));
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_ENTRY_NOT_EXIST;
        } else {
          LOG_WARN("fail to get system variable", K(tenant_id), K(var_name), K(ret));
        }
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_system_variable_obj(tenant_id, *result, allocator, out_var_obj))) {
        LOG_WARN("fail to retrieve system variable obj", K(ret), K(tenant_id), K(var_name));
      }
    }
  }
  return ret;
}

#define GET_ALL_SCHEMA_FUNC_DEFINE(SCHEMA, SCHEMA_TYPE) \
  int ObSchemaServiceSQLImpl::get_all_##SCHEMA##s(ObISQLClient &client, \
                                                  const ObRefreshSchemaStatus &schema_status, \
                                                  const int64_t schema_version, \
                                                  const uint64_t tenant_id, \
                                                  ObIArray<SCHEMA_TYPE> &schema_array) \
  {                                                         \
    int ret = OB_SUCCESS;                                   \
    schema_array.reset();                                   \
    if (!check_inner_stat()) {                              \
      ret = OB_NOT_INIT;                                    \
      LOG_WARN("check inner stat fail");                    \
    } else if (OB_FAIL(fetch_##SCHEMA##s(client, schema_status, schema_version, tenant_id, schema_array))) { \
      LOG_WARN("fetch "#SCHEMA"s failed", K(ret), K(schema_status), K(schema_version), K(tenant_id)); \
      /* for liboblog compatibility */ \
      if (-ER_NO_SUCH_TABLE == ret && ObSchemaService::g_liboblog_mode_) { \
        LOG_WARN("liboblog mode, ignore "#SCHEMA" schema table NOT EXIST error", K(ret), K(schema_status), K(schema_version), K(tenant_id)); \
        ret = OB_SUCCESS;                                   \
      }                                                     \
    }                                                       \
    return ret;                                             \
  }

GET_ALL_SCHEMA_FUNC_DEFINE(user, ObSimpleUserSchema);
GET_ALL_SCHEMA_FUNC_DEFINE(database, ObSimpleDatabaseSchema);
GET_ALL_SCHEMA_FUNC_DEFINE(tablegroup, ObSimpleTablegroupSchema);
GET_ALL_SCHEMA_FUNC_DEFINE(table, ObSimpleTableSchemaV2);
GET_ALL_SCHEMA_FUNC_DEFINE(outline, ObSimpleOutlineSchema);
GET_ALL_SCHEMA_FUNC_DEFINE(synonym, ObSimpleSynonymSchema);
GET_ALL_SCHEMA_FUNC_DEFINE(routine, ObSimpleRoutineSchema);
GET_ALL_SCHEMA_FUNC_DEFINE(package, ObSimplePackageSchema);
GET_ALL_SCHEMA_FUNC_DEFINE(udf, ObSimpleUDFSchema);
GET_ALL_SCHEMA_FUNC_DEFINE(udt, ObSimpleUDTSchema);
GET_ALL_SCHEMA_FUNC_DEFINE(sequence, ObSequenceSchema);
GET_ALL_SCHEMA_FUNC_DEFINE(dblink, ObDbLinkSchema);
GET_ALL_SCHEMA_FUNC_DEFINE(context, ObContextSchema);
GET_ALL_SCHEMA_FUNC_DEFINE(mock_fk_parent_table, ObSimpleMockFKParentTableSchema);

#define GET_ALL_SCHEMA_IGNORE_FAIL_FUNC_DEFINE(SCHEMA, SCHEMA_TYPE, CLUSTER_VERSION) \
  int ObSchemaServiceSQLImpl::get_all_##SCHEMA##s(ObISQLClient &client, \
                                                  const ObRefreshSchemaStatus &schema_status, \
                                                  const int64_t schema_version, \
                                                  const uint64_t tenant_id, \
                                                  ObIArray<SCHEMA_TYPE> &schema_array) \
  {                                                         \
    int ret = OB_SUCCESS;                                   \
    schema_array.reset();                                   \
    if (!check_inner_stat()) {                              \
      ret = OB_NOT_INIT;                                    \
      LOG_WARN("check inner stat fail");                    \
    } else if (!ObSchemaService::g_liboblog_mode_           \
               && GCONF.in_upgrade_mode()                 \
               && GET_MIN_CLUSTER_VERSION() < CLUSTER_VERSION) { \
      /* system table in tenant space will be created in upgrade post stage after ver 2.2.1.*/ \
      /* To avoid error while restart observer in upgrade stage, we skip refresh of new schema */ \
      /* when cluster is still in upgrade stage. */ \
      /* In addition, new schema history can be recorded after upgradation. */ \
      LOG_INFO("ignore "#SCHEMA" schema table NOT EXIST error while upgrade", K(ret), K(schema_status), K(schema_version), K(tenant_id)); \
    } else if (OB_FAIL(fetch_##SCHEMA##s(client, schema_status, schema_version, tenant_id, schema_array))) { \
      LOG_WARN("fetch "#SCHEMA"s failed", K(ret), K(schema_status), K(schema_version), K(tenant_id)); \
      /* for liboblog compatibility */ \
      if (-ER_NO_SUCH_TABLE == ret && ObSchemaService::g_liboblog_mode_) { \
        LOG_WARN("liboblog mode, ignore "#SCHEMA" schema table NOT EXIST error", K(ret), K(schema_status), K(schema_version), K(tenant_id)); \
        ret = OB_SUCCESS;                                   \
      }                                                     \
    }                                                       \
    return ret;                                             \
  }
GET_ALL_SCHEMA_IGNORE_FAIL_FUNC_DEFINE(keystore, ObKeystoreSchema, CLUSTER_VERSION_2220);
GET_ALL_SCHEMA_IGNORE_FAIL_FUNC_DEFINE(tablespace, ObTablespaceSchema, CLUSTER_VERSION_2220);
GET_ALL_SCHEMA_IGNORE_FAIL_FUNC_DEFINE(trigger, ObSimpleTriggerSchema, CLUSTER_VERSION_2220);
GET_ALL_SCHEMA_IGNORE_FAIL_FUNC_DEFINE(label_se_policy, ObLabelSePolicySchema, CLUSTER_VERSION_2230);
GET_ALL_SCHEMA_IGNORE_FAIL_FUNC_DEFINE(label_se_component, ObLabelSeComponentSchema, CLUSTER_VERSION_2230);
GET_ALL_SCHEMA_IGNORE_FAIL_FUNC_DEFINE(label_se_label, ObLabelSeLabelSchema, CLUSTER_VERSION_2230);
GET_ALL_SCHEMA_IGNORE_FAIL_FUNC_DEFINE(label_se_user_level, ObLabelSeUserLevelSchema, CLUSTER_VERSION_2230);
GET_ALL_SCHEMA_IGNORE_FAIL_FUNC_DEFINE(profile, ObProfileSchema, CLUSTER_VERSION_2230);
GET_ALL_SCHEMA_IGNORE_FAIL_FUNC_DEFINE(audit, ObSAuditSchema, CLUSTER_VERSION_2230);
GET_ALL_SCHEMA_IGNORE_FAIL_FUNC_DEFINE(sys_priv, ObSysPriv, CLUSTER_VERSION_2250);
GET_ALL_SCHEMA_IGNORE_FAIL_FUNC_DEFINE(obj_priv, ObObjPriv, CLUSTER_VERSION_2260);
GET_ALL_SCHEMA_IGNORE_FAIL_FUNC_DEFINE(directory, ObDirectorySchema, CLUSTER_VERSION_4_0_0_0); // TODO: change version to 3200

int ObSchemaServiceSQLImpl::get_all_db_privs(ObISQLClient &client,
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObIArray<ObDBPriv> &schema_array) {
  int ret = OB_SUCCESS;
  schema_array.reset();

  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  } else if (OB_FAIL(fetch_db_privs(client, schema_status, schema_version, tenant_id, schema_array))) {
    LOG_WARN("fetch db_privs failed", K(ret));
  }

  return ret;
}

int ObSchemaServiceSQLImpl::get_all_table_privs(ObISQLClient &client,
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObIArray<ObTablePriv> &schema_array)
{
  int ret = OB_SUCCESS;
  schema_array.reset();

  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  } else if (OB_FAIL(fetch_table_privs(client, schema_status, schema_version, tenant_id, schema_array))) {
    LOG_WARN("fetch tenants failed", K(ret));
  }

  return ret;
}

int ObSchemaServiceSQLImpl::get_sys_variable_schema(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const int64_t schema_version,
    ObSysVariableSchema &sys_variable_schema)
{
  int ret = OB_SUCCESS;
  bool try_sys_variable = false;
  ObSqlString sql;
  ObMySQLResult *result = NULL;
  int64_t fetch_version = OB_INVALID_VERSION;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  bool check_sys_variable = false;
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_PARAMETER(sql_client, snapshot_timestamp, check_sys_variable);
  if (OB_FAIL(fetch_sys_variable_version(sql_client, schema_status, tenant_id, schema_version, fetch_version))) {
    LOG_WARN("fail to fetch simple sys variable version", K(ret), K(tenant_id), K(schema_version));
  } else if (OB_INVALID_VERSION == fetch_version) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid version", K(ret), K(tenant_id), K(schema_version), K(fetch_version));
  }
  if (OB_SUCC(ret)) {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      if (OB_FAIL(sql.append_fmt(FETCH_ALL_SYS_VARIABLE_HISTORY_SQL,
                                 OB_ALL_SYS_VARIABLE_HISTORY_TNAME,
                                 fill_extract_tenant_id(schema_status, tenant_id),
                                 fetch_version))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY ZONE DESC, NAME DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        if (ret == -ER_NO_SUCH_TABLE && ObSchemaService::g_liboblog_mode_) {
          ret = OB_SUCCESS;
          try_sys_variable = true;
        } else {
          LOG_WARN("read all system variable failed", K(ret));
        }
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_system_variable(tenant_id, *result, sys_variable_schema))) {
        LOG_WARN("retrieve tenant global system variable failed", K(ret));
      } else if (sys_variable_schema.get_real_sysvar_count() <= 0 && ObSchemaService::g_liboblog_mode_) {
        try_sys_variable = true;
      }
    }
  }

  if (OB_SUCC(ret) && try_sys_variable) {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sql.reuse();
      // While cluster is in upgradation, __all_sys_variable_history may not be modified yet,
      // so we try to use __all_sys_variable to fetch system variable schema.
      LOG_INFO("__all_sys_variable_history is empty, get system variable from __all_sys_variable");
      if (OB_FAIL(sql.append_fmt("select *, 0 as is_deleted, 0 as schema_version from %s where tenant_id=%lu",
                                 OB_ALL_SYS_VARIABLE_TNAME,
                                 fill_extract_tenant_id(schema_status, tenant_id)))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("read all system variable failed", K(ret));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_system_variable(tenant_id, *result, sys_variable_schema))) {
        LOG_WARN("retrieve tenant global system variable failed", K(ret));
      }
    }
  }

  if (OB_SUCC(ret)) {
    // To avoid -5044 error, mock missed system variable schema with default value by hardcoded schema.
    for (int64_t i = 0; OB_SUCC(ret) && i < ObSysVariables::get_amount(); i++) {
      ObSysVarClassType sys_var_id = ObSysVariables::get_sys_var_id(i);
      const ObSysVarSchema *sys_var = NULL;
      if (OB_FAIL(sys_variable_schema.get_sysvar_schema(sys_var_id, sys_var))) {
        if (OB_ERR_SYS_VARIABLE_UNKNOWN != ret) {
          LOG_WARN("fail to get sys_var", KR(ret), K(schema_status),
                   K(tenant_id), K(schema_version));
        } else { // overwrite ret
          // sys_var not exist, need mock
          ObSysParam sys_param;
          ObSysVarSchema sys_var_schema;
          sys_param.init(tenant_id,
                          "",
                          ObSysVariables::get_name(i),
                          ObSysVariables::get_type(i),
                          ObSysVariables::get_value(i),
                          ObSysVariables::get_min(i),
                          ObSysVariables::get_max(i),
                          ObSysVariables::get_info(i),
                          ObSysVariables::get_flags(i));
          if (OB_FAIL(ObSchemaUtils::convert_sys_param_to_sysvar_schema(sys_param, sys_var_schema))) {
            LOG_WARN("convert to sysvar schema failed", KR(ret), K(tenant_id), K(sys_var_id));
          } else if (OB_FAIL(sys_variable_schema.add_sysvar_schema(sys_var_schema))) {
            LOG_WARN("fail to add sys var schema", KR(ret), K(tenant_id), K(sys_var_id));
          } else {
            LOG_INFO("sys var miss, maybe in upgrade/physical restore stage",
                     K(tenant_id), K(sys_param));
          }
        }
      } else if (OB_ISNULL(sys_var)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("sys_var is null", KR(ret), K(sys_var_id),
                 K(schema_status), K(tenant_id), K(schema_version));
      } else {
        // sys_var exist, no need to deal with it
      }
    }
  }

  if (OB_SUCC(ret)) {
    sys_variable_schema.set_tenant_id(tenant_id);
    sys_variable_schema.set_schema_version(fetch_version);
  }

  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_column_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObArray<ObTableSchema *> &table_schema_array,
    const uint64_t *table_ids /* = NULL */,
    const int64_t table_ids_size /*= 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_INVALID_TENANT_ID == tenant_id) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid tenant_id", K(ret), K(tenant_id));
    } else if (INT64_MAX == schema_version) {
      if (OB_FAIL(sql.append_fmt(FETCH_ALL_COLUMN_SQL, OB_ALL_COLUMN_TNAME,
                                 fill_extract_tenant_id(schema_status, tenant_id)))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        if (NULL != table_ids && table_ids_size > 0) {
          if (OB_FAIL(sql.append_fmt(" AND table_id IN "))) {
            LOG_WARN("append sql failed", K(ret));
          } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
            LOG_WARN("sql append table ids failed");
          }
        }
      }
    } else {
      if (OB_FAIL(sql.append_fmt(FETCH_ALL_COLUMN_HISTORY_SQL, OB_ALL_COLUMN_HISTORY_TNAME,
                                 fill_extract_tenant_id(schema_status, tenant_id)))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        if (NULL != table_ids && table_ids_size > 0) {
          if (OB_FAIL(sql.append_fmt(" AND table_id IN "))) {
            LOG_WARN("append sql failed", K(ret));
          } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
            LOG_WARN("sql append table ids failed");
          }
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
          LOG_WARN("append sql failed", K(ret));
        } else if (OB_FAIL(sql.append_fmt(" ORDER BY TENANT_ID, TABLE_ID, COLUMN_ID, SCHEMA_VERSION"))) {
          LOG_WARN("append sql failed", K(ret));
        }
      }
    }

    if (OB_SUCC(ret)) {
      const bool check_deleted = (INT64_MAX != schema_version);
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(sql), K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_column_schema(
                         tenant_id, check_deleted, *result, table_schema_array))) {
        LOG_WARN("failed to retrieve all column schema", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_constraint_info_ignore_inner_table(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObArray<ObTableSchema *> &table_schema_array,
    const uint64_t *table_ids /* = NULL */,
    const int64_t table_ids_size /*= 0 */)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_ids)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Table ids is NULL", K(ret));
  } else if (OB_INVALID_TENANT_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", K(ret), K(tenant_id));
  } else {
    ObSEArray<uint64_t, 16> non_inner_tables;
    ObTableSchema *table_schema = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && i < table_ids_size; ++i) {
      uint64_t table_id = table_ids[i];
      if (OB_ISNULL(table_schema = ObSchemaRetrieveUtils::find_table_schema(table_id,
                                                                            table_schema_array))) {
        //ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Failed to find table schema", K(ret), K(table_id));
        // for compatibility
        continue;
      } else if (is_inner_table(table_id)) {
        // To avoid cyclc dependence, inner table should not contain any constraint.
        continue;
      } else {
        ret = non_inner_tables.push_back(table_id);
      }
    }//end of for

    //fetch constraint info
    if (OB_FAIL(ret)) {
    } else if (non_inner_tables.count() <= 0) {
    } else if (OB_FAIL(fetch_all_constraint_info(schema_status, schema_version, tenant_id,
                                                 sql_client, table_schema_array,
                                                 &non_inner_tables.at(0),
                                                 non_inner_tables.count()))) {
      LOG_WARN("fetch all constraint info failed", K(tenant_id), K(schema_version), K(ret));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_aux_tables(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const uint64_t table_id,
    const int64_t schema_version,
    ObISQLClient &sql_client,
    ObIArray<ObAuxTableMetaInfo> &aux_tables)
{
  int ret = OB_SUCCESS;
  aux_tables.reset();

  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    ObSqlString hint;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    const static char *FETCH_INDEX_SQL_FORMAT = "SELECT /*+index(%s idx_data_table_id)*/ table_id, table_type, index_type FROM "\
                                                "(SELECT TABLE_ID, SCHEMA_VERSION, TABLE_TYPE, INDEX_TYPE, IS_DELETED, ROW_NUMBER() OVER " \
                                                "(PARTITION BY TABLE_ID ORDER BY SCHEMA_VERSION DESC) AS RN FROM %s " \
                                                "WHERE TENANT_ID = %lu AND DATA_TABLE_ID = %lu AND SCHEMA_VERSION <= %ld) V "\
                                                "WHERE RN = 1 and IS_DELETED = 0 ORDER BY TABLE_ID";
    const char *table_name = NULL;
    if (!check_inner_stat()) {
      ret = OB_NOT_INIT;
      LOG_WARN("check inner stat fail", K(ret));
    } else if (OB_FAIL(ObSchemaUtils::get_all_table_history_name(exec_tenant_id,
                                                                 table_name,
                                                                 schema_service_))) {
      LOG_WARN("fail to get all table name", K(ret), K(exec_tenant_id));
    } else {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql.append_fmt(FETCH_INDEX_SQL_FORMAT,
                                 table_name, table_name,
                                 fill_extract_tenant_id(schema_status, tenant_id),
                                 fill_extract_schema_id(schema_status, table_id),
                                 schema_version))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(sql), K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_aux_tables(tenant_id, *result, aux_tables))) {
        LOG_WARN("failed to retrieve aux tables", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_constraint_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObArray<ObTableSchema *> &table_schema_array,
    const uint64_t *table_ids /* = NULL */,
    const int64_t table_ids_size /*= 0 */)
{
  int ret = OB_SUCCESS;
  ObMySQLResult *result = NULL;
  ObSqlString sql;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);

  if (OB_SUCC(ret)) {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      if (OB_INVALID_TENANT_ID == tenant_id) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid tenant_id", K(ret), K(tenant_id));
      } else if (INT64_MAX == schema_version) {
        if (OB_FAIL(sql.append_fmt(FETCH_ALL_CONSTRAINT_SQL, OB_ALL_CONSTRAINT_TNAME,
                                   fill_extract_tenant_id(schema_status, tenant_id)))) {
          LOG_WARN("append sql failed", K(ret));
        } else {
          if (NULL != table_ids && table_ids_size > 0) {
            if (OB_FAIL(sql.append_fmt(" AND table_id IN "))) {
              LOG_WARN("append sql failed", K(ret));
            } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
              LOG_WARN("sql append table ids failed");
            }
          }
        }
      } else {
        if (OB_FAIL(sql.append_fmt(FETCH_ALL_CONSTRAINT_HISTORY_SQL, OB_ALL_CONSTRAINT_HISTORY_TNAME,
                                   fill_extract_tenant_id(schema_status, tenant_id)))) {
          LOG_WARN("append sql failed", K(ret));
        } else {
          if (NULL != table_ids && table_ids_size > 0) {
            if (OB_FAIL(sql.append_fmt(" AND table_id IN "))) {
              LOG_WARN("append sql failed", K(ret));
            } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
              LOG_WARN("sql append table ids failed");
            }
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
            LOG_WARN("append sql failed", K(ret));
          } else if (OB_FAIL(sql.append_fmt(" ORDER BY TENANT_ID, TABLE_ID, CONSTRAINT_ID, SCHEMA_VERSION"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
      if (OB_SUCC(ret)) {
        const bool check_deleted = (INT64_MAX != schema_version);
        if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fail to get result. ", K(sql), K(ret));
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_constraint(
                           tenant_id, check_deleted, *result, table_schema_array))) {
          LOG_WARN("failed to retrieve all constraint schema", K(ret));
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (NULL != table_ids && table_ids_size > 0) {
      for (int64_t i = 0; OB_SUCC(ret) && (i < table_ids_size); ++i) {
        ObTableSchema *table_schema =
            ObSchemaRetrieveUtils::find_table_schema(table_ids[i], table_schema_array);
        if (OB_ISNULL(table_schema)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fail to get table_schema", K(ret), K(table_ids[i]));
        } else {
          for (ObTableSchema::constraint_iterator iter =
                 table_schema->constraint_begin_for_non_const_iter();
               OB_SUCC(ret) && iter != table_schema->constraint_end_for_non_const_iter();
               ++iter) {
            if (OB_FAIL(fetch_constraint_column_info(
                        schema_status, tenant_id,
                        table_schema->get_table_id(), schema_version,
                        sql_client, *iter))) {
              LOG_WARN("failed to fetch constraint column info", K(ret));
            }
          }
        }
      }
    }
  }
  return ret;
}

/**
 * fetch partition info from __all_part, __all_part_history
 */
template<typename T>
int ObSchemaServiceSQLImpl::fetch_all_part_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObArray<T *> &range_part_tables,
    const uint64_t *table_ids /* = NULL */,
    const int64_t table_ids_size /*= 0 */)
{
  int ret = OB_SUCCESS;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  if (OB_INVALID_TENANT_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", K(ret), K(tenant_id));
  } else if (NULL != table_ids && table_ids_size > 0) {
    ObSqlString sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (INT64_MAX == schema_version) {
        if (OB_FAIL(sql.append_fmt(FETCH_ALL_PART_SQL, OB_ALL_PART_TNAME,
                                   fill_extract_tenant_id(schema_status, tenant_id)))) {
          LOG_WARN("append sql failed", K(ret));
        } else {
          if (NULL != table_ids && table_ids_size > 0) {
            if (OB_FAIL(sql.append_fmt(" AND table_id IN "))) {
              LOG_WARN("append sql faield", K(ret));
            } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
              LOG_WARN("sql append table is failed", K(ret));
            }
          }
        }
      } else {
        if (OB_FAIL(sql.append_fmt(FETCH_ALL_PART_HISTORY_SQL, OB_ALL_PART_HISTORY_TNAME,
                                   fill_extract_tenant_id(schema_status, tenant_id)))) {
          LOG_WARN("append sql failed", K(ret));
        } else {
          if (NULL != table_ids && table_ids_size > 0) {
            if (OB_FAIL(sql.append_fmt(" AND table_id IN "))) {
              LOG_WARN("append sql failed", K(ret));
            } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
              LOG_WARN("sql append table ids failed", K(ret));
            }
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
            LOG_WARN("append sql failed", K(ret));
          } else if (OB_FAIL(sql.append_fmt(" ORDER BY TENANT_ID, TABLE_ID, PART_ID, SCHEMA_VERSION"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
      if (OB_SUCC(ret)) {
        LOG_TRACE("fetch all part history", KR(ret), K(exec_tenant_id), K(sql));
        const bool check_deleted = (INT64_MAX != schema_version);
        DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
        if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fail to get result. ", K(sql), K(ret));
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_part_info(
                           tenant_id, check_deleted, *result, range_part_tables))) {
          LOG_WARN("failed to retrieve all column schema", K(ret));
        } else { }//do nothing
      }
    }
  }
  return ret;
}

/**
 * fetch partition info from __all_def_sub_part, __all_def_sub_part_history
 */
template<typename T>
int ObSchemaServiceSQLImpl::fetch_all_def_subpart_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObArray<T *> &range_subpart_tables,
    const uint64_t *table_ids /* = NULL */,
    const int64_t table_ids_size /*= 0 */)
{
  int ret = OB_SUCCESS;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  if (OB_INVALID_TENANT_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", K(ret), K(tenant_id));
  } else if (range_subpart_tables.count() > 0) {
    ObSqlString sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (INT64_MAX == schema_version) {
        if (OB_FAIL(sql.append_fmt(FETCH_ALL_DEF_SUBPART_SQL, OB_ALL_DEF_SUB_PART_TNAME,
                                   fill_extract_tenant_id(schema_status, tenant_id)))) {
          LOG_WARN("Failed to append all part", K(ret));
        } else {
          if (NULL != table_ids && table_ids_size > 0) {
            if (OB_FAIL(sql.append_fmt(" AND table_id IN "))) {
              LOG_WARN("append sql failed", K(ret));
            } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
              LOG_WARN("sql append table ids failed");
            }
          }
        }
      } else {
        if (OB_FAIL(sql.append_fmt(FETCH_ALL_DEF_SUBPART_HISTORY_SQL, OB_ALL_DEF_SUB_PART_HISTORY_TNAME,
                                   fill_extract_tenant_id(schema_status, tenant_id)))) {
          LOG_WARN("append sql failed", K(ret));
        } else {
          if (NULL != table_ids && table_ids_size > 0) {
            if (OB_FAIL(sql.append_fmt(" AND table_id IN "))) {
              LOG_WARN("append sql failed", K(ret));
            } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
              LOG_WARN("sql append table ids failed");
            }
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append_fmt(" AND schema_version <= %ld"
                                     " ORDER BY tenant_id, table_id, sub_part_id, schema_version",
                                     schema_version))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
      if (OB_SUCC(ret)) {
        LOG_TRACE("fetch all def subpart history", KR(ret), K(exec_tenant_id), K(sql));
        const bool check_deleted = (INT64_MAX != schema_version);
        DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
        if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fail to get result. ", K(sql), K(ret));
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_def_subpart_info(
                           tenant_id, check_deleted, *result, range_subpart_tables))) {
          LOG_WARN("failed to retrieve all column schema", K(ret));
        } else { }//do nothing
      }
    }
  }
  return ret;
}

/**
 * fetch partition info from __all_sub_part, __all_sub_part_history
 */
template<typename T>
int ObSchemaServiceSQLImpl::fetch_all_subpart_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObArray<T *> &range_subpart_tables,
    const uint64_t *table_ids /* = NULL */,
    const int64_t table_ids_size /*= 0 */)
{
  int ret = OB_SUCCESS;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  if (OB_INVALID_TENANT_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", K(ret), K(tenant_id));
  } else if (range_subpart_tables.count() > 0) {
    ObSqlString sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (INT64_MAX == schema_version) {
        if (OB_FAIL(sql.append_fmt(FETCH_ALL_SUB_PART_SQL, OB_ALL_SUB_PART_TNAME,
                                   fill_extract_tenant_id(schema_status, tenant_id)))) {
          LOG_WARN("Failed to append all part", K(ret));
        } else {
          if (NULL != table_ids && table_ids_size > 0) {
            if (OB_FAIL(sql.append_fmt(" AND table_id IN "))) {
              LOG_WARN("append sql failed", K(ret));
            } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
              LOG_WARN("sql append table ids failed");
            }
          }
        }
      } else {
        if (OB_FAIL(sql.append_fmt(FETCH_ALL_SUB_PART_HISTORY_SQL, OB_ALL_SUB_PART_HISTORY_TNAME,
                                   fill_extract_tenant_id(schema_status, tenant_id)))) {
          LOG_WARN("append sql failed", K(ret));
        } else {
          if (NULL != table_ids && table_ids_size > 0) {
            if (OB_FAIL(sql.append_fmt(" AND table_id IN "))) {
              LOG_WARN("append sql failed", K(ret));
            } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
              LOG_WARN("sql append table ids failed");
            }
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append_fmt(" AND schema_version <= %ld"
                                     " ORDER BY tenant_id, table_id, part_id, sub_part_id, schema_version",
                                     schema_version))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
      if (OB_SUCC(ret)) {
        LOG_TRACE("fetch all subpart history", KR(ret), K(exec_tenant_id), K(sql));
        const bool check_deleted = (INT64_MAX != schema_version);
        DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
        if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fail to get result. ", K(sql), K(ret));
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_subpart_info(
                           tenant_id, check_deleted, *result, range_subpart_tables))) {
          LOG_WARN("failed to retrieve all column schema", K(ret));
        } else { }//do nothing
      }
    }
  }
  return ret;
}

template<typename T>
int ObSchemaServiceSQLImpl::gen_batch_fetch_array(
    common::ObArray<T *> &table_schema_array,
    const uint64_t *table_ids /* = NULL */,
    const int64_t table_ids_size /*= 0 */,
    common::ObIArray<uint64_t> &part_tables,
    common::ObIArray<uint64_t> &def_subpart_tables,
    common::ObIArray<uint64_t> &subpart_tables,
    common::ObIArray<int64_t> &part_idxs,
    common::ObIArray<int64_t> &def_subpart_idxs,
    common::ObIArray<int64_t> &subpart_idxs)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_ids)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Table ids is NULL", K(ret));
  } else {
    int64_t batch_part_num = 0;
    int64_t batch_def_subpart_num = 0;
    int64_t batch_subpart_num = 0;
    T *table_schema = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && i < table_ids_size; ++i) {
      uint64_t table_id = table_ids[i];
      if (OB_ISNULL(table_schema = ObSchemaRetrieveUtils::find_table_schema(
                                   table_id, table_schema_array))) {
        //ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Failed to find table schema", K(ret), K(table_id));
        // for compatibility
        continue;
      } else if (is_sys_table(table_id)) {
        // To avoid cyclic dependence, system table can't get partition schema from inner table.
        // As a compensation, we mock system tables' partition schema.
        continue;
      } else if (table_schema->is_user_partition_table()) {
        if (PARTITION_LEVEL_ONE <= table_schema->get_part_level()) {
          if (OB_FAIL(part_tables.push_back(table_id))) {
              LOG_WARN("Failed to push back table id", K(ret));
          } else {
            batch_part_num += table_schema->get_part_option().get_part_num();
            if (batch_part_num >= MAX_BATCH_PART_NUM) {
              int64_t cnt = part_tables.count();
              if (cnt <= 0) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("cnt is invalid", K(ret), K(cnt), K(i));
              } else if (OB_FAIL(part_idxs.push_back(cnt - 1))) {
                LOG_WARN("fail to push back part_idx", K(ret), K(table_id));
              } else {
                LOG_INFO("part num reach limit", K(ret), K(i),
                         K(table_id), K(batch_part_num), K(cnt));
                batch_part_num = 0;
              }
            }
          }
        }
        if (OB_SUCC(ret)
            && PARTITION_LEVEL_TWO == table_schema->get_part_level()
            && table_schema->has_sub_part_template_def()) {
          if (OB_FAIL(def_subpart_tables.push_back(table_id))) {
              LOG_WARN("Failed to push back table id", K(ret));
          } else {
            batch_def_subpart_num += table_schema->get_sub_part_option().get_part_num();
            if (batch_def_subpart_num >= MAX_BATCH_PART_NUM) {
              int64_t cnt = def_subpart_tables.count();
              if (cnt <= 0) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("cnt is invalid", K(ret), K(cnt), K(i));
              } else if (OB_FAIL(def_subpart_idxs.push_back(cnt - 1))) {
                LOG_WARN("fail to push back def_subpart_idx", K(ret), K(table_id));
              } else {
                LOG_INFO("subpart num reach limit", K(ret), K(i),
                         K(table_id), K(batch_def_subpart_num), K(cnt));
                batch_def_subpart_num = 0;
              }
            }
          }
        }
        if (OB_SUCC(ret)
            && PARTITION_LEVEL_TWO == table_schema->get_part_level()) {
          if (OB_FAIL(subpart_tables.push_back(table_id))) {
              LOG_WARN("Failed to push back table id", K(ret));
          } else {
            //FIXME:(yanmu.ztl) use precise total partition num instead
            batch_subpart_num += (table_schema->get_part_option().get_part_num() * 100);
            if (batch_subpart_num >= MAX_BATCH_PART_NUM) {
              int64_t cnt = subpart_tables.count();
              if (cnt <= 0) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("cnt is invalid", K(ret), K(cnt), K(i));
              } else if (OB_FAIL(subpart_idxs.push_back(cnt - 1))) {
                LOG_WARN("fail to push back subpart_idx", K(ret), K(table_id));
              } else {
                LOG_INFO("subpart num reach limit", K(ret), K(i),
                         K(table_id), K(batch_subpart_num), K(cnt));
                batch_subpart_num = 0;
              }
            }
          }
        }
      }
    }//end of for

    if (OB_SUCC(ret)) {
      // deal with border case
      int64_t cnt = part_tables.count();
      int64_t idx_cnt = part_idxs.count();
      if (cnt > 0 && (0 == idx_cnt || cnt - 1 != part_idxs.at(idx_cnt - 1))) {
        if (OB_FAIL(part_idxs.push_back(cnt - 1))) {
          LOG_WARN("fail to push back part_idx", K(ret), K(cnt));
        }
      }
      cnt = def_subpart_tables.count();
      idx_cnt = def_subpart_idxs.count();
      if (OB_SUCC(ret) && cnt > 0 && (0 == idx_cnt || cnt - 1 != def_subpart_idxs.at(idx_cnt - 1))) {
        if (OB_FAIL(def_subpart_idxs.push_back(cnt - 1))) {
          LOG_WARN("fail to push back def_subpart_idx", K(ret), K(cnt));
        }
      }
      cnt = subpart_tables.count();
      idx_cnt = subpart_idxs.count();
      if (OB_SUCC(ret) && cnt > 0 && (0 == idx_cnt || cnt - 1 != subpart_idxs.at(idx_cnt - 1))) {
        if (OB_FAIL(subpart_idxs.push_back(cnt - 1))) {
          LOG_WARN("fail to push back subpart_idx", K(ret), K(cnt));
        }
      }
    }
  }
  return ret;
}


template<typename T>
int ObSchemaServiceSQLImpl::fetch_all_partition_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObArray<T *> &table_schema_array,
    const uint64_t *table_ids /* = NULL */,
    const int64_t table_ids_size /*= 0 */)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_ids)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Table ids is NULL", K(ret));
  } else if (OB_INVALID_TENANT_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", K(ret), K(tenant_id));
  } else {
    ObSEArray<uint64_t, 10> part_tables;
    ObSEArray<uint64_t, 10> def_subpart_tables;
    ObSEArray<uint64_t, 10> subpart_tables;
    ObSEArray<int64_t, 10> part_idxs;
    ObSEArray<int64_t, 10> def_subpart_idxs;
    ObSEArray<int64_t, 10> subpart_idxs;
    if (OB_FAIL(gen_batch_fetch_array(table_schema_array, table_ids, table_ids_size,
                                      part_tables, def_subpart_tables, subpart_tables,
                                      part_idxs, def_subpart_idxs, subpart_idxs))) {
      LOG_WARN("fail to gen batch fetch array", K(ret),
               K(schema_status), K(tenant_id), K(schema_version));
    } else {
      LOG_TRACE("table_ids:", K(table_ids_size), "table_ids", ObArrayWrap<uint64_t>(table_ids, table_ids_size));
      LOG_TRACE("part_tables:", K(part_tables), K(part_idxs));
      LOG_TRACE("subpart_tables:", K(subpart_tables), K(subpart_idxs));
      LOG_TRACE("def_subpart_tables:", K(def_subpart_tables), K(def_subpart_idxs));
    }

    //fetch part info
    if (OB_SUCC(ret) && part_tables.count() > 0) {
      for (int64_t i = 0; OB_SUCC(ret) && i < part_idxs.count(); i++) {
        int64_t start_idx = 0 == i ? 0 : part_idxs.at(i - 1) + 1;
        int64_t part_cnt = part_idxs.at(i) - start_idx + 1;
        LOG_TRACE("batch parts:", K(start_idx), K(part_cnt),
                  "part_tables", ObArrayWrap<uint64_t>(&part_tables.at(start_idx), part_cnt));
        if (OB_FAIL(fetch_all_part_info(schema_status, schema_version, tenant_id,
                                        sql_client, table_schema_array,
                                        &part_tables.at(start_idx),
                                        part_cnt))) {
          LOG_WARN("fetch all part info failed", K(tenant_id), K(schema_version), K(ret));
        }
      }
    }

    //fetch def subpart info
    if (OB_SUCC(ret) && def_subpart_tables.count() > 0) {
      for (int64_t i = 0; OB_SUCC(ret) && i < def_subpart_idxs.count(); i++) {
        int64_t start_idx = 0 == i ? 0 : def_subpart_idxs.at(i - 1) + 1;
        int64_t part_cnt = def_subpart_idxs.at(i) - start_idx + 1;
        LOG_TRACE("batch def_subparts:", K(start_idx), K(part_cnt),
                  "def_subpart_tables", ObArrayWrap<uint64_t>(&def_subpart_tables.at(start_idx), part_cnt));
        if (OB_FAIL(fetch_all_def_subpart_info(schema_status, schema_version, tenant_id,
                                                   sql_client, table_schema_array,
                                                   &def_subpart_tables.at(start_idx),
                                                   part_cnt))) {
          LOG_WARN("fetch all def subpart info failed", K(tenant_id), K(schema_version), K(ret));
        }
      }
    }

    //fetch subpart info
    if (OB_SUCC(ret) && subpart_tables.count() > 0) {
      for (int64_t i = 0; OB_SUCC(ret) && i < subpart_idxs.count(); i++) {
        int64_t start_idx = 0 == i ? 0 : subpart_idxs.at(i - 1) + 1;
        int64_t part_cnt = subpart_idxs.at(i) - start_idx + 1;
        LOG_TRACE("batch subparts:", K(start_idx), K(part_cnt),
                  "subpart_tables", ObArrayWrap<uint64_t>(&subpart_tables.at(start_idx), part_cnt));
        if (OB_FAIL(fetch_all_subpart_info(schema_status, schema_version, tenant_id,
                                           sql_client, table_schema_array,
                                           &subpart_tables.at(start_idx),
                                           part_cnt))) {
          LOG_WARN("fetch all subpart info failed", K(tenant_id), K(schema_version), K(ret));
        }
      }
    }

  }
  return ret;
}

template<typename T>
int ObSchemaServiceSQLImpl::sort_tables_partition_info(
    const ObIArray<T *> &table_schema_array)
{
  int ret = OB_SUCCESS;
  // generate partition info for compatible
  for (int64_t i = 0; OB_SUCC(ret) && i < table_schema_array.count(); i++) {
    T *table = table_schema_array.at(i);
    if (OB_ISNULL(table)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table schema is null", K(ret));
    } else if (OB_FAIL(sort_table_partition_info(*table))) {
      LOG_WARN("fail to sort table partition info", K(ret), KPC(table));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_tenant_info(
    const int64_t schema_version,
    ObISQLClient &sql_client,
    ObIArray<ObTenantSchema> &tenant_schema_array,
    const uint64_t *tenant_ids,
    const int64_t tenant_ids_size)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const uint64_t tenant_id = OB_SYS_TENANT_ID;
    ObRefreshSchemaStatus dummy_schema_status;

    if (OB_FAIL(sql.append_fmt(FETCH_ALL_TENANT_HISTORY_SQL, OB_ALL_TENANT_HISTORY_TNAME))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" WHERE SCHEMA_VERSION <= %ld", schema_version))) {
      LOG_WARN("append failed", K(ret));
    } else if (NULL != tenant_ids && tenant_ids_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND tenant_id IN "))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql_append_pure_ids(dummy_schema_status, tenant_ids, tenant_ids_size, sql))) {
        LOG_WARN("sql append tenant ids failed");
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }
    DEFINE_SQL_CLIENT_RETRY_WEAK(sql_client);
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql_client_retry_weak.read(res, sql.ptr()))) {
      LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
    } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to get result. ", K(ret));
    } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_tenant_schema(sql_client_retry_weak, *result, tenant_schema_array))) {
      LOG_WARN("failed to retrieve all tenant schema", K(ret));
    }
  }
  return ret;
}

#define SQL_APPEND_TENANT_ID(schema_keys, schema_key_size, sql) \
  ({                                                                 \
    int ret = OB_SUCCESS; \
    if (OB_FAIL(sql.append("("))) { \
      LOG_WARN("append sql failed", K(ret)); \
    } else { \
      for (int64_t i = 0; OB_SUCC(ret) && i < schema_key_size; ++i) {  \
        if (OB_FAIL(sql.append_fmt("%s(%lu)", 0 == i ? "" : ", ", \
                                   schema_keys[i].tenant_id_))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } \
      } \
      if (OB_SUCC(ret)) { \
        if (OB_FAIL(sql.append(")"))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } \
      } \
    } \
    ret; \
  })

#define SQL_APPEND_SCHEMA_ID(SCHEMA, schema_keys, schema_key_size, sql) \
  ({                                                                 \
    int ret = OB_SUCCESS; \
    if (OB_FAIL(sql.append("("))) { \
      LOG_WARN("append sql failed", K(ret)); \
    } else { \
      for (int64_t i = 0; OB_SUCC(ret) && i < schema_key_size; ++i) {  \
        const uint64_t schema_id = fill_extract_schema_id(schema_status, schema_keys[i].SCHEMA##_id_); \
        if (OB_FAIL(sql.append_fmt("%s%lu", 0 == i ? "" : ", ", \
                                   schema_id))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } \
      } \
      if (OB_SUCC(ret)) { \
        if (OB_FAIL(sql.append(")"))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } \
      } \
    } \
    ret; \
  })

#define SQL_APPEND_DB_PRIV_ID(schema_keys, tenant_id, schema_key_size, sql) \
  ({                                                                 \
    int ret = OB_SUCCESS; \
    if (OB_FAIL(sql.append("("))) { \
      LOG_WARN("append sql failed", K(ret)); \
    } else { \
      for (int64_t i = 0; OB_SUCC(ret) && i < schema_key_size; ++i) {  \
        if (OB_FAIL(sql.append_fmt("%s(%lu, %lu, ", 0 == i ? "" : ", ", \
                                   fill_extract_tenant_id(schema_status, tenant_id), \
                                   fill_extract_schema_id(schema_status, schema_keys[i].user_id_)))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } else if (OB_FAIL(sql_append_hex_escape_str(schema_keys[i].database_name_, sql))) { \
          LOG_WARN("fail to append database name", K(ret)); \
        } else if (OB_FAIL(sql.append(")"))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } \
      } \
      if (OB_SUCC(ret)) { \
        if (OB_FAIL(sql.append(")"))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } \
      } \
    } \
    ret; \
  })

#define SQL_APPEND_SYS_PRIV_ID(schema_keys, tenant_id, schema_key_size, sql) \
  ({                                                                 \
    int ret = OB_SUCCESS; \
    if (OB_FAIL(sql.append("("))) { \
      LOG_WARN("append sql failed", K(ret)); \
    } else { \
      for (int64_t i = 0; OB_SUCC(ret) && i < schema_key_size; ++i) {  \
        if (OB_FAIL(sql.append_fmt("%s(%lu, %lu)", 0 == i ? "" : ", ", \
                      fill_extract_tenant_id(schema_status, tenant_id), \
                      fill_extract_schema_id(schema_status, schema_keys[i].grantee_id_)))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } \
      } \
      if (OB_SUCC(ret)) { \
        if (OB_FAIL(sql.append(")"))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } \
      } \
    } \
    ret; \
  })

#define SQL_APPEND_TABLE_PRIV_ID(schema_keys, tenant_id, schema_key_size, sql) \
  ({                                                                 \
    int ret = OB_SUCCESS; \
    if (OB_FAIL(sql.append("("))) { \
      LOG_WARN("append sql failed", K(ret)); \
    } else { \
      for (int64_t i = 0; OB_SUCC(ret) && i < schema_key_size; ++i) {  \
        if (OB_FAIL(sql.append_fmt("%s(%lu, %lu, ", 0 == i ? "" : ", ", \
                                   fill_extract_tenant_id(schema_status, tenant_id), \
                                   fill_extract_schema_id(schema_status, schema_keys[i].user_id_)))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } else if (OB_FAIL(sql_append_hex_escape_str(schema_keys[i].database_name_, sql))) { \
          LOG_WARN("fail to append database name", K(ret)); \
        } else if (OB_FAIL(sql.append(", "))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } else if (OB_FAIL(sql_append_hex_escape_str(schema_keys[i].table_name_, sql))) { \
          LOG_WARN("fail to append database name", K(ret)); \
        } else if (OB_FAIL(sql.append(")"))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } \
      } \
      if (OB_SUCC(ret)) { \
        if (OB_FAIL(sql.append(")"))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } \
      } \
    } \
    ret; \
  })

#define SQL_APPEND_OBJ_PRIV_ID(schema_keys, tenant_id, schema_key_size, sql) \
  ({                                                                 \
    int ret = OB_SUCCESS; \
    if (OB_FAIL(sql.append("("))) { \
      LOG_WARN("append sql failed", K(ret)); \
    } else { \
      for (int64_t i = 0; OB_SUCC(ret) && i < schema_key_size; ++i) {  \
        if (OB_FAIL(sql.append_fmt(\
            "%s(%lu, %lu, %lu, %lu, %lu, %lu)", 0 == i ? "" : ", ", \
            fill_extract_tenant_id(schema_status, tenant_id), \
            fill_extract_schema_id(schema_status, schema_keys[i].table_id_), \
            schema_keys[i].obj_type_, \
            schema_keys[i].col_id_, \
            fill_extract_schema_id(schema_status, schema_keys[i].grantor_id_), \
            fill_extract_schema_id(schema_status, schema_keys[i].grantee_id_)))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } \
      } \
      if (OB_SUCC(ret)) { \
        if (OB_FAIL(sql.append(")"))) { \
          LOG_WARN("append sql failed", K(ret)); \
        } \
      } \
    } \
    ret; \
  })

#define SQL_APPEND_UDF_ID(schema_keys, exec_tenant_id, schema_key_size, sql) \
  ({                                                                    \
    int ret = OB_SUCCESS;                                               \
    UNUSED(exec_tenant_id);                                                  \
    if (OB_FAIL(sql.append("("))) {                                     \
      LOG_WARN("append sql failed", K(ret));                            \
    } else {                                                            \
      for (int64_t i = 0; OB_SUCC(ret) && i < schema_key_size; ++i) {   \
        if (OB_FAIL(sql.append_fmt("%s('%.*s')", 0 == i ? "" : ", ",    \
                                   schema_keys[i].udf_name_.length(),   \
                                   schema_keys[i].udf_name_.ptr()))) {  \
          LOG_WARN("append sql failed", K(ret));                        \
        }                                                               \
      }                                                                 \
      if (OB_SUCC(ret)) {                                               \
        if (OB_FAIL(sql.append(")"))) {                                 \
          LOG_WARN("append sql failed", K(ret));                        \
        }                                                               \
      }                                                                 \
    }                                                                   \
    ret;                                                                \
  })

int ObSchemaServiceSQLImpl::fetch_all_database_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObDatabaseSchema> &db_schema_array,
    const uint64_t *db_ids /* = NULL */,
    const int64_t db_ids_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);

    if (OB_FAIL(sql.append_fmt(FETCH_ALL_DATABASE_HISTORY_SQL, OB_ALL_DATABASE_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else {
      if (NULL != db_ids && db_ids_size > 0) {
        if (OB_FAIL(sql.append_fmt(" AND database_id IN "))) {
          LOG_WARN("append sql failed", K(ret));
        } else if (OB_FAIL(sql_append_pure_ids(schema_status, db_ids, db_ids_size, sql))) {
          LOG_WARN("sql append database ids failed");
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, DATABASE_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("append failed", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_database_schema(tenant_id, *result, db_schema_array))) {
        LOG_WARN("failed to retrieve all database schema", K(ret));
      }
    }
  }
  return ret;
}

template <typename T>
int ObSchemaServiceSQLImpl::fetch_all_table_info(const ObRefreshSchemaStatus &schema_status,
                                                 const int64_t schema_version,
                                                 const uint64_t tenant_id,
                                                 ObISQLClient &sql_client,
                                                 ObIAllocator &allocator,
                                                 ObIArray<T> &table_schema_array,
                                                 const uint64_t *table_ids /* = NULL */,
                                                 const int64_t table_ids_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
  // schema_version == INT64_MAX means get all __all_table rather than __all_table_history,
  // system table's schema should read from __all_table
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail", K(ret));
  } else if (OB_INVALID_TENANT_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", K(ret), K(tenant_id));
  } else if (INT64_MAX == schema_version) {
    const char *table_name = NULL;
    if (OB_FAIL(ObSchemaUtils::get_all_table_name(exec_tenant_id,
                                                  table_name,
                                                  schema_service_))) {
      LOG_WARN("fail to get all table name", K(ret), K(exec_tenant_id));
    } else if (OB_FAIL(sql.append_fmt(FETCH_ALL_TABLE_SQL, table_name,
                                      fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    }
    if (OB_SUCC(ret)) {
      if (NULL != table_ids && table_ids_size > 0) {
        if (OB_FAIL(sql.append_fmt(" AND table_id IN "))) {
          LOG_WARN("append sql failed", K(ret));
        } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
          LOG_WARN("sql append table ids failed");
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" ORDER BY TENANT_ID DESC, TABLE_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("append sql failed", K(ret));
      }
    }
  } else {
    const char *table_name = NULL;
    if (OB_FAIL(ObSchemaUtils::get_all_table_history_name(exec_tenant_id,
                                                          table_name,
                                                          schema_service_))) {
      LOG_WARN("fail to get all table name", K(ret), K(exec_tenant_id));
    } else if (OB_FAIL(sql.append_fmt(FETCH_ALL_TABLE_HISTORY_SQL, table_name,
                       fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    }
    if (OB_SUCC(ret)) {
      if (NULL != table_ids && table_ids_size > 0) {
        if (OB_FAIL(sql.append_fmt(" AND table_id IN "))) {
          LOG_WARN("append sql failed", K(ret));
        } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
          LOG_WARN("sql append table ids failed");
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql.append_fmt(" ORDER BY TENANT_ID DESC, TABLE_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("append sql failed", K(ret));
      }
    }
  }

  if (OB_SUCC(ret)) {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      const bool check_deleted = (INT64_MAX != schema_version);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_table_schema(
                 tenant_id, check_deleted, *result, allocator, table_schema_array))) {
        LOG_WARN("failed to retrieve all table schema:", K(check_deleted), K(ret));
      }
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(fetch_temp_table_schemas(schema_status, tenant_id, sql_client_retry_weak, table_schema_array))) {
      LOG_WARN("failed to fill temp table schemas", K(ret));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_temp_table_schemas(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObTableSchema*> &table_schema_array)
{
  int ret = OB_SUCCESS;
  FOREACH_CNT_X(table_schema, table_schema_array, OB_SUCC(ret)) {
    if (OB_ISNULL(table_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("NULL ptr", K(table_schema), K(ret));
    } else if (OB_ISNULL(*table_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("NULL ptr", K(*table_schema), K(ret));
    } else if (OB_FAIL(fetch_temp_table_schema(schema_status, tenant_id, sql_client, **table_schema))) {
      LOG_WARN("fill temp table failed", K(ret));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_temp_table_schema(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObString create_host;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    common::sqlclient::ObMySQLResult *result = NULL;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (table_schema.is_tmp_table() || table_schema.is_ctas_tmp_table()) {
      if (OB_FAIL(sql.assign_fmt("SELECT create_host FROM %s where tenant_id = %lu and table_id = %lu",
                              OB_ALL_TEMP_TABLE_TNAME,
                              fill_extract_tenant_id(schema_status, tenant_id),
                              fill_extract_schema_id(schema_status, table_schema.get_table_id())))) {
        LOG_WARN("append sql failed", K(table_schema), K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get result.", K(table_schema), K(ret));
      } else if (OB_FAIL(result->next())) {
        LOG_WARN("failed to get temp table info", K(table_schema), K(ret));
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      } else if (OB_FAIL(ObSchemaRetrieveUtils::fill_temp_table_schema(tenant_id, *result, table_schema))) {
        LOG_WARN("fail to fill temp table schema", K(ret), K(tenant_id));
      }
    }
  }
  return ret;
}
int ObSchemaServiceSQLImpl::fetch_new_tenant_id(uint64_t &new_tenant_id)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(fetch_new_schema_id(OB_SYS_TENANT_ID, OB_MAX_USED_TENANT_ID_TYPE, new_tenant_id))) {
    LOG_WARN("fetch_new_tenant_id faild", K(ret));
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_new_tablet_ids(const ObTableSchema &table_schema, uint64_t &new_tablet_id, const uint64_t size)
{
  int ret = OB_SUCCESS;
  if (table_schema.has_rowid() && !table_schema.is_extended_rowid_mode()) {
    if (OB_FAIL(fetch_new_normal_rowid_table_tablet_ids(
                table_schema.get_tenant_id(), new_tablet_id, size))) {
      LOG_WARN("fail to fetch new tablet id", K(table_schema), K(ret));
    }
  } else {
    if (OB_FAIL(fetch_new_extended_rowid_table_tablet_ids(
                table_schema.get_tenant_id(), new_tablet_id, size))) {
      LOG_WARN("fail to fetch new tablet id", K(table_schema), K(ret));
    }
  }
  return ret;
}


#define FETCH_NEW_SCHEMA_ID(SCHEMA_TYPE, SCHEMA) \
int ObSchemaServiceSQLImpl::fetch_new_##SCHEMA##_id(const uint64_t tenant_id, uint64_t &new_schema_id)  \
{                                                                                                    \
  return fetch_new_schema_id(tenant_id, OB_MAX_USED_##SCHEMA_TYPE##_ID_TYPE, new_schema_id);         \
}
// When ret = OB_SUCCESS, partition_ids in [new_partition_id - num + 1, partition_id] are available.
int ObSchemaServiceSQLImpl::fetch_new_partition_ids(
    const uint64_t tenant_id, const int64_t partition_num, uint64_t &new_partition_id)
{
  int ret = OB_SUCCESS;
  ObMaxIdFetcher id_fetcher(*mysql_proxy_);
  if (OB_ISNULL(mysql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("proxy is NULL", KR(ret));
  } else if (OB_FAIL(id_fetcher.fetch_new_max_id(tenant_id, OB_MAX_USED_PARTITION_ID_TYPE,
    new_partition_id, UINT64_MAX/*initial value should exist*/, partition_num))) {
    LOG_WARN("get new schema id failed", KR(ret), K(tenant_id), K(partition_num));
  }
  return ret;
}

FETCH_NEW_SCHEMA_ID(DATABASE, database);
FETCH_NEW_SCHEMA_ID(TABLE, table);
FETCH_NEW_SCHEMA_ID(TABLEGROUP, tablegroup);
FETCH_NEW_SCHEMA_ID(OUTLINE, outline);
FETCH_NEW_SCHEMA_ID(USER, user);
FETCH_NEW_SCHEMA_ID(SYNONYM, synonym);
FETCH_NEW_SCHEMA_ID(UDF, udf);
FETCH_NEW_SCHEMA_ID(CONSTRAINT, constraint);
FETCH_NEW_SCHEMA_ID(SEQUENCE, sequence);
FETCH_NEW_SCHEMA_ID(UDT, udt);
FETCH_NEW_SCHEMA_ID(ROUTINE, routine);
FETCH_NEW_SCHEMA_ID(PACKAGE, package);
FETCH_NEW_SCHEMA_ID(KEYSTORE, keystore);
FETCH_NEW_SCHEMA_ID(MASTER_KEY, master_key);
FETCH_NEW_SCHEMA_ID(LABEL_SE_POLICY, label_se_policy);
FETCH_NEW_SCHEMA_ID(LABEL_SE_COMPONENT, label_se_component);
FETCH_NEW_SCHEMA_ID(LABEL_SE_LABEL, label_se_label);
FETCH_NEW_SCHEMA_ID(LABEL_SE_USER_LEVEL, label_se_user_level);
FETCH_NEW_SCHEMA_ID(TABLESPACE, tablespace);
FETCH_NEW_SCHEMA_ID(TRIGGER, trigger);
FETCH_NEW_SCHEMA_ID(PROFILE, profile);
FETCH_NEW_SCHEMA_ID(AUDIT, audit);
FETCH_NEW_SCHEMA_ID(DBLINK, dblink);
FETCH_NEW_SCHEMA_ID(DIRECTORY, directory);
// FETCH_NEW_SCHEMA_ID(NON_PRIMARY_KEY_TABLE_TABLET, non_primary_key_table_tablet);
// FETCH_NEW_SCHEMA_ID(PRIMARY_KEY_TABLE_TABLET, primary_key_table_tablet);
FETCH_NEW_SCHEMA_ID(CONTEXT, context);
FETCH_NEW_SCHEMA_ID(SYS_PL_OBJECT, sys_pl_object);

#undef FETCH_NEW_SCHEMA_ID

int ObSchemaServiceSQLImpl::fetch_new_normal_rowid_table_tablet_ids(const uint64_t tenant_id, uint64_t &new_schema_id, const uint64_t size)
{
  return fetch_new_schema_ids(tenant_id, OB_MAX_USED_NORMAL_ROWID_TABLE_TABLET_ID_TYPE, new_schema_id, size);
}

int ObSchemaServiceSQLImpl::fetch_new_extended_rowid_table_tablet_ids(const uint64_t tenant_id, uint64_t &new_schema_id, const uint64_t size)
{
  return fetch_new_schema_ids(tenant_id, OB_MAX_USED_EXTENDED_ROWID_TABLE_TABLET_ID_TYPE, new_schema_id, size);
}

int ObSchemaServiceSQLImpl::fetch_new_schema_id(const uint64_t tenant_id,
                                                const enum ObMaxIdType max_id_type,
                                                uint64_t &new_schema_id)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(mysql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("proxy is NULL");
  } else {
    ObMaxIdFetcher id_fetcher(*mysql_proxy_);
    if (OB_FAIL(id_fetcher.fetch_new_max_id(tenant_id, max_id_type, new_schema_id))) {
      LOG_WARN("get new schema id failed", K(ret), K(max_id_type));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_new_schema_ids(const uint64_t tenant_id,
                                                const enum ObMaxIdType max_id_type,
                                                uint64_t &new_schema_id,
                                                const uint64_t size/* = 1 */)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(mysql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("proxy is NULL");
  } else {
    ObMaxIdFetcher id_fetcher(*mysql_proxy_);
    if (OB_FAIL(id_fetcher.fetch_new_max_ids(tenant_id, max_id_type, new_schema_id, size))) {
      LOG_WARN("get new schema id failed", K(ret), K(max_id_type));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_increment_schema_operations(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t base_version,
    const int64_t new_schema_version,
    ObISQLClient &sql_client,
    SchemaOperationSetWithAlloc &schema_operations)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  } else if (base_version < 0 || new_schema_version < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid schema version", K(base_version), K(new_schema_version), K(ret));
  } else {
    ObSqlString sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      schema_operations.reset();
      const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql.append_fmt(FETCH_ALL_DDL_OPERATION_SQL_WITH_VERSION_RANGE
                                 " ORDER BY schema_version ASC",
                                 OB_ALL_DDL_OPERATION_TNAME,
                                 base_version,
                                 new_schema_version))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, fill_exec_tenant_id(schema_status), sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else {
        ObSchemaOperation schema_operation;
        bool will_break = false;
        while (OB_SUCCESS == ret && !will_break && OB_SUCCESS == (ret = result->next())) {
          if (OB_FAIL(ObSchemaRetrieveUtils::fill_schema_operation(schema_status.tenant_id_, *result, schema_operations, schema_operation))) {
            LOG_WARN("fill_schema_operation failed", K(ret));
            result->print_info();
            will_break = true;
          } else if (OB_INVALID_DDL_OP == schema_operation.op_type_) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("invalid table operation type: ", K(schema_operation), K(ret));
          } else {
            LOG_DEBUG("schema operation:", K(schema_operation));
            if (OB_FAIL(schema_operations.push_back(schema_operation))) {
              LOG_WARN("failed to push back operation", K(ret));
              will_break = true;
            }
          }
        }
        if (ret != OB_ITER_END) {
          LOG_WARN("fail to get all schema. iter quit. ", K(ret));
        } else {
          ret = OB_SUCCESS;
        }
      }
    }
  }

  return ret;
}

int ObSchemaServiceSQLImpl::check_sys_schema_change(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    const ObIArray<uint64_t> &sys_table_ids,
    const int64_t schema_version,
    const int64_t new_schema_version,
    bool &sys_schema_change)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;

    if (schema_version >= new_schema_version) {
      sys_schema_change = false;
    } else {
      if (!check_inner_stat()) {
        ret = OB_NOT_INIT;
        LOG_WARN("check inner stat fail");
      } else if (OB_FAIL(sql.append_fmt("SELECT 1 FROM %s WHERE SCHEMA_VERSION > %lu "
              "AND SCHEMA_VERSION <= %lu AND OPERATION_TYPE > %d AND OPERATION_TYPE < %d "
              "AND TABLE_ID IN (", OB_ALL_DDL_OPERATION_TNAME, schema_version, new_schema_version,
              OB_DDL_TABLE_OPERATION_BEGIN, OB_DDL_TABLE_OPERATION_END))) {
        LOG_WARN("append_fmt failed", K(ret));
      } else {
        // no need to change table_id
        for (int64_t i = 0; OB_SUCC(ret) && i < sys_table_ids.count(); ++i) {
          if (OB_FAIL(sql.append_fmt("%s%lu%s", (0 == i) ? "" : ",",
                      fill_extract_schema_id(schema_status, sys_table_ids.at(i)),
                      (sys_table_ids.count() - 1 == i) ? ")" : ""))) {
            LOG_WARN("append_fmt failed", K(ret));
          }
        }
      }

      const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(sql_client_retry_weak.read(res, tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get_result failed", K(ret));
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          sys_schema_change = false;
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("next failed", K(ret));
        }
      } else {
        sys_schema_change = true;
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_schema_version(
    const ObRefreshSchemaStatus &schema_status,
    ObISQLClient &sql_client,
    int64_t &schema_version)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    int64_t begin_time = ::oceanbase::common::ObTimeUtility::current_time();
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    bool check_sys_variable = false;
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_PARAMETER(sql_client, snapshot_timestamp, check_sys_variable);
    ret = sql.append_fmt("SELECT MAX(schema_version) as version, host_ip() as myip, rpc_port() as myport FROM %s",
                         OB_ALL_DDL_OPERATION_TNAME);

    if (OB_FAIL(ret)) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql_client_retry_weak.read(res, fill_exec_tenant_id(schema_status), sql.ptr()))) {
      LOG_WARN("execute sql failed", K(sql), K(ret));
      // Unittest cases use MySQL as oceanbase, and host_ip()/rpc_port() is not supported in MySQL.
      // To avoid error of unittest case, we ignore error when specified error occur.
      if (-ER_SP_DOES_NOT_EXIST == ret) {
        ret = OB_SUCCESS;
        LOG_WARN("return mysql error code, try to read again", K(ret));
        sql.reuse();
        if (OB_FAIL(sql.append_fmt("SELECT MAX(schema_version) as version FROM %s", OB_ALL_DDL_OPERATION_TNAME))) {
          LOG_WARN("fail to append sql", K(ret));
        } else if (OB_FAIL(sql_client_retry_weak.read(res, fill_exec_tenant_id(schema_status), sql.ptr()))) {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else {
        int64_t end_time = ::oceanbase::common::ObTimeUtility::current_time();
        LOG_TRACE("get_schema_version time.", "cost us", end_time - begin_time);
        if (OB_FAIL(retrieve_schema_version(*result, schema_version))) {
          LOG_WARN("failed to retrive schema version: ", K(sql), K(ret));
        }
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_tenants(
    ObISQLClient &sql_client,
    const int64_t schema_version,
    ObArray<SchemaKey> &schema_keys,
    ObIArray<ObSimpleTenantSchema> &schema_array)
{
  int ret = OB_SUCCESS;
  schema_array.reserve(schema_keys.count());

  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  } else {
    std::sort(schema_keys.begin(), schema_keys.end(), SchemaKey::cmp_with_tenant_id);
    // split query to tenant space && split big query
    int64_t begin = 0;
    int64_t end = 0;
    while (OB_SUCCESS == ret && end < schema_keys.count()) {
      while (OB_SUCCESS == ret && end < schema_keys.count()
          && end - begin < MAX_IN_QUERY_PER_TIME) {
        end++;
      }
      if (OB_FAIL(fetch_tenants(sql_client,
                                schema_version,
                                schema_array,
                                &schema_keys.at(begin),
                                end - begin))) {
        LOG_WARN("fetch batch tenants failed", K(ret));
      }
      begin = end;
    }
  }
  LOG_INFO("get batch tenants finish", K(ret));
  return ret;
}

#define GET_BATCH_SCHEMAS_FUNC_DEFINE(SCHEMA, SCHEMA_TYPE)       \
  int ObSchemaServiceSQLImpl::get_batch_##SCHEMA##s( \
      const ObRefreshSchemaStatus &schema_status,    \
      ObISQLClient &sql_client,                      \
      const int64_t schema_version,                  \
      ObArray<SchemaKey> &schema_keys,               \
      ObIArray<SCHEMA_TYPE> &schema_array)           \
  {                                                                \
    int ret = OB_SUCCESS;                                          \
    schema_array.reset();                                          \
    schema_array.reserve(schema_keys.count());                     \
    LOG_INFO("get batch "#SCHEMA"s", K(schema_version), K(schema_keys));           \
    if (!check_inner_stat()) {                                     \
      ret = OB_NOT_INIT;                                           \
      LOG_WARN("check inner stat fail");                           \
    } else {                                                       \
      std::sort(schema_keys.begin(), schema_keys.end(), SchemaKey::cmp_with_tenant_id); \
      int64_t begin = 0;                                                        \
      int64_t end = 0;                                                          \
      while (OB_SUCCESS == ret && end < schema_keys.count()) {                  \
        const uint64_t tenant_id = schema_keys.at(begin).tenant_id_;            \
        while (OB_SUCCESS == ret && end < schema_keys.count()                   \
               && tenant_id == schema_keys.at(end).tenant_id_                   \
               && end - begin < MAX_IN_QUERY_PER_TIME) {                        \
          end++;                                                                \
        }                                                                       \
        if (OB_FAIL(fetch_##SCHEMA##s(sql_client,                               \
                                      schema_status,                            \
                                      schema_version,                           \
                                      tenant_id,                                \
                                      schema_array,                             \
                                      &schema_keys.at(begin),                   \
                                      end - begin))) {                          \
          LOG_WARN("fetch batch "#SCHEMA"s failed", K(ret));                    \
          /* for liboblog compatibility */ \
          if (-ER_NO_SUCH_TABLE == ret && ObSchemaService::g_liboblog_mode_) {  \
            LOG_WARN("liboblog mode, ignore "#SCHEMA" schema table NOT EXIST error", \
                     K(ret), K(schema_version), K(tenant_id), K(schema_status)); \
            ret = OB_SUCCESS;                                                   \
          }                                                                     \
        }                                                                       \
        LOG_DEBUG("finish fetch batch "#SCHEMA"s", K(begin), K(end), "total_count", schema_keys.count()); \
        begin = end;                                                            \
      }                                                                         \
      LOG_DEBUG("finish fetch batch "#SCHEMA"s", K(schema_array));              \
    }                                                                           \
    return ret;                                                                 \
  }

GET_BATCH_SCHEMAS_FUNC_DEFINE(user, ObSimpleUserSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(database, ObSimpleDatabaseSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(tablegroup, ObSimpleTablegroupSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(table, ObSimpleTableSchemaV2);
GET_BATCH_SCHEMAS_FUNC_DEFINE(db_priv, ObDBPriv);
GET_BATCH_SCHEMAS_FUNC_DEFINE(table_priv, ObTablePriv);
GET_BATCH_SCHEMAS_FUNC_DEFINE(outline, ObSimpleOutlineSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(synonym, ObSimpleSynonymSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(routine, ObSimpleRoutineSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(package, ObSimplePackageSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(trigger, ObSimpleTriggerSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(udf, ObSimpleUDFSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(udt, ObSimpleUDTSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(sequence, ObSequenceSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(keystore, ObKeystoreSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(label_se_policy, ObLabelSePolicySchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(label_se_component, ObLabelSeComponentSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(label_se_label, ObLabelSeLabelSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(label_se_user_level, ObLabelSeUserLevelSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(tablespace, ObTablespaceSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(profile, ObProfileSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(audit, ObSAuditSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(sys_priv, ObSysPriv);
GET_BATCH_SCHEMAS_FUNC_DEFINE(obj_priv, ObObjPriv);
GET_BATCH_SCHEMAS_FUNC_DEFINE(dblink, ObDbLinkSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(directory, ObDirectorySchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(context, ObContextSchema);
GET_BATCH_SCHEMAS_FUNC_DEFINE(mock_fk_parent_table, ObSimpleMockFKParentTableSchema);

int ObSchemaServiceSQLImpl::sql_append_pure_ids(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t *ids,
    const int64_t ids_size,
    common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ids) || ids_size <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ids), K(ids_size));
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(sql.append("("))) {
      LOG_WARN("append sql failed", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < ids_size; ++i) {
        if (OB_FAIL(sql.append_fmt("%s%lu", 0 == i ? "" : ", ",
                                   fill_extract_schema_id(schema_status, ids[i])))) {
          LOG_WARN("append sql failed", K(ret), K(i));
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(sql.append(")"))) {
          LOG_WARN("append sql failed", K(ret));
        }
      }
    }
  }

  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_tenants(
    common::ObISQLClient &sql_client,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_ids,
    common::ObIArray<ObTenantSchema> &tenant_info_array)
{
  int ret = OB_SUCCESS;
  tenant_info_array.reserve(tenant_ids.count());
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid schema_version", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }

  std::sort(tenant_ids.begin(), tenant_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_ids.count()
        && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_tenant_info(schema_version,
        sql_client, tenant_info_array, &tenant_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all tenants info failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch tenants info finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_synonyms(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_synonym_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObSynonymInfo> &synonym_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  synonym_info_array.reserve(tenant_synonym_ids.count());
  LOG_DEBUG("fetch batch synonyms begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }

  std::sort(tenant_synonym_ids.begin(), tenant_synonym_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_synonym_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_synonym_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_synonym_info(schema_status, schema_version, tenant_id,
        sql_client, synonym_info_array, &tenant_synonym_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all synonym info failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch synonym info finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_databases(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    ObArray<uint64_t> &db_ids,
    ObISQLClient &sql_client,
    ObIArray<ObDatabaseSchema> &db_schema_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  db_schema_array.reserve(db_ids.count());
  LOG_DEBUG("fetch batch database schema begin.");
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  } else if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid schema_version", K(schema_version), K(ret));
  }

  // split query to tenant space && split big query
  if (OB_SUCC(ret)) {
    int64_t begin = 0;
    int64_t end = 0;
    while (OB_SUCCESS == ret && end < db_ids.count()) {
      while (OB_SUCCESS == ret && end < db_ids.count()
             && end - begin < MAX_IN_QUERY_PER_TIME) {
        end++;
      }
      if (OB_FAIL(fetch_all_database_info(schema_status, schema_version, tenant_id,
          sql_client, db_schema_array, &db_ids.at(begin), end - begin))) {
        LOG_WARN("fetch all database info failed", K(ret), K(tenant_id), K(schema_version));
      }
      begin = end;
    }
    LOG_INFO("get batch database schema finish", K(schema_version), K(ret));
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_tablegroup_schema(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tablegroup_id,
    const int64_t schema_version,
    ObISQLClient &sql_client,
    ObIAllocator &allocator,
    ObTablegroupSchema *&tablegroup_schema)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail, ", K(ret));
  } else if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid schema_version", K(schema_version), K(ret));
  } else if (OB_FAIL(fetch_tablegroup_info(schema_status, tenant_id, tablegroup_id, schema_version,
                                           sql_client, allocator, tablegroup_schema))) {
    LOG_WARN("fetch tablegroup info failed", K(tenant_id), K(tablegroup_id), K(schema_version), K(ret));
  } else if (is_sys_tablegroup_id(tablegroup_id)) {
    // skip
  } else if (OB_FAIL(fetch_partition_info(schema_status, tenant_id, tablegroup_id, schema_version,
                                          sql_client, tablegroup_schema))) {
    LOG_WARN("Failed to fetch part info", K(ret));
  } else { }//do nothing
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_tablegroup_info(const ObRefreshSchemaStatus &schema_status,
                                                  const uint64_t tenant_id,
                                                  const uint64_t tablegroup_id,
                                                  const int64_t schema_version,
                                                  ObISQLClient &sql_client,
                                                  ObIAllocator &allocator,
                                                  ObTablegroupSchema *&tablegroup_schema)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_TABLEGROUP_HISTORY_SQL, OB_ALL_TABLEGROUP_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND tablegroup_id = %lu and schema_version <= %ld "
                                      " ORDER BY tenant_id desc, tablegroup_id desc, schema_version desc",
                                      fill_extract_schema_id(schema_status, tablegroup_id),
                                      schema_version))) {
      LOG_WARN("append tablegroup_id and schema_version failed", K(ret), K(tablegroup_id),
               K(schema_version));
    } else {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_tablegroup_schema(
                 tenant_id, *result, allocator, tablegroup_schema))) {
        LOG_WARN("failed to retrieve tablegroup schema:", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_outlines(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_outline_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObOutlineInfo> &outline_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  outline_info_array.reserve(tenant_outline_ids.count());
  LOG_DEBUG("fetch batch outlines begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }

  std::sort(tenant_outline_ids.begin(), tenant_outline_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_outline_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_outline_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_outline_info(schema_status, schema_version, tenant_id, sql_client, outline_info_array,
        &tenant_outline_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all outline info failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch outline info finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_routines(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_routine_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObRoutineInfo> &routine_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  routine_info_array.reserve(tenant_routine_ids.count());
  LOG_DEBUG("fetch batch routines begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }

  std::sort(tenant_routine_ids.begin(), tenant_routine_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_routine_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_routine_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_routine_info(schema_status, schema_version, tenant_id, sql_client, routine_info_array,
        &tenant_routine_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all routine info failed", K(schema_version), K(ret));
    } else if (OB_FAIL(fetch_all_routine_param_info(schema_status, schema_version, tenant_id, sql_client,
        routine_info_array, &tenant_routine_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all routine param info failed", K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch routine info finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_udts(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_udt_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObUDTTypeInfo> &udt_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  udt_info_array.reserve(tenant_udt_ids.count());
  LOG_DEBUG("fetch batch routines begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }

  std::sort(tenant_udt_ids.begin(), tenant_udt_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_udt_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_udt_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_udt_info(schema_status, schema_version, tenant_id, sql_client, udt_info_array,
        &tenant_udt_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all routine info failed", K(schema_version), K(ret));
    } else if (OB_FAIL(fetch_all_udt_attr_info(schema_status, schema_version, tenant_id, sql_client,
        udt_info_array, &tenant_udt_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all routine param info failed", K(ret));
    } else if (OB_FAIL(fetch_all_udt_coll_info(schema_status, schema_version, tenant_id, sql_client,
        udt_info_array, &tenant_udt_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all udt coll info failed", K(ret));
    } else if (OB_FAIL(fetch_all_udt_object_info(schema_status, schema_version,
        tenant_id, sql_client,
        udt_info_array, &tenant_udt_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all udt object type info failed", K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch udt info finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_users(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_user_ids,
    common::ObISQLClient &sql_client,
    common::ObArray<ObUserInfo> &user_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  user_info_array.reserve(tenant_user_ids.count());
  LOG_DEBUG("fetch batch users begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }

  std::sort(tenant_user_ids.begin(), tenant_user_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_user_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_user_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_user_info(schema_status,schema_version, tenant_id, sql_client, user_info_array,
        &tenant_user_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all user privileges info failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch user privileges finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_outline_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObOutlineInfo> &outline_array,
    const uint64_t *outline_keys /* = NULL */,
    const int64_t outlines_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);

    if (OB_FAIL(sql.append_fmt(FETCH_ALL_OUTLINE_HISTORY_SQL,
                               OB_ALL_OUTLINE_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != outline_keys && outlines_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND outline_id IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < outlines_size; ++i) {
          const uint64_t outline_id = fill_extract_schema_id(schema_status, outline_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu",
                                     0 == i ? "" : ", ",
                                     outline_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, OUTLINE_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_outline_schema(tenant_id, *result, outline_array))) {
        LOG_WARN("Failed to retrieve outline infos", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_packages(const ObRefreshSchemaStatus &schema_status,
                                               const int64_t schema_version,
                                               common::ObArray<uint64_t> &tenant_package_ids,
                                               common::ObISQLClient &sql_client,
                                               common::ObIArray<ObPackageInfo> &package_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  package_info_array.reserve(tenant_package_ids.count());
  LOG_DEBUG("fetch batch packages begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }

  std::sort(tenant_package_ids.begin(), tenant_package_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_package_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_package_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_package_info(schema_status, schema_version, tenant_id, sql_client, package_info_array,
        &tenant_package_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all package info failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch package info finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_mock_fk_parent_tables(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_mock_fk_parent_table_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObMockFKParentTableSchema> &mock_fk_parent_table_schema_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  mock_fk_parent_table_schema_array.reserve(tenant_mock_fk_parent_table_ids.count());
  LOG_DEBUG("fetch batch mock_fk_parent_table begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }
  std::sort(tenant_mock_fk_parent_table_ids.begin(), tenant_mock_fk_parent_table_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_mock_fk_parent_table_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_mock_fk_parent_table_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_mock_fk_parent_table_info(schema_status, schema_version, tenant_id, sql_client, mock_fk_parent_table_schema_array,
        &tenant_mock_fk_parent_table_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all mock_fk_parent_table info failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch mock_fk_parent_table info finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_triggers(const ObRefreshSchemaStatus &schema_status,
                                               const int64_t schema_version,
                                               common::ObArray<uint64_t> &tenant_trigger_ids,
                                               common::ObISQLClient &sql_client,
                                               common::ObIArray<ObTriggerInfo> &trigger_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  trigger_info_array.reserve(tenant_trigger_ids.count());
  LOG_DEBUG("fetch batch triggers begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }

  std::sort(tenant_trigger_ids.begin(), tenant_trigger_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_trigger_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_trigger_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_trigger_info(schema_status, schema_version, tenant_id, sql_client, trigger_info_array,
        &tenant_trigger_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all trigger info failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch trigger info finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_routine_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObRoutineInfo> &routine_array,
    const uint64_t *routine_ids /* = NULL */,
    const int64_t routine_ids_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_ROUTINE_HISTORY_SQL, OB_ALL_ROUTINE_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != routine_ids && routine_ids_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND ROUTINE_ID IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < routine_ids_size; ++i) {
          const uint64_t routine_id = fill_extract_schema_id(schema_status, routine_ids[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu", 0 == i ? "" : ", ", routine_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID ASC, ROUTINE_ID ASC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_routine_schema(tenant_id, *result, routine_array))) {
        LOG_WARN("Failed to retrieve routine infos", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_routine_param_info(const ObRefreshSchemaStatus &schema_status,
                                                         int64_t schema_version,
                                                         uint64_t tenant_id,
                                                         ObISQLClient &sql_client,
                                                         ObIArray<ObRoutineInfo> &routine_infos,
                                                         const uint64_t *object_ids /* = NULL */,
                                                         const int64_t object_ids_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_ROUTINE_PARAM_HISTORY_SQL, OB_ALL_ROUTINE_PARAM_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != object_ids && object_ids_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND ROUTINE_ID IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < object_ids_size; ++i) {
          const uint64_t routine_id = fill_extract_schema_id(schema_status, object_ids[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu", 0 == i ? "" : ", ", routine_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID ASC, ROUTINE_ID ASC, SEQUENCE ASC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_routine_param_schema(tenant_id, *result, routine_infos))) {
        LOG_WARN("Failed to retrieve routine infos", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_udt_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObUDTTypeInfo> &udt_array,
    const uint64_t *udt_ids /* = NULL */,
    const int64_t udt_ids_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_TYPE_HISTORY_SQL, OB_ALL_TYPE_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != udt_ids && udt_ids_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND TYPE_ID IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < udt_ids_size; ++i) {
          const uint64_t type_id = fill_extract_schema_id(schema_status, udt_ids[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu", 0 == i ? "" : ", ", type_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID ASC, TYPE_ID ASC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_udt_schema(tenant_id, *result, udt_array))) {
        LOG_WARN("Failed to retrieve udt infos", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_udt_attr_info(
    const ObRefreshSchemaStatus &schema_status,
    int64_t schema_version,
    uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObUDTTypeInfo> &udt_infos,
    const uint64_t *object_ids /* = NULL */,
    const int64_t object_ids_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_TYPE_ATTR_HISTORY_SQL, OB_ALL_TYPE_ATTR_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != object_ids && object_ids_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND TYPE_ID IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < object_ids_size; ++i) {
          const uint64_t type_id = fill_extract_schema_id(schema_status, object_ids[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu", 0 == i ? "" : ", ", type_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID ASC, TYPE_ID ASC, ATTRIBUTE ASC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_udt_attr_schema(tenant_id, *result, udt_infos))) {
        LOG_WARN("Failed to retrieve udt infos", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_udt_coll_info(
    const ObRefreshSchemaStatus &schema_status,
    int64_t schema_version,
    uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObUDTTypeInfo> &udt_infos,
    const uint64_t *object_ids /* = NULL */,
    const int64_t object_ids_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_COLL_TYPE_HISTORY_SQL, OB_ALL_COLL_TYPE_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != object_ids && object_ids_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND COLL_TYPE_ID IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < object_ids_size; ++i) {
          const uint64_t coll_type_id = fill_extract_schema_id(schema_status, object_ids[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu", 0 == i ? "" : ", ", coll_type_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID ASC, COLL_TYPE_ID ASC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_udt_coll_schema(tenant_id, *result, udt_infos))) {
        LOG_WARN("Failed to retrieve udt infos", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_udt_object_info(
    const ObRefreshSchemaStatus &schema_status,
    int64_t schema_version,
    uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObUDTTypeInfo> &udt_infos,
    const uint64_t *object_ids /* = NULL */,
    const int64_t object_ids_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if ((ObSchemaService::g_liboblog_mode_ && GET_MIN_CLUSTER_VERSION() < CLUSTER_VERSION_2270)
       || (GCONF.in_upgrade_mode() && GET_MIN_CLUSTER_VERSION() < CLUSTER_VERSION_2270)) {
      LOG_WARN("in upgrade mode, table maybe not ready, just skip",
                 K(ret), K(schema_status), K(tenant_id), K(schema_version));
    } else {
      if (OB_FAIL(sql.append_fmt(FETCH_ALL_OBJECT_TYPE_HISTORY_SQL,
                                OB_ALL_TENANT_OBJECT_TYPE_HISTORY_TNAME,
                                fill_extract_tenant_id(schema_status, tenant_id)))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (NULL != object_ids && object_ids_size > 0) {
        if (OB_FAIL(sql.append_fmt(" AND OBJECT_TYPE_ID IN ("))) {
          LOG_WARN("append sql failed", K(ret));
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < object_ids_size; ++i) {
            const uint64_t object_type_id = fill_extract_schema_id(schema_status, object_ids[i]);
            if (OB_FAIL(sql.append_fmt("%s%lu", 0 == i ? "" : ", ", object_type_id))) {
              LOG_WARN("append sql failed", K(ret), K(i));
            }
          }
          if (OB_SUCC(ret)) {
            if (OB_FAIL(sql.append(")"))) {
              LOG_WARN("append sql failed", K(ret));
            }
          }
        }
      } else { }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
          LOG_WARN("append failed", K(ret));
        } else if (OB_FAIL(
          sql.append(" ORDER BY TENANT_ID ASC, OBJECT_TYPE_ID ASC, TYPE ASC, SCHEMA_VERSION DESC"))) {
          LOG_WARN("sql append failed", K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
        DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
        if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
        } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Fail to get result", K(ret));
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_udt_object_schema(tenant_id,
                                                                            *result, udt_infos))) {
          LOG_WARN("Failed to retrieve udt object type infos", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_synonym_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObSynonymInfo> &synonym_array,
    const uint64_t *synonym_keys /* = NULL */,
    const int64_t synonyms_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_SYNONYM_HISTORY_SQL, OB_ALL_SYNONYM_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != synonym_keys && synonyms_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND synonym_id IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < synonyms_size; ++i) {
          const uint64_t synonym_id = fill_extract_schema_id(schema_status, synonym_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu",
                                     0 == i ? "" : ", ",
                                     synonym_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, SYNONYM_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_synonym_schema(tenant_id, *result, synonym_array))) {
        LOG_WARN("Failed to retrieve synonym infos", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_user_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObArray<ObUserInfo> &user_array,
    const uint64_t *user_keys /* = NULL */,
    const int64_t users_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const bool is_full_schema = (NULL != user_keys && users_size > 0) ? false : true;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_USER_HISTORY_SQL, OB_ALL_USER_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (!is_full_schema) {
      if (OB_FAIL(sql.append_fmt(" AND user_id IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < users_size; ++i) {
          const uint64_t user_id = fill_extract_schema_id(schema_status, user_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu",
                                     0 == i ? "" : ", ",
                                     user_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, USER_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_user_schema(tenant_id, *result, user_array))) {
        LOG_WARN("Failed to retrieve user infos", K(ret));
      } else if (!is_full_schema && user_array.count() != users_size) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Failed to retrieve user infos", K(ret), K(user_array.count()), K(users_size));
      }
    }
    if (OB_SUCC(ret)) {
      if (!ObSchemaService::g_liboblog_mode_
          && GCONF.in_upgrade_mode()
          && GET_MIN_CLUSTER_VERSION() < CLUSTER_VERSION_2260) {
        LOG_WARN("in upgrade mode, table maybe not ready, just skip",
                 K(ret), K(schema_status), K(tenant_id), K(schema_version));
      } else if (OB_FAIL(fetch_role_grantee_map_info(schema_status,
              schema_version,
              tenant_id,
              sql_client,
              user_array,
              true /*this case grantees fetch role ids*/,
              user_keys,
              users_size))) {
        LOG_WARN("failed to fetch_role_grantee_map_info", K(ret));
      } else if (OB_FAIL(fetch_role_grantee_map_info(schema_status,
              schema_version,
              tenant_id,
              sql_client,
              user_array,
              false /*this case roles fetch grantee ids*/,
              user_keys,
              users_size))) {
        LOG_WARN("failed to fetch_role_grantee_map_info", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_package_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObPackageInfo> &package_array,
    const uint64_t *package_keys /* = NULL */,
    const int64_t packages_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);

    if (OB_FAIL(
        sql.append_fmt(FETCH_ALL_PACKAGE_HISTORY_SQL, OB_ALL_PACKAGE_HISTORY_TNAME,
        fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != package_keys && packages_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND PACKAGE_ID IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < packages_size; ++i) {
          const uint64_t package_id = fill_extract_schema_id(schema_status, package_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu", 0 == i ? "" : ", ", package_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else {}

    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(
          sql.append(" ORDER BY TENANT_ID DESC, PACKAGE_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_package_schema(tenant_id, *result, package_array))) {
        LOG_WARN("Failed to retrieve package infos", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_trigger_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObTriggerInfo> &trigger_array,
    const uint64_t *trigger_keys /* = NULL */,
    const int64_t triggers_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);

    if (OB_FAIL(
        sql.append_fmt(FETCH_ALL_TRIGGER_HISTORY_SQL, OB_ALL_TENANT_TRIGGER_HISTORY_TNAME,
        fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != trigger_keys && triggers_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND TRIGGER_ID IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < triggers_size; ++i) {
          const uint64_t trigger_id = fill_extract_schema_id(schema_status, trigger_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu", 0 == i ? "" : ", ", trigger_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else {}

    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(
          sql.append(" ORDER BY TENANT_ID DESC, TRIGGER_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_trigger_schema(tenant_id, *result, trigger_array))) {
        LOG_WARN("Failed to retrieve trigger infos", K(ret));
      }
    }
  }
  return ret;
}

// fill ObUserInfo
// 1. If user info is not a role, role_id_array_ means roles the user has.
// 2. If user info is a role, grantee_id_array_means users the role is granted.
int ObSchemaServiceSQLImpl::fetch_role_grantee_map_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObArray<ObUserInfo> &user_array,
    const bool is_fetch_role,
    const uint64_t *user_keys /* = NULL */,
    const int64_t users_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const bool is_full_schema = (NULL != user_keys && users_size > 0) ? false : true;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    bool is_need_inc_fetch = false; // control generation logic of sql
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_ROLE_GRANTEE_MAP_HISTORY_SQL, OB_ALL_TENANT_ROLE_GRANTEE_MAP_HISTORY_TNAME,
                               ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (!is_full_schema) {
      for (int64_t i = 0; OB_SUCC(ret) && i < users_size; ++i) {
        const uint64_t user_id = ObSchemaUtils::get_extract_schema_id(exec_tenant_id, user_keys[i]);
        if (is_fetch_role) {
          if (!is_need_inc_fetch) {
            if (OB_FAIL(sql.append_fmt(" AND grantee_id IN (%lu", user_id))) {
              LOG_WARN("append sql failed", K(ret), K(user_id));
            } else {
              is_need_inc_fetch = true;
            }
          } else if (OB_FAIL(sql.append_fmt(", %lu", user_id))) {
            LOG_WARN("append sql failed", K(ret), K(i), K(user_id));
          }
        } else {
          if (!user_array.at(i).is_role()) {
            // skip, user_array.at(i) is user, it's no need to fetch grantee ids
          } else {
            if (!is_need_inc_fetch) {
              if (OB_FAIL(sql.append_fmt(" AND role_id IN (%lu", user_id))) {
                LOG_WARN("append sql failed", K(ret), K(user_id));
              } else {
                is_need_inc_fetch = true;
              }
            } else if (OB_FAIL(sql.append_fmt(", %lu", user_id))) {
              LOG_WARN("append sql failed", K(ret), K(i), K(user_id));
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        if (is_need_inc_fetch) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        } else {
          // reset sql if no schema objects need to be fetched from inner table.
          sql.reset();
        }
      }
    }

    if (OB_SUCC(ret) && !sql.empty()) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(is_fetch_role ? " ORDER BY TENANT_ID DESC, GRANTEE_ID DESC, ROLE_ID DESC, SCHEMA_VERSION DESC"
                                   : " ORDER BY TENANT_ID DESC, ROLE_ID DESC, GRANTEE_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }

    if (OB_SUCC(ret) && !sql.empty()) {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_role_grantee_map_schema(tenant_id, *result, is_fetch_role, user_array))) {
        LOG_WARN("Failed to retrieve user infos", K(ret));
      }
    }

  }
  return ret;
}

//FIXME@xiyu: to add fetch_tenants
int ObSchemaServiceSQLImpl::fetch_tenants(
    ObISQLClient &sql_client,
    const int64_t schema_version,
    ObIArray<ObSimpleTenantSchema> &schema_array,
    const SchemaKey *schema_keys,
    const int64_t schema_key_size)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_TENANT_HISTORY_SQL3, OB_ALL_TENANT_HISTORY_TNAME))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != schema_keys && schema_key_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND (tenant_id) in"))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(SQL_APPEND_TENANT_ID(schema_keys, schema_key_size, sql))) {
        LOG_WARN("sql append tenant id failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      DEFINE_SQL_CLIENT_RETRY_WEAK(sql_client);
      if (OB_FAIL(sql.append(" ORDER BY tenant_id desc, schema_version desc"))) {
        LOG_WARN("sql append failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_tenant_schema(sql_client_retry_weak, *result, schema_array))) {
        LOG_WARN("failed to retrieve tenant schema", K(ret));
      }
    }

  }
  return ret;
}

/*
 * System variable schema is stripped from tenant schema since ver 2.2.0/2.2.1.
 * In the original implementation, we fetch max system variable schema version from __all_ddl_operation,
 * which it's low performance if __all_ddl_operation contains lots of records.
 * For compatibility and performance, we fetch max system variable schema version from different tables in different versions.
 * case 1. Before ver 2.2.20, we can fetch max system variable schema version from __all_tenant_history.
 * case 2. Since ver 2.2.20, we can fetch max system variable schema version from __all_sys_variable_history.
 * case 3. After schema split in ver 2.2.0/2.2.1, we can fetch max system variable schema version from __all_sys_variable_history.
 */
int ObSchemaServiceSQLImpl::fetch_sys_variable_version(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const int64_t schema_version,
    int64_t &fetch_schema_version)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    fetch_schema_version = OB_INVALID_VERSION;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt("SELECT max(schema_version) as max_schema_version "
                               "FROM %s WHERE tenant_id = %lu AND schema_version <= %ld",
                               OB_ALL_SYS_VARIABLE_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id),
                               schema_version))) {
      LOG_WARN("append sql failed", K(ret), K(sql));
    } else {
      const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
      bool check_sys_variable = false;
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_PARAMETER(sql_client, snapshot_timestamp, check_sys_variable);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END != ret) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("empty row", K(ret), K(tenant_id), K(schema_version));
        } else {
          LOG_WARN("fail to fetch next row", K(ret));
        }
      } else {
        EXTRACT_INT_FIELD_MYSQL(*result, "max_schema_version", fetch_schema_version, uint64_t);
        if (fetch_schema_version > schema_version) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected query result", K(ret),
                   K(fetch_schema_version), K(schema_version), K(tenant_id));
        }
      }
    }

  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_tables(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObIArray<ObSimpleTableSchemaV2> &schema_array,
    const SchemaKey *schema_keys,
    const int64_t schema_key_size)
{
  int ret = OB_SUCCESS;
  ObMySQLResult *result = NULL;
  ObSqlString sql;
  bool is_increase_schema = (NULL != schema_keys && schema_key_size > 0);
  const int64_t orig_cnt = schema_array.count();
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  const char *table_name = NULL;
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail", K(ret));
  } else if (OB_FAIL(ObSchemaUtils::get_all_table_history_name(exec_tenant_id,
                                                               table_name,
                                                               schema_service_))) {
    LOG_WARN("fail to get all table name", K(ret), K(exec_tenant_id));
  } else if (!is_increase_schema) {
    /* no_rewrite() hint for the following sql stmt is needed since ver 2.2.1.
     * Otherwise, the following sql stmt will run in low performance.
     * To avoid possible problem (no_rewrite() hint may cause error in ver 1.4.21),
     * no_rewrite() hint is only added since ver 2.2.1.
     */
    const char *addition_hint = GET_MIN_CLUSTER_VERSION() < CLUSTER_VERSION_2210 ?
                                "" : " no_rewrite() ";
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_TABLE_HISTORY_FULL_SCHEMA,
                addition_hint, table_name, table_name,
                fill_extract_tenant_id(schema_status, tenant_id),
                schema_version,
                fill_extract_schema_id(schema_status, OB_ALL_CORE_TABLE_TID)))) {
      LOG_WARN("append sql failed", K(ret));
    }
  } else {
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_TABLE_HISTORY_SQL3, table_name,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND table_id in"))) {
      LOG_WARN("append failed", K(ret));
    } else if (OB_FAIL(SQL_APPEND_SCHEMA_ID(table, schema_keys, schema_key_size, sql))) {
      LOG_WARN("sql append table id failed", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql.append(" ORDER BY tenant_id desc, table_id desc, schema_version desc"))) {
        LOG_WARN("sql append failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_table_schema(tenant_id, *result, schema_array))) {
        LOG_WARN("failed to retrieve table schema", K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObArray<uint64_t> table_ids;
    ObArray<ObSimpleTableSchemaV2 *> tables;
    for (int64_t i = orig_cnt; OB_SUCC(ret) && i < schema_array.count(); ++i) {
      const uint64_t table_id = schema_array.at(i).get_table_id();
      if (OB_FAIL(table_ids.push_back(table_id))) {
        LOG_WARN("fail to push back table_id", KR(ret), K(table_id));
      } else if (OB_FAIL(tables.push_back(&schema_array.at(i)))) {
        LOG_WARN("fail to push back table schema", KR(ret), K(table_id));
      }
    }
    if (OB_SUCC(ret) && table_ids.count() > 0) {
      LOG_TRACE("build table_ids", KR(ret), K(orig_cnt), "table_ids_cnt", table_ids.count(), K(table_ids));
      const int64_t BATCH_FETCH_NUM = 100;
      int64_t begin = 0;
      int64_t end = min(begin + BATCH_FETCH_NUM, table_ids.count());
      // fetch table schema from the range [begin, end) of table_ids.
      while (OB_SUCC(ret) && end - begin > 0 && end <= table_ids.count()) {
        LOG_TRACE("batch table_ids", KR(ret), K(begin), K(end), K(schema_version), K(table_ids.at(begin)));
        if (OB_FAIL(fetch_all_partition_info(
                    schema_status, schema_version, tenant_id, sql_client,
                    tables, &table_ids.at(begin), end - begin))) {
          LOG_WARN("Failed to fetch all partition info",
                   KR(ret), K(schema_status), K(schema_version), K(begin), K(end));
        } else if (OB_FAIL(fetch_foreign_key_array_for_simple_table_schemas(
                   schema_status, schema_version, tenant_id, sql_client,
                   tables, &table_ids.at(begin), end - begin))) {
          LOG_WARN("Failed to fetch foreign key array info for simple table schemas",
                   KR(ret), K(schema_status), K(schema_version), K(begin), K(end));
        } else if (OB_FAIL(fetch_constraint_array_for_simple_table_schemas(
                   schema_status, schema_version, tenant_id, sql_client,
                   tables, &table_ids.at(begin), end - begin))) {
          LOG_WARN("Failed to fetch constraint array info for simple table schemas",
                   KR(ret), K(schema_status), K(schema_version), K(begin), K(end));
        } else {
          begin = end;
          end = min(begin + BATCH_FETCH_NUM, table_ids.count());
        }
      }

      if (FAILEDx(sort_tables_partition_info(tables))) {
        LOG_WARN("fail to sort tables partition info", KR(ret), K(tenant_id));
      }
    }
  }
  return ret;
}

#define FETCH_SCHEMAS_FUNC_DEFINE(SCHEMA, SCHEMA_TYPE, table_name_str)  \
  int ObSchemaServiceSQLImpl::fetch_##SCHEMA##s(                        \
      ObISQLClient &sql_client,                                         \
      const ObRefreshSchemaStatus &schema_status,                       \
      const int64_t schema_version,                                     \
      const uint64_t tenant_id,                                         \
      ObIArray<SCHEMA_TYPE> &schema_array,                              \
      const SchemaKey *schema_keys,                                     \
      const int64_t schema_key_size)                                    \
  {                                                                     \
    int ret = OB_SUCCESS;                                               \
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {                         \
      ObMySQLResult *result = NULL;                                       \
      ObSqlString sql;                                                    \
      const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_; \
      const char *sql_str_fmt = "SELECT * FROM %s WHERE tenant_id = %lu"; \
      const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status); \
      if (OB_FAIL(sql.append_fmt(sql_str_fmt, table_name_str, \
                                 fill_extract_tenant_id(schema_status, tenant_id)))) {  \
        LOG_WARN("append sql failed", K(ret));                            \
      } else if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) { \
        LOG_WARN("append sql failed", K(ret));                                            \
      } else if (NULL != schema_keys && schema_key_size > 0) {              \
        if (OB_FAIL(sql.append_fmt(" AND "#SCHEMA"_id in"))) {              \
          LOG_WARN("append failed", K(ret));                                \
        } else if (OB_FAIL(SQL_APPEND_SCHEMA_ID(SCHEMA, schema_keys, schema_key_size, sql))) { \
          LOG_WARN("sql append "#SCHEMA" id failed", K(ret));                           \
        }                                                                               \
      }                                                                                 \
      if (OB_SUCC(ret)) {                                                               \
        DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);     \
        if (OB_FAIL(sql.append(" ORDER BY tenant_id desc, "#SCHEMA"_id desc,                              \
                               schema_version desc"))) {                                                  \
          LOG_WARN("sql append failed", K(ret));                                                          \
        } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {                 \
          LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));                                   \
        } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {                                    \
          ret = OB_ERR_UNEXPECTED;                                                                        \
          LOG_WARN("fail to get result. ", K(ret));                                                       \
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_##SCHEMA##_schema(tenant_id, *result, schema_array))) {   \
          LOG_WARN("failed to retrieve "#SCHEMA" schema", K(ret));                                        \
        } else {                                                                                          \
          LOG_DEBUG("finish fetch schema", K(sql.string()), K(tenant_id), K(schema_array));               \
        }                                                                                                 \
      }                                                                                                   \
    }                                                                                                     \
    return ret;                                                                                           \
  }

FETCH_SCHEMAS_FUNC_DEFINE(user, ObSimpleUserSchema, OB_ALL_USER_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(database, ObSimpleDatabaseSchema, OB_ALL_DATABASE_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(tablegroup, ObSimpleTablegroupSchema, OB_ALL_TABLEGROUP_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(outline, ObSimpleOutlineSchema, OB_ALL_OUTLINE_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(synonym, ObSimpleSynonymSchema, OB_ALL_SYNONYM_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(package, ObSimplePackageSchema, OB_ALL_PACKAGE_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(routine, ObSimpleRoutineSchema, OB_ALL_ROUTINE_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(trigger, ObSimpleTriggerSchema, OB_ALL_TENANT_TRIGGER_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(sequence, ObSequenceSchema, OB_ALL_SEQUENCE_OBJECT_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(keystore, ObKeystoreSchema, OB_ALL_TENANT_KEYSTORE_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(label_se_policy, ObLabelSePolicySchema, OB_ALL_TENANT_OLS_POLICY_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(label_se_component, ObLabelSeComponentSchema, OB_ALL_TENANT_OLS_COMPONENT_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(label_se_label, ObLabelSeLabelSchema, OB_ALL_TENANT_OLS_LABEL_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(label_se_user_level, ObLabelSeUserLevelSchema, OB_ALL_TENANT_OLS_USER_LEVEL_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(tablespace, ObTablespaceSchema, OB_ALL_TENANT_TABLESPACE_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(profile, ObProfileSchema, OB_ALL_TENANT_PROFILE_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(audit, ObSAuditSchema, OB_ALL_TENANT_SECURITY_AUDIT_HISTORY_TNAME);
//FETCH_SCHEMAS_FUNC_DEFINE(sys_priv, ObSysPriv, OB_ALL_TENANT_SYSAUTH_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(dblink, ObDbLinkSchema, OB_ALL_DBLINK_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(directory, ObDirectorySchema, OB_ALL_TENANT_DIRECTORY_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(context, ObContextSchema, OB_ALL_CONTEXT_HISTORY_TNAME);
FETCH_SCHEMAS_FUNC_DEFINE(mock_fk_parent_table, ObSimpleMockFKParentTableSchema, OB_ALL_MOCK_FK_PARENT_TABLE_HISTORY_TNAME);

int ObSchemaServiceSQLImpl::fetch_all_mock_fk_parent_table_info(
      const ObRefreshSchemaStatus &schema_status,
      const int64_t schema_version,
      const uint64_t tenant_id,
      ObISQLClient &sql_client,
      ObIArray<ObMockFKParentTableSchema> &schema_array,
      const uint64_t *schema_keys,
      const int64_t schema_key_size)
{
  int ret = OB_SUCCESS;
  const int64_t orig_cnt = schema_array.count();
  // fetch table_info for mock_fk_parent_tables
  {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      ObSqlString sql;
      const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
      const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql.append_fmt(FETCH_ALL_TENANT_MOCK_FK_PARENT_TABLE_HISTORY_SQL, OB_ALL_MOCK_FK_PARENT_TABLE_HISTORY_TNAME,
                                 fill_extract_tenant_id(schema_status, tenant_id)))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (NULL != schema_keys && schema_key_size > 0) {
        if (OB_FAIL(sql.append_fmt(" AND mock_fk_parent_table_id in ("))) {
          LOG_WARN("append failed", K(ret));
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < schema_key_size; ++i) {
            const uint64_t mock_fk_parent_table_id = fill_extract_schema_id(schema_status, schema_keys[i]);
            if (OB_FAIL(sql.append_fmt("%s%lu", 0 == i ? "" : ", ", mock_fk_parent_table_id))) {
              LOG_WARN("append sql failed", K(ret), K(i));
            }
          }
          if (OB_SUCC(ret)) {
            if (OB_FAIL(sql.append(")"))) {
              LOG_WARN("append sql failed", K(ret));
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
          LOG_WARN("append sql failed", K(ret));
        } else if (OB_FAIL(sql.append(" ORDER BY tenant_id desc, mock_fk_parent_table_id desc, schema_version desc"))) {
          LOG_WARN("sql append failed", K(ret));
        } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
        } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fail to get result. ", K(ret));
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_mock_fk_parent_table_schema(tenant_id, *result, schema_array))) {
          LOG_WARN("failed to retrieve mock_fk_parent_table schema", K(ret));
        }
      }
    }
  }
  // fetch column_info for mock_fk_parent_tables
  if (OB_SUCC(ret)) {
    for (int64_t i = orig_cnt; OB_SUCC(ret) && i < schema_array.count(); ++i) {
      if (OB_FAIL(fetch_mock_fk_parent_table_column_info(
                  schema_status, tenant_id, schema_version, sql_client, schema_array.at(i)))) {
        LOG_WARN("failed to fetch mock_table column info", K(ret), K(schema_array.at(i)));
      }
    }
  }
  // fetch foreign_key_info for mock_fk_parent_tables
  if (OB_SUCC(ret)) {
    for (int64_t i = orig_cnt; OB_SUCC(ret) && i < schema_array.count(); ++i) {
      if (OB_FAIL(fetch_foreign_key_info(
          schema_status, tenant_id, schema_array.at(i).get_mock_fk_parent_table_id(),
          schema_version, sql_client, schema_array.at(i)))) {
        LOG_WARN("Failed to fetch foreign key info", K(ret), K(schema_array.at(i)));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_mock_fk_parent_table_column_info(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const int64_t schema_version,
    common::ObISQLClient &sql_client,
    ObMockFKParentTableSchema &mock_fk_parent_table)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
    if (OB_FAIL(sql.append_fmt(COMMON_SQL_WITH_TENANT,
                               OB_ALL_MOCK_FK_PARENT_TABLE_COLUMN_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(
      " AND mock_fk_parent_table_id = %lu AND schema_version <= %ld",
      fill_extract_schema_id(schema_status, mock_fk_parent_table.get_mock_fk_parent_table_id()),
      schema_version))) {
      LOG_WARN("append table_id and mock_fk_parent_table_id and schema version",
               K(ret), K(mock_fk_parent_table.get_mock_fk_parent_table_id()), K(schema_version));
    } else if (OB_FAIL(sql.append(" ORDER BY parent_column_id asc, schema_version desc"))) {
      LOG_WARN("sql append failed", K(ret));
    } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
      LOG_WARN("execute sql failed", K(sql), K(ret));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to get result. ", K(ret));
    } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_mock_fk_parent_table_schema_column(
                       tenant_id, *result, mock_fk_parent_table))) {
      LOG_WARN("failed to retrieve foreign key column info", K(ret), K(mock_fk_parent_table));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_udts(
      ObISQLClient &sql_client,
      const ObRefreshSchemaStatus &schema_status,
      const int64_t schema_version,
      const uint64_t tenant_id,
      ObIArray<ObSimpleUDTSchema> &schema_array,
      const SchemaKey *schema_keys,
      const int64_t schema_key_size)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const char *sql_str_fmt = "SELECT * FROM %s WHERE tenant_id = %lu";
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(sql_str_fmt, OB_ALL_TYPE_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != schema_keys && schema_key_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND type_id in"))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(SQL_APPEND_SCHEMA_ID(udt, schema_keys, schema_key_size, sql))) {
        LOG_WARN("sql append type id failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql.append(" ORDER BY tenant_id desc, type_id desc, schema_version desc"))) {
        LOG_WARN("sql append failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_udt_schema(tenant_id, *result, schema_array))) {
        LOG_WARN("failed to retrieve udt schema", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_db_privs(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObIArray<ObDBPriv> &schema_array,
    const SchemaKey *schema_keys,
    const int64_t schema_key_size)
{
  int ret = OB_SUCCESS;

  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_DB_PRIV_HISTORY_SQL,
                               OB_ALL_DATABASE_PRIVILEGE_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != schema_keys && schema_key_size > 0) {
      // database_name is case sensitive
      if (OB_FAIL(sql.append_fmt(" AND (tenant_id, user_id, BINARY database_name) in"))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(SQL_APPEND_DB_PRIV_ID(schema_keys, tenant_id, schema_key_size, sql))) {
        LOG_WARN("sql append db priv id failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql.append(" ORDER BY tenant_id desc, user_id desc, \
                             BINARY database_name desc, schema_version desc"))) {
        LOG_WARN("sql append failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_db_priv_schema(tenant_id, *result, schema_array))) {
        LOG_WARN("failed to retrieve db priv", K(ret));
      }
    }

  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_sys_privs(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObIArray<ObSysPriv> &schema_array,
    const SchemaKey *schema_keys,
    const int64_t schema_key_size)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_SYS_PRIV_HISTORY_SQL,
                               OB_ALL_TENANT_SYSAUTH_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != schema_keys && schema_key_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND (tenant_id, grantee_id) in"))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(SQL_APPEND_SYS_PRIV_ID(schema_keys, tenant_id, schema_key_size, sql))) {
        LOG_WARN("sql append sys priv id failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql.append(
        " ORDER BY tenant_id desc, grantee_id desc, priv_id desc, schema_version desc"))) {
        LOG_WARN("sql append failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_sys_priv_schema(tenant_id,
                                                                         *result,
                                                                         schema_array))) {
        LOG_WARN("failed to retrieve sys priv", K(ret));
      }
    }

  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_table_privs(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObIArray<ObTablePriv> &schema_array,
    const SchemaKey *schema_keys,
    const int64_t schema_key_size)
{
  int ret = OB_SUCCESS;

  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_TABLE_PRIV_HISTORY_SQL, OB_ALL_TABLE_PRIVILEGE_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != schema_keys && schema_key_size > 0) {
      // database_name/table_name is case sensitive
      if (OB_FAIL(sql.append_fmt(" AND (tenant_id, user_id, BINARY database_name, \
                                        BINARY table_name) in"))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(SQL_APPEND_TABLE_PRIV_ID(schema_keys, tenant_id, schema_key_size, sql))) {
        LOG_WARN("sql append table priv id failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql.append(" ORDER BY tenant_id desc, user_id desc, \
                             BINARY database_name desc, BINARY table_name desc, schema_version desc"))) {
        LOG_WARN("sql append failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_table_priv_schema(tenant_id, *result, schema_array))) {
        LOG_WARN("failed to retrieve table priv schema", K(ret));
      }
    }

  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_obj_privs(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObIArray<ObObjPriv> &schema_array,
    const SchemaKey *schema_keys,
    const int64_t schema_key_size)
{
  int ret = OB_SUCCESS;

  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_OBJ_PRIV_HISTORY_SQL, OB_ALL_TENANT_OBJAUTH_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != schema_keys && schema_key_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND (tenant_id, obj_id, objtype, col_id, grantor_id, \
                                        grantee_id) in"))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(SQL_APPEND_OBJ_PRIV_ID(schema_keys, tenant_id, schema_key_size, sql))) {
        LOG_WARN("sql append obj priv id failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql.append(" ORDER BY tenant_id desc, obj_id desc, objtype desc, col_id desc,\
                              grantor_id desc, grantee_id desc, priv_id desc,\
                              schema_version desc"))) {
        LOG_WARN("sql append failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_obj_priv_schema(tenant_id,
                                                *result, schema_array))) {
        LOG_WARN("failed to retrieve obj priv schema", K(ret));
      }
    }

  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_udfs(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObIArray<ObSimpleUDFSchema> &schema_array,
    const SchemaKey *schema_keys,
    const int64_t schema_key_size)
{
  int ret = OB_SUCCESS;

  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_FUNC_HISTORY_SQL, OB_ALL_FUNC_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != schema_keys && schema_key_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND (name) in"))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(SQL_APPEND_UDF_ID(schema_keys, exec_tenant_id, schema_key_size, sql))) {
        LOG_WARN("sql append udf name failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql.append(" ORDER BY tenant_id desc, name desc,\
                               schema_version desc"))) {
        LOG_WARN("sql append failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_udf_schema(tenant_id, *result, schema_array))) {
        LOG_WARN("failed to retrieve udf", K(ret));
      }
    }
  }
  return ret;
}

/*
  new schema_cache related
*/

int ObSchemaServiceSQLImpl::get_table_schema(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t table_id,
    const int64_t schema_version,
    ObISQLClient &sql_client,
    ObIAllocator &allocator,
    ObTableSchema *&table_schema)
{
  int ret = OB_SUCCESS;

  // __all_core_table
  if (OB_ALL_CORE_TABLE_TID == table_id) {
    ObTableSchema all_core_table_schema;
    if (OB_FAIL(get_all_core_table_schema(all_core_table_schema))) {
      LOG_WARN("get all core table schema failed", K(ret));
    } else if (OB_FAIL(alloc_table_schema(all_core_table_schema, allocator,
                                          table_schema))) {
      LOG_WARN("alloc table schema failed", K(ret));
    } else {
      LOG_INFO("get all core table schema succeed", K(table_schema->get_table_name_str()));
    }
  } else {
    // normal_table(contain core table)
    if (OB_FAIL(get_not_core_table_schema(schema_status, table_id, schema_version,
                                          sql_client, allocator, table_schema))) {
      LOG_WARN("get not core table schema failed", K(ret), K(table_id));
    } else {
      LOG_INFO("get not core table schema succeed",
               K(schema_status), K(table_id), K(table_schema->get_table_name_str()));
    }
  }

  return ret;
}

int ObSchemaServiceSQLImpl::get_not_core_table_schema(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t table_id,
    const int64_t schema_version,
    ObISQLClient &sql_client,
    ObIAllocator &allocator,
    ObTableSchema *&table_schema)
{
  int ret = OB_SUCCESS;

  const uint64_t tenant_id = schema_status.tenant_id_;
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail, ", K(ret));
  } else if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid schema_version", K(schema_version), K(ret));
  } else if (OB_FAIL(fetch_table_info(schema_status, tenant_id, table_id, schema_version,
                                      sql_client, allocator, table_schema))) {
    LOG_WARN("fetch all table info failed", K(tenant_id), K(table_id),
             K(schema_version), K(ret));
  } else if (table_schema->is_view_table()) {
    // do-nothing
  } else if (OB_FAIL(fetch_column_info(schema_status, tenant_id, table_id, schema_version,
                                       sql_client, table_schema))) {
    LOG_WARN("Failed to fetch column info", K(ret));
  } else if (OB_FAIL(fetch_partition_info(schema_status, tenant_id, table_id, schema_version,
                                         sql_client, table_schema))) {
    LOG_WARN("Failed to fetch part info", K(ret));
  }
  if (OB_SUCCESS == ret) {
    if (OB_FAIL(fetch_foreign_key_info(schema_status, tenant_id, table_id,
                                       schema_version, sql_client, *table_schema))) {
      LOG_WARN("Failed to fetch foreign key info", K(ret));
      if (-ER_NO_SUCH_TABLE == ret && ObSchemaService::g_liboblog_mode_) {
        LOG_WARN("liboblog mode, ignore foreign key info schema table NOT EXIST error",
            K(ret), K(schema_version), K(tenant_id), K(table_id),
            "table_name", table_schema->get_table_name());
        ret = OB_SUCCESS;
      }
    }
  }
  if (OB_SUCC(ret)
      && OB_NOT_NULL(table_schema)
      && !is_inner_table(table_schema->get_table_id())) {
    ObArray<ObTableSchema *>table_schemas;
    if (OB_FAIL(table_schemas.push_back(table_schema))) {
      LOG_WARN("fail to push table schema", K(ret));
    } else if (OB_FAIL(fetch_all_encrypt_info(schema_status, schema_version,
        tenant_id, sql_client, table_schemas, table_schemas.count()))) {
      LOG_WARN("fail to fetch encrypt info", K(ret));
    }
  }
  if (OB_SUCCESS == ret) {
    if (OB_FAIL(fetch_constraint_info(schema_status, tenant_id, table_id,
                                      schema_version, sql_client, table_schema))) {
      LOG_WARN("Failed to fetch constraints info", K(ret));
      if (-ER_NO_SUCH_TABLE == ret && ObSchemaService::g_liboblog_mode_) {
        LOG_WARN("liboblog mode, ignore constraint info schema table NOT EXIST error",
            K(ret), K(schema_version), K(tenant_id), K(table_id),
            "table_name", table_schema->get_table_name());
        ret = OB_SUCCESS;
      }
    }
  }
  if (OB_SUCCESS == ret) {
    if (!ObSchemaService::g_liboblog_mode_ &&
        GCONF.in_upgrade_mode() &&
        GET_MIN_CLUSTER_VERSION() < CLUSTER_VERSION_2220) {
      LOG_WARN("skip fetch trigger list while upgrade in process from 221 to 222", K(tenant_id));
    } else if (OB_FAIL(fetch_trigger_list(schema_status, tenant_id, table_id,
                                          schema_version, sql_client, *table_schema))) {
      LOG_WARN("Failed to fetch trigger list", K(ret));
      if (-ER_NO_SUCH_TABLE == ret && ObSchemaService::g_liboblog_mode_) {
        LOG_WARN("liboblog mode, ignore constraint info schema table NOT EXIST error",
                 K(ret), K(schema_version), K(tenant_id), K(table_id),
                 "table_name", table_schema->get_table_name());
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}


int ObSchemaServiceSQLImpl::fetch_table_info(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const uint64_t table_id,
    const int64_t schema_version,
    ObISQLClient &sql_client,
    ObIAllocator &allocator,
    ObTableSchema *&table_schema)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  const char *table_name = NULL;
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail", K(ret));
  } else if (OB_FAIL(ObSchemaUtils::get_all_table_history_name(exec_tenant_id,
                                                               table_name,
                                                               schema_service_))) {
    LOG_WARN("fail to get all table name", K(ret), K(exec_tenant_id));
  } else if (OB_FAIL(sql.append_fmt(FETCH_ALL_TABLE_HISTORY_SQL, table_name,
                     fill_extract_tenant_id(schema_status, tenant_id)))) {
    LOG_WARN("append sql failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt(" AND table_id = %lu and schema_version <= %ld order by schema_version desc limit 1",
                                    fill_extract_schema_id(schema_status, table_id),
                                    schema_version))) {
    LOG_WARN("append table_id and schema_version failed", K(ret), K(table_id),
             K(schema_version));
  } else {
    const bool check_deleted = true;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_table_schema(
                 tenant_id, check_deleted, *result, allocator, table_schema))) {
        LOG_WARN("failed to retrieve all table schema:", K(check_deleted), K(ret));
      }
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(fetch_temp_table_schema(schema_status, tenant_id, sql_client, *table_schema))) {
      LOG_WARN("failed to fill temp table schema:", K(ret));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_column_info(const ObRefreshSchemaStatus &schema_status,
                                              const uint64_t tenant_id,
                                              const uint64_t table_id,
                                              const int64_t schema_version,
                                              ObISQLClient &sql_client,
                                              ObTableSchema *&table_schema)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_COLUMN_HISTORY_SQL, OB_ALL_COLUMN_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND table_id = %lu and schema_version <= %ld"
                                      " ORDER BY TENANT_ID, TABLE_ID, COLUMN_ID, SCHEMA_VERSION",
                                      fill_extract_schema_id(schema_status, table_id),
                                      schema_version))) {
      LOG_WARN("append sql failed", K(ret));
    } else {
      const bool check_deleted = true;
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(sql), K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_column_schema(
                 tenant_id, check_deleted, *result, table_schema))) {
        LOG_WARN("failed to retrieve all column schema", K(check_deleted), K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_constraint_info(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const uint64_t table_id,
    const int64_t schema_version,
    ObISQLClient &sql_client,
    ObTableSchema *&table_schema)
{
  int ret = OB_SUCCESS;
  if (OB_SUCC(ret)) {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      ObSqlString sql;
      const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
      const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
      if (OB_FAIL(sql.append_fmt(FETCH_ALL_CONSTRAINT_HISTORY_SQL, OB_ALL_CONSTRAINT_HISTORY_TNAME,
                                 fill_extract_tenant_id(schema_status, tenant_id)))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql.append_fmt(" AND table_id = %lu and schema_version <= %ld",
                                        fill_extract_schema_id(schema_status, table_id),
                                        schema_version))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        const bool check_deleted = true;
        DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
        if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fail to get result. ", K(sql), K(ret));
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_constraint(
                   tenant_id, check_deleted, *result, table_schema))) {
          LOG_WARN("failed to retrieve all constraint schema", K(check_deleted), K(ret));
        }
      }
    }
  }
  for (ObTableSchema::constraint_iterator iter =
         table_schema->constraint_begin_for_non_const_iter();
       OB_SUCC(ret) && iter != table_schema->constraint_end_for_non_const_iter();
       ++iter) {
    if (OB_FAIL(fetch_constraint_column_info(
                schema_status, tenant_id, table_id, schema_version,
                sql_client, *iter))) {
      LOG_WARN("failed to fetch constraint column info", K(ret));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_constraint_column_info(const ObRefreshSchemaStatus &schema_status,
                                                         const uint64_t tenant_id,
                                                         const uint64_t table_id,
                                                         const int64_t schema_version,
                                                         common::ObISQLClient &sql_client,
                                                         ObConstraint *&cst)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);

    if (OB_FAIL(sql.append_fmt(FETCH_ALL_TENANT_CONSTRAINT_COLUMN_HISTORY_SQL, OB_ALL_TENANT_CONSTRAINT_COLUMN_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND table_id = %lu AND constraint_id = %lu AND schema_version <= %ld ORDER BY schema_version desc",
                                      fill_extract_schema_id(schema_status, table_id),
                                      cst->get_constraint_id(),
                                      schema_version))) {
      LOG_WARN("append table_id and foreign key id and schema version",
               K(ret), K(table_id), K(cst->get_constraint_id()), K(schema_version));
    } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
      if ((OB_TABLE_NOT_EXIST == ret) && (GET_MIN_CLUSTER_VERSION() < CLUSTER_VERSION_3100)) {
        LOG_INFO("cluster version is earlier than 3.1.0, ignore OB_TABLE_NOT_EXIST",
                 K(ret), K(GET_MIN_CLUSTER_VERSION()), K(sql));
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("execute sql failed", K(ret), K(GET_MIN_CLUSTER_VERSION()), K(sql));
      }
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to get result. ", K(ret));
    } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_constraint_column_info(tenant_id, *result, cst))) {
      LOG_WARN("failed to retrieve constraint column info", K(ret), K(cst->get_constraint_name_str()));
    }
  }

  return ret;
}

int ObSchemaServiceSQLImpl::fetch_partition_info(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const uint64_t table_id,
    const int64_t schema_version,
    ObISQLClient &sql_client,
    ObTableSchema *&table_schema)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Table schme should not be NULL", K(ret));
  } else if (OB_FAIL(fetch_part_info(schema_status, tenant_id, table_id,
                                     schema_version, sql_client, table_schema))) {
    LOG_WARN("fail to fetch part info", K(ret), K(schema_status),
             K(tenant_id), K(table_id), K(schema_version));
  } else if (OB_FAIL(fetch_sub_part_info(schema_status, tenant_id, table_id,
                                         schema_version, sql_client, table_schema))) {
    LOG_WARN("fail to fetch sub part info", K(ret), K(schema_status),
             K(tenant_id), K(table_id), K(schema_version));
  } else if (OB_FAIL(sort_table_partition_info(*table_schema))) {
    LOG_WARN("fail to generate table partition info for compatible", K(ret), KPC(table_schema));
  }
  return ret;
}

/*
 * The function is implemented as follows:
 * case 1.
 * If user table meets all the following conditions, we may get table schema with empty partition_array or subpartition array.
 * - User table is created before ver 2.0.
 * - Partition related ddl is never executed on this user table.
 * - User table's partition/subpartition is hash like.
 * In such situation, missed part_id/subpart_id must start with 0, so we can mock partition_array/subpartition_array simply.
 *
 * case 2.
 * If system table is partitioned, its partition/subpartition must be hash like.
 * In addition, system table is not allowed to execute partition related ddl.
 * In such situation, missed part_id/subpart_id must start with 0,
 * so we can mock partition_array/subpartition_array simply.
 *
 * case 3.
 * If partitioned table's partition_array/subpartition_array is not empty, such array should be sorted by part type and value.
 */
template<typename SCHEMA>
int ObSchemaServiceSQLImpl::sort_table_partition_info(
    SCHEMA &table_schema)
{
  int ret = OB_SUCCESS;
  bool is_oracle_mode = false;
  if (!table_schema.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid table_schema", KR(ret), K(table_schema));
  } else if (OB_FAIL(table_schema.check_if_oracle_compat_mode(is_oracle_mode))) {
    LOG_WARN("fail to check oracle mode", KR(ret),
             "tenant_id", table_schema.get_tenant_id(), "table_id", table_schema.get_table_id());
  } else {
    // Value comparsion is differ from mysql and oracle. eg:
    // mysql: min < null < other < max
    // oracle: min < other < null < max
    // To make sorted result stable, compat guard should be used.
    lib::CompatModeGuard g(is_oracle_mode ?
                      lib::Worker::CompatMode::ORACLE :
                      lib::Worker::CompatMode::MYSQL);
    if (OB_FAIL(sort_partition_array(table_schema))) {
      LOG_WARN("failed to sort partition array", KR(ret), K(table_schema));
    } else if (OB_FAIL(try_mock_partition_array(table_schema))) {
      LOG_WARN("fail to mock partition array", KR(ret), K(table_schema));
    } else if (OB_FAIL(sort_subpartition_array(table_schema))) {
      LOG_WARN("failed to sort subpartition array", KR(ret), K(table_schema));
    }
  }
  LOG_TRACE("fetch partition info", KR(ret), K(table_schema));
  return ret;
}

template<typename SCHEMA>
int ObSchemaServiceSQLImpl::try_mock_partition_array(
    SCHEMA &table_schema)
{
  int ret = OB_SUCCESS;
  if (!table_schema.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid table_schema", KR(ret), K(table_schema));
  } else if (OB_NOT_NULL(table_schema.get_part_array())) {
    // skip
  } else if (is_virtual_table(table_schema.get_table_id())
             && PARTITION_LEVEL_ONE == table_schema.get_part_level()) {
    // only mock vtable's partition array after 4.0
    if (OB_FAIL(table_schema.mock_list_partition_array())) {
      LOG_WARN("fail to mock list partition array", KR(ret), K(table_schema));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::sort_tablegroup_partition_info(ObTablegroupSchema &tablegroup_schema)
{
  int ret = OB_SUCCESS;
  bool is_oracle_mode = false;
  if (OB_FAIL(tablegroup_schema.check_if_oracle_compat_mode(is_oracle_mode))) {
    LOG_WARN("fail to check oracle mode", KR(ret),
             "tenant_id", tablegroup_schema.get_tenant_id(),
             "tablegroup_id", tablegroup_schema.get_tablegroup_id());
  } else {
    // Value comparsion is differ from mysql and oracle. eg:
    // mysql: min < null < other < max
    // oracle: min < other < null < max
    // To make sorted result stable, compat guard should be used.
    lib::CompatModeGuard g(is_oracle_mode ?
                      lib::Worker::CompatMode::ORACLE :
                      lib::Worker::CompatMode::MYSQL);
    if (OB_FAIL(sort_partition_array(tablegroup_schema))) {
      LOG_WARN("failed to sort partition array", KR(ret), K(tablegroup_schema));
    } else if (OB_FAIL(sort_subpartition_array(tablegroup_schema))) {
      LOG_WARN("failed to sort subpartition array", KR(ret), K(tablegroup_schema));
    }
  }
  LOG_TRACE("fetch partition info", KR(ret), K(tablegroup_schema));
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_partition_info(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const uint64_t tablegroup_id,
    const int64_t schema_version,
    ObISQLClient &sql_client,
    ObTablegroupSchema *&tablegroup_schema)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(tablegroup_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablegroup schema should not be NULL", K(ret));
  } else if (OB_FAIL(fetch_part_info(schema_status, tenant_id, tablegroup_id,
                                     schema_version, sql_client, tablegroup_schema))) {
    LOG_WARN("fail to fetch part info", K(ret), K(schema_status),
             K(tenant_id), K(tablegroup_id), K(schema_version));
  } else if (OB_FAIL(fetch_sub_part_info(schema_status, tenant_id, tablegroup_id,
                                         schema_version, sql_client, tablegroup_schema))) {
    LOG_WARN("fail to fetch sub part info", K(ret), K(schema_status),
             K(tenant_id), K(tablegroup_id), K(schema_version));
  } else if (OB_FAIL(sort_tablegroup_partition_info(*tablegroup_schema))) {
    LOG_WARN("fail to sort tablegroup partition info", K(ret), K(tenant_id), K(tablegroup_id));
  }
  return ret;
}

template<typename SCHEMA>
int ObSchemaServiceSQLImpl::fetch_part_info(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const uint64_t schema_id,
    const int64_t schema_version,
    ObISQLClient &sql_client,
    SCHEMA *&schema)
{
  int ret = OB_SUCCESS;
  ObMySQLResult *result = NULL;
  ObSqlString sql;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  if (OB_ISNULL(schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema should not be NULL", K(ret));
  } else if (PARTITION_LEVEL_ZERO == schema->get_part_level()) {
    // skip
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      //fetch part info
      if (OB_FAIL(sql.append_fmt(FETCH_ALL_PART_HISTORY_SQL, OB_ALL_PART_HISTORY_TNAME,
                                 fill_extract_tenant_id(schema_status, tenant_id)))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql.append_fmt(" AND table_id = %lu AND schema_version <= %ld",
                                        fill_extract_schema_id(schema_status, schema_id),
                                        schema_version))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql.append_fmt(" ORDER BY tenant_id, table_id, part_id, schema_version"))) {
        LOG_WARN("Failed to append fmt", K(ret));
      } else {
        const bool check_deleted = true;
        DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
        if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
          if ((OB_TABLE_NOT_EXIST == ret) && (GET_MIN_CLUSTER_VERSION() < CLUSTER_VERSION_3100)) {
            LOG_INFO("cluster version is earlier than 3.1.0, ignore OB_TABLE_NOT_EXIST",
                     K(ret), K(GET_MIN_CLUSTER_VERSION()), K(sql));
            ret = OB_SUCCESS;
          } else {
            LOG_WARN("execute sql failed", K(ret), K(GET_MIN_CLUSTER_VERSION()), K(sql));
          }
        } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fail to get result. ", K(sql), K(ret));
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_part_info(
                   tenant_id, check_deleted, *result, schema))) {
          LOG_WARN("failed to retrieve all part info", K(check_deleted), K(ret));
        }
      }
    }
  }
  return ret;
}

template<typename SCHEMA>
int ObSchemaServiceSQLImpl::fetch_sub_part_info(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const uint64_t schema_id,
    const int64_t schema_version,
    ObISQLClient &sql_client,
    SCHEMA *&schema)
{
  int ret = OB_SUCCESS;
  ObMySQLResult *result = NULL;
  ObSqlString sql;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  if (OB_ISNULL(schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema should not be NULL", K(ret));
  } else if (PARTITION_LEVEL_TWO != schema->get_part_level()) {
    // skip
  } else if (schema->has_sub_part_template_def()) {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sql.reuse();
      if (OB_FAIL(sql.append_fmt(FETCH_ALL_DEF_SUBPART_HISTORY_SQL, OB_ALL_DEF_SUB_PART_HISTORY_TNAME,
                                 fill_extract_tenant_id(schema_status, tenant_id)))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql.append_fmt(" AND table_id = %lu and schema_version <= %ld "
                                        " ORDER BY tenant_id, table_id, sub_part_id, schema_version",
                                        fill_extract_schema_id(schema_status, schema_id),
                                        schema_version))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        const bool check_deleted = true;
        DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
        if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fail to get result. ", K(sql), K(ret));
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_def_subpart_info(
                   tenant_id, check_deleted, *result, schema))) {
          LOG_WARN("failed to retrieve all subpart info", K(check_deleted), K(ret));
        } else { }//do nothing
      }
    }
  }

  if (OB_SUCC(ret)) {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sql.reuse();
      if (OB_FAIL(sql.append_fmt(FETCH_ALL_SUB_PART_HISTORY_SQL, OB_ALL_SUB_PART_HISTORY_TNAME,
                                 fill_extract_tenant_id(schema_status, tenant_id)))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql.append_fmt(" AND table_id = %lu and schema_version <= %ld "
                 " ORDER BY tenant_id, table_id, part_id, sub_part_id, schema_version",
                 fill_extract_schema_id(schema_status, schema_id),
                 schema_version))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        const bool check_deleted = true;
        DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
        if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fail to get result. ", K(sql), K(ret));
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_subpart_info(
                   tenant_id, check_deleted, *result, schema))) {
          LOG_WARN("failed to retrieve all subpart info", K(check_deleted), K(ret));
        } else { }//do nothing
      }
    }
  }
  return ret;
}

// for ddl, strong read
int ObSchemaServiceSQLImpl::insert_recyclebin_object(const ObRecycleObject &recycle_obj,
                                                     common::ObISQLClient &sql_client)
{
  int ret = OB_SUCCESS;
  ObSqlString sql_string;
  const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(recycle_obj.get_tenant_id());
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail, ", K(ret));
  } else if (!recycle_obj.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("recycle object is invalid ", K(ret));
  } else {
    int64_t affected_rows = 0;
    ObDMLSqlSplicer dml;
    uint64_t exec_tenant_id = OB_INVALID_ID;
    if (recycle_obj.get_type() == ObRecycleObject::RecycleObjType::TENANT) {
      exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(OB_SYS_TENANT_ID);
    } else {
      exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(recycle_obj.get_tenant_id());
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(dml.add_pk_column("tenant_id", ObSchemaUtils::get_extract_tenant_id(
                                                 exec_tenant_id,
                                                 recycle_obj.get_tenant_id())))
          || OB_FAIL(dml.add_pk_column(OBJ_GET_K(recycle_obj, object_name)))
          || OB_FAIL(dml.add_pk_column(OBJ_GET_K(recycle_obj, type)))
          || OB_FAIL(dml.add_column("original_name", ObHexEscapeSqlStr(recycle_obj.get_original_name())))
          || OB_FAIL(dml.add_column("database_id", ObSchemaUtils::get_extract_schema_id(
                                    exec_tenant_id, recycle_obj.get_database_id())))
          || OB_FAIL(dml.add_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                    exec_tenant_id, recycle_obj.get_table_id())))
          || OB_FAIL(dml.add_column("tablegroup_id", ObSchemaUtils::get_extract_schema_id(
                                    exec_tenant_id, recycle_obj.get_tablegroup_id())))) {
        LOG_WARN("add column failed", K(ret));
      }
    }
    ObDMLExecHelper exec(sql_client, exec_tenant_id);
    if (FAILEDx(exec.exec_replace(OB_ALL_RECYCLEBIN_TNAME, dml, affected_rows))) {
      LOG_WARN("execute insert failed", K(ret));
    } else if (!is_single_row(affected_rows)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("affected_rows unexpected", K(affected_rows), K(ret));
    }
  }
  return ret;
}

// for ddl, strong read
int ObSchemaServiceSQLImpl::delete_recycle_object(
    const uint64_t tenant_id,
    const ObRecycleObject &recycle_object,
    ObISQLClient &sql_client)
{
  int ret = OB_SUCCESS;
  ObSqlString sql_string;
  const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id);
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail, ", K(ret));
  } else if (!recycle_object.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("recycle object is invalid ", K(ret));
  } else {
    ObSqlString sql;
    int64_t affected_rows = 0;
    if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE tenant_id = %lu and object_name = '%.*s' AND type = %d",
                               OB_ALL_RECYCLEBIN_TNAME,
                               ObSchemaUtils::get_extract_tenant_id(exec_tenant_id,
                                                                    recycle_object.get_tenant_id()),
                               recycle_object.get_object_name().length(),
                               recycle_object.get_object_name().ptr(),
                               recycle_object.get_type()))) {
      LOG_WARN("assign_fmt failed", K(ret));
    } else if (OB_FAIL(sql_client.write(exec_tenant_id, sql.ptr(), affected_rows))) {
      LOG_WARN("execute sql failed", K(sql), K(ret));
    } else if (!is_single_row(affected_rows)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("affected_rows is expected to one", K(affected_rows), K(ret));
    }
  }
  return ret;
}

// for ddl, strong read
int ObSchemaServiceSQLImpl::fetch_recycle_object(
    const uint64_t tenant_id,
    const ObString &object_name,
    const ObRecycleObject::RecycleObjType recycle_obj_type,
    ObISQLClient &sql_client,
    ObIArray<ObRecycleObject> &recycle_objs)
{
  int ret = OB_SUCCESS;
  if (OB_INVALID_ID == tenant_id || ObRecycleObject::INVALID == recycle_obj_type
      || object_name.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret),
             K(tenant_id), K(recycle_obj_type), K(object_name));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      ObSqlString sql;
      recycle_objs.reset();
      const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id);
      if (OB_FAIL(sql.append_fmt(FETCH_ALL_RECYCLEBIN_SQL,
                                 OB_ALL_RECYCLEBIN_TNAME,
                                 ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id),
                                 object_name.length(),
                                 object_name.ptr(),
                                 recycle_obj_type))) {
        LOG_WARN("append sql failed", K(ret),
                 K(object_name), K(recycle_obj_type), K(tenant_id));
      } else {
        DEFINE_SQL_CLIENT_RETRY_WEAK(sql_client);
        if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fail to get result. ", K(ret));
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_recycle_object(exec_tenant_id,
                                                                          *result,
                                                                          recycle_objs))) {
          LOG_WARN("failed to retrieve recycle_object", K(ret));
        }
      }
    }
  }
  return ret;
}

// for ddl, strong read
int ObSchemaServiceSQLImpl::fetch_expire_recycle_objects(
    const uint64_t tenant_id,
    const int64_t expire_time,
    ObISQLClient &sql_client,
    ObIArray<ObRecycleObject> &recycle_objs)
{
  int ret = OB_SUCCESS;
  if (OB_INVALID == tenant_id || expire_time <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("argument is invalid", K(ret), K(tenant_id), K(expire_time));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      ObSqlString sql;
      // FIXME: The query may time out.
      const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id);
      if (tenant_id == OB_SYS_TENANT_ID) {
        if (OB_FAIL(sql.append_fmt(FETCH_EXPIRE_SYS_ALL_RECYCLEBIN_SQL,
                                   OB_ALL_RECYCLEBIN_TNAME,
                                   ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id),
                                   expire_time))) {
          LOG_WARN("append sql failed", K(ret), K(tenant_id), K(expire_time));
        }
      } else {
        if (OB_FAIL(sql.append_fmt(FETCH_EXPIRE_ALL_RECYCLEBIN_SQL,
                                   OB_ALL_RECYCLEBIN_TNAME,
                                   ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id),
                                   expire_time))) {
          LOG_WARN("append sql failed", K(ret), K(tenant_id), K(expire_time));
        }
      }
      if (OB_SUCC(ret)) {
        DEFINE_SQL_CLIENT_RETRY_WEAK(sql_client);
        if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("failed to get result.", K(ret));
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_recycle_object(exec_tenant_id,
                                                                          *result,
                                                                          recycle_objs))) {
          LOG_WARN("failed to retrieve recycle object", K(ret));
        }
      }
    }
  }
  return ret;
}


// for ddl, strong read
int ObSchemaServiceSQLImpl::fetch_recycle_objects_of_db(
    const uint64_t tenant_id,
    const uint64_t database_id,
    ObISQLClient &sql_client,
    ObIArray<ObRecycleObject> &recycle_objs)
{
  int ret = OB_SUCCESS;
  if (OB_INVALID_ID == tenant_id || OB_INVALID_ID == database_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("argument is invalid", K(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      ObSqlString sql;
      const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id);
      if (OB_FAIL(sql.append_fmt(FETCH_ALL_RECYCLEBIN_SQL_WITH_CONDITION,
                                 OB_ALL_RECYCLEBIN_TNAME,
                                 ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id)))) {
        LOG_WARN("append sql failed", K(ret), K(tenant_id));
      } else if (OB_FAIL(sql.append_fmt(" AND database_id = %ld AND (type = %d OR type = %d)",
                                        ObSchemaUtils::get_extract_schema_id(exec_tenant_id, database_id),
                                        ObRecycleObject::TABLE,
                                        ObRecycleObject::VIEW))) {
        LOG_WARN("append sql failed", K(ret), K(database_id));
      } else {
        DEFINE_SQL_CLIENT_RETRY_WEAK(sql_client);
        if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("failed to get result.", K(ret));
        } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_recycle_object(
                                                  exec_tenant_id,
                                                  *result,
                                                  recycle_objs))) {
          LOG_WARN("failed to retrieve recycle object", K(ret));
        }
      }
    }
  }
  return ret;
}

// strong read
int ObSchemaServiceSQLImpl::construct_recycle_table_object(
    common::ObISQLClient &sql_client,
    const ObSimpleTableSchemaV2 &table,
    ObRecycleObject &recycle_object)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = table.get_tenant_id();
  const int64_t table_id = table.get_table_id();
  const common::ObString &table_name = table.get_table_name();
  const int64_t schema_version = table.get_schema_version();
  recycle_object.reset();
  if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail", K(ret));
  } else if (OB_SYS_TENANT_ID == tenant_id
             || !is_valid_tenant_id(tenant_id)
             || table_id <= 0
             || table_name.empty()
             || !ObSchemaService::is_formal_version(schema_version)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(tenant_id), K(table_id), K(table_name), K(schema_version));
  } else if (!ObSchemaService::g_liboblog_mode_) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("for backup used only", K(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      ObSqlString sql;
      const char *history_table_name = NULL;
      if (OB_FAIL(ObSchemaUtils::get_all_table_history_name(tenant_id,
                                                            history_table_name,
                                                            schema_service_))) {
        LOG_WARN("fail to get all table name", K(ret), K(tenant_id));
      } else if (OB_FAIL(sql.append_fmt(FETCH_RECYCLE_TABLE_OBJECT, history_table_name,
                                 ObSchemaUtils::get_extract_tenant_id(tenant_id, tenant_id),
                                 ObSchemaUtils::get_extract_schema_id(tenant_id, table_id),
                                 schema_version, table_name.ptr()))) {
        LOG_WARN("append sql failed", K(ret), K(tenant_id));
      } else {
        DEFINE_SQL_CLIENT_RETRY_WEAK(sql_client);
        if (OB_FAIL(sql_client_retry_weak.read(res, tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("failed to get result.", K(ret));
        } else if (OB_FAIL(result->next())) {
          LOG_WARN("fail to get next", K(tenant_id), K(table_id), K(table_name), K(schema_version));
        } else {
          ObString orig_table_name;
          uint64_t orig_tablegroup_id = OB_INVALID_ID;
          uint64_t orig_database_id = OB_INVALID_ID;
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "table_name", orig_table_name);
          EXTRACT_INT_FIELD_MYSQL_WITH_TENANT_ID(*result, "tablegroup_id", orig_tablegroup_id, tenant_id);
          EXTRACT_INT_FIELD_MYSQL_WITH_TENANT_ID(*result, "database_id", orig_database_id, tenant_id);
          if (OB_SUCC(ret)) {
            recycle_object.set_tenant_id(tenant_id);
            recycle_object.set_database_id(orig_database_id);
            recycle_object.set_table_id(table_id);
            recycle_object.set_tablegroup_id(orig_tablegroup_id);
            if (OB_FAIL(recycle_object.set_type_by_table_schema(table))) {
              LOG_WARN("fail to set type", K(ret), K(table));
            } else if (OB_FAIL(recycle_object.set_object_name(table_name))) {
              LOG_WARN("fail to set object name", K(ret), K(table_name));
            } else if (OB_FAIL(recycle_object.set_original_name(orig_table_name))) {
              LOG_WARN("fail to set orignal name", K(ret), K(orig_table_name));
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(result->next())) {
            if (OB_ITER_END != ret) {
              LOG_WARN("fail to get next", K(ret), K(table));
            } else {
              ret = OB_SUCCESS; //overwrite ret
            }
          } else {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("should be only one record", K(ret), K(table));
          }
        }
      }
    }
  }
  return ret;
}

// strong read
int ObSchemaServiceSQLImpl::construct_recycle_database_object(
    common::ObISQLClient &sql_client,
    const ObDatabaseSchema &database,
    ObRecycleObject &recycle_object)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = database.get_tenant_id();
  const int64_t database_id = database.get_database_id();
  const common::ObString &database_name = database.get_database_name();
  const int64_t schema_version = database.get_schema_version();
  recycle_object.reset();
  if (OB_SYS_TENANT_ID == tenant_id
      || !is_valid_tenant_id(tenant_id)
      || database_id <= 0
      || database_name.empty()
      || !ObSchemaService::is_formal_version(schema_version)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(tenant_id), K(database_id), K(database_name), K(schema_version));
  } else if (!ObSchemaService::g_liboblog_mode_) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("for backup used only", K(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      ObSqlString sql;
      if (OB_FAIL(sql.append_fmt(FETCH_RECYCLE_DATABASE_OBJECT, OB_ALL_DATABASE_HISTORY_TNAME,
                                 ObSchemaUtils::get_extract_tenant_id(tenant_id, tenant_id),
                                 ObSchemaUtils::get_extract_schema_id(tenant_id, database_id),
                                 schema_version, database_name.ptr()))) {
        LOG_WARN("append sql failed", K(ret), K(tenant_id));
      } else {
        DEFINE_SQL_CLIENT_RETRY_WEAK(sql_client);
        if (OB_FAIL(sql_client_retry_weak.read(res, tenant_id, sql.ptr()))) {
          LOG_WARN("execute sql failed", K(sql), K(ret));
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("failed to get result.", K(ret));
        } else if (OB_FAIL(result->next())) {
          LOG_WARN("fail to get next", K(tenant_id), K(database_id), K(database_name), K(schema_version));
        } else {
          ObString orig_database_name;
          uint64_t orig_tablegroup_id = OB_INVALID_ID;
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "database_name", orig_database_name);
          EXTRACT_INT_FIELD_MYSQL_WITH_TENANT_ID(*result, "default_tablegroup_id", orig_tablegroup_id, tenant_id);

          if (OB_SUCC(ret)) {
            recycle_object.set_tenant_id(tenant_id);
            recycle_object.set_table_id(OB_INVALID_ID);
            recycle_object.set_database_id(database_id);
            recycle_object.set_tablegroup_id(orig_tablegroup_id);
            recycle_object.set_type(ObRecycleObject::DATABASE);
            if (OB_FAIL(recycle_object.set_object_name(database_name))) {
              LOG_WARN("fail to set object name", K(ret), K(database_name));
            } else if (OB_FAIL(recycle_object.set_original_name(orig_database_name))) {
              LOG_WARN("fail to set orignal name", K(ret), K(orig_database_name));
            }
          }

          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(result->next())) {
            if (OB_ITER_END != ret) {
              LOG_WARN("fail to get next", K(ret), K(database));
            } else {
              ret = OB_SUCCESS; //overwrite ret
            }
          } else {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("should be only one record", K(ret), K(database));
          }
        }
      }
    }
  }
  return ret;
}

template<typename TABLE_SCHEMA>
int ObSchemaServiceSQLImpl::fetch_foreign_key_info(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const uint64_t table_id,
    const int64_t schema_version,
    ObISQLClient &sql_client,
    TABLE_SCHEMA &table_schema)
{
  int ret = OB_SUCCESS;
  bool has_error = false;
  ObMySQLResult *result = NULL;
  ObSqlString sql;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  if (OB_SUCC(ret)) {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      if (OB_FAIL(sql.append_fmt(FETCH_ALL_FOREIGN_KEY_HISTORY_SQL, OB_ALL_FOREIGN_KEY_HISTORY_TNAME,
                                 fill_extract_tenant_id(schema_status, tenant_id)))) {
        LOG_WARN("failed to append sql", K(table_id), K(ret));
      } else if (OB_FAIL(sql.append_fmt(" AND (child_table_id = %lu OR parent_table_id = %lu)",
                                        fill_extract_schema_id(schema_status, table_id),
                                        fill_extract_schema_id(schema_status, table_id)))) {
        LOG_WARN("failed to append sql", K(tenant_id), K(table_id), K(ret));
      } else if (OB_FAIL(sql.append_fmt(" AND schema_version <= %ld", schema_version))) {
        LOG_WARN("failed to append sql", K(schema_version), K(ret));
      } else if (OB_FAIL(sql.append_fmt(" ORDER BY tenant_id desc, foreign_key_id desc, schema_version desc"))) {
        LOG_WARN("Failed to append fmt", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("failed to execute sql", K(sql), K(ret));
        if (-ER_NO_SUCH_TABLE == ret && ObSchemaService::g_liboblog_mode_) {
          LOG_INFO("liboblog mode, ignore");
          ret = OB_SUCCESS;
          has_error = true;
        }
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_foreign_key_info(tenant_id, *result, table_schema))) {
        LOG_WARN("failed to retrieve foreign key info", K(ret));
      }
    }
  }
  if (OB_SUCC(ret) && !has_error) {
    int64_t foreign_key_count = table_schema.get_foreign_key_infos().count();
    for (int64_t i = 0; OB_SUCC(ret) && i < foreign_key_count; i++) {
      ObForeignKeyInfo &foreign_key_info = table_schema.get_foreign_key_infos().at(i);
      if (OB_FAIL(fetch_foreign_key_column_info(schema_status, tenant_id, schema_version, sql_client, foreign_key_info))) {
        LOG_WARN("failed to fetch foreign key column info", K(foreign_key_info), K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_foreign_key_column_info(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const int64_t schema_version,
    ObISQLClient &sql_client,
    ObForeignKeyInfo &foreign_key_info)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_FOREIGN_KEY_COLUMN_HISTORY_SQL, OB_ALL_FOREIGN_KEY_COLUMN_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if ((!ObSchemaService::g_liboblog_mode_
                || (ObSchemaService::g_liboblog_mode_ && GET_MIN_CLUSTER_VERSION() >= CLUSTER_VERSION_2250))
               && OB_FAIL(sql.append_fmt(" AND foreign_key_id = %lu AND schema_version <= %ld ORDER BY position ASC, schema_version DESC",
                          fill_extract_schema_id(schema_status, foreign_key_info.foreign_key_id_),
                          schema_version))) {
      LOG_WARN("append table_id and foreign key id and schema version",
               K(ret), K(foreign_key_info.foreign_key_id_), K(schema_version));
    } else if (ObSchemaService::g_liboblog_mode_
               && GET_MIN_CLUSTER_VERSION() < CLUSTER_VERSION_2250
               && OB_FAIL(sql.append_fmt(" AND foreign_key_id = %lu AND schema_version <= %ld",
                          fill_extract_schema_id(schema_status, foreign_key_info.foreign_key_id_),
                          schema_version))) {
      LOG_WARN("append table_id and foreign key id and schema version",
               K(ret), K(foreign_key_info.foreign_key_id_), K(schema_version));
    } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
      LOG_WARN("execute sql failed", K(sql), K(ret));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to get result. ", K(ret));
    } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_foreign_key_column_info(tenant_id, *result, foreign_key_info))) {
      LOG_WARN("failed to retrieve foreign key column info", K(ret), K(foreign_key_info));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_foreign_key_array_for_simple_table_schemas(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    common::ObISQLClient &sql_client,
    ObArray<ObSimpleTableSchemaV2 *> &table_schema_array,
    const uint64_t *table_ids,
    const int64_t table_ids_size)
{
  int ret = OB_SUCCESS;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  if (OB_NOT_NULL(table_ids) && table_ids_size > 0) {
    ObSqlString sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
      // FIXME: The following SQL will cause full table scan, which it's poor performance when the amount of table data is large.
      if (ObSchemaService::g_liboblog_mode_ && GET_MIN_CLUSTER_VERSION() < CLUSTER_VERSION_2100) {
        LOG_INFO("liboblog mode, ignore");
        ret = OB_SUCCESS;
      } else if (OB_FAIL(sql.append_fmt(FETCH_TABLE_ID_AND_NAME_FROM_ALL_FOREIGN_KEY_SQL, OB_ALL_FOREIGN_KEY_HISTORY_TNAME,
                         fill_extract_tenant_id(schema_status, tenant_id)))) {
        LOG_WARN("failed to append sql", K(ret), K(tenant_id));
      } else if (OB_FAIL(sql.append_fmt(" AND child_table_id IN "))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
        LOG_WARN("sql append table ids failed", K(ret));
      } else if (OB_FAIL(sql.append_fmt(" AND schema_version <= %ld", schema_version))) {
        LOG_WARN("failed to append sql", K(ret), K(schema_version));
      } else if (OB_FAIL(sql.append_fmt(" ORDER BY tenant_id desc, child_table_id desc, foreign_key_id desc, schema_version desc"))) {
        LOG_WARN("Failed to append fmt", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("failed to execute sql", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_simple_foreign_key_info(tenant_id, *result, table_schema_array))) {
        LOG_WARN("failed to retrieve simple foreign key info", K(ret));
      }
    }
  }
  return ret;
}

template<typename T>
int ObSchemaServiceSQLImpl::fetch_all_encrypt_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    common::ObISQLClient &sql_client,
    ObArray<T *> &table_schema_array,
    const int64_t array_size)
{
  int ret = OB_SUCCESS;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  ObArray<uint64_t> tablespace_ids;
  ObArray<T *>tables;
  for (int64_t i = 0; OB_SUCC(ret) && i < array_size; ++i) {
    uint64_t tablespace_id = table_schema_array.at(i)->get_tablespace_id();
    if (tablespace_id != OB_INVALID_ID && !is_inner_table(table_schema_array.at(i)->get_table_id())) {
      bool find = false;
      // tablespace_id deduplication
      for (int j = 0; OB_SUCC(ret) && j < tablespace_ids.count(); ++j) {
        if (tablespace_ids.at(j) == tablespace_id) {
          find = true;
          break;
        }
      }
      if (!find) {
        if (OB_FAIL(tablespace_ids.push_back(tablespace_id))) {
          LOG_WARN("fail to push back tablespace id", K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(tables.push_back(table_schema_array.at(i)))) {
          LOG_WARN("fail to push back table schema", K(ret));
        }
      }
    }
  }

  if (OB_SUCC(ret) && tables.count() > 0 && tablespace_ids.count() > 0) {
    ObSqlString sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
      if (OB_FAIL(sql.append_fmt(FETCH_ENCRYPT_INFO_FROM_ALL_TENANT_TABLESAPCE_SQL, OB_ALL_TENANT_TABLESPACE_HISTORY_TNAME,
                         fill_extract_tenant_id(schema_status, tenant_id)))) {
        LOG_WARN("failed to append sql", K(ret), K(tenant_id));
      } else if (OB_FAIL(sql.append_fmt(" AND tablespace_id IN "))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql_append_pure_ids(schema_status, &tablespace_ids.at(0), tablespace_ids.count(), sql))) {
        LOG_WARN("sql append table ids failed", K(ret));
      } else if (OB_FAIL(sql.append_fmt(" AND schema_version <= %ld", schema_version))) {
        LOG_WARN("failed to append sql", K(ret), K(schema_version));
      } else if (OB_FAIL(sql.append_fmt(" ORDER BY tenant_id desc, tablespace_id desc, schema_version desc"))) {
        LOG_WARN("Failed to append fmt", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("failed to execute sql", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_simple_encrypt_info(tenant_id, *result, tables))) {
        LOG_WARN("failed to retrieve simple encrypt info", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_trigger_list(const ObRefreshSchemaStatus &schema_status,
                                               const uint64_t tenant_id,
                                               const uint64_t table_id,
                                               const int64_t schema_version,
                                               ObISQLClient &sql_client,
                                               ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  ObMySQLResult *result = NULL;
  ObSqlString sql;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_TRIGGER_ID_HISTORY_SQL, OB_ALL_TENANT_TRIGGER_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("failed to append sql", K(table_id), K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND base_object_id = %lu",
                                      fill_extract_schema_id(schema_status, table_id)))) {
      LOG_WARN("failed to append sql", K(tenant_id), K(table_id), K(ret));
    } else if (OB_FAIL(sql.append_fmt(" AND schema_version <= %ld", schema_version))) {
      LOG_WARN("failed to append sql", K(schema_version), K(ret));
    } else if (OB_FAIL(sql.append_fmt(" ORDER BY trigger_id desc, schema_version desc"))) {
      LOG_WARN("Failed to append fmt", K(ret));
    } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
      LOG_WARN("failed to execute sql", K(sql), K(ret));
      if (-ER_NO_SUCH_TABLE == ret && ObSchemaService::g_liboblog_mode_) {
        LOG_INFO("liboblog mode, ignore");
        ret = OB_SUCCESS;
      }
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to get result", K(ret));
    } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_trigger_list(tenant_id, *result,
                                                                    table_schema.get_trigger_list()))) {
      LOG_WARN("failed to retrieve trigger list", K(ret));
    } else {
      LOG_DEBUG("TRIGGER", K(table_schema.get_trigger_list()));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_constraint_array_for_simple_table_schemas(const ObRefreshSchemaStatus &schema_status,
                                                                            const int64_t schema_version,
                                                                            const uint64_t tenant_id,
                                                                            common::ObISQLClient &sql_client,
                                                                            ObArray<ObSimpleTableSchemaV2 *> &table_schema_array,
                                                                            const uint64_t *table_ids,
                                                                            const int64_t table_ids_size)
{
  int ret = OB_SUCCESS;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
  if (OB_NOT_NULL(table_ids) && table_ids_size > 0) {
    ObSqlString sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (ObSchemaService::g_liboblog_mode_ && GET_MIN_CLUSTER_VERSION() < CLUSTER_VERSION_2110) {
        LOG_INFO("liboblog mode, ignore");
        ret = OB_SUCCESS;
      } else if (OB_FAIL(sql.append_fmt(FETCH_TABLE_ID_AND_CST_NAME_FROM_ALL_CONSTRAINT_HISTORY_SQL, OB_ALL_CONSTRAINT_HISTORY_TNAME,
                         fill_extract_tenant_id(schema_status, tenant_id)))) {
        LOG_WARN("failed to append sql", K(ret), K(tenant_id));
      } else if (OB_FAIL(sql.append_fmt(" AND table_id IN "))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql_append_pure_ids(schema_status, table_ids, table_ids_size, sql))) {
        LOG_WARN("sql append table ids failed", K(ret));
      } else if (OB_FAIL(sql.append_fmt(" AND schema_version <= %ld", schema_version))) {
        LOG_WARN("failed to append sql", K(ret), K(schema_version));
      } else if (OB_FAIL(sql.append_fmt(" ORDER BY tenant_id desc, table_id desc, constraint_id desc, schema_version desc"))) {
        LOG_WARN("Failed to append fmt", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("failed to execute sql", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_simple_constraint_info(tenant_id, *result, table_schema_array))) {
        LOG_WARN("failed to retrieve simple constraint info", K(ret));
      }
    }
  }

  return ret;
}

int ObSchemaServiceSQLImpl::can_read_schema_version(
    const ObRefreshSchemaStatus &schema_status,
    int64_t expected_version)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("inner stat error", KR(ret), K(schema_status));
  } else if (0 >= expected_version) {
    // fine
  } else {
    int64_t core_schema_version = 0;
    int64_t schema_version = OB_INVALID_VERSION;
    ObISQLClient &sql_client = *mysql_proxy_;
    if (OB_FAIL(get_core_version(sql_client, schema_status, core_schema_version))) {
      LOG_WARN("failed to get core schema version", KR(ret), K(schema_status));
    } else if (OB_FAIL(fetch_schema_version(schema_status, sql_client, schema_version))) {
      LOG_WARN("fail to fetch normal schema version", KR(ret), K(schema_status));
    } else if (expected_version > core_schema_version
               && expected_version > schema_version) {
      ret = OB_SCHEMA_EAGAIN;
      LOG_WARN("__all_ddl_operation or __all_core_table is older than the expected version",
               KR(ret), K(schema_status), K(expected_version),
               K(schema_version), K(core_schema_version));
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_ori_schema_version(
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    const uint64_t table_id,
    int64_t &ori_schema_version)
{
  int ret = OB_SUCCESS;
  const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
  ori_schema_version = OB_INVALID_VERSION;

  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("inner stat error", K(ret));
  } else if (is_core_table(table_id)) {
    // To avoid cyclic dependence, system table won't record ori_schema_version.
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      ObISQLClient &sql_client = *mysql_proxy_;
      ObSqlString sql;
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
      ret = sql.append_fmt("SELECT ori_schema_version FROM %s "
                           "WHERE (TENANT_ID = %lu AND TABLE_ID = %lu) OR "
                           "      (TENANT_ID = %lu AND TABLE_ID = %lu)",
                           OB_ALL_ORI_SCHEMA_VERSION_TNAME,
                           tenant_id, table_id, // for compatibility
                           fill_extract_tenant_id(schema_status, tenant_id),
                           fill_extract_schema_id(schema_status, table_id));
      if (OB_FAIL(ret)) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(result->next())) {
        LOG_WARN("fail to get last_schema_version", K(ret));
      } else {
        EXTRACT_INT_FIELD_MYSQL(*result, "ori_schema_version", ori_schema_version, int64_t);
        LOG_TRACE("Get last_schema_version", K(ori_schema_version), K(tenant_id), K(table_id));
        int tmp_ret = OB_SUCCESS;
        if (0 >= ori_schema_version) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected ori_schema_version. ", K(ret), K(ori_schema_version));
        } else if (OB_ITER_END != (tmp_ret = result->next())) {
          ret = OB_SUCCESS == tmp_ret ? OB_ERR_UNEXPECTED : tmp_ret;
          LOG_WARN("should be only one row", K(ret), K(tenant_id), K(table_id), K(schema_status));
        }
      }
    }
  }
  return ret;
}

// for ddl, strong read
int ObSchemaServiceSQLImpl::check_ddl_id_exist(
    common::ObISQLClient &sql_client,
    const uint64_t tenant_id,
    const ObString &ddl_id_str,
    bool &is_exists)
{
  int ret = OB_SUCCESS;
  is_exists = false;

  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    sqlclient::ObMySQLResult *result = NULL;
    ObSqlString sql;
    ObSqlString ddl_id_str_sql;
    ObString ddl_id_str_hex;
    DEFINE_SQL_CLIENT_RETRY_WEAK(sql_client);
    uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id);
    if (ddl_id_str.empty()) {
      // do-nothing
    } else if (OB_FAIL(sql_append_hex_escape_str(ddl_id_str, ddl_id_str_sql))) {
      LOG_WARN("sql_append_hex_escape_str failed", K(ddl_id_str));
    } else {
      ddl_id_str_hex = ddl_id_str_sql.string();
      if (OB_SUCCESS !=
          (ret = sql.append_fmt("SELECT 1 FROM %s WHERE tenant_id = %lu and ddl_id_str = %.*s LIMIT 1",
              OB_ALL_DDL_ID_TNAME, tenant_id, ddl_id_str_hex.length(), ddl_id_str_hex.ptr()))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (NULL == (result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) { //no record
          is_exists = false;
          ret = common::OB_SUCCESS;
        } else {
          LOG_WARN("fail to do query from __all_ddl_id. iter quit.", K(ret));
        }
      } else {
        is_exists = true;
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_sequences(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_sequence_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObSequenceSchema> &sequence_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  sequence_info_array.reserve(tenant_sequence_ids.count());
  LOG_DEBUG("fetch batch sequences begin.", K(tenant_sequence_ids));
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }

  std::sort(tenant_sequence_ids.begin(), tenant_sequence_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_sequence_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_sequence_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_sequence_info(schema_status, schema_version, tenant_id, sql_client, sequence_info_array,
        &tenant_sequence_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all sequence info failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch sequence info finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_sequence_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObSequenceSchema> &sequence_array,
    const uint64_t *sequence_keys /* = NULL */,
    const int64_t sequences_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_SEQUENCE_OBJECT_HISTORY_SQL, OB_ALL_SEQUENCE_OBJECT_HISTORY_TNAME,
        fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != sequence_keys && sequences_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND sequence_id IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < sequences_size; ++i) {
          const uint64_t sequence_id = fill_extract_schema_id(schema_status, sequence_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu",
                                     0 == i ? "" : ", ",
                                     sequence_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, SEQUENCE_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_sequence_schema(tenant_id, *result, sequence_array))) {
        LOG_WARN("Failed to retrieve sequence infos", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_label_se_policys(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_label_se_policy_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObLabelSePolicySchema> &label_se_policy_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  label_se_policy_info_array.reserve(tenant_label_se_policy_ids.count());
  LOG_DEBUG("fetch batch label se policy begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }
  std::sort(tenant_label_se_policy_ids.begin(), tenant_label_se_policy_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCC(ret) && end < tenant_label_se_policy_ids.count()) {
    while (end < tenant_label_se_policy_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_label_se_policy_info(schema_status, schema_version, tenant_id, sql_client, label_se_policy_info_array,
                                               &tenant_label_se_policy_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all label security schemas failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch label se policy info finish", K(schema_version), K(ret));
  return ret;
}
int ObSchemaServiceSQLImpl::fetch_all_label_se_policy_info(
   const ObRefreshSchemaStatus &schema_status,
   const int64_t schema_version,
   const uint64_t tenant_id,
   ObISQLClient &sql_client,
   ObIArray<ObLabelSePolicySchema> &label_se_policy_array,
   const uint64_t *label_se_policy_keys /* = NULL */,
   const int64_t label_se_policy_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_LABEL_SE_POLICY_OBJECT_HISTORY_SQL, OB_ALL_TENANT_OLS_POLICY_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != label_se_policy_keys && label_se_policy_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND label_se_policy_id IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < label_se_policy_size; ++i) {
          const uint64_t label_se_policy_id = fill_extract_schema_id(schema_status, label_se_policy_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu",
                                      0 == i ? "" : ", ",
                                     label_se_policy_id))) {
             LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, LABEL_SE_POLICY_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_label_se_policy_schema(tenant_id, *result, label_se_policy_array))) {
        LOG_WARN("Failed to retrieve sequence infos", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_profiles(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_profile_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObProfileSchema> &profile_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  profile_info_array.reserve(tenant_profile_ids.count());
  LOG_DEBUG("fetch batch profile begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }
  std::sort(tenant_profile_ids.begin(), tenant_profile_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCC(ret) && end < tenant_profile_ids.count()) {
    while (end < tenant_profile_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_profile_info(schema_status, schema_version, tenant_id, sql_client, profile_info_array,
                                       &tenant_profile_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all profile schemas failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch profile info finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_profile_info(
   const ObRefreshSchemaStatus &schema_status,
   const int64_t schema_version,
   const uint64_t tenant_id,
   ObISQLClient &sql_client,
   ObIArray<ObProfileSchema> &profile_array,
   const uint64_t *profile_keys /* = NULL */,
   const int64_t profile_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_PROFILE_HISTORY_SQL, OB_ALL_TENANT_PROFILE_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != profile_keys && profile_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND profile_id IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < profile_size; ++i) {
          const uint64_t profile_id = fill_extract_schema_id(schema_status, profile_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu",
               0 == i ? "" : ", ",
              profile_id))) {
             LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, PROFILE_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_profile_schema(tenant_id, *result, profile_array))) {
        LOG_WARN("Failed to retrieve sequence infos", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_label_se_components(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_label_se_component_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObLabelSeComponentSchema> &label_se_component_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  label_se_component_info_array.reserve(tenant_label_se_component_ids.count());
  LOG_DEBUG("fetch batch label se component begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }
  std::sort(tenant_label_se_component_ids.begin(), tenant_label_se_component_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCC(ret) && end < tenant_label_se_component_ids.count()) {
    while (end < tenant_label_se_component_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_label_se_component_info(schema_status, schema_version, tenant_id, sql_client, label_se_component_info_array,
                    &tenant_label_se_component_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all label security schemas failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch label se component info finish", K(schema_version), K(ret));
  return ret;
}
int ObSchemaServiceSQLImpl::fetch_all_label_se_component_info(
   const ObRefreshSchemaStatus &schema_status,
   const int64_t schema_version,
   const uint64_t tenant_id,
   ObISQLClient &sql_client,
   ObIArray<ObLabelSeComponentSchema> &label_se_component_array,
   const uint64_t *label_se_component_keys /* = NULL */,
   const int64_t label_se_component_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_LABEL_SE_COMPONENT_OBJECT_HISTORY_SQL, OB_ALL_TENANT_OLS_COMPONENT_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != label_se_component_keys && label_se_component_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND label_se_component_id IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < label_se_component_size; ++i) {
          const uint64_t label_se_component_id = fill_extract_schema_id(schema_status, label_se_component_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu",
           0 == i ? "" : ", ",
          label_se_component_id))) {
             LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, LABEL_SE_COMPONENT_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_label_se_component_schema(tenant_id, *result, label_se_component_array))) {
        LOG_WARN("Failed to retrieve sequence infos", K(ret));
      }
    }
  }
  return ret;
}
int ObSchemaServiceSQLImpl::get_batch_label_se_labels(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_label_se_label_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObLabelSeLabelSchema> &label_se_label_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  label_se_label_info_array.reserve(tenant_label_se_label_ids.count());
  LOG_DEBUG("fetch batch label se label begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }
  std::sort(tenant_label_se_label_ids.begin(), tenant_label_se_label_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCC(ret) && end < tenant_label_se_label_ids.count()) {
    while (end < tenant_label_se_label_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_label_se_label_info(schema_status, schema_version, tenant_id, sql_client, label_se_label_info_array,
                    &tenant_label_se_label_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all label security schemas failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch label se label info finish", K(schema_version), K(ret));
  return ret;
}
int ObSchemaServiceSQLImpl::fetch_all_label_se_label_info(
   const ObRefreshSchemaStatus &schema_status,
   const int64_t schema_version,
   const uint64_t tenant_id,
   ObISQLClient &sql_client,
   ObIArray<ObLabelSeLabelSchema> &label_se_label_array,
   const uint64_t *label_se_label_keys /* = NULL */,
   const int64_t label_se_label_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_LABEL_SE_LABEL_OBJECT_HISTORY_SQL, OB_ALL_TENANT_OLS_LABEL_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != label_se_label_keys && label_se_label_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND label_se_label_id IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < label_se_label_size; ++i) {
          const uint64_t label_se_label_id = fill_extract_schema_id(schema_status, label_se_label_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu",
           0 == i ? "" : ", ",
          label_se_label_id))) {
             LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, LABEL_SE_LABEL_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_label_se_label_schema(tenant_id, *result, label_se_label_array))) {
        LOG_WARN("Failed to retrieve sequence infos", K(ret));
      }
    }
  }
  return ret;
}
int ObSchemaServiceSQLImpl::get_batch_label_se_user_levels(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_label_se_user_level_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObLabelSeUserLevelSchema> &label_se_user_level_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  label_se_user_level_info_array.reserve(tenant_label_se_user_level_ids.count());
  LOG_DEBUG("fetch batch label se label begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }
  std::sort(tenant_label_se_user_level_ids.begin(), tenant_label_se_user_level_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCC(ret) && end < tenant_label_se_user_level_ids.count()) {
    while (end < tenant_label_se_user_level_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_label_se_user_level_info(schema_status, schema_version, tenant_id, sql_client, label_se_user_level_info_array,
                    &tenant_label_se_user_level_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all label security schemas failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch label se label info finish", K(schema_version), K(ret));
  return ret;
}
int ObSchemaServiceSQLImpl::fetch_all_label_se_user_level_info(
   const ObRefreshSchemaStatus &schema_status,
   const int64_t schema_version,
   const uint64_t tenant_id,
   ObISQLClient &sql_client,
   ObIArray<ObLabelSeUserLevelSchema> &label_se_user_level_array,
   const uint64_t *label_se_user_level_keys /* = NULL */,
   const int64_t label_se_user_level_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_LABEL_SE_USER_LEVEL_OBJECT_HISTORY_SQL, OB_ALL_TENANT_OLS_USER_LEVEL_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != label_se_user_level_keys && label_se_user_level_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND label_se_user_level_id IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < label_se_user_level_size; ++i) {
          const uint64_t label_se_user_level_id = fill_extract_schema_id(schema_status, label_se_user_level_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu",
           0 == i ? "" : ", ",
          label_se_user_level_id))) {
             LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, LABEL_SE_USER_LEVEL_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_label_se_user_level_schema(tenant_id, *result, label_se_user_level_array))) {
        LOG_WARN("Failed to retrieve sequence infos", K(ret));
      }
    }
  }
  return ret;
}


int ObSchemaServiceSQLImpl::get_batch_tablespaces(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_tablespace_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObTablespaceSchema> &tablespace_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  tablespace_info_array.reserve(tenant_tablespace_ids.count());
  LOG_DEBUG("fetch batch tablespaces begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }
  std::sort(tenant_tablespace_ids.begin(), tenant_tablespace_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_tablespace_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_tablespace_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_tablespace_info(schema_status, schema_version, tenant_id, sql_client, tablespace_info_array,
        &tenant_tablespace_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all tablespace info failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch tablespace info finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_tablespace_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObTablespaceSchema> &tablespace_array,
    const uint64_t *tablespace_keys /* = NULL */,
    const int64_t tablespaces_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_TABLESPACE_HISTORY_SQL, OB_ALL_TENANT_TABLESPACE_HISTORY_TNAME,
        fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != tablespace_keys && tablespaces_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND tablespace_id IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < tablespaces_size; ++i) {
          const uint64_t tablespace_id = fill_extract_schema_id(schema_status, tablespace_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu",
                                     0 == i ? "" : ", ",
                                     tablespace_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, TABLESPACE_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_tablespace_schema(tenant_id, *result, tablespace_array))) {
        LOG_WARN("Failed to retrieve tablespace infos", K(ret));
      }
    }
  }
  return ret;
}


int ObSchemaServiceSQLImpl::get_batch_sys_variables(
    const ObRefreshSchemaStatus &schema_status,
    common::ObISQLClient &sql_client,
    const int64_t schema_version,
    common::ObArray<SchemaKey> &sys_variable_keys,
    common::ObIArray<ObSimpleSysVariableSchema> &sys_variable_array)
{
  UNUSED(sql_client);
  int ret = OB_SUCCESS;
  sys_variable_array.reserve(sys_variable_keys.count());
  LOG_DEBUG("fetch batch sys variables begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  } else {
    ObSimpleSysVariableSchema tmp_schema;
    FOREACH_X(key, sys_variable_keys, OB_SUCC(ret)) {
      if (OB_ISNULL(key)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("key is null", K(ret));
      } else {
        tmp_schema.reset();
        if (OB_FAIL(fetch_sys_variable(sql_client, schema_status, key->tenant_id_, key->schema_version_, tmp_schema))) {
          LOG_WARN("fail to fetch sys variable", K(ret), KPC(key));
        } else if (OB_FAIL(sys_variable_array.push_back(tmp_schema))) {
          LOG_WARN("fail to push back", K(ret), K(tmp_schema));
        }
      }
    }
  }

  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_udfs(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_udf_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObUDF> &udf_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  udf_info_array.reserve(tenant_udf_ids.count());
  LOG_DEBUG("fetch batch udfs begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }

  std::sort(tenant_udf_ids.begin(), tenant_udf_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_udf_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_udf_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_udf_info(schema_status, schema_version, tenant_id, sql_client, udf_info_array,
        &tenant_udf_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all udf info failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch udf info finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_udf_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObUDF> &udf_array,
    const uint64_t *udf_keys /* = NULL */,
    const int64_t udf_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);

    if (OB_FAIL(sql.append_fmt(FETCH_ALL_FUNC_HISTORY_SQL, OB_ALL_FUNC_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != udf_keys && udf_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND udf_id IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < udf_size; ++i) {
          const uint64_t udf_id = fill_extract_schema_id(schema_status, udf_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu",
                                     0 == i ? "" : ", ",
                                     udf_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, NAME DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_udf_schema(tenant_id, *result, udf_array))) {
        LOG_WARN("Failed to retrieve udf infos", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_keystores(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_keystore_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObKeystoreSchema> &keystore_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  keystore_info_array.reserve(tenant_keystore_ids.count());
  LOG_DEBUG("fetch batch keystores begin.");
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (!check_inner_stat()) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }
  std::sort(tenant_keystore_ids.begin(), tenant_keystore_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_keystore_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_keystore_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_keystore_info(schema_status, schema_version, tenant_id, sql_client, keystore_info_array,
        &tenant_keystore_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all keystore info failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch keystore info finish", K(schema_version), K(ret));
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_all_keystore_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObKeystoreSchema> &keystore_array,
    const uint64_t *keystore_keys /* = NULL */,
    const int64_t keystores_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_KEYSTORE_HISTORY_SQL, OB_ALL_TENANT_KEYSTORE_HISTORY_TNAME,
        fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != keystore_keys && keystores_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND keystore_id IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < keystores_size; ++i) {
          const uint64_t keystore_id = fill_extract_schema_id(schema_status, keystore_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu",
                                     0 == i ? "" : ", ",
                                     keystore_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, KEYSTORE_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_keystore_schema(tenant_id, *result, keystore_array))) {
        LOG_WARN("Failed to retrieve keystore infos", K(ret));
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::get_batch_audits(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    common::ObArray<uint64_t> &tenant_audit_ids,
    common::ObISQLClient &sql_client,
    common::ObIArray<ObSAuditSchema> &audit_info_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = schema_status.tenant_id_;
  audit_info_array.reserve(tenant_audit_ids.count());
  LOG_DEBUG("fetch batch audits begin.", K(lbt()), K(tenant_audit_ids));
  if (OB_UNLIKELY(schema_version <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(schema_version), K(ret));
  } else if (OB_UNLIKELY(!check_inner_stat())) {
    ret = OB_NOT_INIT;
    LOG_WARN("check inner stat fail");
  }
  std::sort(tenant_audit_ids.begin(), tenant_audit_ids.end());
  // split query to tenant space && split big query
  int64_t begin = 0;
  int64_t end = 0;
  while (OB_SUCCESS == ret && end < tenant_audit_ids.count()) {
    while (OB_SUCCESS == ret && end < tenant_audit_ids.count()
           && end - begin < MAX_IN_QUERY_PER_TIME) {
      end++;
    }
    if (OB_FAIL(fetch_all_audit_info(schema_status, schema_version, tenant_id, sql_client, audit_info_array,
        &tenant_audit_ids.at(begin), end - begin))) {
      LOG_WARN("fetch all audit info failed", K(schema_version), K(ret));
    }
    begin = end;
  }
  LOG_INFO("get batch audit info finish", K(schema_version), K(ret));
  return ret;
}
int ObSchemaServiceSQLImpl::fetch_all_audit_info(
    const ObRefreshSchemaStatus &schema_status,
    const int64_t schema_version,
    const uint64_t tenant_id,
    ObISQLClient &sql_client,
    ObIArray<ObSAuditSchema> &audit_array,
    const uint64_t *audit_keys /* = NULL */,
    const int64_t audits_size /* = 0 */)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    const uint64_t exec_tenant_id = fill_exec_tenant_id(schema_status);
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
    if (OB_FAIL(sql.append_fmt(FETCH_ALL_SECURITY_AUDIT_HISTORY_SQL, OB_ALL_TENANT_SECURITY_AUDIT_HISTORY_TNAME,
                               fill_extract_tenant_id(schema_status, tenant_id)))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (NULL != audit_keys && audits_size > 0) {
      if (OB_FAIL(sql.append_fmt(" AND audit_id IN ("))) {
        LOG_WARN("append sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < audits_size; ++i) {
          const uint64_t audit_id = fill_extract_schema_id(schema_status, audit_keys[i]);
          if (OB_FAIL(sql.append_fmt("%s%lu",
                                     0 == i ? "" : ", ",
                                     audit_id))) {
            LOG_WARN("append sql failed", K(ret), K(i));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("append sql failed", K(ret));
          }
        }
      }
    } else { }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append_fmt(" AND SCHEMA_VERSION <= %ld", schema_version))) {
        LOG_WARN("append failed", K(ret));
      } else if (OB_FAIL(sql.append(" ORDER BY TENANT_ID DESC, AUDIT_ID DESC, SCHEMA_VERSION DESC"))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(ret), K(tenant_id), K(sql));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Fail to get result", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_audit_schema(tenant_id, *result, audit_array))) {
        LOG_WARN("Failed to retrieve audit infos", K(ret));
      }
    }
  }
  return ret;
}


#define CONSTRUCT_SCHEMA_VERSION_HISTORY_SQL1(SCHEMA_ID) \
  "select schema_version, is_deleted, min(schema_version) over () as min_version from %s "\
  "where tenant_id = %lu and "#SCHEMA_ID" = %lu and schema_version <= %ld order by schema_version desc limit %d"

#define CONSTRUCT_SCHEMA_VERSION_HISTORY_SQL2(SCHEMA_ID) \
  "select * from (select schema_version, is_deleted from %s where tenant_id = %lu and "#SCHEMA_ID" = %lu and schema_version <= %ld order by schema_version desc limit %d) as a, "\
  "(select min(schema_version) as min_version from %s where tenant_id = %lu and "#SCHEMA_ID" = %lu and schema_version <= %ld) as b"

#define CONSTRUCT_TABLE_SCHEMA_VERSION_HISTORY_SQL1 CONSTRUCT_SCHEMA_VERSION_HISTORY_SQL1(table_id)
#define CONSTRUCT_TABLE_SCHEMA_VERSION_HISTORY_SQL2 CONSTRUCT_SCHEMA_VERSION_HISTORY_SQL2(table_id)
#define CONSTRUCT_TABLEGROUP_SCHEMA_VERSION_HISTORY_SQL1 CONSTRUCT_SCHEMA_VERSION_HISTORY_SQL1(tablegroup_id)
#define CONSTRUCT_TABLEGROUP_SCHEMA_VERSION_HISTORY_SQL2 CONSTRUCT_SCHEMA_VERSION_HISTORY_SQL2(tablegroup_id)
#define CONSTRUCT_DATABASE_SCHEMA_VERSION_HISTORY_SQL1 CONSTRUCT_SCHEMA_VERSION_HISTORY_SQL1(database_id)
#define CONSTRUCT_DATABASE_SCHEMA_VERSION_HISTORY_SQL2 CONSTRUCT_SCHEMA_VERSION_HISTORY_SQL2(database_id)

#define CONSTRUCT_TENANT_SCHEMA_VERSION_HISTORY_SQL1 "select schema_version, is_deleted, min(schema_version) over () as min_version from %s "\
                                                     "where tenant_id = %lu and schema_version <= %ld order by schema_version desc limit %d"

#define CONSTRUCT_TENANT_SCHEMA_VERSION_HISTORY_SQL2 "select * from (select schema_version, is_deleted from %s where tenant_id = %lu and schema_version <= %ld order by schema_version desc limit %d) as a, "\
                                                     "(select min(schema_version) as min_version from %s where tenant_id = %lu and schema_version <= %ld) as b"
int ObSchemaServiceSQLImpl::construct_schema_version_history(
    const ObRefreshSchemaStatus &schema_status,
    ObISQLClient &sql_client,
    const int64_t snapshot_version,
    const VersionHisKey &version_his_key,
    VersionHisVal &version_his_val)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObSqlString sql;
    const ObSchemaType &schema_type = version_his_key.schema_type_;
    const uint64_t &schema_id = version_his_key.schema_id_;
    const uint64_t tenant_id = TENANT_SCHEMA == schema_type ? schema_id : schema_status.tenant_id_;
    const uint64_t exec_tenant_id = TENANT_SCHEMA == schema_type ? OB_SYS_TENANT_ID : fill_exec_tenant_id(schema_status);
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    switch (schema_type) {
      case TABLE_SCHEMA: {
        const char *table_name = NULL;
        if (!check_inner_stat()) {
          ret = OB_NOT_INIT;
          LOG_WARN("check inner stat fail", K(ret));
        } else if (OB_FAIL(ObSchemaUtils::get_all_table_history_name(exec_tenant_id,
                                                                     table_name,
                                                                     schema_service_))) {
          LOG_WARN("fail to get all table name", K(ret), K(exec_tenant_id));
        } else if (!g_liboblog_mode_) {
          if (OB_FAIL(sql.append_fmt(CONSTRUCT_TABLE_SCHEMA_VERSION_HISTORY_SQL1,
                                     table_name,
                                     fill_extract_tenant_id(schema_status, tenant_id),
                                     fill_extract_schema_id(schema_status, schema_id),
                                     snapshot_version, MAX_CACHED_VERSION_CNT))) {
            LOG_WARN("append failed", K(ret), K(schema_id), K(snapshot_version));
          } else { } // do-nothing
        } else {
          if (OB_FAIL(sql.append_fmt(CONSTRUCT_TABLE_SCHEMA_VERSION_HISTORY_SQL2,
                                     table_name,
                                     fill_extract_tenant_id(schema_status, tenant_id),
                                     fill_extract_schema_id(schema_status, schema_id),
                                     snapshot_version, MAX_CACHED_VERSION_CNT,
                                     table_name,
                                     fill_extract_tenant_id(schema_status, tenant_id),
                                     fill_extract_schema_id(schema_status, schema_id),
                                     snapshot_version))) {
            LOG_WARN("append failed", K(ret), K(schema_id), K(snapshot_version));
          } else { } // do-nothing
        }
        break;
      }
      case TABLEGROUP_SCHEMA: {
        if (!g_liboblog_mode_) {
          if (OB_FAIL(sql.append_fmt(CONSTRUCT_TABLEGROUP_SCHEMA_VERSION_HISTORY_SQL1,
                                     OB_ALL_TABLEGROUP_HISTORY_TNAME,
                                     fill_extract_tenant_id(schema_status, tenant_id),
                                     fill_extract_schema_id(schema_status, schema_id),
                                     snapshot_version, MAX_CACHED_VERSION_CNT))) {
            LOG_WARN("append failed", K(ret), K(schema_id), K(snapshot_version));
          } else { } // do-nothing
        } else {
          if (OB_FAIL(sql.append_fmt(CONSTRUCT_TABLEGROUP_SCHEMA_VERSION_HISTORY_SQL2,
                                     OB_ALL_TABLEGROUP_HISTORY_TNAME,
                                     fill_extract_tenant_id(schema_status, tenant_id),
                                     fill_extract_schema_id(schema_status, schema_id),
                                     snapshot_version, MAX_CACHED_VERSION_CNT,
                                     OB_ALL_TABLEGROUP_HISTORY_TNAME,
                                     fill_extract_tenant_id(schema_status, tenant_id),
                                     fill_extract_schema_id(schema_status, schema_id),
                                     snapshot_version))) {
            LOG_WARN("append failed", K(ret), K(schema_id), K(snapshot_version));
          } else { } // do-nothing
        }
        break;
      }
      case DATABASE_SCHEMA: {
        if (!g_liboblog_mode_) {
          if (OB_FAIL(sql.append_fmt(CONSTRUCT_DATABASE_SCHEMA_VERSION_HISTORY_SQL1,
                                     OB_ALL_DATABASE_HISTORY_TNAME,
                                     fill_extract_tenant_id(schema_status, tenant_id),
                                     fill_extract_schema_id(schema_status, schema_id),
                                     snapshot_version, MAX_CACHED_VERSION_CNT))) {
            LOG_WARN("append failed", K(ret), K(schema_id), K(snapshot_version));
          } else { } // do-nothing
        } else {
          if (OB_FAIL(sql.append_fmt(CONSTRUCT_DATABASE_SCHEMA_VERSION_HISTORY_SQL2,
                                     OB_ALL_DATABASE_HISTORY_TNAME,
                                     fill_extract_tenant_id(schema_status, tenant_id),
                                     fill_extract_schema_id(schema_status, schema_id),
                                     snapshot_version, MAX_CACHED_VERSION_CNT,
                                     OB_ALL_DATABASE_HISTORY_TNAME,
                                     fill_extract_tenant_id(schema_status, tenant_id),
                                     fill_extract_schema_id(schema_status, schema_id),
                                     snapshot_version))) {
            LOG_WARN("append failed", K(ret), K(schema_id), K(snapshot_version));
          } else { } // do-nothing
        }
        break;
      }
      case TENANT_SCHEMA: {
        if (!g_liboblog_mode_) {
          if (OB_FAIL(sql.append_fmt(CONSTRUCT_TENANT_SCHEMA_VERSION_HISTORY_SQL1,
                                     OB_ALL_TENANT_HISTORY_TNAME, schema_id,
                                     snapshot_version, MAX_CACHED_VERSION_CNT))) {
            LOG_WARN("append failed", K(ret), K(schema_id), K(snapshot_version));
          } else { } // do-nothing
        } else {
          if (OB_FAIL(sql.append_fmt(CONSTRUCT_TENANT_SCHEMA_VERSION_HISTORY_SQL2,
                                     OB_ALL_TENANT_HISTORY_TNAME, schema_id,
                                     snapshot_version, MAX_CACHED_VERSION_CNT,
                                     OB_ALL_TENANT_HISTORY_TNAME,
                                     schema_id, snapshot_version))) {
            LOG_WARN("append failed", K(ret), K(schema_id), K(snapshot_version));
          } else { } // do-nothing
        }
        break;
      }
      default: {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("unexpected schema type", K(schema_type), K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_SNAPSHOT(sql_client, snapshot_timestamp);
      if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_schema_version(*result, version_his_val))) {
        LOG_WARN("failed to retrieve schema version", K(ret));
      } else {
        version_his_val.snapshot_version_ = snapshot_version;
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::init_sequence_id(const int64_t rootservice_epoch)
{
  int ret = OB_SUCCESS;
  if (rootservice_epoch < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("rootservice_epoch is invalid", K(ret), K(rootservice_epoch));
  } else {
    SpinWLockGuard guard(rw_lock_);
    uint64_t pure_sequence_id = 0;
    sequence_id_ = combine_sequence_id(rootservice_epoch, pure_sequence_id);
    LOG_INFO("init sequence id", K(ret), K(schema_info_), K(sequence_id_));
  }
  return ret;
}

int ObSchemaServiceSQLImpl::inc_sequence_id()
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(rw_lock_);
  if (OB_INVALID_ID == sequence_id_ || OB_INVALID_ID == sequence_id_ + 1) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid sequence_id", K(ret), K(sequence_id_));
  } else {
    uint64_t new_sequence_id = sequence_id_ + 1;
    if (extract_rootservice_epoch(new_sequence_id)
        != extract_rootservice_epoch(sequence_id_)) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("sequence_id reach max, need restart/switch rs", K(ret), K(new_sequence_id), K(sequence_id_));
    } else {
      LOG_INFO("inc sequence id", K(ret), K(sequence_id_), K(new_sequence_id));
      sequence_id_ = new_sequence_id;
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::set_last_operation_info(const uint64_t tenant_id, const int64_t schema_version)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(rw_lock_);
  if (OB_INVALID_TENANT_ID == tenant_id
      || schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id or schema_version", K(ret), K(tenant_id), K(schema_version));
  } else {
    last_operation_schema_version_ = schema_version;
    last_operation_tenant_id_ = tenant_id;
  }
  return ret;
}

int ObSchemaServiceSQLImpl::set_refresh_schema_info(const ObRefreshSchemaInfo &schema_info)
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(rw_lock_);
  schema_info_.set_tenant_id(schema_info.get_tenant_id());
  schema_info_.set_schema_version(schema_info.get_schema_version());
  schema_info_.set_sequence_id(sequence_id_);
  LOG_INFO("set refresh schema info", K(ret), K(schema_info_));
  return ret;
}

int ObSchemaServiceSQLImpl::get_refresh_schema_info(ObRefreshSchemaInfo &schema_info)
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(rw_lock_);
  schema_info.reset();
  if (OB_FAIL(schema_info.assign(schema_info_))) {
    LOG_WARN("fail to assign schema_info", K(ret), K(schema_info_));
  } else {}
  return ret;
}

int ObSchemaServiceSQLImpl::fetch_tenant_compat_mode(
    ObISQLClient &sql_client,
    const uint64_t tenant_id,
    common::ObCompatibilityMode &mode)
{
  int ret = OB_SUCCESS;
  mode = ObCompatibilityMode::MYSQL_MODE;
  ObSqlString sql;
  ObMySQLResult *result = NULL;
  DEFINE_SQL_CLIENT_RETRY_WEAK(sql_client);

  if (is_sys_tenant(tenant_id)
      || is_meta_tenant(tenant_id)) {
    // system tenant must run in MySQL mode
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      if (OB_FAIL(sql.append_fmt("SELECT compatibility_mode FROM %s WHERE TENANT_ID = %lu AND is_deleted = 0 LIMIT 1",
                                 OB_ALL_TENANT_HISTORY_TNAME, tenant_id))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, OB_SYS_TENANT_ID, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(result->next())) {
        if (common::OB_ITER_END == ret) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("select ob_compatibility_mode return no row", K(ret), K(tenant_id));
        } else {
          LOG_WARN("failed to get system variable ob_compatibility_mode", K(ret), K(tenant_id));
        }
      } else {
        int64_t value = 0;
        EXTRACT_INT_FIELD_MYSQL(*result, "compatibility_mode", value, int64_t);
        if (OB_FAIL(ret)) {
          LOG_WARN("failed to get ob_compatibility_mode sysvar value", K(ret), K(tenant_id));
        } else if (0 == value) {
          mode = ObCompatibilityMode::MYSQL_MODE;
        } else if (1 == value) {
          mode = ObCompatibilityMode::ORACLE_MODE;
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ob_compatibility_mode shoulde be either MYSQL_MODE or ORACLE_MODE", K(ret), K(tenant_id));
        }
      }
    }
  }

  // For compatibility, we can get ob_compatibility_mode from __all_sys_variable_history
  // if __all_tenant doesn't contain compatibility_mode column.
  if (-ER_BAD_FIELD_ERROR == ret && ObSchemaService::g_liboblog_mode_) {
    ret = OB_SUCCESS;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sql.reset();
      if (OB_FAIL(sql.append_fmt("SELECT value FROM %s WHERE TENANT_ID = %lu AND NAME = 'ob_compatibility_mode' "
                                 "ORDER BY schema_version DESC LIMIT 1", OB_ALL_SYS_VARIABLE_HISTORY_TNAME, tenant_id))) {
        LOG_WARN("append sql failed", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, OB_SYS_TENANT_ID, sql.ptr()))) {
        LOG_WARN("execute sql failed", K(sql), K(ret));
      } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get result. ", K(ret));
      } else if (OB_FAIL(result->next())) {
        if (common::OB_ITER_END == ret) {
          if (ObSchemaService::g_liboblog_mode_) {
            ret = OB_SUCCESS;
          } else {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("select ob_compatibility_mode return no row", K(ret), K(tenant_id));
          }
        } else {
          LOG_WARN("failed to get system variable ob_compatibility_mode", K(ret), K(tenant_id));
        }
      } else {
        ObString sysvar_value;
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "value", sysvar_value);
        if (OB_FAIL(ret)) {
          LOG_WARN("failed to get ob_compatibility_mode sysvar value", K(ret), K(tenant_id));
        } else if (0 == sysvar_value.case_compare("0")) {
          mode = ObCompatibilityMode::MYSQL_MODE;
        } else if (0 == sysvar_value.case_compare("1")) {
          mode = ObCompatibilityMode::ORACLE_MODE;
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ob_compatibility_mode shoulde be either MYSQL_MODE or ORACLE_MODE", K(ret), K(tenant_id));
        }
      }
    }
  }
  LOG_INFO("fetch the ob_compatibility_mode ", K(mode), K(tenant_id), K(ret));
  return ret;
}

// strong read
int ObSchemaServiceSQLImpl::get_drop_tenant_infos(
    common::ObISQLClient &sql_client,
    int64_t schema_version,
    common::ObIArray<ObDropTenantInfo> &drop_tenant_infos)
{
  int ret = OB_SUCCESS;
  drop_tenant_infos.reset();
  ObMySQLResult *result = NULL;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObSqlString sql;
    if (schema_version <= 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid schema_version", K(ret), K(schema_version));
    } else if (OB_FAIL(sql.assign_fmt("SELECT tenant_id, schema_version FROM %s "
                                      "WHERE is_deleted = 1 AND schema_version <= %ld "
                                      "ORDER BY tenant_id ASC",
                                      OB_ALL_TENANT_HISTORY_TNAME, schema_version))) {
      LOG_WARN("fail to append sql", K(ret), K(schema_version));
    } else if (OB_FAIL(sql_client.read(res, OB_SYS_TENANT_ID, sql.ptr()))) {
      LOG_WARN("execute sql failed", K(ret), K(sql));
    } else if (OB_UNLIKELY(NULL == (result = res.get_result()))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to get result. ", K(sql), K(ret));
    } else if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_drop_tenant_infos(*result, drop_tenant_infos))) {
      LOG_WARN("fail to retrieve drop tenant infos", K(ret), K(schema_version));
    }
  }
  return ret;
}

// For compatibility
uint64_t ObSchemaServiceSQLImpl::fill_exec_tenant_id(const ObRefreshSchemaStatus &schema_status)
{
  uint64_t tenant_id = schema_status.tenant_id_;
  if (OB_INVALID_TENANT_ID == tenant_id) {
    tenant_id = OB_SYS_TENANT_ID;
  }
  return tenant_id;
}

// For compatibility
uint64_t ObSchemaServiceSQLImpl::fill_extract_tenant_id(const ObRefreshSchemaStatus &schema_status, const uint64_t tenant_id)
{
  UNUSEDx(schema_status, tenant_id);
  return OB_INVALID_TENANT_ID;
}

// For compatibility
uint64_t ObSchemaServiceSQLImpl::fill_extract_schema_id(const ObRefreshSchemaStatus &schema_status, const uint64_t schema_id)
{
  UNUSED(schema_status);
  return schema_id;
}

int ObSchemaServiceSQLImpl::sort_partition_array(ObPartitionSchema &partition_schema)
{
  int ret = OB_SUCCESS;
  if (!partition_schema.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid partition schema", K(ret), K(partition_schema));
  } else if (!partition_schema.is_user_partition_table()) {
    // skip
  } else if (OB_ISNULL(partition_schema.get_part_array())) {
    // for compat, should mock partition_array
  } else {
    int64_t part_num = partition_schema.get_first_part_num();
    int64_t partition_num = partition_schema.get_partition_num();
    if (part_num != partition_num) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("partition num not match", K(ret), K(part_num), K(partition_num));
    } else if (OB_ISNULL(partition_schema.get_part_array())
               || partition_num <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("partition array is empty", K(ret), K(partition_schema));
    } else if (partition_schema.is_range_part()) {
      std::sort(partition_schema.get_part_array(),
          partition_schema.get_part_array() + partition_num,
          ObBasePartition::range_like_func_less_than);
      std::sort(partition_schema.get_hidden_part_array(),
          partition_schema.get_hidden_part_array() + partition_schema.get_hidden_partition_num(),
          ObBasePartition::range_like_func_less_than);
    } else if (partition_schema.is_hash_like_part()) {
      std::sort(partition_schema.get_part_array(),
          partition_schema.get_part_array() + partition_num,
          ObBasePartition::hash_like_func_less_than);
      std::sort(partition_schema.get_hidden_part_array(),
          partition_schema.get_hidden_part_array() + partition_schema.get_hidden_partition_num(),
          ObBasePartition::hash_like_func_less_than);
    } else if (partition_schema.is_list_part()) {
      std::sort(partition_schema.get_part_array(),
          partition_schema.get_part_array() + partition_num,
          ObBasePartition::list_part_func_layout);
      std::sort(partition_schema.get_hidden_part_array(),
          partition_schema.get_hidden_part_array() + partition_schema.get_hidden_partition_num(),
          ObBasePartition::list_part_func_layout);
    } else {
      //nothing
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::sort_subpartition_array(ObPartitionSchema &partition_schema)
{
  int ret = OB_SUCCESS;
  if (!partition_schema.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid partition schema", K(ret), K(partition_schema));
  } else if (!partition_schema.is_user_partition_table()
             || PARTITION_LEVEL_TWO != partition_schema.get_part_level()) {
    // skip
  } else if (OB_ISNULL(partition_schema.get_part_array())
             || partition_schema.get_partition_num() <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("partition array is empty", K(ret), K(partition_schema));
  } else {
    // sort subpartition array
    for (int64_t i = 0; OB_SUCC(ret) && i < partition_schema.get_partition_num(); i++) {
      ObPartition* partition = partition_schema.get_part_array()[i];
      if (OB_ISNULL(partition)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("partition is null", K(ret), K(i), K(partition_schema));
      } else {
        ObSubPartition **subpart_array = partition->get_subpart_array();
        int64_t subpart_num = partition->get_sub_part_num();
        int64_t subpartition_num = partition->get_subpartition_num();
        if (subpart_num != subpartition_num) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("subpartition num not match", K(ret), K(subpart_num), K(subpartition_num));
        } else if (OB_ISNULL(subpart_array) || subpartition_num <= 0) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("subpartition array is empty", K(ret), K(i), K(partition_schema));
        } else if (partition_schema.is_range_subpart()) {
          std::sort(subpart_array, subpart_array + subpartition_num, ObBasePartition::less_than);
          std::sort(partition->get_hidden_subpart_array(),
                    partition->get_hidden_subpart_array() + partition->get_hidden_subpartition_num(),
                    ObBasePartition::less_than);
        } else if (partition_schema.is_hash_like_subpart()) {
          std::sort(subpart_array, subpart_array + subpartition_num,
                    ObSubPartition::hash_like_func_less_than);
          std::sort(partition->get_hidden_subpart_array(),
                    partition->get_hidden_subpart_array() + partition->get_hidden_subpartition_num(),
                    ObSubPartition::hash_like_func_less_than);
        } else if (partition_schema.is_list_subpart()) {
          std::sort(subpart_array, subpart_array + subpartition_num,
                    ObBasePartition::list_part_func_layout);
          std::sort(partition->get_hidden_subpart_array(),
                    partition->get_hidden_subpart_array() + partition->get_hidden_subpartition_num(),
                    ObBasePartition::list_part_func_layout);
        }
      }
    }

    // sort def_subpartition_array
    if (OB_SUCC(ret) && partition_schema.has_sub_part_template_def()) {
      int64_t def_subpart_num = partition_schema.get_def_sub_part_num();
      int64_t def_subpartition_num = partition_schema.get_def_subpartition_num();
      if (OB_ISNULL(partition_schema.get_def_subpart_array())) {
        // for compat, should mock subpartition array
      } else if (def_subpart_num != def_subpartition_num) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("def_subpartition num not match", K(ret), K(def_subpart_num), K(def_subpartition_num));
      } else if (OB_ISNULL(partition_schema.get_def_subpart_array())
                 || def_subpartition_num <= 0) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("def_subpartition array is empty", K(ret), K(partition_schema));
      } else if (partition_schema.is_range_subpart()) {
        std::sort(partition_schema.get_def_subpart_array(),
            partition_schema.get_def_subpart_array() + def_subpartition_num,
            ObBasePartition::less_than);
      } else if (partition_schema.is_hash_like_subpart()) {
        std::sort(partition_schema.get_def_subpart_array(),
            partition_schema.get_def_subpart_array() + def_subpartition_num,
            ObSubPartition::hash_like_func_less_than);
      } else if (partition_schema.is_list_subpart()) {
        std::sort(partition_schema.get_def_subpart_array(),
            partition_schema.get_def_subpart_array() + def_subpartition_num,
            ObBasePartition::list_part_func_layout);
      } else {
        //nothing
      }
    } else {
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::query_tenant_status(
    ObISQLClient &sql_client,
    const uint64_t tenant_id,
    TenantStatus &tenant_status)
{
  int ret = OB_SUCCESS;
  if (OB_INVALID_TENANT_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", K(ret), K(tenant_id));
  } else {
    ObSqlString sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(sql.assign_fmt("SELECT is_deleted FROM %s WHERE tenant_id = %ld "
                                 "ORDER BY schema_version DESC LIMIT 1",
                                 OB_ALL_TENANT_HISTORY_TNAME, tenant_id))) {
        LOG_WARN("fail to append sql", K(ret));
      } else if (OB_FAIL(sql_client.read(res, sql.ptr()))) {
        LOG_WARN("fail to execute sql", K(ret), K(sql));
      } else if (NULL == (result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get sql result", K(ret));
      } else if (OB_FAIL(result->next())) {
        if (ret == OB_ITER_END) { //no record
          ret = OB_SUCCESS;
          LOG_INFO("query_tenant_status: TENANT_NOT_CREATE", K(tenant_id));
          tenant_status = TENANT_NOT_CREATE;
        } else {
          LOG_WARN("fail to query. iter quit", K(ret), K(sql));
        }
      } else {
        bool is_deleted = false;
        EXTRACT_INT_FIELD_MYSQL_SKIP_RET(*result, "is_deleted", is_deleted, bool);
        if (OB_FAIL(ret)) {
          LOG_WARN("fail to retrieve is_deleted", K(ret), K(tenant_id));
        } else if (OB_ITER_END != (ret = result->next())) {
          // check if this is single row
          LOG_WARN("query tenant status fail, return multiple rows, unexcepted", K(ret),
              K(sql), K(tenant_id));
          ret = OB_ERR_UNEXPECTED;
        } else {
          ret = OB_SUCCESS;
          tenant_status = is_deleted ? TENANT_DELETED : TENANT_EXIST;
          if (is_deleted) {
            LOG_INFO("query_tenant_status: TENANT_HAS_BEEN_DELETED", K(tenant_id));
          }
        }
      }
    }
  }
  return ret;
}

// for liboblog & schema history recycle
int ObSchemaServiceSQLImpl::get_schema_version_by_timestamp(
    ObISQLClient &sql_client,
    const ObRefreshSchemaStatus &schema_status,
    const uint64_t tenant_id,
    int64_t timestamp,
    int64_t &schema_version)
{
  int ret = OB_SUCCESS;
  const uint64_t exec_tenant_id = tenant_id;
  schema_version = OB_INVALID_VERSION;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("inner stat error", K(ret));
  } else if (OB_INVALID_TENANT_ID == tenant_id
             || timestamp <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(tenant_id), K(timestamp));
  } else {
    const int64_t snapshot_timestamp = schema_status.snapshot_timestamp_;
    bool check_sys_variable = false;
    DEFINE_SQL_CLIENT_RETRY_WEAK_WITH_PARAMETER(sql_client, snapshot_timestamp, check_sys_variable);
    ObSqlString sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      // After ver 2.2.0, We use ddl operation with 1503 operation type to distinguish ddl operations of different ddl trans.
      // For compatibility, we can't assume that ddl operation with 1503 operation type is always existed.
      if (OB_FAIL(sql.assign_fmt("(SELECT MAX(schema_version) as schema_version FROM %s "
                                 " WHERE schema_version <= %ld)"
                                 " UNION "
                                 "(SELECT MAX(schema_version) as schema_version FROM %s "
                                 " WHERE schema_version <= %ld and "
                                 "(operation_type = %d or operation_type = %d))",
                                 OB_ALL_DDL_OPERATION_TNAME, timestamp,
                                 OB_ALL_DDL_OPERATION_TNAME, timestamp, OB_DDL_END_SIGN,
                                 OB_DDL_FINISH_SCHEMA_SPLIT))) {
        LOG_WARN("fail to append sql", K(ret));
      } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("fail to execute sql", K(ret), K(exec_tenant_id), K(sql));
      } else if (NULL == (result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get sql result", K(ret));
      } else {
        int64_t i = 0;
        int64_t max_row_count = 2;
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          if (++i > max_row_count) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unexpected row count", K(ret));
          } else {
            int64_t version = OB_INVALID_VERSION;
            EXTRACT_INT_FIELD_MYSQL_SKIP_RET(*result, "schema_version", version, int64_t);
            if (OB_FAIL(ret)) {
            } else if (version <= 0) {
              // 1. ddl trans not end with ddl opertaion with 1503 operation type.
              // 2. __all_ddl_operation is empty.
            } else if (OB_INVALID_VERSION == schema_version || version < schema_version) {
              schema_version = version;
            }
          }
        }
        if (OB_ITER_END == ret) {
          if (schema_version <= 0 || !is_formal_version(schema_version)) {
            // 1. __all_ddl_operation is empty.
            // 2. max(schema_version) is not a format schema version.
            // 3. schema_version is invalid.
            ret = OB_EAGAIN;
            LOG_WARN("schema_version is invalid", K(ret), K(schema_version));
          } else {
            ret = OB_SUCCESS;
          }
        } else {
          ret = OB_SUCC(ret) ? OB_ERR_UNEXPECTED : ret;
          LOG_WARN("unexpected result", K(ret));
        }
      }
    }
  }

  if (OB_FAIL(ret) && ObSchemaService::g_liboblog_mode_) {
    TenantStatus tenant_status = TENANT_STATUS_INVALID;
    int tmp_ret = query_tenant_status(sql_client, tenant_id, tenant_status);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("fail to query tenant status", K(tmp_ret), K(ret), K(tenant_id));
    } else if (TENANT_DELETED == tenant_status) {
      LOG_INFO("tenant has been dropped, no need retry", K(tmp_ret), K(tenant_id));
      ret = OB_TENANT_HAS_BEEN_DROPPED; //overwrite ret
    }
  }
  return ret;
}

// only for liboblog used
int ObSchemaServiceSQLImpl::get_first_trans_end_schema_version(
    ObISQLClient &sql_client,
    const uint64_t tenant_id,
    int64_t &schema_version)
{
  int ret = OB_SUCCESS;
  schema_version = OB_INVALID_VERSION;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("inner stat error", K(ret));
  } else if (!ObSchemaService::g_liboblog_mode_) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("only work for liboblog", K(ret));
  } else if (OB_INVALID_TENANT_ID == tenant_id
             || OB_SYS_TENANT_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(tenant_id));
  } else {
    ObSqlString sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      // Liboblog will only use this function when cluster runs in new schema refresh mode(after schema split),
      // so compatibility is not considered here.
      if (OB_FAIL(sql.assign_fmt("SELECT schema_version FROM %s WHERE operation_type = %d "
                                 "ORDER BY schema_version ASC LIMIT 1",
                                 OB_ALL_DDL_OPERATION_TNAME, OB_DDL_END_SIGN))) {
        LOG_WARN("fail to append sql", K(ret));
      } else if (OB_FAIL(sql_client.read(res, tenant_id, sql.ptr()))) {
        LOG_WARN("fail to execute sql", K(ret), K(tenant_id), K(sql));
      } else if (NULL == (result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get sql result", K(ret));
      } else if (OB_FAIL(result->next())) {
        LOG_WARN("next sql result fail", KR(ret), K(tenant_id), K(sql));
      } else if (OB_FAIL((*result).get_int("schema_version", schema_version))) {
        LOG_WARN("fail to get schema_version", K(ret), K(tenant_id), K(sql));
      } else if (schema_version <= 0 || !is_formal_version(schema_version)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid schema version", K(ret), K(tenant_id), K(schema_version));
      }
    }
  }
  if (OB_FAIL(ret) && ObSchemaService::g_liboblog_mode_) {
    TenantStatus tenant_status = TENANT_STATUS_INVALID;
    int tmp_ret = query_tenant_status(sql_client, tenant_id, tenant_status);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("fail to query tenant status", K(tmp_ret), K(ret), K(tenant_id));
    } else if (TENANT_DELETED == tenant_status) {
      LOG_INFO("tenant has been dropped, no need retry", K(tmp_ret), K(tenant_id));
      ret = OB_TENANT_HAS_BEEN_DROPPED; //overwrite ret
    }
  }
  return ret;
}


// link table.
int ObSchemaServiceSQLImpl::get_link_table_schema(const ObDbLinkSchema &dblink_schema,
                                                  const ObString &database_name,
                                                  const ObString &table_name,
                                                  ObIAllocator &alloctor,
                                                  ObTableSchema *&table_schema,
                                                  uint32_t sessid)
{
  int ret = OB_SUCCESS;
  ObTableSchema *tmp_schema = NULL;
  DblinkDriverProto link_type = static_cast<DblinkDriverProto>(dblink_schema.get_driver_proto());
  dblink_param_ctx param_ctx;
  param_ctx.pool_type_ = DblinkPoolType::DBLINK_POOL_SCHEMA;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("inner stat error", K(ret));
  } else if (OB_ISNULL(dblink_proxy_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("dblink proxy is empty", K(ret));
  } else if (OB_FAIL(dblink_proxy_->create_dblink_pool(dblink_schema.get_tenant_id(),
                                               dblink_schema.get_dblink_id(),
                                               link_type,
                                               dblink_schema.get_host_addr(),
                                               dblink_schema.get_tenant_name(),
                                               dblink_schema.get_user_name(),
                                               dblink_schema.get_password(),
                                               database_name,
                                               dblink_schema.get_conn_string(),
                                               dblink_schema.get_cluster_name(),
                                               param_ctx))) {
    LOG_WARN("create dblink pool failed", K(ret));
  } else if (OB_FAIL(fetch_link_table_info(dblink_schema, database_name, table_name,
                                           *dblink_proxy_, alloctor, tmp_schema, sessid))) {
    LOG_WARN("fetch link table info failed", K(ret), K(dblink_schema), K(database_name), K(table_name));
  } else if (OB_ISNULL(tmp_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table schema is NULL", K(ret));
  } else {
    table_schema = tmp_schema;
  }
  return ret;
}

template<typename T>
int ObSchemaServiceSQLImpl::fetch_link_table_info(const ObDbLinkSchema &dblink_schema,
                                                  const ObString &database_name,
                                                  const ObString &table_name,
                                                  ObDbLinkProxy &dblink_client,
                                                  ObIAllocator &alloctor,
                                                  T *&table_schema,
                                                  uint32_t sessid)
{
  int ret = OB_SUCCESS;
  int dblink_read_ret = OB_SUCCESS;
  const char *dblink_read_errmsg = NULL;
  ObMySQLResult *result = NULL;
  ObSqlString sql;
  uint64_t tenant_id = dblink_schema.get_tenant_id();
  uint64_t dblink_id = dblink_schema.get_dblink_id();
  DblinkDriverProto link_type = static_cast<DblinkDriverProto>(dblink_schema.get_driver_proto());
  const char *sql_str_fmt = "SELECT * FROM \"%.*s\".\"%.*s\" WHERE ROWNUM < 1";
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    common::sqlclient::ObISQLConnection *dblink_conn = NULL;
    if (database_name.empty() || table_name.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("table name is empty", K(ret), K(database_name), K(table_name));
    } else if (OB_FAIL(sql.append_fmt(sql_str_fmt, 
                                      database_name.length(), database_name.ptr(),
                                      table_name.length(), table_name.ptr()))) {
      LOG_WARN("append sql failed", K(ret));
    } else if (OB_FAIL(dblink_proxy_->acquire_dblink(dblink_id, link_type, dblink_conn, sessid))) {
      int dblink_errno = ret;
      ObDblinkUtils::process_dblink_errno(link_type, ret);
      LOG_WARN("failed to acquire dblink", K(dblink_errno), K(ret), K(dblink_id));
    } else if (OB_FAIL(dblink_proxy_->dblink_read(dblink_conn, res, sql.ptr()))) {
      int dblink_errno = ret;
      ObDblinkUtils::process_dblink_errno(link_type, dblink_conn, ret);
      LOG_WARN("read link failed", K(dblink_errno), K(ret), K(dblink_id), K(sql.ptr()));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to get result. ", K(ret));
      // dblink will convert the result to AL32UTF8 when pulling schema meta
    } else if (OB_FAIL(result->set_expected_charset_id(common::ObNlsCharsetId::CHARSET_AL32UTF8_ID, 
                                                       common::ObNlsCharsetId::CHARSET_AL32UTF8_ID))) {
      LOG_WARN("failed to set expected charset id", K(ret));
    } else {
      T tmp_table_schema;
      table_schema = NULL;
      ObObjMeta type;
      int64_t column_count = result->get_column_count();
      tmp_table_schema.set_tenant_id(tenant_id);
      tmp_table_schema.set_table_id(1); //no use
      tmp_table_schema.set_dblink_id(dblink_id);
      tmp_table_schema.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      tmp_table_schema.set_charset_type(ObCharset::charset_type_by_coll(tmp_table_schema.get_collation_type()));
      if (OB_FAIL(tmp_table_schema.set_table_name(table_name))) {
        LOG_WARN("set table name failed", K(ret), K(table_name));
      } else if (OB_FAIL(tmp_table_schema.set_link_database_name(database_name))) {
        LOG_WARN("set database name failed", K(ret), K(database_name));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
        ObColumnSchemaV2 column_schema;
        int16_t precision = 0;
        int16_t scale = 0;
        int32_t length = 0;
        ObString column_name;
        bool old_max_length = (GET_MIN_CLUSTER_VERSION() < CLUSTER_VERSION_1470) && !ObSchemaService::g_liboblog_mode_;
        if (OB_FAIL(result->get_col_meta(i, old_max_length, column_name, type, precision, scale, length))) {
          LOG_WARN("failed to get column meta", K(i), K(old_max_length), K(ret));
        } else if (OB_FAIL(column_schema.set_column_name(column_name))) {
          LOG_WARN("failed to set column name", K(i), K(column_name), K(ret));
        } else {
          column_schema.set_table_id(tmp_table_schema.get_table_id());
          column_schema.set_tenant_id(tenant_id);
          column_schema.set_column_id(i + OB_END_RESERVED_COLUMN_ID_NUM);
          column_schema.set_meta_type(type);
          column_schema.set_charset_type(ObCharset::charset_type_by_coll(column_schema.get_collation_type()));
          column_schema.set_data_precision(precision);
          column_schema.set_data_scale(scale);
          column_schema.set_data_length(length);
          LOG_DEBUG("dblink column schema", K(i), K(column_schema.get_data_precision()), 
                                            K(column_schema.get_data_scale()), 
                                            K(column_schema.get_data_length()), 
                                            K(column_schema.get_data_type()));
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(tmp_table_schema.add_column(column_schema))) {
          LOG_WARN("fail to add link column schema. ", K(i), K(column_schema), K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(ObSchemaUtils::alloc_schema(alloctor, tmp_table_schema, table_schema))) {
          LOG_WARN("failed to alloc table_schema", K(ret));
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(try_mock_link_table_column(*table_schema))) {
      LOG_WARN("failed to mock link table pkey", K(ret));
    } 
    if (NULL != dblink_conn) {
      int tmp_ret = OB_SUCCESS;
      if (DBLINK_DRV_OB == link_type && 
          NULL != result &&
          OB_SUCCESS != (tmp_ret = result->close())) {
        LOG_WARN("failed to close result", K(tmp_ret));
      }
      if (OB_SUCCESS != (tmp_ret = dblink_proxy_->release_dblink(link_type, dblink_conn, sessid))) {
        LOG_WARN("failed to relese connection", K(tmp_ret));
      }
      if (OB_SUCC(ret)) {
        ret = tmp_ret;
      }
    }
  }
  return ret;
}

int ObSchemaServiceSQLImpl::try_mock_link_table_column(ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  ObTableSchema::const_column_iterator cs_iter = table_schema.column_begin();
  ObTableSchema::const_column_iterator cs_iter_end = table_schema.column_end();
  uint64_t max_column_id = 0;
  bool need_mock = true;
  for (; OB_SUCC(ret) && cs_iter != cs_iter_end; cs_iter++) {
    const ObColumnSchemaV2 &column_schema = **cs_iter;
    if (need_mock && column_schema.get_rowkey_position() > 0) {
      need_mock = false;
    }
    if (column_schema.get_column_id() > max_column_id) {
      max_column_id = column_schema.get_column_id();
    }
  }
  if (OB_SUCC(ret) && need_mock) {
    // This function should be consistent with ObSchemaRetrieveUtils::retrieve_link_column_schema().
    ObColumnSchemaV2 pkey_column;
    pkey_column.set_table_id(table_schema.get_table_id());
    pkey_column.set_column_id(OB_MOCK_LINK_TABLE_PK_COLUMN_ID);
    pkey_column.set_rowkey_position(1);
    pkey_column.set_data_type(ObUInt64Type);
    pkey_column.set_data_precision(-1);
    pkey_column.set_data_scale(-1);
    pkey_column.set_nullable(0);
    pkey_column.set_collation_type(CS_TYPE_BINARY);
    pkey_column.set_charset_type(CHARSET_BINARY);
    pkey_column.set_column_name(OB_MOCK_LINK_TABLE_PK_COLUMN_NAME);
    pkey_column.set_is_hidden(true);
    if (OB_FAIL(table_schema.add_column(pkey_column))) {
      LOG_WARN("failed to add link table pkey column", K(ret), K(pkey_column));
    }
  }
  return ret;
}

}//namespace schema
}//namespace share
}//namespace oceanbase
