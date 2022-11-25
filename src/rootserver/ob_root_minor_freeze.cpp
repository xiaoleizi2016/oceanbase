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

#define USING_LOG_PREFIX RS

#include "ob_root_minor_freeze.h"

#include "share/ob_srv_rpc_proxy.h"
#include "share/location_cache/ob_location_service.h"
#include "lib/container/ob_se_array.h"
#include "rootserver/ddl_task/ob_ddl_scheduler.h"
#include "rootserver/ob_server_manager.h"
#include "rootserver/ob_unit_manager.h"
#include "rootserver/ob_rs_async_rpc_proxy.h"

namespace oceanbase
{
using namespace common;
using namespace obrpc;
using namespace share;
using namespace share::schema;

namespace rootserver
{
ObRootMinorFreeze::ObRootMinorFreeze()
    :inited_(false),
     stopped_(false),
     rpc_proxy_(NULL),
     server_manager_(NULL),
     unit_manager_(NULL)
{
}

ObRootMinorFreeze::~ObRootMinorFreeze()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(destroy())) {
    LOG_WARN("destroy failed", K(ret));
  }
}

int ObRootMinorFreeze::init(ObSrvRpcProxy &rpc_proxy,
                            ObServerManager &server_manager,
                            ObUnitManager &unit_manager)
{
  int ret = OB_SUCCESS;
  if (inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret));
  } else {
    rpc_proxy_ = &rpc_proxy;
    server_manager_ = &server_manager;
    unit_manager_ = &unit_manager;
    stopped_ = false;
    inited_ = true;
  }

  return ret;
}

void ObRootMinorFreeze::start()
{
  ATOMIC_STORE(&stopped_, false);
}

void ObRootMinorFreeze::stop()
{
  ATOMIC_STORE(&stopped_, true);
}

int ObRootMinorFreeze::destroy()
{
  int ret = OB_SUCCESS;
  inited_ = false;
  return ret;
}

inline
int ObRootMinorFreeze::check_cancel() const
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (ATOMIC_LOAD(&stopped_)) {
    ret = OB_CANCELED;
    LOG_WARN("rs is stopped", K(ret));
  }
  return ret;
}

inline
bool ObRootMinorFreeze::is_server_alive(const ObAddr &server) const
{
  int ret = OB_SUCCESS;
  bool is_alive = false;

  if (OB_LIKELY(server.is_valid())) {
    if (OB_FAIL(server_manager_->check_server_alive(server, is_alive))) {
      LOG_WARN("fail to check whether server is alive, ", K(server), K(ret));
      is_alive = false;
    }
  }

  return is_alive;
}

int ObRootMinorFreeze::get_tenant_server_list(uint64_t tenant_id,
                                              ObIArray<ObAddr> &target_server_list) const
{
  int ret = OB_SUCCESS;

  target_server_list.reset();
  ObSEArray<uint64_t, 2> pool_ids;
  if (OB_FAIL(unit_manager_->get_pool_ids_of_tenant(tenant_id, pool_ids))) {
    LOG_WARN("fail to get pool ids of tenant", K(tenant_id), K(ret));
  } else {
    ObSEArray<share::ObUnitInfo, 4> units;

    for (int i = 0; OB_SUCC(ret) && i < pool_ids.count(); ++i) {
      units.reset();
      if (OB_FAIL(unit_manager_->get_unit_infos_of_pool(pool_ids.at(i), units))) {
        LOG_WARN("fail to get unit infos of pool", K(pool_ids.at(i)), K(ret));
      } else {
        for (int j = 0; j < units.count(); ++j) {
          if (OB_LIKELY(units.at(j).is_valid())) {
            const share::ObUnit &unit = units.at(j).unit_;
            if (is_server_alive(unit.migrate_from_server_)) {
              if (OB_FAIL(target_server_list.push_back(unit.migrate_from_server_))) {
                LOG_WARN("fail to push server, ", K(ret));
              }
            }

            if (is_server_alive(unit.server_)) {
              if (OB_FAIL(target_server_list.push_back(unit.server_))) {
                LOG_WARN("fail to push server, ", K(ret));
              }
            }
          }
        }
      }
    }
  }

  return ret;
}

int ObRootMinorFreeze::try_minor_freeze(const ObIArray<uint64_t> &tenant_ids,
                                        const ObIArray<ObAddr> &server_list,
                                        const common::ObZone &zone,
                                        const common::ObTabletID &tablet_id) const
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObRootMinorFreeze not init", K(ret));
  } else {
    ParamsContainer params;
    if (tablet_id.is_valid()) {
      if (1 == tenant_ids.count()) {
        if (OB_FAIL(init_params_by_tablet_id(tenant_ids.at(0),
                                             tablet_id,
                                             params))) {
          LOG_WARN("fail to init param by tablet_id");
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("only one tenant is required for tablet_freeze", K(ret), K(tablet_id), K(tenant_ids));
      }
    } else if (tenant_ids.count() > 0) {
      if (OB_FAIL(init_params_by_tenant(tenant_ids, zone, server_list, params))) {
        LOG_WARN("fail to init param by tenant, ", K(tenant_ids), K(tablet_id), K(ret));
      }
    } else if (server_list.count() == 0 && zone.size() > 0) {
      if (OB_FAIL(init_params_by_zone(zone, params))) {
        LOG_WARN("fail to init param by tenant, ", K(tenant_ids), K(ret));
      }
    } else {
      if (OB_FAIL(init_params_by_server(server_list, params))) {
        LOG_WARN("fail to init param by server, ", K(server_list), K(ret));
      }
    }

    if (OB_SUCC(ret) && !params.is_empty()) {
      if (OB_FAIL(do_minor_freeze(params))) {
        LOG_WARN("fail to do minor freeze, ", K(ret));
      }
    }
  }

  return ret;
}

