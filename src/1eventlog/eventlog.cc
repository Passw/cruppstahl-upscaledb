/*
 * Copyright (C) 2005-2015 Christoph Rupp (chris@crupp.de).
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAM_ENABLE_EVENT_LOGGING

#include "0root/root.h"

#include <stdio.h>
#include <stdarg.h>
#include <map>
#include <string>

// Always verify that a file of level N does not include headers > N!
#include "1base/util.h"
#include "1base/spinlock.h"
#include "1eventlog/eventlog.h"

#ifndef HAM_ROOT_H
#  error "root.h was not included"
#endif

namespace hamsterdb {

namespace EventLog {

struct EventLogDesc {
  // For synchronization
  Spinlock mutex;

  // The file handles
  std::map<std::string, FILE *> files;

  // temporary buffer for escaping incoming data
  char temp[1024 * 8];
};

static EventLogDesc event_log;

static std::string
path_from_filename(const char *filename)
{
  std::string path = filename;
  path += ".elog";
  return (path);
}

static void
open_or_create(const char *filename, const char *mode)
{
  if (!filename || !*filename)
    filename = "hamsterdb-inmem";

  FILE *f = event_log.files[filename];
  if (f && ::strcmp(filename, "hamsterdb-inmem") != 0) {
    ::fprintf(f, "ERROR creating/opening log which already exists (%s, %s)\n",
            filename, mode);
    ::fflush(f);
    return;
  }

  std::string path = path_from_filename(filename);
  f = ::fopen(path.c_str(), mode);
  if (!f) {
    ham_trace(("failed to create event log: %s", ::strerror(errno)));
    path = "lost+found.elog";
    f = ::fopen(path.c_str(), mode);
    if (!f)
      throw Exception(HAM_IO_ERROR);
  }
  event_log.files[filename] = f;
}

void
lock()
{
  event_log.mutex.lock();
}

void
unlock()
{
  event_log.mutex.unlock();
}

void
close(const char *filename)
{
  if (!filename || !*filename)
    filename = "hamsterdb-inmem";

  FILE *f = event_log.files[filename];
  if (f) {
    ::fclose(f);
    event_log.files.erase(event_log.files.find(filename));
  }
}

void
create(const char *filename)
{
  open_or_create(filename, "w");
}

void
open(const char *filename)
{
  open_or_create(filename, "a+");
}

void
append(const char *filename, const char *tag, const char *format, ...)
{
  if (!filename || !*filename)
    filename = "hamsterdb-inmem";

  char buffer[1024 * 4];
  va_list ap;
  va_start(ap, format);
  util_vsnprintf(buffer, sizeof(buffer), format, ap);
  va_end(ap);

  FILE *f = event_log.files[filename];
  if (!f) {
    open(filename);
    f = event_log.files[filename];
  }
  if (!f) {
    create(filename);
    f = event_log.files[filename];
  }
  if (!f)
    return;

  ::fprintf(f, "%s(%s);\n", tag, buffer);
  ::fflush(f);
}

const char *
escape(const void *data, size_t size)
{
  if (size > 512)
    size = 512;

  const char *d = (const char *)data;
  char *out = &event_log.temp[0];
  *out = '\"';
  out++;
  for (size_t i = 0; i < size; i++) {
    if (isascii(d[i])) {
      *out = d[i];
      out++;
    }
    else {
      out += ::sprintf(out, "\\x%02x", d[i]);
    }
  }
  *out = '\"';
  out++;
  *out = '\0';
  return (event_log.temp);
}

} // namespace EventLog

} // namespace hamsterdb

#endif /* HAM_ENABLE_EVENT_LOGGING */
