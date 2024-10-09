/****************************************************************************
 * fs/procfs/fs_procfscritmon.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/procfs.h>

#include "fs_heap.h"

#if !defined(CONFIG_DISABLE_MOUNTPOINT) && defined(CONFIG_FS_PROCFS) && \
     defined(CONFIG_SCHED_CRITMONITOR)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Determines the size of an intermediate buffer that must be large enough
 * to handle the longest line generated by this logic.
 */

#define CRITMON_LINELEN 64

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure describes one open "file" */

struct critmon_file_s
{
  struct procfs_file_s  base;   /* Base open file structure */
  unsigned int linesize;        /* Number of valid characters in line[] */
  char line[CRITMON_LINELEN];   /* Pre-allocated buffer for formatted lines */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* File system methods */

static int     critmon_open(FAR struct file *filep, FAR const char *relpath,
                 int oflags, mode_t mode);
static int     critmon_close(FAR struct file *filep);
static ssize_t critmon_read(FAR struct file *filep, FAR char *buffer,
                 size_t buflen);
static int     critmon_dup(FAR const struct file *oldp,
                 FAR struct file *newp);
static int     critmon_stat(FAR const char *relpath, FAR struct stat *buf);

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* See fs_mount.c -- this structure is explicitly externed there.
 * We use the old-fashioned kind of initializers so that this will compile
 * with any compiler.
 */

const struct procfs_operations g_critmon_operations =
{
  critmon_open,       /* open */
  critmon_close,      /* close */
  critmon_read,       /* read */
  NULL,               /* write */
  NULL,               /* poll */

  critmon_dup,        /* dup */

  NULL,              /* opendir */
  NULL,              /* closedir */
  NULL,              /* readdir */
  NULL,              /* rewinddir */

  critmon_stat        /* stat */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: critmon_open
 ****************************************************************************/

static int critmon_open(FAR struct file *filep, FAR const char *relpath,
                      int oflags, mode_t mode)
{
  FAR struct critmon_file_s *attr;

  finfo("Open '%s'\n", relpath);

  /* PROCFS is read-only.  Any attempt to open with any kind of write
   * access is not permitted.
   *
   * REVISIT:  Write-able proc files could be quite useful.
   */

  if ((oflags & O_WRONLY) != 0 || (oflags & O_RDONLY) == 0)
    {
      ferr("ERROR: Only O_RDONLY supported\n");
      return -EACCES;
    }

  /* Allocate a container to hold the file attributes */

  attr = fs_heap_zalloc(sizeof(struct critmon_file_s));
  if (!attr)
    {
      ferr("ERROR: Failed to allocate file attributes\n");
      return -ENOMEM;
    }

  /* Save the attributes as the open-specific state in filep->f_priv */

  filep->f_priv = (FAR void *)attr;
  return OK;
}

/****************************************************************************
 * Name: critmon_close
 ****************************************************************************/

static int critmon_close(FAR struct file *filep)
{
  FAR struct critmon_file_s *attr;

  /* Recover our private data from the struct file instance */

  attr = (FAR struct critmon_file_s *)filep->f_priv;
  DEBUGASSERT(attr);

  /* Release the file attributes structure */

  fs_heap_free(attr);
  filep->f_priv = NULL;
  return OK;
}

/****************************************************************************
 * Name: critmon_read_cpu
 ****************************************************************************/

static ssize_t critmon_read_cpu(FAR struct critmon_file_s *attr,
                                FAR char *buffer, size_t buflen,
                                FAR off_t *offset, int cpu)
{
  struct timespec maxtime;
  size_t linesize;
  size_t copysize;
  size_t totalsize;

  UNUSED(maxtime);
  UNUSED(linesize);
  UNUSED(copysize);

  totalsize = 0;

  /* Generate output for CPU Serial Number */

  linesize = procfs_snprintf(attr->line, CRITMON_LINELEN, "%d", cpu);
  copysize = procfs_memcpy(attr->line, linesize, buffer, buflen, offset);

  totalsize += copysize;
  buffer    += copysize;
  buflen    -= copysize;

  if (buflen <= 0)
    {
      return totalsize;
    }

#if CONFIG_SCHED_CRITMONITOR_MAXTIME_PREEMPTION >= 0
  /* Convert the for maximum time pre-emption disabled */

  if (g_premp_max[cpu] > 0)
    {
      perf_convert(g_premp_max[cpu], &maxtime);
    }
  else
    {
      maxtime.tv_sec = 0;
      maxtime.tv_nsec = 0;
    }

  /* Reset the maximum */

  g_premp_max[cpu] = 0;

  /* Generate output for maximum time pre-emption disabled */

  linesize = procfs_snprintf(attr->line, CRITMON_LINELEN, ",%lu.%09lu",
                             (unsigned long)maxtime.tv_sec,
                             (unsigned long)maxtime.tv_nsec);
  copysize = procfs_memcpy(attr->line, linesize, buffer, buflen, offset);

  totalsize += copysize;
  buffer    += copysize;
  buflen    -= copysize;

  if (buflen <= 0)
    {
      return totalsize;
    }
#endif

#if CONFIG_SCHED_CRITMONITOR_MAXTIME_CSECTION >= 0
  /* Convert and generate output for maximum time in a critical section */

  if (g_crit_max[cpu] > 0)
    {
      perf_convert(g_crit_max[cpu], &maxtime);
    }
  else
    {
      maxtime.tv_sec = 0;
      maxtime.tv_nsec = 0;
    }

  /* Reset the maximum */

  g_crit_max[cpu] = 0;

  /* Generate output for maximum time in a critical section */

  linesize = procfs_snprintf(attr->line, CRITMON_LINELEN, ",%lu.%09lu",
                             (unsigned long)maxtime.tv_sec,
                             (unsigned long)maxtime.tv_nsec);
  copysize = procfs_memcpy(attr->line, linesize, buffer, buflen, offset);

  totalsize += copysize;
  buffer    += copysize;
  buflen    -= copysize;

  if (buflen <= 0)
    {
      return totalsize;
    }
#endif

  linesize = procfs_snprintf(attr->line, CRITMON_LINELEN, "\n");
  copysize = procfs_memcpy(attr->line, linesize, buffer, buflen, offset);

  totalsize += copysize;

  return totalsize;
}

/****************************************************************************
 * Name: critmon_read
 ****************************************************************************/

static ssize_t critmon_read(FAR struct file *filep, FAR char *buffer,
                            size_t buflen)
{
  FAR struct critmon_file_s *attr;
  off_t offset;
  ssize_t ret;
  int cpu;

  finfo("buffer=%p buflen=%d\n", buffer, (int)buflen);

  /* Recover our private data from the struct file instance */

  attr = (FAR struct critmon_file_s *)filep->f_priv;
  DEBUGASSERT(attr);

  ret    = 0;
  offset = filep->f_pos;

  /* Get the status for each CPU  */

  for (cpu = 0; cpu < CONFIG_SMP_NCPUS; cpu++)
    {
      ssize_t nbytes = critmon_read_cpu(attr, buffer + ret, buflen - ret,
                                        &offset, cpu);

      ret += nbytes;
      if (ret > buflen)
        {
          break;
        }
    }

  if (ret > 0)
    {
      filep->f_pos += ret;
    }

  return ret;
}

/****************************************************************************
 * Name: critmon_dup
 *
 * Description:
 *   Duplicate open file data in the new file structure.
 *
 ****************************************************************************/

static int critmon_dup(FAR const struct file *oldp, FAR struct file *newp)
{
  FAR struct critmon_file_s *oldattr;
  FAR struct critmon_file_s *newattr;

  finfo("Dup %p->%p\n", oldp, newp);

  /* Recover our private data from the old struct file instance */

  oldattr = (FAR struct critmon_file_s *)oldp->f_priv;
  DEBUGASSERT(oldattr);

  /* Allocate a new container to hold the task and attribute selection */

  newattr = fs_heap_malloc(sizeof(struct critmon_file_s));
  if (!newattr)
    {
      ferr("ERROR: Failed to allocate file attributes\n");
      return -ENOMEM;
    }

  /* The copy the file attributes from the old attributes to the new */

  memcpy(newattr, oldattr, sizeof(struct critmon_file_s));

  /* Save the new attributes in the new file structure */

  newp->f_priv = (FAR void *)newattr;
  return OK;
}

/****************************************************************************
 * Name: critmon_stat
 *
 * Description: Return information about a file or directory
 *
 ****************************************************************************/

static int critmon_stat(const char *relpath, struct stat *buf)
{
  /* "critmon" is the name for a read-only file */

  memset(buf, 0, sizeof(struct stat));
  buf->st_mode = S_IFREG | S_IROTH | S_IRGRP | S_IRUSR;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#endif /* !CONFIG_DISABLE_MOUNTPOINT && CONFIG_FS_PROCFS && CONFIG_SCHED_CRITMONITOR */