int ObRootMinorFreeze::do_minor_freeze(const ParamsContainer &params) const
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  int64_t failure_cnt = 0;
  ObMinorFreezeProxy proxy(*rpc_proxy_, &ObSrvRpcProxy::minor_freeze);
  LOG_INFO("do minor freeze", K(params));

  for (int64_t i = 0; i < params.get_params().count() && OB_SUCC(check_cancel()); ++i) {
    const MinorFreezeParam &param = params.get_params().at(i);

    if (OB_UNLIKELY(OB_SUCCESS != (tmp_ret = proxy.call(param.server,
                                                        MINOR_FREEZE_TIMEOUT, param.arg)))) {
      LOG_WARN("proxy call failed", K(tmp_ret), K(param.arg),
               "dest addr", param.server);
      failure_cnt ++;
    }
  }

  if (OB_FAIL(proxy.wait())) {
    LOG_WARN("proxy wait failed", K(ret));
  } else {
    for (int i = 0; i < proxy.get_results().count(); ++i) {
      if (OB_SUCCESS != (tmp_ret = static_cast<int>(*proxy.get_results().at(i)))) {
        LOG_WARN("fail to do minor freeze on target server, ", K(tmp_ret),
                 "dest addr:", proxy.get_dests().at(i),
                 "param:", proxy.get_args().at(i));
        failure_cnt ++;
      }
    }
  }

  if (0 != failure_cnt && OB_CANCELED != ret) {
    ret = OB_PARTIAL_FAILED;
  }

  return ret;
}

