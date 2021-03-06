/****************************************************************************
 *
 *   Copyright (c) 2012-2015 PX4 Development Team. All rights reserved.
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
 * 3. Neither the name PX4 nor the names of its contributors may be
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
 ****************************************************************************/

#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "uORBUtils.hpp"
#include "uORBManager.hpp"
#include "uORBDevices.hpp"
#include "px4_config.h"


//=========================  Static initializations =================
uORB::Manager uORB::Manager::_Instance;

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
uORB::Manager* uORB::Manager::get_instance()
{
  return &_Instance;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
uORB::Manager::Manager()
{
}

int uORB::Manager::orb_exists(const struct orb_metadata *meta, int instance)
{
  /*
   * Generate the path to the node and try to open it.
   */
  char path[orb_maxpath];
  int inst = instance;
  int ret = uORB::Utils::node_mkpath(path, PUBSUB, meta, &inst);

  if (ret != OK) {
    errno = -ret;
    return ERROR;
  }

  struct stat buffer;
  return stat(path, &buffer);
}

orb_advert_t uORB::Manager::orb_advertise(const struct orb_metadata *meta, const void *data)
{
  //warnx("orb_advertise meta = %p", meta);
  return orb_advertise_multi(meta, data, nullptr, ORB_PRIO_DEFAULT);
}

orb_advert_t uORB::Manager::orb_advertise_multi(const struct orb_metadata *meta, const void *data, int *instance, int priority)
{
  int result, fd;
  orb_advert_t advertiser;

  //warnx("orb_advertise_multi meta = %p\n", meta);

  /* open the node as an advertiser */
  fd = node_open(PUBSUB, meta, data, true, instance, priority);
  if (fd == ERROR) {
    warnx("node_open as advertiser failed.");
    return nullptr;
  }

  /* get the advertiser handle and close the node */
  result = px4_ioctl(fd, ORBIOCGADVERTISER, (unsigned long)&advertiser);
  px4_close(fd);
  if (result == ERROR) {
    warnx("px4_ioctl ORBIOCGADVERTISER  failed. fd = %d", fd);
    return nullptr;
  }

  /* the advertiser must perform an initial publish to initialise the object */
  result = orb_publish(meta, advertiser, data);
  if (result == ERROR) {
    warnx("orb_publish failed");
    return nullptr;
  }

  return advertiser;
}

int uORB::Manager::orb_subscribe(const struct orb_metadata *meta)
{
  return node_open(PUBSUB, meta, nullptr, false);
}

int uORB::Manager::orb_subscribe_multi(const struct orb_metadata *meta, unsigned instance)
{
  int inst = instance;
  return node_open(PUBSUB, meta, nullptr, false, &inst);
}

int uORB::Manager::orb_unsubscribe(int fd)
{
  return px4_close(fd);
}

int uORB::Manager::orb_publish(const struct orb_metadata *meta, orb_advert_t handle, const void *data)
{
  return uORB::DeviceNode::publish(meta, handle, data);
}

int uORB::Manager::orb_copy(const struct orb_metadata *meta, int handle, void *buffer)
{
  int ret;

  ret = px4_read(handle, buffer, meta->o_size);

  if (ret < 0)
    return ERROR;

  if (ret != (int)meta->o_size) {
    errno = EIO;
    return ERROR;
  }

  return PX4_OK;
}

int uORB::Manager::orb_check(int handle, bool *updated)
{
  return px4_ioctl(handle, ORBIOCUPDATED, (unsigned long)(uintptr_t)updated);
}

int uORB::Manager::orb_stat(int handle, uint64_t *time)
{
  return px4_ioctl(handle, ORBIOCLASTUPDATE, (unsigned long)(uintptr_t)time);
}

int uORB::Manager::orb_priority(int handle, int *priority)
{
  return px4_ioctl(handle, ORBIOCGPRIORITY, (unsigned long)(uintptr_t)priority);
}

int uORB::Manager::orb_set_interval(int handle, unsigned interval)
{
  return px4_ioctl(handle, ORBIOCSETINTERVAL, interval * 1000);
}


int uORB::Manager::node_advertise
(
    const struct orb_metadata *meta,
    int *instance,
    int priority
)
{
  int fd = -1;
  int ret = ERROR;

  /* fill advertiser data */
  const struct orb_advertdata adv = { meta, instance, priority };

  /* open the control device */
  fd = px4_open(TOPIC_MASTER_DEVICE_PATH, 0);

  if (fd < 0)
    goto out;

  /* advertise the object */
  ret = px4_ioctl(fd, ORBIOCADVERTISE, (unsigned long)(uintptr_t)&adv);

  /* it's PX4_OK if it already exists */
  if ((PX4_OK != ret) && (EEXIST == errno)) {
    ret = PX4_OK;
  }

out:

  if (fd >= 0)
    px4_close(fd);

  return ret;
}

int uORB::Manager::node_open
(
    Flavor f,
    const struct orb_metadata *meta,
    const void *data,
    bool advertiser,
    int *instance,
    int priority
)
{
  char path[orb_maxpath];
  int fd, ret;

  /*
   * If meta is null, the object was not defined, i.e. it is not
   * known to the system.  We can't advertise/subscribe such a thing.
   */
  if (nullptr == meta) {
    errno = ENOENT;
    return ERROR;
  }

  /*
   * Advertiser must publish an initial value.
   */
  if (advertiser && (data == nullptr)) {
    errno = EINVAL;
    return ERROR;
  }

  /*
   * Generate the path to the node and try to open it.
   */

  // FIXME - if *instance is uninitialized, why is this being called? Seems risky and
        // its definiately a waste. This is the case in muli-topic test.
  ret = uORB::Utils::node_mkpath(path, f, meta, instance);

  if (ret != PX4_OK) {
    errno = -ret;
    return ERROR;
  }

  /* open the path as either the advertiser or the subscriber */
  fd = px4_open(path, (advertiser) ? PX4_F_WRONLY : PX4_F_RDONLY);

  /* if we want to advertise and the node existed, we have to re-try again */
  if ((fd >= 0) && (instance != nullptr) && (advertiser)) {
    /* close the fd, we want a new one */
    px4_close(fd);

    /* the node_advertise call will automatically go for the next free entry */
    fd = -1;
  }

  /* we may need to advertise the node... */
  if (fd < 0) {

    /* try to create the node */
    ret = node_advertise(meta, instance, priority);

    if (ret == PX4_OK) {
      /* update the path, as it might have been updated during the node_advertise call */
      ret = uORB::Utils::node_mkpath(path, f, meta, instance);

      if (ret != PX4_OK) {
        errno = -ret;
        return ERROR;
      }
    }

    /* on success, try the open again */
    if (ret == PX4_OK) {
      fd = px4_open(path, (advertiser) ? PX4_F_WRONLY : PX4_F_RDONLY);
    }
  }

  if (fd < 0) {
    errno = EIO;
    return ERROR;
  }

  /* everything has been OK, we can return the handle now */
  return fd;
}
