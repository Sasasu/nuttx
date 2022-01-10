/****************************************************************************
 * drivers/pipes/pipe.c
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

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>

#include <nuttx/fs/fs.h>
#include <nuttx/semaphore.h>

#include "pipe_common.h"

#if CONFIG_DEV_PIPE_SIZE > 0

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MAX_PIPES 32

/****************************************************************************
 * Private Types
 ****************************************************************************/

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int pipe_close(FAR struct file *filep);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations pipe_fops =
{
  pipecommon_open,     /* open */
  pipe_close,          /* close */
  pipecommon_read,     /* read */
  pipecommon_write,    /* write */
  NULL,                /* seek */
  pipecommon_ioctl,    /* ioctl */
  pipecommon_poll      /* poll */
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
  , pipecommon_unlink  /* unlink */
#endif
};

static sem_t  g_pipesem       = SEM_INITIALIZER(1);
static uint32_t g_pipeset     = 0;
static uint32_t g_pipecreated = 0;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pipe_allocate
 ****************************************************************************/

static inline int pipe_allocate(void)
{
  int pipeno;
  int ret = -ENFILE;

  for (pipeno = 0; pipeno < MAX_PIPES; pipeno++)
    {
      if ((g_pipeset & (1 << pipeno)) == 0)
        {
          g_pipeset |= (1 << pipeno);
          ret = pipeno;
          break;
        }
    }

  return ret;
}

/****************************************************************************
 * Name: pipe_free
 ****************************************************************************/

static inline void pipe_free(int pipeno)
{
  int ret;

  ret = nxsem_wait(&g_pipesem);
  if (ret == OK)
    {
      g_pipeset &= ~(1 << pipeno);
      nxsem_post(&g_pipesem);
    }
}

/****************************************************************************
 * Name: pipe_close
 ****************************************************************************/

static int pipe_close(FAR struct file *filep)
{
  FAR struct inode *inode    = filep->f_inode;
  FAR struct pipe_dev_s *dev = inode->i_private;
  int ret;

  DEBUGASSERT(dev);

  /* Perform common close operations */

  ret = pipecommon_close(filep);
  if (ret == 0 && inode->i_crefs == 1)
    {
      /* Release the pipe when there are no further open references to it. */

      pipe_free(dev->d_pipeno);
    }

  return ret;
}

/****************************************************************************
 * Name: pipe_register
 ****************************************************************************/

static int pipe_register(size_t bufsize, int flags,
                         FAR char *devname, size_t namesize)
{
  FAR struct pipe_dev_s *dev;
  int pipeno;
  int ret;

  /* Get exclusive access to the pipe allocation data */

  ret = nxsem_wait(&g_pipesem);
  if (ret < 0)
    {
      goto errout;
    }

  /* Allocate a minor number for the pipe device */

  pipeno = pipe_allocate();
  if (pipeno < 0)
    {
      ret = pipeno;
      goto errout_with_sem;
    }

  /* Create a pathname to the pipe device */

  snprintf(devname, namesize, "/dev/pipe%d", pipeno);

  /* Check if the pipe device has already been created */

  if ((g_pipecreated & (1 << pipeno)) == 0)
    {
      /* No.. Allocate and initialize a new device structure instance */

      dev = pipecommon_allocdev(bufsize);
      if (!dev)
        {
          ret = -ENOMEM;
          goto errout_with_pipe;
        }

      dev->d_pipeno = pipeno;

      /* Register the pipe device */

      ret = register_driver(devname, &pipe_fops, 0666, (FAR void *)dev);
      if (ret != 0)
        {
          nxsem_post(&g_pipesem);
          goto errout_with_dev;
        }

      /* Remember that we created this device */

       g_pipecreated |= (1 << pipeno);
    }

  nxsem_post(&g_pipesem);
  return OK;

errout_with_dev:
  pipecommon_freedev(dev);

errout_with_pipe:
  pipe_free(pipeno);

errout_with_sem:
  nxsem_post(&g_pipesem);

errout:
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nx_pipe
 *
 * Description:
 *   nx_pipe() creates a pair of file descriptors, pointing to a pipe inode,
 *   and  places them in the array pointed to by 'fd'. fd[0] is for reading,
 *   fd[1] is for writing.
 *
 *   NOTE: nx_pipe is a special, non-standard, NuttX-only interface.  Since
 *   the NuttX FIFOs are based in in-memory, circular buffers, the ability
 *   to control the size of those buffers is critical for system tuning.
 *
 * Input Parameters:
 *   fd[2] - The user provided array in which to catch the pipe file
 *   descriptors
 *   bufsize - The size of the in-memory, circular buffer in bytes.
 *   flags - The file status flags.
 *
 * Returned Value:
 *   0 is returned on success; a negated errno value is returned on a
 *   failure.
 *
 ****************************************************************************/

int file_pipe(FAR struct file *filep[2], size_t bufsize, int flags)
{
  char devname[16];
  int ret;

  /* Register a new pipe device */

  ret = pipe_register(bufsize, flags, devname, sizeof(devname));
  if (ret < 0)
    {
      return ret;
    }

  /* Get a write file descriptor */

  ret = file_open(filep[1], devname, O_WRONLY | flags);
  if (ret < 0)
    {
      goto errout_with_driver;
    }

  /* Get a read file descriptor */

  ret = file_open(filep[0], devname, O_RDONLY | flags);
  if (ret < 0)
    {
      goto errout_with_wrfd;
    }

  return OK;

errout_with_wrfd:
  file_close(filep[1]);

errout_with_driver:
  unregister_driver(devname);
  return ret;
}

int nx_pipe(int fd[2], size_t bufsize, int flags)
{
  char devname[16];
  int ret;

  /* Register a new pipe device */

  ret = pipe_register(bufsize, flags, devname, sizeof(devname));
  if (ret < 0)
    {
      return ret;
    }

  /* Get a write file descriptor */

  fd[1] = nx_open(devname, O_WRONLY | flags);
  if (fd[1] < 0)
    {
      ret = fd[1];
      goto errout_with_driver;
    }

  /* Get a read file descriptor */

  fd[0] = nx_open(devname, O_RDONLY | flags);
  if (fd[0] < 0)
    {
      ret = fd[0];
      goto errout_with_wrfd;
    }

  return OK;

errout_with_wrfd:
  nx_close(fd[1]);

errout_with_driver:
  unregister_driver(devname);
  return ret;
}

#endif /* CONFIG_DEV_PIPE_SIZE > 0 */
