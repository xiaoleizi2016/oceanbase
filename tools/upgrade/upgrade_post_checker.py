#!/usr/bin/env python
# -*- coding: utf-8 -*-

import sys
import os
import time
import mysql.connector
from mysql.connector import errorcode
import logging
import getopt
import time

class UpgradeParams:
  log_filename = 'upgrade_post_checker.log'
  new_version = '4.1.0.0'
#### --------------start : my_error.py --------------
class MyError(Exception):
  def __init__(self, value):
    self.value = value
  def __str__(self):
    return repr(self.value)
#### --------------start : actions.py------------
class Cursor:
  __cursor = None
  def __init__(self, cursor):
    self.__cursor = cursor
  def exec_sql(self, sql, print_when_succ = True):
    try:
      self.__cursor.execute(sql)
      rowcount = self.__cursor.rowcount
      if True == print_when_succ:
        logging.info('succeed to execute sql: %s, rowcount = %d', sql, rowcount)
      return rowcount
    except mysql.connector.Error, e:
      logging.exception('mysql connector error, fail to execute sql: %s', sql)
      raise e
    except Exception, e:
      logging.exception('normal error, fail to execute sql: %s', sql)
      raise e
  def exec_query(self, sql, print_when_succ = True):
    try:
      self.__cursor.execute(sql)
      results = self.__cursor.fetchall()
      rowcount = self.__cursor.rowcount
      if True == print_when_succ:
        logging.info('succeed to execute query: %s, rowcount = %d', sql, rowcount)
      return (self.__cursor.description, results)
    except mysql.connector.Error, e:
      logging.exception('mysql connector error, fail to execute sql: %s', sql)
      raise e
    except Exception, e:
      logging.exception('normal error, fail to execute sql: %s', sql)
      raise e

def set_parameter(cur, parameter, value):
  sql = """alter system set {0} = '{1}'""".format(parameter, value)
  logging.info(sql)
  cur.execute(sql)
  wait_parameter_sync(cur, parameter, value)

def wait_parameter_sync(cur, key, value):
  sql = """select count(*) as cnt from oceanbase.__all_virtual_sys_parameter_stat
           where name = '{0}' and value != '{1}'""".format(key, value)
  times = 10
  while times > 0:
    logging.info(sql)
    cur.execute(sql)
    result = cur.fetchall()
    if len(result) != 1 or len(result[0]) != 1:
      logging.exception('result cnt not match')
      raise e
    elif result[0][0] == 0:
      logging.info("""{0} is sync, value is {1}""".format(key, value))
      break
    else:
      logging.info("""{0} is not sync, value should be {1}""".format(key, value))

    times -= 1
    if times == 0:
      logging.exception("""check {0}:{1} sync timeout""".format(key, value))
      raise e
    time.sleep(5)

#### --------------start :  opt.py --------------
help_str = \
"""
Help:
""" +\
sys.argv[0] + """ [OPTIONS]""" +\
'\n\n' +\
'-I, --help          Display this help and exit.\n' +\
'-V, --version       Output version information and exit.\n' +\
'-h, --host=name     Connect to host.\n' +\
'-P, --port=name     Port number to use for connection.\n' +\
'-u, --user=name     User for login.\n' +\
'-p, --password=name Password to use when connecting to server. If password is\n' +\
'                    not given it\'s empty string "".\n' +\
'-m, --module=name   Modules to run. Modules should be a string combined by some of\n' +\
'                    the following strings: ddl, normal_dml, each_tenant_dml,\n' +\
'                    system_variable_dml, special_action, all. "all" represents\n' +\
'                    that all modules should be run. They are splitted by ",".\n' +\
'                    For example: -m all, or --module=ddl,normal_dml,special_action\n' +\
'-l, --log-file=name Log file path. If log file path is not given it\'s ' + os.path.splitext(sys.argv[0])[0] + '.log\n' +\
'\n\n' +\
'Maybe you want to run cmd like that:\n' +\
sys.argv[0] + ' -h 127.0.0.1 -P 3306 -u admin -p admin\n'

version_str = """version 1.0.0"""

