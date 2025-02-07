/****************************************************************************
 * boards/arm/stm32f7/ardusimple-mapkit/src/stm32_w25.c
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
#include <nuttx/board.h>
#ifdef CONFIG_STM32F7_SPI3
#  include <nuttx/spi/spi.h>
#  include <nuttx/mtd/mtd.h>
#  include <nuttx/drivers/drivers.h>
#  include <nuttx/fs/fs.h>
#endif
#include <arch/board/board.h>

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <debug.h>

#include "stm32_spi.h"
#include "ardusimple-mapkit.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Debug ********************************************************************/

/* Non-standard debug that may be enabled just for testing the watchdog
 * timer
 */

#define W25_SPI_PORT 3

/* Configuration ************************************************************/

/* Can't support the W25 device if it SPI3 or W25 support is not enabled */

#define HAVE_W25  1
#if !defined(CONFIG_STM32F7_SPI3) || !defined(CONFIG_MTD_W25)
#  undef HAVE_W25
#endif

/* Can't support W25 features if mountpoints are disabled */

#if defined(CONFIG_DISABLE_MOUNTPOINT)
#  undef HAVE_W25
#endif

/* Default W25 minor number */

#if defined(HAVE_W25) && !defined(CONFIG_NSH_W25MINOR)
#  define CONFIG_NSH_W25MINOR 0
#endif

/* Can't support both FAT and SMARTFS */

#if defined(CONFIG_FS_FAT) && defined(CONFIG_FS_SMARTFS)
#  warning "Can't support both FAT and SMARTFS -- using FAT"
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef HAVE_W25
/****************************************************************************
 * Name: create_main_partitions
 *
 * Description:
 *   Create the main MTD partitions.
 *
 ****************************************************************************/

int create_main_partitions(struct mtd_dev_s *mtd, off_t offset, off_t nblocks)
{
  struct mtd_dev_s *part[CONFIG_ARDUSIMPLE_MAPKIT_SPIFLASH_NPARTMAIN]={0};
  char blockname[32];
  char charname[32];
  char mntpoint[32];
  int ret, i;

  /* Now create main MTD FLASH partitions */

  syslog(LOG_ERR, "INFO: Creating main partitions\n");

  for (i = 0;
       i < CONFIG_ARDUSIMPLE_MAPKIT_SPIFLASH_NPARTMAIN;
       offset += nblocks, i++)
    {
      syslog(LOG_INFO, "INFO: Partition %d. Block offset=%lu, size=%lu\n",
                       i, (unsigned long)offset, (unsigned long)nblocks);

      /* Create the partition */

      part[i] = mtd_partition(mtd, offset, nblocks);
      if (!part[i])
        {
          syslog(LOG_ERR, "ERROR: Failed to create main MTD partition\n");
          return -ENODEV;
        }

      /* Initialize to provide an FTL block driver on the MTD FLASH
       * interface
       */

      snprintf(blockname, sizeof(blockname), "/dev/mtdblock%d", i);
      snprintf(charname, sizeof(charname), "/dev/mtd%d", i);
      snprintf(mntpoint, sizeof(mntpoint), "/data%d", i);

      ret = ftl_initialize_by_path(blockname, part[i]);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: ftl_initialize %s failed: %d\n", blockname, ret);
          return ret;
        }

      /* Now create a character device on the block device */

      ret = bchdev_register(blockname, charname, false);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: bchdev_register %s failed: %d\n", charname, ret);
          return ret;
        }

      /* Initialize to provide flash file system on the MTD partitions */

      ret = board_spiflash_init(part[i], blockname, mntpoint);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: Flash file system initialization failed: %d\n", -ret);
          return ret;
        }
    }

  return offset;
}

/****************************************************************************
 * Name: create_resv_partitions
 *
 * Description:
 *   Create the reserved MTD partitions.
 *
 ****************************************************************************/