int ObRootMinorFreeze::is_server_belongs_to_zone(const ObAddr &addr,
                                                 const ObZone &zone,
                                                 bool &server_in_zone) const
{
  int ret = OB_SUCCESS;
  ObZone server_zone;

  if (OB_ISNULL(server_manager_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("server_manager_ is NULL", K(ret));
  } else if (0 == zone.size()) {
    server_in_zone = true;
  } else if (OB_FAIL(server_manager_->get_server_zone(addr, server_zone))) {
    LOG_WARN("fail to get server zone", K(ret));
  } else if (server_zone == zone) {
    server_in_zone = true;
  } else {
    server_in_zone = false;
  }

  return ret;
}

int ObRootMinorFreeze::init_params_by_tablet_id(const uint64_t tenant_id,
                                                const common::ObTabletID &tablet_id,
                                                ParamsContainer &params) const
{
  int ret = OB_SUCCESS;

  const int64_t cluster_id = GCONF.cluster_id;
  share::ObLSID ls_id;
  int64_t expire_renew_time = INT64_MAX;
  bool is_cache_hit = false;
  share::ObLSLocation location;
  if (OB_UNLIKELY(nullptr == GCTX.location_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("location service ptr is null", KR(ret));
  } else if (OB_FAIL(GCTX.location_service_->get(tenant_id,
                                                 tablet_id,
                                                 INT64_MAX,
                                                 is_cache_hit,
                                                 ls_id))) {
    LOG_WARN("fail to get ls id according to tablet_id", K(ret), K(tenant_id), K(tablet_id));
  } else if (OB_FAIL(GCTX.location_service_->get(cluster_id,
                                                 tenant_id,
                                                 ls_id,
                                                 expire_renew_time,
                                                 is_cache_hit,
                                                 location))) {
    LOG_WARN("fail to get ls location", KR(ret), K(cluster_id), K(tenant_id), K(ls_id), K(tablet_id));
  } else {
    const ObIArray<ObLSReplicaLocation> &ls_locations = location.get_replica_locations();
    for (int i = 0; i < ls_locations.count() && OB_SUCC(ret); ++i) {
      const ObAddr &server = ls_locations.at(i).get_server();
      if (is_server_alive(server)) {
        if (OB_FAIL(params.add_tenant_server(tenant_id,
                                             tablet_id,
                                             server))) {
          LOG_WARN("fail to add tenant & server, ", K(ret), K(tenant_id), K(ls_id),
                   K(tablet_id));
        }
      } else {
        int tmp_ret = OB_SERVER_NOT_ACTIVE;
        LOG_WARN("server not alive or invalid", "server", server, K(tmp_ret), K(tenant_id),
                 K(ls_id), K(tablet_id));
      }
    }
  }

  return ret;
}

int ObRootMinorFreeze::init_params_by_tenant(const ObIArray<uint64_t> &tenant_ids,
                                             const ObZone &zone,
                                             const ObIArray<ObAddr> &server_list,
                                             ParamsContainer &params) const
{
  int ret = OB_SUCCESS;
  ObSEArray<ObAddr, 256> target_server_list;
  common::ObTabletID tablet_id;
  tablet_id.reset();

  for (int i = 0; i < tenant_ids.count() && OB_SUCC(ret); ++i) {
    if (server_list.count() > 0) {
      for (int j = 0; j < server_list.count() && OB_SUCC(ret); ++j) {
        if (is_server_alive(server_list.at(j))) {
          if (OB_FAIL(params.add_tenant_server(tenant_ids.at(i),
                                               tablet_id,
                                               server_list.at(j)))) {
            LOG_WARN("fail to add tenant & server, ", K(ret));
          }
        } else {
          ret = OB_SERVER_NOT_ACTIVE;
          LOG_WARN("server not alive or invalid", "server", server_list.at(j), K(ret));
        }
      }
    } else {
      // TODO: filter servers according to tenant_id
      if (OB_FAIL(get_tenant_server_list(tenant_ids.at(i), target_server_list))) {
        LOG_WARN("fail to get tenant server list, ", K(ret));
      } else {
        bool server_in_zone = false;
        for (int j = 0; j < target_server_list.count() && OB_SUCC(ret); ++j) {
          const ObAddr &server = target_server_list.at(j);
          if (OB_FAIL(is_server_belongs_to_zone(server, zone, server_in_zone))) {
            LOG_WARN("fail to check server", K(ret));
          } else if (server_in_zone && OB_FAIL(params.add_tenant_server(tenant_ids.at(i),
                                                                        tablet_id,
                                                                        server))) {
            LOG_WARN("fail to add tenant & server", K(ret));
          }
        }
      }
    }
  }

  return ret;
}

int ObRootMinorFreeze::init_params_by_zone(const ObZone &zone,
                                           ParamsContainer &params) const
{
  int ret = OB_SUCCESS;
  ObArray<ObAddr> target_server_list;

  if (OB_UNLIKELY(0 == zone.size())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    if (OB_FAIL(server_manager_->get_servers_of_zone(zone, target_server_list))) {
      LOG_WARN("fail to get tenant server list, ", K(ret));
    } else if (0 == target_server_list.count()) {
      ret = OB_ZONE_NOT_ACTIVE;
      LOG_WARN("empty zone or invalid", K(zone), K(ret));
    } else {
      for (int i = 0; i < target_server_list.count() && OB_SUCC(ret); ++i) {
        if (OB_FAIL(params.add_server(target_server_list.at(i)))) {
          LOG_WARN("fail to add server", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObRootMinorFreeze::init_params_by_server(const ObIArray<ObAddr> &server_list,
                                             ParamsContainer &params) const
{
  int ret = OB_SUCCESS;
  if (server_list.count() > 0) {
    for (int i = 0; i < server_list.count() && OB_SUCC(ret); ++i) {
      if (is_server_alive(server_list.at(i))) {
        if (OB_FAIL(params.add_server(server_list.at(i)))) {
          LOG_WARN("fail to add server, ", K(ret));
        }
      } else {
        ret = OB_SERVER_NOT_ACTIVE;
        LOG_WARN("server not alive or invalid", "server", server_list.at(i), K(ret));
      }
    }
  } else {
    ObZone zone; // empty zone, get all server status
    ObSEArray<ObAddr, 256> target_server_list;

    // get all alive server
    if (OB_FAIL(server_manager_->get_alive_servers(zone, target_server_list))) {
      LOG_WARN("fail to get alive servers, ", K(ret));
    } else {
      for (int i = 0; i < target_server_list.count() && OB_SUCC(ret); ++i) {
        if (OB_FAIL(params.add_server(target_server_list.at(i)))) {
          LOG_WARN("fail to add server, ", K(ret));
        }
      }
    }
  }

  return ret;
}

int ObRootMinorFreeze::ParamsContainer::add_server(const ObAddr &server)
{
  int ret = OB_SUCCESS;

  MinorFreezeParam param;
  param.server = server;
  // leave empty with param.arg means **all**

  if (OB_FAIL(params_.push_back(param))) {
    LOG_WARN("fail to push server, ", K(ret));
  }

  return ret;
}

int ObRootMinorFreeze::ParamsContainer::add_tenant_server(uint64_t tenant_id,
                                                          const common::ObTabletID &tablet_id,
                                                          const ObAddr &server)
{
  int ret = OB_SUCCESS;

  MinorFreezeParam param;
  param.server = server;
  param.arg.tablet_id_ = tablet_id;

  if (OB_FAIL(param.arg.tenant_ids_.push_back(tenant_id))) {
    LOG_WARN("fail to push tenant_id, ", K(ret));
  } else if (OB_FAIL(params_.push_back(param))) {
    LOG_WARN("fail to push tenant_id & server, ", K(ret));
  }

  return ret;
}

} // namespace rootserver
} // namespace oceanbase
