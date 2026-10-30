/*
 * Derived from DXMT src/winemetal/unix/cache.c (https://github.com/3Shain/dxmt)
 * Copyright (c) 2023-2026 Feifan He for CodeWeavers
 * SPDX-License-Identifier: LGPL-2.1-or-later (see bridge/LICENSE, bridge/COPYING.LIB)
 *
 * Changed for d9mt by:
 *   neo773 (d9mt-v2), 2026-07 to 2026-08
 *   Jan Drlý and contributors (d9mt), 2026-10
 * The changes are listed in bridge/UPSTREAM; the git history has each one with its date.
 */

#import <Foundation/Foundation.h>
#include "sqlite3.h"
#define WINEMETAL_API
#include "../winemetal_thunks.h"

@interface CacheReader : NSObject
- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version;
- (dispatch_data_t)get:(NSData *)key;
@end

@interface CacheWriter : NSObject
- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version;
- (void)set:(NSData *)key value:(dispatch_data_t)value;
@end

@interface CacheReader () {
  sqlite3 *_db;
  sqlite3_stmt *_stmt;
}
@end

static inline NSString *
resolve_cache_dir(NSString *path, bool path_is_file) {
  if (![path hasPrefix:@"/"]) {
    char buf[PATH_MAX];
    size_t len = confstr(_CS_DARWIN_USER_CACHE_DIR, buf, sizeof(buf));

    if (!len) {
      return nil;
    }

    NSString *base = [NSString stringWithUTF8String:buf];
    path = [base stringByAppendingPathComponent:path];
  }
  [[NSFileManager defaultManager] createDirectoryAtPath:path_is_file ? [path stringByDeletingLastPathComponent] : path
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  return path;
}

@implementation CacheReader

- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version {
  if ((self = [super init])) {
    NSString *dbPath = resolve_cache_dir(path, true);
    if (!dbPath) {
      NSLog(@"[CacheReader] Failed to resolve cache path");
      return nil;
    }
    NSString *tableName = [NSString stringWithFormat:@"cache_%llu", version];
    if (sqlite3_open_v2([dbPath fileSystemRepresentation], &_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) !=
        SQLITE_OK) {
      NSLog(@"[CacheReader] Failed to open DB: %s", sqlite3_errmsg(_db));
      sqlite3_close(_db);
      return nil;
    }

    NSString *sqlGet = [NSString stringWithFormat:@"SELECT value FROM %@ WHERE key = ?;", tableName];
    if (sqlite3_prepare_v2(_db, sqlGet.UTF8String, -1, &_stmt, NULL) != SQLITE_OK) {
      NSLog(@"[CacheReader] Failed to prepare SELECT: %s", sqlite3_errmsg(_db));
      sqlite3_close(_db);
      return nil;
    }
  }
  return self;
}

- (dispatch_data_t)get:(NSData *)key {
  sqlite3_reset(_stmt);
  sqlite3_clear_bindings(_stmt);
  sqlite3_bind_blob64(_stmt, 1, key.bytes, key.length, SQLITE_STATIC);

  dispatch_data_t result = nil;
  if (sqlite3_step(_stmt) == SQLITE_ROW) {
    const void *bytes = sqlite3_column_blob(_stmt, 0);
    int len = sqlite3_column_bytes(_stmt, 0);
    result = dispatch_data_create(bytes, len, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  }
  sqlite3_reset(_stmt);
  return result;
}

- (void)dealloc {
  if (_stmt)
    sqlite3_finalize(_stmt);
  if (_db)
    sqlite3_close(_db);
  [super dealloc];
}

@end

@interface CacheWriter () {
  sqlite3 *_db;
  sqlite3_stmt *_stmt;
}
@end

@implementation CacheWriter

- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version {
  if ((self = [super init])) {
    NSString *dbPath = resolve_cache_dir(path, true);
    if (!dbPath) {
      NSLog(@"[CacheReader] Failed to resolve cache path");
      return nil;
    }

    NSString *lockPath = [dbPath stringByAppendingString:@"-lock"];
    int fd = open([lockPath fileSystemRepresentation], O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
      NSLog(@"[CacheWriter] Failed to open file for locking %@", lockPath);
      return nil;
    }
    flock(fd, LOCK_EX);

    if (sqlite3_open_v2(
            [dbPath fileSystemRepresentation], &_db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL
        ) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to open DB: %s", sqlite3_errmsg(_db));
      flock(fd, LOCK_UN);
      close(fd);
      return nil;
    }

    sqlite3_exec(_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    NSString *tableName = [NSString stringWithFormat:@"cache_%llu", version];
    NSString *sqlCreate = [NSString stringWithFormat:@"CREATE TABLE IF NOT EXISTS %@ ("
                                                      "key BLOB PRIMARY KEY, "
                                                      "value BLOB NOT NULL);",
                                                     tableName];

    char *errMsg = NULL;
    if (sqlite3_exec(_db, sqlCreate.UTF8String, NULL, NULL, &errMsg) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to create table: %s", errMsg);
      sqlite3_free(errMsg);
    }

    flock(fd, LOCK_UN);
    close(fd);

    NSString *sqlSet = [NSString stringWithFormat:@"INSERT OR REPLACE INTO %@ (key, value) VALUES (?, ?);", tableName];
    if (sqlite3_prepare_v2(_db, sqlSet.UTF8String, -1, &_stmt, NULL) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to prepare INSERT: %s", sqlite3_errmsg(_db));
      sqlite3_close(_db);
      return nil;
    }
  }
  return self;
}

- (void)set:(NSData *)key value:(dispatch_data_t)value {
  sqlite3_reset(_stmt);
  sqlite3_clear_bindings(_stmt);
  sqlite3_bind_blob64(_stmt, 1, key.bytes, key.length, SQLITE_STATIC);

  const void *bytes = NULL;
  size_t length = 0;
  dispatch_data_t flat = dispatch_data_create_map(value, &bytes, &length);
  sqlite3_bind_blob64(_stmt, 2, bytes, length, SQLITE_STATIC);

  if (sqlite3_step(_stmt) != SQLITE_DONE) {
    NSLog(@"[CacheWriter] Failed to insert: %s", sqlite3_errmsg(_db));
  }

  dispatch_release(flat);
  sqlite3_reset(_stmt);
}

- (void)dealloc {
  if (_stmt)
    sqlite3_finalize(_stmt);
  if (_db)
    sqlite3_close(_db);
  [super dealloc];
}

@end

/* d9mt: before the reader and writer open cache_<version>. With adopt != 0,
 * if cache_<version> doesn't exist and cache_<adopt> does, cache_<adopt> is
 * renamed to cache_<version>: a predecessor's metallibs kept under the same
 * keys (ADR-0006). Every other cache_<n> table is dropped, so per-build tables
 * don't accumulate. One write transaction: another process sees all of it or
 * none. Returns 1 if a table was adopted, 0 if not, -1 if nothing could be
 * done (the tables are then left as they are; the writer creates its own). */
int
d9mt_cache_prepare(NSString *path, uint64_t version, uint64_t adopt) {
  NSString *dbPath = resolve_cache_dir(path, true);
  if (!dbPath || ![[NSFileManager defaultManager] fileExistsAtPath:dbPath])
    return 0;
  sqlite3 *db = NULL;
  if (sqlite3_open_v2([dbPath fileSystemRepresentation], &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) !=
      SQLITE_OK) {
    sqlite3_close(db);
    return -1;
  }
  sqlite3_busy_timeout(db, 2000);
  if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
    sqlite3_close(db);
    return -1;
  }
  NSString *keep = [NSString stringWithFormat:@"cache_%llu", version];
  NSString *from = adopt ? [NSString stringWithFormat:@"cache_%llu", adopt] : nil;
  NSMutableArray<NSString *> *drop = [NSMutableArray array];
  bool have_keep = false, have_from = false;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'cache\\_%' ESCAPE '\\';",
                         -1, &stmt, NULL) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const unsigned char *name = sqlite3_column_text(stmt, 0);
      NSString *n = name ? [NSString stringWithUTF8String:(const char *)name] : nil;
      if (!n)
        continue;
      if ([n isEqualToString:keep])
        have_keep = true;
      else if (from && [n isEqualToString:from])
        have_from = true;
      else
        [drop addObject:n];
    }
  }
  sqlite3_finalize(stmt);
  int adopted = 0;
  if (have_from && !have_keep) {
    NSString *sql = [NSString stringWithFormat:@"ALTER TABLE \"%@\" RENAME TO \"%@\";", from, keep];
    if (sqlite3_exec(db, sql.UTF8String, NULL, NULL, NULL) == SQLITE_OK)
      adopted = 1;
    else
      [drop addObject:from];
  } else if (have_from) {
    [drop addObject:from];
  }
  for (NSString *n in drop) {
    NSString *sql = [NSString stringWithFormat:@"DROP TABLE IF EXISTS \"%@\";", n];
    sqlite3_exec(db, sql.UTF8String, NULL, NULL, NULL);
  }
  if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    sqlite3_close(db);
    return -1;
  }
  if (drop.count)
    sqlite3_exec(db, "VACUUM;", NULL, NULL, NULL);
  sqlite3_close(db);
  return adopted;
}

