/**************************************************************************************
 * drivers/lcd/ft80x_base.c
 *
 *   Copyright (C) 2018 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * References:
 *  - Document No.: FT_000792, "FT800 Embedded Video Engine", Datasheet Version 1.1,
 *    Clearance No.: FTDI# 334, Future Technology Devices International Ltd.
 *  - Document No.: FT_000986, "FT801 Embedded Video Engine Datasheet", Version 1.0,
 *    Clearance No.: FTDI#376, Future Technology Devices International Ltd.
 *  - Application Note AN_240AN_240, "FT800 From the Ground Up", Version 1.1,
 *    Issue Date: 2014-06-09, Future Technology Devices International Ltd.
 *  - "FT800 Series Programmer Guide Guide", Version 2.1, Issue Date: 2016-09-19,
 *    Future Technology Devices International Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 **************************************************************************************/

/**************************************************************************************
 * Included Files
 **************************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <poll.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/semaphore.h>
#include <nuttx/kmalloc.h>
#include <nuttx/fs/fs.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/ft80x.h>

#include <arch/irq.h>

#include "ft80x.h"

#ifdef CONFIG_LCD_FT80X

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if defined(CONFIG_LCD_FT800)
#  define DEVNAME "/dev/ft800"
#  define ROMID   0x01000800
#elif defined(CONFIG_LCD_FT801)
#  define DEVNAME "/dev/ft801"
#  define ROMID   0x01000801
#else
#  error No FT80x device configured
#endif

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
static void ft80x_destroy(FAR struct ft80x_dev_s *priv);
#endif

/* Character driver methods */

static int  ft80x_open(FAR struct file *filep);
static int  ft80x_close(FAR struct file *filep);

static ssize_t ft80x_read(FAR struct file *filep, FAR char *buffer,
              size_t buflen);
static ssize_t ft80x_write(FAR struct file *filep, FAR const char *buffer,
              size_t buflen);
static int  ft80x_ioctl(FAR struct file *filep, int cmd, unsigned long arg);
#ifndef CONFIG_DISABLE_POLL
static int  ft80x_poll(FAR struct file *filep, FAR struct pollfd *fds,
              bool setup);
#endif
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
static int ft80x_unlink(FAR struct inode *inode);
#endif

/* Initialization */

