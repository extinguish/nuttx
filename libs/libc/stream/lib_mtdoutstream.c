/****************************************************************************
 * libs/libc/stream/lib_mtdoutstream.c
 *
 * SPDX-License-Identifier: Apache-2.0
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

#include <unistd.h>
#include <nuttx/streams.h>
#include <nuttx/fs/fs.h>
#include <nuttx/mtd/mtd.h>

#include "libc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if !defined(CONFIG_DISABLE_MOUNTPOINT) && defined(CONFIG_MTD)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mtdoutstream_flush
 ****************************************************************************/

static int mtdoutstream_flush(FAR struct lib_sostream_s *self)
{
  FAR struct lib_mtdoutstream_s *stream =
    (FAR struct lib_mtdoutstream_s *)self;
  FAR struct mtd_dev_s *i_mtd = stream->inode->u.i_mtd;
  size_t erasesize = stream->geo.erasesize;
  size_t nblkpererase = erasesize / stream->geo.blocksize;
  int ret = OK;

  if (self->nput % erasesize > 0)
    {
      size_t sblock = self->nput / erasesize;

      ret = MTD_ERASE(i_mtd, sblock, 1);
      if (ret < 0)
        {
          return ret;
        }

      ret = MTD_BWRITE(i_mtd, sblock * nblkpererase,
                        nblkpererase, stream->cache);
    }

  return ret;
}

/****************************************************************************
 * Name: mtdoutstream_puts
 ****************************************************************************/

static ssize_t mtdoutstream_puts(FAR struct lib_sostream_s *self,
                                 FAR const void *buf, size_t len)
{
  FAR struct lib_mtdoutstream_s *stream =
    (FAR struct lib_mtdoutstream_s *)self;
  FAR struct mtd_dev_s *i_mtd = stream->inode->u.i_mtd;
  size_t erasesize = stream->geo.erasesize;
  size_t nblkpererase = erasesize / stream->geo.blocksize;
  FAR const unsigned char *ptr = buf;
  size_t remain = len;
  ssize_t ret;

  if (self->nput + len > erasesize * stream->geo.neraseblocks)
    {
      return -ENOSPC;
    }

  while (remain > 0)
    {
      off_t sblock = self->nput / erasesize;
      off_t offset = self->nput % erasesize;

      if (offset > 0)
        {
          size_t copying = offset + remain > erasesize ?
                           erasesize - offset : remain;

          memcpy(stream->cache + offset, ptr, copying);

          ptr        += copying;
          offset     += copying;
          self->nput += copying;
          remain     -= copying;

          if (offset == erasesize)
            {
              ret = MTD_ERASE(i_mtd, sblock, 1);
              if (ret < 0)
                {
                  return ret;
                }

              ret = MTD_BWRITE(i_mtd, sblock * nblkpererase,
                               nblkpererase, stream->cache);
              if (ret < 0)
                {
                  return ret;
                }
            }
        }
      else if (remain < erasesize)
        {
          ret = MTD_BREAD(i_mtd, sblock * nblkpererase,
                          nblkpererase, stream->cache);
          if (ret < 0)
            {
              return ret;
            }

          memcpy(stream->cache, ptr, remain);
          self->nput += remain;
          remain      = 0;
        }
      else
        {
          size_t nblock = remain / erasesize;
          size_t copying = nblock * erasesize;

          ret = MTD_ERASE(i_mtd, sblock, nblock);
          if (ret < 0)
            {
              return ret;
            }

          ret = MTD_BWRITE(i_mtd, sblock * nblkpererase,
                           nblock * nblkpererase, ptr);
          if (ret < 0)
            {
              return ret;
            }

          ptr        += copying;
          self->nput += copying;
          remain     -= copying;
        }
    }

  return len;
}

/****************************************************************************
 * Name: mtdoutstream_putc
 ****************************************************************************/

static void mtdoutstream_putc(FAR struct lib_sostream_s *self, int ch)
{
  char tmp = ch;
  mtdoutstream_puts(self, &tmp, 1);
}

/****************************************************************************
 * Name: mtdoutstream_seek
 ****************************************************************************/