int
_CacheReader_alloc_init(void *obj) {
  struct unixcall_cache_alloc_init *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  params->ret_cache = (obj_handle_t)[[CacheReader alloc] initWithPath:path version:params->version];
  [path release];
  return 0;
}

int
_CacheReader_get(void *obj) {
  struct unixcall_cache_get *params = obj;
  NSData *key =
      [[NSData alloc] initWithBytesNoCopy:(void *)params->key.ptr length:params->key_length freeWhenDone:false];
  CacheReader *reader = (CacheReader *)params->cache;
  params->ret_data = (obj_handle_t)[reader get:key];
  [key release];
  return 0;
}

int
_CacheWriter_alloc_init(void *obj) {
  struct unixcall_cache_alloc_init *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  params->ret_cache = (obj_handle_t)[[CacheWriter alloc] initWithPath:path version:params->version];
  [path release];
  return 0;
}

int
_CacheWriter_set(void *obj) {
  struct unixcall_cache_set *params = obj;
  NSData *key =
      [[NSData alloc] initWithBytesNoCopy:(void *)params->key.ptr length:params->key_length freeWhenDone:false];
  CacheWriter *writer = (CacheWriter *)params->cache;
  [writer set:key value:(dispatch_data_t)params->value_data];
  [key release];
  return 0;
}

#ifndef DXMT_NO_PRIVATE_API

extern void MTLSetShaderCachePath(NSString* path);
extern NSString* MTLGetShaderCachePath();

int
_WMTSetMetalShaderCachePath(void *obj) {
  struct unixcall_setmetalcachepath *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  NSString *resolved_path = resolve_cache_dir(path, false);
  MTLSetShaderCachePath(resolved_path);
  params->ret_success = [MTLGetShaderCachePath() isEqualToString:resolved_path];
  [path release];
  return 0;
};

#else

int
WMTSetMetalShaderCachePath(void *obj) {
  struct unixcall_setmetalcachepath *params = obj;
  params->ret_success = 0;
  return 0;
}

#endif