class Option:
  __g_short_name_set = set([])
  __g_long_name_set = set([])
  __short_name = None
  __long_name = None
  __is_with_param = None
  __is_local_opt = None
  __has_value = None
  __value = None
  def __init__(self, short_name, long_name, is_with_param, is_local_opt, default_value = None):
    if short_name in Option.__g_short_name_set:
      raise MyError('duplicate option short name: {0}'.format(short_name))
    elif long_name in Option.__g_long_name_set:
      raise MyError('duplicate option long name: {0}'.format(long_name))
    Option.__g_short_name_set.add(short_name)
    Option.__g_long_name_set.add(long_name)
    self.__short_name = short_name
    self.__long_name = long_name
    self.__is_with_param = is_with_param
    self.__is_local_opt = is_local_opt
    self.__has_value = False
    if None != default_value:
      self.set_value(default_value)
  def is_with_param(self):
    return self.__is_with_param
  def get_short_name(self):
    return self.__short_name
  def get_long_name(self):
    return self.__long_name
  def has_value(self):
    return self.__has_value
  def get_value(self):
    return self.__value
  def set_value(self, value):
    self.__value = value
    self.__has_value = True
  def is_local_opt(self):
    return self.__is_local_opt
  def is_valid(self):
    return None != self.__short_name and None != self.__long_name and True == self.__has_value and None != self.__value

g_opts =\
[\
Option('I', 'help', False, True),\
Option('V', 'version', False, True),\
Option('h', 'host', True, False),\
Option('P', 'port', True, False),\
Option('u', 'user', True, False),\
Option('p', 'password', True, False, ''),\
# 要跑哪个模块，默认全跑
Option('m', 'module', True, False, 'all'),\
# 日志文件路径，不同脚本的main函数中中会改成不同的默认值
Option('l', 'log-file', True, False)
]\

def change_opt_defult_value(opt_long_name, opt_default_val):
  global g_opts
  for opt in g_opts:
    if opt.get_long_name() == opt_long_name:
      opt.set_value(opt_default_val)
      return

def has_no_local_opts():
  global g_opts
  no_local_opts = True
  for opt in g_opts:
    if opt.is_local_opt() and opt.has_value():
      no_local_opts = False
  return no_local_opts

def check_db_client_opts():
  global g_opts
  for opt in g_opts:
    if not opt.is_local_opt() and not opt.has_value():
      raise MyError('option "-{0}" has not been specified, maybe you should run "{1} --help" for help'\
          .format(opt.get_short_name(), sys.argv[0]))

def parse_option(opt_name, opt_val):
  global g_opts
  for opt in g_opts:
    if opt_name in (('-' + opt.get_short_name()), ('--' + opt.get_long_name())):
      opt.set_value(opt_val)

def parse_options(argv):
  global g_opts
  short_opt_str = ''
  long_opt_list = []
  for opt in g_opts:
    if opt.is_with_param():
      short_opt_str += opt.get_short_name() + ':'
    else:
      short_opt_str += opt.get_short_name()
  for opt in g_opts:
    if opt.is_with_param():
      long_opt_list.append(opt.get_long_name() + '=')
    else:
      long_opt_list.append(opt.get_long_name())
  (opts, args) = getopt.getopt(argv, short_opt_str, long_opt_list)
  for (opt_name, opt_val) in opts:
    parse_option(opt_name, opt_val)
  if has_no_local_opts():
    check_db_client_opts()

def deal_with_local_opt(opt):
  if 'help' == opt.get_long_name():
    global help_str
    print help_str
  elif 'version' == opt.get_long_name():
    global version_str
    print version_str

def deal_with_local_opts():
  global g_opts
  if has_no_local_opts():
    raise MyError('no local options, can not deal with local options')
  else:
    for opt in g_opts:
      if opt.is_local_opt() and opt.has_value():
        deal_with_local_opt(opt)
        # 只处理一个
        return

def get_opt_host():
  global g_opts
  for opt in g_opts:
    if 'host' == opt.get_long_name():
      return opt.get_value()

def get_opt_port():
  global g_opts
  for opt in g_opts:
    if 'port' == opt.get_long_name():
      return opt.get_value()

def get_opt_user():
  global g_opts
  for opt in g_opts:
    if 'user' == opt.get_long_name():
      return opt.get_value()

def get_opt_password():
  global g_opts
  for opt in g_opts:
    if 'password' == opt.get_long_name():
      return opt.get_value()

def get_opt_module():
  global g_opts
  for opt in g_opts:
    if 'module' == opt.get_long_name():
      return opt.get_value()

def get_opt_log_file():
  global g_opts
  for opt in g_opts:
    if 'log-file' == opt.get_long_name():
      return opt.get_value()
#### ---------------end----------------------