static off_t mtdoutstream_seek(FAR struct lib_sostream_s *self,
                               off_t offset, int whence)
{
  FAR struct lib_mtdoutstream_s *stream =
    (FAR struct lib_mtdoutstream_s *)self;
  FAR struct mtd_dev_s *i_mtd = stream->inode->u.i_mtd;
  size_t erasesize    = stream->geo.erasesize;
  off_t  streamsize   = erasesize * stream->geo.neraseblocks;
  size_t nblkpererase = erasesize / stream->geo.blocksize;
  size_t block;
  off_t  ret;

  switch (whence)
    {
      case SEEK_SET:
        break;
      case SEEK_END:
        offset += streamsize;
        break;
      case SEEK_CUR:
        offset += self->nput;
        break;
      default:
        return -ENOTSUP;
    }

  /* Seek to negative value or value larger than maximum size shall fail */

  if (offset < 0 || offset > streamsize)
    {
      return -EINVAL;
    }

  if (self->nput % erasesize)
    {
      block = self->nput / erasesize;
      if (offset >= block * erasesize &&
          offset < (block + 1) * erasesize)
        {
          /* Inside same erase block */

          goto out;
        }

      ret = MTD_ERASE(i_mtd, block, 1);
      if (ret < 0)
        {
          return ret;
        }

      ret = MTD_BWRITE(i_mtd, block * nblkpererase,
                       nblkpererase, stream->cache);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (offset % erasesize)
    {
      block = offset / erasesize;

      ret = MTD_BREAD(i_mtd, block * nblkpererase,
                      nblkpererase, stream->cache);
      if (ret < 0)
        {
          return ret;
        }
    }

out:
  self->nput = offset;
  return offset;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: lib_mtdoutstream_close
 *
 * Description:
 *  close mtd driver stream backend
 *
 * Input Parameters:
 *   stream  - User allocated, uninitialized instance of struct
 *                lib_mtdoutstream_s to be initialized.
 *
 * Returned Value:
 *   None (User allocated instance initialized).
 *
 ****************************************************************************/

void lib_mtdoutstream_close(FAR struct lib_mtdoutstream_s *stream)
{
  if (stream != NULL)
    {
      if (stream->inode != NULL)
        {
          close_mtddriver(stream->inode);
          stream->inode = NULL;
        }

      if (stream->cache != NULL)
        {
          mtdoutstream_flush(&stream->common);
          lib_free(stream->cache);
          stream->cache = NULL;
        }
    }
}

/****************************************************************************
 * Name: lib_mtdoutstream_open
 *
 * Description:
 *  mtd driver stream backend
 *
 * Input Parameters:
 *   stream   - User allocated, uninitialized instance of struct
 *                lib_mtdoutstream_s to be initialized.
 *   name     - The full path of mtd device.
 *
 * Returned Value:
 *   Returns zero on success or a negated errno on failure
 *
 ****************************************************************************/

int lib_mtdoutstream_open(FAR struct lib_mtdoutstream_s *stream,
                          FAR const char *name)
{
  FAR struct inode *node = NULL;
  int ret;

  if (stream == NULL || name == NULL)
    {
      return -EINVAL;
    }

  ret = find_mtddriver(name, &node);
  if (ret < 0)
    {
      return ret;
    }

  memset(stream, 0, sizeof(*stream));

  if (node->u.i_mtd->ioctl == NULL ||
      node->u.i_mtd->erase == NULL ||
      node->u.i_mtd->bwrite == NULL ||
      node->u.i_mtd->ioctl(node->u.i_mtd, MTDIOC_GEOMETRY,
                           (unsigned long)&stream->geo) < 0 ||
      stream->geo.blocksize <= 0 ||
      stream->geo.erasesize <= 0 ||
      stream->geo.neraseblocks <= 0)
    {
      close_mtddriver(node);
      return -EINVAL;
    }

  stream->cache = lib_malloc(stream->geo.erasesize);
  if (stream->cache == NULL)
    {
      close_mtddriver(node);
      return -ENOMEM;
    }

  stream->inode        = node;
  stream->common.putc  = mtdoutstream_putc;
  stream->common.puts  = mtdoutstream_puts;
  stream->common.flush = mtdoutstream_flush;
  stream->common.seek  = mtdoutstream_seek;

  return OK;
}

#endif /* CONFIG_MTD */