static int create_resv_partitions(struct mtd_dev_s *mtd, off_t offset, off_t nblocks)
{
  struct mtd_dev_s *part[CONFIG_ARDUSIMPLE_MAPKIT_SPIFLASH_NPARTRESV]={0};
  char blockname[32];
  char charname[32];
  int ret, i;

  /* Now create reserved MTD FLASH partitions */

  syslog(LOG_ERR, "INFO: Creating partitions\n");

  for (i = 0;
       i < CONFIG_ARDUSIMPLE_MAPKIT_SPIFLASH_NPARTRESV;
       offset += nblocks, i++)
    {
      syslog(LOG_INFO, "INFO: Partition %d. Block offset=%lu, size=%lu\n",
                       i, (unsigned long)offset, (unsigned long)nblocks);

      /* Create the partition */

      part[i] = mtd_partition(mtd, offset, nblocks);
      if (!part[i])
        {
          syslog(LOG_ERR, "ERROR: Failed to create reserved MTD partition\n");
          return -ENODEV;
        }

      /* Initialize to provide an FTL block driver on the MTD FLASH
       * interface
       */

      snprintf(blockname, sizeof(blockname), "/dev/imgblock%d", i);
      snprintf(charname, sizeof(charname), "/dev/img%d", i);

      ret = ftl_initialize_by_path(blockname, part[i]);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: ftl_initialize %s failed: %d\n", blockname, ret);
          return ret;
        }

      /* Now create a character device on the block device */

      ret = bchdev_register(blockname, charname, false);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: bchdev_register %s failed: %d\n", charname, ret);
          return ret;
        }
    }

  return offset;
}
#endif /* HAVE_W25 */

/****************************************************************************
 * Name: stm32_w25initialize
 *
 * Description:
 *   Initialize and register the W25 FLASH file system.
 *
 ****************************************************************************/

int stm32_w25initialize(int minor)
{
#ifdef HAVE_W25
  struct mtd_geometry_s geo;
  struct mtd_dev_s *mtd;
  struct spi_dev_s *spi;
  uint32_t blkpererase;
  uint32_t blkreserved;
  off_t nblocks;
  off_t offset;
  int ret;

  /* Get the SPI port */

  spi = stm32_spibus_initialize(W25_SPI_PORT);
  if (!spi)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize SPI port %d\n", W25_SPI_PORT);
      return -ENODEV;
    }

  /* Now bind the SPI interface to the W25 SPI FLASH driver */

  mtd = w25_initialize(spi);
  if (!mtd)
    {
      syslog(LOG_ERR, "ERROR: Failed to bind SPI port %d to the W25 FLASH driver\n", W25_SPI_PORT);
      return -ENODEV;
    }

  /* Get the device geometry */

  ret = mtd->ioctl(mtd, MTDIOC_GEOMETRY,
                        (unsigned long)((uintptr_t)&geo));
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: mtd->ioctl failed: %d\n", ret);
      return ret;
    }

  /* Determine the size of each partition.  Make each partition an even
   * multiple of the erase block size (perhaps not using some space at the
   * end of the FLASH).
   */

  /* The reserved area will be used for firmware upgrade/recovery */

  blkreserved = (CONFIG_ARDUSIMPLE_MAPKIT_SPIFLASH_SIZERESV / geo.neraseblocks);
  blkpererase = geo.erasesize / geo.blocksize;
  nblocks     = ((geo.neraseblocks - blkreserved) / CONFIG_ARDUSIMPLE_MAPKIT_SPIFLASH_NPARTMAIN) *
                blkpererase;

  /* Now create the main MTD FLASH partitions */

  ret = create_main_partitions(mtd, 0, nblocks);
  if (ret < 0)
    {
      return ret;
    }
  offset = ret;

  /* Create the reserved partition */

  ret = create_resv_partitions(mtd,
                               offset,
                               blkreserved / CONFIG_ARDUSIMPLE_MAPKIT_SPIFLASH_NPARTRESV *
                               blkpererase);
  if (ret < 0)
    {
      return ret;
    }
#endif /* HAVE_W25 */

  return OK;
}