def config_logging_module(log_filenamme):
  logging.basicConfig(level=logging.INFO,\
      format='[%(asctime)s] %(levelname)s %(filename)s:%(lineno)d %(message)s',\
      datefmt='%Y-%m-%d %H:%M:%S',\
      filename=log_filenamme,\
      filemode='w')
  # 定义日志打印格式
  formatter = logging.Formatter('[%(asctime)s] %(levelname)s %(filename)s:%(lineno)d %(message)s', '%Y-%m-%d %H:%M:%S')
  #######################################
  # 定义一个Handler打印INFO及以上级别的日志到sys.stdout
  stdout_handler = logging.StreamHandler(sys.stdout)
  stdout_handler.setLevel(logging.INFO)
  # 设置日志打印格式
  stdout_handler.setFormatter(formatter)
  # 将定义好的stdout_handler日志handler添加到root logger
  logging.getLogger('').addHandler(stdout_handler)

#### STAR
# 1 检查版本号
def check_cluster_version(query_cur):
  # 一方面配置项生效是个异步生效任务,另一方面是2.2.0之后新增租户级配置项刷新，和系统级配置项刷新复用同一个timer，这里暂且等一下。
  times = 30
  sql="select distinct value = '{0}' from oceanbase.GV$OB_PARAMETERS  where name='min_observer_version'".format(upgrade_params.new_version)
  while times > 0 :
    (desc, results) = query_cur.exec_query(sql)
    if len(results) == 1 and results[0][0] == 1:
      break;
    time.sleep(1)
    times -=1
  if times == 0:
    logging.warn("check cluster version timeout!")
    raise e
  else:
    logging.info("check_cluster_version success")

# 2 检查内部表自检是否成功
def check_root_inspection(query_cur):
  sql = "select count(*) from oceanbase.__all_virtual_upgrade_inspection where info != 'succeed'"
  times = 180
  while times > 0 :
    (desc, results) = query_cur.exec_query(sql)
    if results[0][0] == 0:
      break
    time.sleep(10)
    times -= 1
  if times == 0:
    logging.warn('check root inspection failed!')
    raise e
  logging.info('check root inspection success')

# 4 开ddl
def enable_ddl(cur):
  set_parameter(cur, 'enable_ddl', 'True')

# 5 打开rebalance
def enable_rebalance(cur):
  set_parameter(cur, 'enable_rebalance', 'True')

# 6 打开rereplication
def enable_rereplication(cur):
  set_parameter(cur, 'enable_rereplication', 'True')

# 7 打开major freeze
def enable_major_freeze(cur):
  set_parameter(cur, 'enable_major_freeze', 'True')

def fetch_tenant_ids(query_cur):
  try:
    tenant_id_list = []
    (desc, results) = query_cur.exec_query("""select distinct tenant_id from oceanbase.__all_tenant order by tenant_id desc""")
    for r in results:
      tenant_id_list.append(r[0])
    return tenant_id_list
  except Exception, e:
    logging.exception('fail to fetch distinct tenant ids')
    raise e

# 开始升级后的检查
def do_check(my_host, my_port, my_user, my_passwd, upgrade_params):
  try:
    conn = mysql.connector.connect(user = my_user,
                                   password = my_passwd,
                                   host = my_host,
                                   port = my_port,
                                   database = 'oceanbase',
                                   raise_on_warnings = True)
    conn.autocommit = True
    cur = conn.cursor(buffered=True)
    try:
      query_cur = Cursor(cur)
      try:
        check_cluster_version(query_cur)
        check_root_inspection(query_cur)
        enable_ddl(cur)
        enable_rebalance(cur)
        enable_rereplication(cur)
        enable_major_freeze(cur)
      except Exception, e:
        logging.exception('run error')
        raise e
    except Exception, e:
      logging.exception('run error')
      raise e
    finally:
      cur.close()
      conn.close()
  except mysql.connector.Error, e:
    logging.exception('connection error')
    raise e
  except Exception, e:
    logging.exception('normal error')
    raise e

if __name__ == '__main__':
  upgrade_params = UpgradeParams()
  change_opt_defult_value('log-file', upgrade_params.log_filename)
  parse_options(sys.argv[1:])
  if not has_no_local_opts():
    deal_with_local_opts()
  else:
    check_db_client_opts()
    log_filename = get_opt_log_file()
    upgrade_params.log_filename = log_filename
    # 日志配置放在这里是为了前面的操作不要覆盖掉日志文件
    config_logging_module(upgrade_params.log_filename)
    try:
      host = get_opt_host()
      port = int(get_opt_port())
      user = get_opt_user()
      password = get_opt_password()
      logging.info('parameters from cmd: host=\"%s\", port=%s, user=\"%s\", password=\"%s\", log-file=\"%s\"',\
          host, port, user, password, log_filename)
      do_check(host, port, user, password, upgrade_params)
    except mysql.connector.Error, e:
      logging.exception('mysql connctor error')
      raise e
    except Exception, e:
      logging.exception('normal error')
      raise e