static int  ft80x_initialize(FAR struct ft80x_dev_s *priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_ft80x_fops =
{
  ft80x_open,    /* open */
  ft80x_close,   /* close */
  ft80x_read,    /* read */
  ft80x_write,   /* write */
  NULL,          /* seek */
  ft80x_ioctl    /* ioctl */
#ifndef CONFIG_DISABLE_POLL
  , ft80x_poll   /* poll */
#endif
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
  , ft80x_unlink /* unlink */
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ft80x_destroy
 *
 * Description:
 *   The driver has been unlinked... clean up as best we can.
 *
 ****************************************************************************/

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
static void ft80x_destroy(FAR struct ft80x_dev_s *priv)
{
  /* If the lower half driver provided a destroy method, then call that
   * method now in order order to clean up resources used by the lower-half
   * driver.
   */

  DEBUGASSERT(priv != NULL && priv->lower != NULL);
  if (priv->lower->destroy != NULL)
    {
      priv->lower->destroy(priv->lower);
    }

  /* Then free our container */

  sem_destroy(&priv->exclsem);
  kmm_free(priv);
}
#endif

/****************************************************************************
 * Name: ft80x_open
 *
 * Description:
 *   This function is called whenever the PWM device is opened.
 *
 ****************************************************************************/

static int ft80x_open(FAR struct file *filep)
{
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
  FAR struct inode *inode;
  FAR struct ft80x_dev_s *priv;
  uint8_t tmp;
  int ret;

  DEBUGASSERT(filep != NULL);
  inode = filep->f_inode;
  DEBUGASSERT(inode != NULL && inode->i_private != NULL);
  priv  = inode->i_private;

  lcdinfo("crefs: %d\n", priv->crefs);

  /* Get exclusive access to the device structures */

  ret = nxsem_wait(&priv->exclsem);
  if (ret < 0)
    {
      goto errout;
    }

  /* Increment the count of references to the device */

  tmp = priv->crefs + 1;
  if (tmp == 0)
    {
      /* More than 255 opens; uint8_t overflows to zero */

      ret = -EMFILE;
      goto errout_with_sem;
    }

  /* Save the new open count */

  priv->crefs = tmp;
  ret = OK;

errout_with_sem:
  nxsem_post(&priv->exclsem);

errout:
  return ret;
#else
  return OK;
#endif
}

/****************************************************************************
 * Name: ft80x_close
 *
 * Description:
 *   This function is called when the PWM device is closed.
 *
 ****************************************************************************/

static int ft80x_close(FAR struct file *filep)
{
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
  FAR struct inode *inode;
  FAR struct ft80x_dev_s *priv;
  int ret;

  DEBUGASSERT(filep != NULL);
  inode = filep->f_inode;
  DEBUGASSERT(inode != NULL && inode->i_private != NULL);
  priv  = inode->i_private;

  lcdinfo("crefs: %d\n", priv->crefs);

  /* Get exclusive access to the device structures */

  ret = nxsem_wait(&priv->exclsem);
  if (ret < 0)
    {
      goto errout;
    }

  /* Will the count decrement to zero? */

  if (priv->crefs <= 1)
    {
      /* Yes.. if the driver has been unlinked, then we need to destroy the
       * driver instance.
       */

      priv->crefs = 0;
      if (priv->unlinked)
        {
          ft80x_destroy(priv);
          return OK;
        }
    }
  else
    {
      /* NO.. decrement the number of references to the driver. */

      priv->crefs--;
    }

  ret = OK;
  nxsem_post(&priv->exclsem);

errout:
  return ret;
#else
  return OK;
#endif
}

/****************************************************************************
 * Name: ft80x_read
 ****************************************************************************/

static ssize_t ft80x_read(FAR struct file *filep, FAR char *buffer,
                          size_t len)
{
  /* Reading from the FT80X is an undefined operation and not support */

  lcdinfo("buffer: %p len %lu\n", buffer, (unsigned long)len);
  return 0;  /* Return EOF */
}

/****************************************************************************
 * Name: ft80x_write
 ****************************************************************************/

static ssize_t ft80x_write(FAR struct file *filep, FAR const char *buffer,
                           size_t len)
{
  FAR struct inode *inode;
  FAR struct ft80x_dev_s *priv;
  int ret;

  lcdinfo("buffer: %p len %lu\n", buffer, (unsigned long)len);
  DEBUGASSERT(buffer != NULL && ((uintptr_t)buffer & 3) == 0 &&
              len > 0 && (len & 3) == 0 && len <= FT80X_RAM_DL_SIZE);

  DEBUGASSERT(filep != NULL);
  inode = filep->f_inode;
  DEBUGASSERT(inode != NULL && inode->i_private != NULL);
  priv  = inode->i_private;

  if (buffer == NULL || ((uintptr_t)buffer & 3) != 0 ||
      len == 0 || (len & 3) != 0 || len > FT80X_RAM_DL_SIZE)
    {
       return -EINVAL;
    }

  /* Get exclusive access to the device structures */

  ret = nxsem_wait(&priv->exclsem);
  if (ret < 0)
    {
      return ret;
    }

  /* Note that there is no check if the driver was opened read-only.  That
   * would be a silly thing to do.
   */

  /* The write method is functionally equivalent to the FT80XIOC_PUTDISPLAYLIST
   * IOCTL command:  It simply copies the display list in the user buffer to
   * the FT80x display list memory.
   */

  ft80x_write_memory(priv, FT80X_RAM_DL, buffer, len);

  nxsem_post(&priv->exclsem);
  return len;
}

/****************************************************************************
 * Name: ft80x_ioctl
 *
 * Description:
 *   The standard ioctl method.  This is where ALL of the PWM work is done.
 *
 ****************************************************************************/

static int ft80x_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
  FAR struct inode *inode;
  FAR struct ft80x_dev_s *priv;
  int ret;

  DEBUGASSERT(filep != NULL);
  inode = filep->f_inode;
  DEBUGASSERT(inode != NULL && inode->i_private != NULL);
  priv  = inode->i_private;

  lcdinfo("cmd: %d arg: %lu\n", cmd, arg);

  /* Get exclusive access to the device structures */

  ret = nxsem_wait(&priv->exclsem);
  if (ret < 0)
    {
      return ret;
    }

  /* Handle built-in ioctl commands */

  switch (cmd)
    {
      /* FT80XIOC_PUTDISPLAYLIST:
       *   Description:  Write a display list to the FT80x display list memory
       *   Argument:     A reference to a display list structure instance.
       *                 See struct ft80x_displaylist_s.
       *   Returns:      None
       */

      case FT80XIOC_PUTDISPLAYLIST:
        {
          FAR struct ft80x_displaylist_s *dl =
            (FAR struct ft80x_displaylist_s *)((uintptr_t)arg);

          if (dl == NULL || ((uintptr_t)&dl->cmd & 3) != 0 ||
              dl->dlsize == 0 || (dl->dlsize & 3) != 0 ||
              dl->dlsize > FT80X_RAM_DL_SIZE)
            {
              ret = -EINVAL;
            }
          else
            {
              /* This IOCTL command simply copies the display list
               * provided into the FT80x display list memory.
               */

              ft80x_write_memory(priv, FT80X_RAM_DL, &dl->cmd, dl->dlsize);
              ret = OK;
            }
        }
        break;

      /* FT80XIOC_GETRESULT32:
       *   Description:  Read a 32-bit value from the display list.
       *   Argument:     A reference to an instance of struct ft80x_result32_s.
       *   Returns:      The 32-bit value read from the display list.
       */

      case FT80XIOC_GETRESULT32:
        {
          FAR struct ft80x_result32_s *result =
            (FAR struct ft80x_result32_s *)((uintptr_t)arg);

          if (result == NULL || ((uintptr_t)&result->offset & 3) != 0 ||
              result->offset >= FT80X_RAM_DL_SIZE)
            {
              ret = -EINVAL;
            }
          else
            {
              result->value = ft80x_read_word(priv,
                                              FT80X_RAM_DL + result->offset);
              ret = OK;
            }
        }
        break;

      /* FT80XIOC_GETTRACKER:
       *   Description:  After CMD_TRACK has been issued, the coprocessor
       *                 will update the TRACKER register with new position
       *                 data.
       *   Argument:     A pointer to a writable uint32_t memory location.
       *   Returns:      The new content of the tracker register.
       */

      case FT80XIOC_GETTRACKER:
        {
          FAR uint32_t *tracker = (FAR uint32_t *)((uintptr_t)arg);

          if (tracker == NULL)
            {
              ret = -EINVAL;
            }
          else
            {
              *tracker = ft80x_read_word(priv, FT80X_REG_TRACKER);
              ret = OK;
            }
        }
        break;

      /* Unrecognized IOCTL command */

      default:
        lcderr("ERROR: Unrecognized cmd: %d arg: %ld\n", cmd, arg);
        ret = -ENOTTY;
        break;
    }

  nxsem_post(&priv->exclsem);
  return ret;
}

/****************************************************************************
 * Name: ft80x_poll
 ****************************************************************************/

#ifndef CONFIG_DISABLE_POLL
static int ft80x_poll(FAR struct file *filep, FAR struct pollfd *fds,
                        bool setup)
{
#warning Missing logic
  return -ENOSYS;
}
#endif

/****************************************************************************
 * Name: ft80x_unlink
 ****************************************************************************/

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
static int ft80x_unlink(FAR struct inode *inode)
{
  FAR struct ft80x_dev_s *priv;

  /* Get the reference to our internal state structure from the inode
   * structure.
   */

  DEBUGASSERT(inode && inode->i_private);
  priv = inode->i_private;

  /* Indicate that the driver has been unlinked */

  priv->unlinked = true;

  /* If there are no further open references to the driver, then commit
   * Hara-Kiri now.
   */

  if (priv->crefs == 0)
    {
      ft80x_destroy(priv);
    }

  return OK;
}
#endif

/****************************************************************************
 * Name: ft80x_initialize
 *
 * Description:
 *  Initialize the FT80x
 *
 ****************************************************************************/

static int ft80x_initialize(FAR struct ft80x_dev_s *priv)
{
  uint8_t regval32;
  uint8_t regval8;

  /* To configure the display, load the timing control registers with values
   * for the particular display. These registers control horizontal timing:
   *
   *   - FT80X_REG_PCLK
   *   - FT80X_REG_PCLK_POL
   *   - FT80X_REG_HCYCLE
   *   - FT80X_REG_HOFFSET
   *   - FT80X_REG_HSIZE
   *   - FT80X_REG_HSYNC0
   *   - FT80X_REG_HSYNC1
   *
   * These registers control vertical timing:
   *
   *   - FT80X_REG_VCYCLE
   *   - FT80X_REG_VOFFSET
   *   - FT80X_REG_VSIZE
   *   - FT80X_REG_VSYNC0
   *   - FT80X_REG_VSYNC1
   *
   * And the FT80X_REG_CSPREAD register changes color clock timing to reduce system
   * noise.
   *
   * GPIO bit 7 is used for the display enable pin of the LCD module. By
   * setting the direction of the GPIO bit to out direction, the display can
   * be enabled by writing value of 1 into GPIO bit 7 or the display can be
   * disabled by writing a value of 0 into GPIO bit 7. By default GPIO bit 7
   * direction is output and the value is 0.
   */

  /* Initialization Sequence from Power Down using PD_N pin:
   *
   * 1. Drive the PD_N pin high
   * 2. Wait for at least 20ms
   * 3. Execute "Initialization Sequence during the Boot up" from steps 1 to 9
   *
   * Initialization Sequence from Sleep Mode:
   *
   * 1. Send Host command "ACTIVE" to enable clock to FT800
   * 2. Wait for at least 20ms
   * 3. Execute "Initialization Sequence during Boot Up" from steps 5 to 8
   *
   * Initialization sequence from standby mode:
   *
   * Execute all the steps mentioned in "Initialization Sequence from Sleep
   * Mode" except waiting for at least 20ms in step 2.
   */

   DEBUGASSERT(priv->lower != NULL && priv->lower->pwrdown != NULL);
   priv->lower->pwrdown(priv->lower, false);
   up_mdelay(20);

  /* Initialization Sequence during the boot up:
   *
   * 1. Use MCU SPI clock not more than 11MHz
   * 2. Send Host command CLKEXT to FT800
   * 3. Send Host command ACTIVE to enable clock to FT800.
   * 4. Configure video timing registers, except FT80X_REG_PCLK
   * 5. Write first display list
   * 6. Write FT80X_REG_DLSWAP, FT800 swaps display list immediately
   * 7. Enable back light control for display
   * 8. Write FT80X_REG_PCLK, video output begins with the first display list
   * 9. Use MCU SPI clock not more than 30MHz
   */

  /* 1. Select the initial SPI frequency */

  DEBUGASSERT(priv->lower->init_frequency <= 11000000);
  priv->frequency = priv->lower->init_frequency;

  /* 2. Send Host command CLKEXT to FT800
   * 3. Send Host command ACTIVE to enable clock to FT800.
   */

  ft80x_host_command(priv, FT80X_CMD_CLKEXT);
  ft80x_host_command(priv, FT80X_CMD_ACTIVE);

  /* Verify the chip ID */

  regval32 = ft80x_read_word(priv, FT80X_REG_ID);
  if ((regval32 & ID_MASK) != 0x7c)
    {
      lcderr("ERROR: Bad chip ID: %02x\n",
             (unsigned int)(regval32 & ID_MASK));
      return -ENODEV;
    }

  regval32 = ft80x_read_word(priv, FT80X_ROM_CHIPID);
  if (regval32 != ROMID)
    {
      lcderr("ERROR: Bad ROM chip ID: %08lx\n", (unsigned long)regval32);
      return -ENODEV;
    }

  /* 4. Configure video timing registers, except FT80X_REG_PCLK
   *
   * Once the FT800 is awake and the internal clock set and Device ID
   * checked, the next task is to configure the LCD display parameters for
   * the chosen display with the values determined in Section 2.3.3 above.
   *
   * a. Set FT80X_REG_PCLK to zero - This disables the pixel clock output while
   *    the LCD and other system parameters are configured
   * b. Set the following registers with values for the chosen display.
   *    Typical WQVGA and QVGA values are shown:
   *
   *    Register            Description                      WQVGA    QVGA 320 x 240
   *                                                     480x272  320x240
   *    FT80X_REG_PCLK_POL  Pixel Clock Polarity             1        0
   *    FT80X_REG_HSIZE     Image width in pixels            480      320
   *    FT80X_REG_HCYCLE    Total number of clocks per line  548      408
   *    FT80X_REG_HOFFSET   Horizontal image start           43       70
   *                        (pixels from left)
   *    FT80X_REG_HSYNC0    Start of HSYNC pulse             0        0
   *                        (falling edge)
   *    FT80X_REG_HSYNC1    End of HSYNC pulse               41       10
   *                        (rising edge)
   *    FT80X_REG_VSIZE     Image height in pixels           272      240
   *    FT80X_REG_VCYCLE    Total number of lines per screen 292      263
   *    FT80X_REG_VOFFSET   Vertical image start             12       13
   *                        (lines from top)
   *    FT80X_REG_VSYNC0    Start of VSYNC pulse             0        0
   *                        (falling edge)
   *    FT80X_REG_VSYNC1    End of VSYNC pulse               10       2
   *                        (rising edge)
   *
   * c. Enable or disable FT80X_REG_CSPREAD with a value of 01h or 00h,
   *    respectively.  Enabling FT80X_REG_CSPREAD will offset the R, G and B
   *    output bits so all they do not all change at the same time.
   */

  ft80x_write_byte(priv, FT80X_REG_PCLK, 0);

#if defined(CONFIG_LCD_FT80X_WQVGA)
  ft80x_write_hword(priv, FT80X_REG_HCYCLE, 548);
  ft80x_write_hword(priv, FT80X_REG_HOFFSET, 43);
  ft80x_write_hword(priv, FT80X_REG_HSYNC0, 0);
  ft80x_write_hword(priv, FT80X_REG_HSYNC1, 41);
  ft80x_write_hword(priv, FT80X_REG_VCYCLE, 292);
  ft80x_write_hword(priv, FT80X_REG_VOFFSET, 12);
  ft80x_write_hword(priv, FT80X_REG_VSYNC0, 0);
  ft80x_write_hword(priv, FT80X_REG_VSYNC1, 10);
  ft80x_write_byte(priv, FT80X_REG_SWIZZLE, 0);
  ft80x_write_byte(priv, FT80X_REG_PCLK_POL, 1);
  ft80x_write_byte(priv, FT80X_REG_CSPREAD, 1);
  ft80x_write_hword(priv, FT80X_REG_HSIZE, 480);
  ft80x_write_hword(priv, FT80X_REG_VSIZE, 272);

#elif defined(CONFIG_LCD_FT80X_QVGA)
  ft80x_write_hword(priv, FT80X_REG_HCYCLE, 408);
  ft80x_write_hword(priv, FT80X_REG_HOFFSET, 70);
  ft80x_write_hword(priv, FT80X_REG_HSYNC0, 0);
  ft80x_write_hword(priv, FT80X_REG_HSYNC1, 10);
  ft80x_write_hword(priv, FT80X_REG_VCYCLE, 263);
  ft80x_write_hword(priv, FT80X_REG_VOFFSET, 13);
  ft80x_write_hword(priv, FT80X_REG_VSYNC0, 0);
  ft80x_write_hword(priv, FT80X_REG_VSYNC1, 2);
  ft80x_write_byte(priv, FT80X_REG_SWIZZLE, 0);  /* REVISIT */
  ft80x_write_byte(priv, FT80X_REG_PCLK_POL, 0);
  ft80x_write_byte(priv, FT80X_REG_CSPREAD, 1);
  ft80x_write_hword(priv, FT80X_REG_HSIZE, 320);
  ft80x_write_hword(priv, FT80X_REG_VSIZE, 240);

#else
#  error Unknown display size
#endif

  /* 5. Write first display list */

  ft80x_write_word(priv, FT80X_RAM_DL + 0, FT80X_CLEAR_COLOR_RGB(0,0,0));
  ft80x_write_word(priv, FT80X_RAM_DL + 4, FT80X_CLEAR(1,1,1));
  ft80x_write_word(priv, FT80X_RAM_DL + 8, FT80X_DISPLAY());

  /* 6. Write FT80X_REG_DLSWAP, FT800 swaps display list immediately */

  ft80x_write_byte(priv, FT80X_REG_DLSWAP, DLSWAP_FRAME);

  /* GPIO bit 7 is used for the display enable pin of the LCD module. By
   * setting the direction of the GPIO bit to out direction, the display can
   * be enabled by writing value of 1 into GPIO bit 7 or the display can be
   * disabled by writing a value of 0 into GPIO bit 7. By default GPIO bit 7
   * direction is output and the value is 0.
   */

  regval8  = ft80x_read_byte(priv, FT80X_REG_GPIO_DIR);
  regval8 |= (1 << 7);
  ft80x_write_byte(priv, FT80X_REG_GPIO_DIR, regval8);

  regval8  = ft80x_read_byte(priv, FT80X_REG_GPIO);
  regval8 |= (1 << 7);
  ft80x_write_byte(priv, FT80X_REG_GPIO, regval8);

  /* 7. Enable back light control for display */
#warning Missing logic

  /* 8. Write FT80X_REG_PCLK, video output begins with the first display list */

  ft80x_write_byte(priv, FT80X_REG_PCLK, 5);

  /* 9. Use MCU SPI clock not more than 30MHz */

  DEBUGASSERT(priv->lower->op_frequency <= 30000000);
  priv->frequency = priv->lower->op_frequency;

  return OK;
}

/**************************************************************************************
 * Public Functions
 **************************************************************************************/

/****************************************************************************
 * Name: ft80x_register
 *
 * Description:
 *   Configure the ADS7843E to use the provided SPI device instance.  This
 *   will register the driver as /dev/ft80x.
 *
 * Input Parameters:
 *   spi     - An SPI driver instance
 *   i2c     - An I2C master driver instance
 *   lower   - Persistent board configuration data / lower half interface
 *
 * Returned Value:
 *   Zero is returned on success.  Otherwise, a negated errno value is
 *   returned to indicate the nature of the failure.
 *
 ****************************************************************************/

#if defined(CONFIG_LCD_FT80X_SPI)
int ft80x_register(FAR struct spi_dev_s *spi,
                   FAR const struct ft80x_config_s *lower)
#elif defined(CONFIG_LCD_FT80X_I2C)
int ft80x_register(FAR struct i2c_master_s *i2c,
                   FAR const struct ft80x_config_s *lower)
#endif
{
  FAR struct ft80x_dev_s *priv;
  int ret;

#if defined(CONFIG_LCD_FT80X_SPI)
  DEBUGASSERT(spi != NULL && lower != NULL);
#elif defined(CONFIG_LCD_FT80X_I2C)
  DEBUGASSERT(i2c != NULL && lower != NULL);
#endif

  /* Allocate the driver state structure */

  priv = (FAR struct ft80x_dev_s *)kmm_zalloc(sizeof(struct ft80x_dev_s));
  if (priv == NULL)
    {
      lcderr("ERROR: Failed to allocate state structure\n");
      return -ENOMEM;
    }

  /* Save the lower level interface and configuration information */

  priv->lower = lower;

#ifdef CONFIG_LCD_FT80X_SPI
  /* Remember the SPI configuration */

  priv->spi = spi;
#else
  /* Remember the I2C configuration */

  priv->i2c = i2c;
#endif

  /* Initialize the mutual exclusion semaphore */

  sem_init(&priv->exclsem, 0, 1);

  /* Initialize the FT80x */

  ret = ft80x_initialize(priv);
  if (ret < 0)
    {
      goto errout_with_sem;
    }

  /* Register the FT80x character driver */

  ret = register_driver(DEVNAME, &g_ft80x_fops, 0666, priv);
  if (ret < 0)
    {
      goto errout_with_sem;
    }

  return OK;

errout_with_sem:
  sem_destroy(&priv->exclsem);
  return ret;
}

#endif /* CONFIG_LCD_FT80X */
