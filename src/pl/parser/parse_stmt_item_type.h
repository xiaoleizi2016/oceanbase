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

#ifndef OCEANBASE_SRC_PL_PARSER_PARSE_STMT_ITEM_TYPE_H_
#define OCEANBASE_SRC_PL_PARSER_PARSE_STMT_ITEM_TYPE_H_
#ifdef __cplusplus
extern "C" {
#endif

enum SpParam
{
  MODE_IN = 0,
  MODE_OUT = 1,
  MODE_INOUT = 2
};

enum SpHandlerType
{
  SP_HANDLER_TYPE_EXIT = 0,
  SP_HANDLER_TYPE_CONTINUE = 1
};

enum SignalCondInfoItem
{
  DIAG_CLASS_ORIGIN = 0,
  DIAG_SUBCLASS_ORIGIN = 1,
  DIAG_CONSTRAINT_CATALOG = 2,
  DIAG_CONSTRAINT_SCHEMA = 3,
  DIAG_CONSTRAINT_NAME = 4,
  DIAG_CATALOG_NAME = 5,
  DIAG_SCHEMA_NAME = 6,
  DIAG_TABLE_NAME = 7,
  DIAG_COLUMN_NAME = 8,
  DIAG_CURSOR_NAME = 9,
  DIAG_MESSAGE_TEXT = 10,
  DIAG_MYSQL_ERRNO = 11
};

enum SpAuthMode
{
  SP_DEFINER = 0,
  SP_CURRENT_USER = 1,
  SP_INVOKER = 2
};

enum SpUnitKind
{
  SP_INVALID = 0,
  SP_FUNCTION,
  SP_PROCEDURE, 
  SP_PACKAGE,
  SP_TRIGGER,
  SP_TYPE
};

enum SpIntegerType
{
  SP_PLS_INTEGER,
  SP_BINARY_INTEGER,
  SP_NATURAL,
  SP_NATURALN,
  SP_POSITIVE,
  SP_POSITIVEN,
  SP_SIGNTYPE,
  SP_SIMPLE_INTEGER
};

enum PackageComplieUnit
{
  PACKAGE_UNIT_PACKAGE = 0,
  PACKAGE_UNIT_SPECIFICATION = 1,
  PACKAGE_UNIT_BODY = 2
};

enum PackageAlterOptions
{
  PACKAGE_ALTER_COMPILE,
  PACKAGE_ALTER_EDITIONABLE,
  PACKAGE_ALTER_NONEDITIONABLE
};

#ifdef __cplusplus
}
#endif
#endif /* OCEANBASE_SRC_PL_PARSER_PARSE_STMT_ITEM_TYPE_H_ */
